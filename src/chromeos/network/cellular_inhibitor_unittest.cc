// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/network/cellular_inhibitor.h"

#include <memory>

#include "base/bind.h"
#include "base/callback_helpers.h"
#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "chromeos/dbus/dbus_thread_manager.h"
#include "chromeos/dbus/shill/fake_shill_device_client.h"
#include "chromeos/dbus/shill/shill_clients.h"
#include "chromeos/dbus/shill/shill_manager_client.h"
#include "chromeos/network/network_device_handler_impl.h"
#include "chromeos/network/network_handler_callbacks.h"
#include "chromeos/network/network_state_handler.h"
#include "chromeos/network/network_state_test_helper.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/cros_system_api/dbus/service_constants.h"
#include "third_party/cros_system_api/dbus/shill/dbus-constants.h"

namespace chromeos {
namespace {

const char kDefaultCellularDevicePath[] = "stub_cellular_device";

enum class GetInhibitedPropertyResult { kTrue, kFalse, kOperationFailed };

class TestObserver : public CellularInhibitor::Observer {
 public:
  TestObserver() = default;
  ~TestObserver() override = default;

  size_t num_observer_events() const { return num_observer_events_; }

 private:
  // CellularInhibitor::Observer:
  void OnInhibitStateChanged() override { ++num_observer_events_; }

  size_t num_observer_events_ = 0u;
};

}  // namespace

class CellularInhibitorTest : public testing::Test {
 protected:
  CellularInhibitorTest()
      : task_environment_(
            base::test::SingleThreadTaskEnvironment::MainThreadType::UI,
            base::test::TaskEnvironment::TimeSource::MOCK_TIME),
        helper_(/*use_default_devices_and_services=*/false) {}
  ~CellularInhibitorTest() override = default;

  // testing::Test:
  void SetUp() override {
    helper_.device_test()->SetSimulateUninhibitScanning(false);
    helper_.device_test()->ClearDevices();
    cellular_inhibitor_.Init(helper_.network_state_handler(),
                             helper_.network_device_handler());
    cellular_inhibitor_.AddObserver(&observer_);
  }

  void TearDown() override {
    cellular_inhibitor_.RemoveObserver(&observer_);
    helper_.device_test()->SetPropertyChangeDelay(base::nullopt);
  }

  void AddCellularDevice() {
    helper_.device_test()->AddDevice(kDefaultCellularDevicePath,
                                     shill::kTypeCellular, "cellular1");
    base::RunLoop().RunUntilIdle();
  }

  std::unique_ptr<CellularInhibitor::InhibitLock> InhibitCellularScanningSync(
      CellularInhibitor::InhibitReason inhibit_reason) {
    base::RunLoop run_loop;

    std::unique_ptr<CellularInhibitor::InhibitLock> inhibit_lock;
    cellular_inhibitor_.InhibitCellularScanning(
        inhibit_reason,
        base::BindLambdaForTesting(
            [&](std::unique_ptr<CellularInhibitor::InhibitLock> result) {
              inhibit_lock = std::move(result);
              run_loop.Quit();
            }));

    run_loop.Run();
    return inhibit_lock;
  }

  void InhibitCellularScanning(
      CellularInhibitor::InhibitReason inhibit_reason,
      std::unique_ptr<CellularInhibitor::InhibitLock>& lock) {
    cellular_inhibitor_.InhibitCellularScanning(
        inhibit_reason,
        base::BindLambdaForTesting(
            [&](std::unique_ptr<CellularInhibitor::InhibitLock> result) {
              lock = std::move(result);
            }));
  }

  void SetScanning(bool is_scanning) {
    helper_.network_device_handler()->SetDeviceProperty(
        kDefaultCellularDevicePath, shill::kScanningProperty,
        base::Value(is_scanning), base::DoNothing(), base::DoNothing());
    base::RunLoop().RunUntilIdle();
  }

  GetInhibitedPropertyResult GetInhibitedProperty() {
    properties_.reset();
    helper_.network_device_handler()->GetDeviceProperties(
        kDefaultCellularDevicePath,
        base::BindOnce(&CellularInhibitorTest::GetPropertiesCallback,
                       base::Unretained(this)));
    base::RunLoop().RunUntilIdle();

    if (!properties_)
      return GetInhibitedPropertyResult::kOperationFailed;

    bool inhibited;
    EXPECT_TRUE(properties_->GetBooleanWithoutPathExpansion(
        shill::kInhibitedProperty, &inhibited));
    return inhibited ? GetInhibitedPropertyResult::kTrue
                     : GetInhibitedPropertyResult::kFalse;
  }

  base::Optional<CellularInhibitor::InhibitReason> GetInhibitReason() const {
    return cellular_inhibitor_.GetInhibitReason();
  }

  void SetDevicePropertyChangeDelay() {
    helper_.device_test()->SetPropertyChangeDelay(
        CellularInhibitor::kInhibitPropertyChangeTimeout);
  }

  void FastForwardInhibitPropertyChangeTimeout() {
    task_environment_.FastForwardBy(
        CellularInhibitor::kInhibitPropertyChangeTimeout);
  }

  size_t GetNumObserverEvents() const {
    return observer_.num_observer_events();
  }

 private:
  void GetPropertiesCallback(const std::string& device_path,
                             base::Optional<base::Value> properties) {
    if (!properties) {
      properties_.reset();
      return;
    }

    properties_ = base::DictionaryValue::From(
        std::make_unique<base::Value>(std::move(*properties)));
  }

  base::test::SingleThreadTaskEnvironment task_environment_;
  NetworkStateTestHelper helper_;
  CellularInhibitor cellular_inhibitor_;
  TestObserver observer_;

  std::unique_ptr<base::DictionaryValue> properties_;
};

TEST_F(CellularInhibitorTest, SuccessSingleRequest) {
  AddCellularDevice();

  std::unique_ptr<CellularInhibitor::InhibitLock> inhibit_lock =
      InhibitCellularScanningSync(
          CellularInhibitor::InhibitReason::kInstallingProfile);
  // Ensure that a valid lock is returned and Inhibit property is set on
  // Cellular device.
  EXPECT_TRUE(inhibit_lock);
  EXPECT_EQ(1u, GetNumObserverEvents());
  EXPECT_EQ(CellularInhibitor::InhibitReason::kInstallingProfile,
            GetInhibitReason());
  EXPECT_EQ(GetInhibitedPropertyResult::kTrue, GetInhibitedProperty());

  // Ensure that deleting lock uninhibits the Cellular device.
  inhibit_lock.reset();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(GetInhibitedPropertyResult::kFalse, GetInhibitedProperty());

  // At this point, we are still in the inhibited state, since scanning has not
  // yet started and stopped.
  EXPECT_EQ(1u, GetNumObserverEvents());
  EXPECT_EQ(CellularInhibitor::InhibitReason::kInstallingProfile,
            GetInhibitReason());

  // Start and stop scanning; this should alert observers that the inhibit
  // operation has completed.
  SetScanning(true);
  SetScanning(false);
  EXPECT_EQ(2u, GetNumObserverEvents());
  EXPECT_FALSE(GetInhibitReason().has_value());
}

TEST_F(CellularInhibitorTest, SuccessMultipleRequests) {
  AddCellularDevice();

  // Make two inhibit requests in parallel.
  std::unique_ptr<CellularInhibitor::InhibitLock> inhibit_lock;
  std::unique_ptr<CellularInhibitor::InhibitLock> inhibit_lock2;
  InhibitCellularScanning(CellularInhibitor::InhibitReason::kInstallingProfile,
                          inhibit_lock);
  InhibitCellularScanning(CellularInhibitor::InhibitReason::kRemovingProfile,
                          inhibit_lock2);
  base::RunLoop().RunUntilIdle();

  // Ensure that the only one inhibit lock has been granted.
  EXPECT_TRUE(inhibit_lock);
  EXPECT_FALSE(inhibit_lock2);
  EXPECT_EQ(1u, GetNumObserverEvents());
  EXPECT_EQ(CellularInhibitor::InhibitReason::kInstallingProfile,
            GetInhibitReason());
  EXPECT_EQ(GetInhibitedPropertyResult::kTrue, GetInhibitedProperty());

  // Release the first lock; though this causes the Inhibited property to false,
  // it should not yet start the next (queued) Inhibit flow, since the scanning
  // flow has not yet started.
  inhibit_lock.reset();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(1u, GetNumObserverEvents());
  EXPECT_EQ(CellularInhibitor::InhibitReason::kInstallingProfile,
            GetInhibitReason());
  EXPECT_EQ(GetInhibitedPropertyResult::kFalse, GetInhibitedProperty());
  EXPECT_FALSE(inhibit_lock2);

  // Set the Cellular device's scanning property to true, simulating the
  // inhibit flow within Shill. This still should not start the next inhibit
  // flow.
  SetScanning(true);
  EXPECT_EQ(1u, GetNumObserverEvents());
  EXPECT_EQ(CellularInhibitor::InhibitReason::kInstallingProfile,
            GetInhibitReason());
  EXPECT_EQ(GetInhibitedPropertyResult::kFalse, GetInhibitedProperty());
  EXPECT_FALSE(inhibit_lock2);

  // Change scanning back to false, which should trigger the second lock being
  // set.
  SetScanning(false);
  EXPECT_TRUE(inhibit_lock2);
  EXPECT_EQ(3u, GetNumObserverEvents());
  EXPECT_EQ(CellularInhibitor::InhibitReason::kRemovingProfile,
            GetInhibitReason());
  EXPECT_EQ(GetInhibitedPropertyResult::kTrue, GetInhibitedProperty());

  // Ensure that inhibited property is set to false when all locks are deleted.
  inhibit_lock2.reset();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(3u, GetNumObserverEvents());
  EXPECT_EQ(CellularInhibitor::InhibitReason::kRemovingProfile,
            GetInhibitReason());
  EXPECT_EQ(GetInhibitedPropertyResult::kFalse, GetInhibitedProperty());
}

TEST_F(CellularInhibitorTest, Failure) {
  // Do not add a Cellular device. This should cause commands below to fail,
  // since the device cannot be inhibited if it does not exist.

  std::unique_ptr<CellularInhibitor::InhibitLock> inhibit_lock =
      InhibitCellularScanningSync(
          CellularInhibitor::InhibitReason::kInstallingProfile);
  EXPECT_EQ(GetInhibitedPropertyResult::kOperationFailed,
            GetInhibitedProperty());
  EXPECT_FALSE(inhibit_lock);
}

TEST_F(CellularInhibitorTest, FailurePropertySetTimeout) {
  AddCellularDevice();
  SetDevicePropertyChangeDelay();

  std::unique_ptr<CellularInhibitor::InhibitLock> inhibit_lock;
  InhibitCellularScanning(CellularInhibitor::InhibitReason::kInstallingProfile,
                          inhibit_lock);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(inhibit_lock);
  FastForwardInhibitPropertyChangeTimeout();
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(inhibit_lock);
}

}  // namespace chromeos
