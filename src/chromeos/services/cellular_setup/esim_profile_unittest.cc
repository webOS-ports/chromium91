// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "base/strings/utf_string_conversions.h"
#include "base/test/bind.h"
#include "base/test/metrics/histogram_tester.h"
#include "chromeos/dbus/hermes/hermes_euicc_client.h"
#include "chromeos/dbus/hermes/hermes_profile_client.h"
#include "chromeos/network/fake_network_connection_handler.h"
#include "chromeos/services/cellular_setup/esim_test_base.h"
#include "chromeos/services/cellular_setup/esim_test_utils.h"
#include "chromeos/services/cellular_setup/public/mojom/esim_manager.mojom-shared.h"
#include "mojo/public/cpp/bindings/pending_remote.h"

namespace chromeos {
namespace cellular_setup {

namespace {

const char kProfileUninstallationResultHistogram[] =
    "Network.Cellular.ESim.ProfileUninstallationResult";
const char kProfileRenameResultHistogram[] =
    "Network.Cellular.ESim.ProfileRenameResult";

mojom::ESimOperationResult UninstallProfile(
    const mojo::Remote<mojom::ESimProfile>& esim_profile) {
  mojom::ESimOperationResult uninstall_result;

  base::RunLoop run_loop;
  esim_profile->UninstallProfile(base::BindOnce(
      [](mojom::ESimOperationResult* out_uninstall_result,
         base::OnceClosure quit_closure,
         mojom::ESimOperationResult uninstall_result) {
        *out_uninstall_result = uninstall_result;
        std::move(quit_closure).Run();
      },
      &uninstall_result, run_loop.QuitClosure()));
  run_loop.Run();
  return uninstall_result;
}

mojom::ESimOperationResult EnableProfile(
    const mojo::Remote<mojom::ESimProfile>& esim_profile) {
  mojom::ESimOperationResult enable_result;

  base::RunLoop run_loop;
  esim_profile->EnableProfile(base::BindOnce(
      [](mojom::ESimOperationResult* out_enable_result,
         base::OnceClosure quit_closure,
         mojom::ESimOperationResult enable_result) {
        *out_enable_result = enable_result;
        std::move(quit_closure).Run();
      },
      &enable_result, run_loop.QuitClosure()));
  run_loop.Run();
  return enable_result;
}

mojom::ESimOperationResult DisableProfile(
    const mojo::Remote<mojom::ESimProfile>& esim_profile) {
  mojom::ESimOperationResult disable_result;

  base::RunLoop run_loop;
  esim_profile->DisableProfile(base::BindOnce(
      [](mojom::ESimOperationResult* out_disable_result,
         base::OnceClosure quit_closure,
         mojom::ESimOperationResult disable_result) {
        *out_disable_result = disable_result;
        std::move(quit_closure).Run();
      },
      &disable_result, run_loop.QuitClosure()));
  run_loop.Run();
  return disable_result;
}

mojom::ESimOperationResult SetProfileNickname(
    const mojo::Remote<mojom::ESimProfile>& esim_profile,
    const std::u16string& nickname) {
  mojom::ESimOperationResult result;

  base::RunLoop run_loop;
  esim_profile->SetProfileNickname(
      nickname, base::BindOnce(
                    [](mojom::ESimOperationResult* out_result,
                       base::OnceClosure quit_closure,
                       mojom::ESimOperationResult result) {
                      *out_result = result;
                      std::move(quit_closure).Run();
                    },
                    &result, run_loop.QuitClosure()));
  run_loop.Run();
  return result;
}

}  // namespace

class ESimProfileTest : public ESimTestBase {
 public:
  ESimProfileTest() = default;
  ESimProfileTest(const ESimProfileTest&) = delete;
  ESimProfileTest& operator=(const ESimProfileTest&) = delete;

  void SetUp() override {
    ESimTestBase::SetUp();
    SetupEuicc();
  }

  void TearDown() override {
    HermesProfileClient::Get()->GetTestInterface()->SetConnectedAfterEnable(
        /*connected_after_enable=*/false);
  }

  mojo::Remote<mojom::ESimProfile> GetESimProfileForIccid(
      const std::string& eid,
      const std::string& iccid) {
    mojo::Remote<mojom::Euicc> euicc = GetEuiccForEid(eid);
    if (!euicc.is_bound()) {
      return mojo::Remote<mojom::ESimProfile>();
    }
    std::vector<mojo::PendingRemote<mojom::ESimProfile>>
        profile_pending_remotes = GetProfileList(euicc);
    for (auto& profile_pending_remote : profile_pending_remotes) {
      mojo::Remote<mojom::ESimProfile> esim_profile(
          std::move(profile_pending_remote));
      mojom::ESimProfilePropertiesPtr profile_properties =
          GetESimProfileProperties(esim_profile);
      if (profile_properties->iccid == iccid) {
        return esim_profile;
      }
    }
    return mojo::Remote<mojom::ESimProfile>();
  }

  mojom::ProfileInstallResult InstallProfile(
      const mojo::Remote<mojom::ESimProfile>& esim_profile,
      bool wait_for_connect,
      bool fail_connect) {
    mojom::ProfileInstallResult out_install_result;

    base::RunLoop run_loop;
    esim_profile->InstallProfile(
        /*confirmation_code=*/std::string(),
        base::BindLambdaForTesting(
            [&](mojom::ProfileInstallResult install_result) {
              out_install_result = install_result;
              run_loop.Quit();
            }));

    if (wait_for_connect) {
      base::RunLoop().RunUntilIdle();
      EXPECT_LE(1u, network_connection_handler()->connect_calls().size());
      if (fail_connect) {
        network_connection_handler()
            ->connect_calls()
            .back()
            .InvokeErrorCallback("fake_error_name", /*error_data=*/nullptr);
      } else {
        network_connection_handler()
            ->connect_calls()
            .back()
            .InvokeSuccessCallback();
      }
    }

    run_loop.Run();
    return out_install_result;
  }
};

TEST_F(ESimProfileTest, GetProperties) {
  HermesEuiccClient::TestInterface* euicc_test =
      HermesEuiccClient::Get()->GetTestInterface();
  dbus::ObjectPath profile_path = euicc_test->AddFakeCarrierProfile(
      dbus::ObjectPath(ESimTestBase::kTestEuiccPath),
      hermes::profile::State::kPending, "",
      HermesEuiccClient::TestInterface::AddCarrierProfileBehavior::
          kAddProfileWithService);
  base::RunLoop().RunUntilIdle();
  HermesProfileClient::Properties* dbus_properties =
      HermesProfileClient::Get()->GetProperties(profile_path);

  mojo::Remote<mojom::ESimProfile> esim_profile = GetESimProfileForIccid(
      ESimTestBase::kTestEid, dbus_properties->iccid().value());
  ASSERT_TRUE(esim_profile.is_bound());
  mojom::ESimProfilePropertiesPtr mojo_properties =
      GetESimProfileProperties(esim_profile);
  EXPECT_EQ(dbus_properties->iccid().value(), mojo_properties->iccid);
}

TEST_F(ESimProfileTest, InstallProfile) {
  HermesEuiccClient::TestInterface* euicc_test =
      HermesEuiccClient::Get()->GetTestInterface();
  dbus::ObjectPath profile_path = euicc_test->AddFakeCarrierProfile(
      dbus::ObjectPath(ESimTestBase::kTestEuiccPath),
      hermes::profile::State::kPending, "",
      HermesEuiccClient::TestInterface::AddCarrierProfileBehavior::
          kAddProfileWithService);
  base::RunLoop().RunUntilIdle();
  HermesProfileClient::Properties* dbus_properties =
      HermesProfileClient::Get()->GetProperties(profile_path);

  // Verify that install errors return error code properly.
  euicc_test->QueueHermesErrorStatus(
      HermesResponseStatus::kErrorNeedConfirmationCode);
  mojo::Remote<mojom::ESimProfile> esim_profile = GetESimProfileForIccid(
      ESimTestBase::kTestEid, dbus_properties->iccid().value());
  ASSERT_TRUE(esim_profile.is_bound());
  mojom::ProfileInstallResult install_result = InstallProfile(
      esim_profile, /*wait_for_connect=*/false, /*fail_connect=*/false);
  EXPECT_EQ(mojom::ProfileInstallResult::kErrorNeedsConfirmationCode,
            install_result);

  base::HistogramTester histogram_tester;
  histogram_tester.ExpectTotalCount(
      "Network.Cellular.ESim.ProfileDownload.PendingProfile.Latency", 0);

  // Verify that installing pending profile returns proper results
  // and updates esim_profile properties.
  install_result = InstallProfile(esim_profile, /*wait_for_connect=*/true,
                                  /*fail_connect=*/false);
  // Wait for property changes to propagate.
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(mojom::ProfileInstallResult::kSuccess, install_result);
  mojom::ESimProfilePropertiesPtr mojo_properties =
      GetESimProfileProperties(esim_profile);
  EXPECT_EQ(dbus_properties->iccid().value(), mojo_properties->iccid);
  EXPECT_NE(mojo_properties->state, mojom::ProfileState::kPending);
  EXPECT_EQ(1u, observer()->profile_list_change_calls().size());

  histogram_tester.ExpectTotalCount(
      "Network.Cellular.ESim.ProfileDownload.PendingProfile.Latency", 1);
}

TEST_F(ESimProfileTest, InstallProfileAlreadyConnected) {
  dbus::ObjectPath profile_path =
      HermesEuiccClient::Get()->GetTestInterface()->AddFakeCarrierProfile(
          dbus::ObjectPath(ESimTestBase::kTestEuiccPath),
          hermes::profile::State::kPending, /*activation_code=*/std::string(),
          HermesEuiccClient::TestInterface::AddCarrierProfileBehavior::
              kAddProfileWithService);
  HermesProfileClient::Properties* dbus_properties =
      HermesProfileClient::Get()->GetProperties(profile_path);
  mojo::Remote<mojom::ESimProfile> esim_profile = GetESimProfileForIccid(
      ESimTestBase::kTestEid, dbus_properties->iccid().value());

  HermesProfileClient::Get()->GetTestInterface()->SetConnectedAfterEnable(
      /*connected_after_enable=*/true);

  mojom::ProfileInstallResult install_result =
      InstallProfile(esim_profile, /*wait_for_connect=*/false,
                     /*fail_connect=*/false);
  EXPECT_EQ(mojom::ProfileInstallResult::kSuccess, install_result);
}

TEST_F(ESimProfileTest, InstallConnectFailure) {
  HermesEuiccClient::TestInterface* euicc_test =
      HermesEuiccClient::Get()->GetTestInterface();
  dbus::ObjectPath profile_path = euicc_test->AddFakeCarrierProfile(
      dbus::ObjectPath(ESimTestBase::kTestEuiccPath),
      hermes::profile::State::kPending, "",
      HermesEuiccClient::TestInterface::AddCarrierProfileBehavior::
          kAddProfileWithService);
  base::RunLoop().RunUntilIdle();
  HermesProfileClient::Properties* dbus_properties =
      HermesProfileClient::Get()->GetProperties(profile_path);
  mojo::Remote<mojom::ESimProfile> esim_profile = GetESimProfileForIccid(
      ESimTestBase::kTestEid, dbus_properties->iccid().value());

  // Verify that connect failures still return success code.
  mojom::ProfileInstallResult install_result =
      InstallProfile(esim_profile, /*wait_for_connect=*/true,
                     /*fail_connect=*/true);
  EXPECT_EQ(mojom::ProfileInstallResult::kSuccess, install_result);
}

TEST_F(ESimProfileTest, UninstallProfile) {
  base::HistogramTester histogram_tester;

  HermesEuiccClient::TestInterface* euicc_test =
      HermesEuiccClient::Get()->GetTestInterface();
  dbus::ObjectPath active_profile_path = euicc_test->AddFakeCarrierProfile(
      dbus::ObjectPath(kTestEuiccPath), hermes::profile::State::kActive, "",
      HermesEuiccClient::TestInterface::AddCarrierProfileBehavior::
          kAddProfileWithService);
  dbus::ObjectPath pending_profile_path = euicc_test->AddFakeCarrierProfile(
      dbus::ObjectPath(kTestEuiccPath), hermes::profile::State::kPending, "",
      HermesEuiccClient::TestInterface::AddCarrierProfileBehavior::
          kAddProfileWithService);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(1u, observer()->profile_list_change_calls().size());
  observer()->Reset();
  HermesProfileClient::Properties* pending_profile_dbus_properties =
      HermesProfileClient::Get()->GetProperties(pending_profile_path);
  HermesProfileClient::Properties* active_profile_dbus_properties =
      HermesProfileClient::Get()->GetProperties(active_profile_path);
  histogram_tester.ExpectTotalCount(kProfileUninstallationResultHistogram, 0);

  // Verify that uninstall error codes are returned properly.
  euicc_test->QueueHermesErrorStatus(
      HermesResponseStatus::kErrorInvalidResponse);
  mojo::Remote<mojom::ESimProfile> active_esim_profile = GetESimProfileForIccid(
      ESimTestBase::kTestEid, active_profile_dbus_properties->iccid().value());
  ASSERT_TRUE(active_esim_profile.is_bound());
  mojom::ESimOperationResult result = UninstallProfile(active_esim_profile);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(mojom::ESimOperationResult::kFailure, result);
  EXPECT_EQ(0u, observer()->profile_list_change_calls().size());
  histogram_tester.ExpectTotalCount(kProfileUninstallationResultHistogram, 1);
  histogram_tester.ExpectBucketCount(kProfileUninstallationResultHistogram,
                                     false, 1);

  // Verify that pending profiles cannot be uninstalled
  observer()->Reset();
  mojo::Remote<mojom::ESimProfile> pending_esim_profile =
      GetESimProfileForIccid(ESimTestBase::kTestEid,
                             pending_profile_dbus_properties->iccid().value());
  ASSERT_TRUE(pending_esim_profile.is_bound());
  result = UninstallProfile(pending_esim_profile);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(mojom::ESimOperationResult::kFailure, result);
  EXPECT_EQ(0u, observer()->profile_list_change_calls().size());
  histogram_tester.ExpectTotalCount(kProfileUninstallationResultHistogram, 1);
  histogram_tester.ExpectBucketCount(kProfileUninstallationResultHistogram,
                                     false, 1);

  // Verify that uninstall removes the profile and notifies observers properly.
  observer()->Reset();
  result = UninstallProfile(active_esim_profile);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(mojom::ESimOperationResult::kSuccess, result);
  ASSERT_EQ(1u, observer()->profile_list_change_calls().size());
  EXPECT_EQ(1u, GetProfileList(GetEuiccForEid(ESimTestBase::kTestEid)).size());
  histogram_tester.ExpectTotalCount(kProfileUninstallationResultHistogram, 2);
  histogram_tester.ExpectBucketCount(kProfileUninstallationResultHistogram,
                                     true, 1);
}

TEST_F(ESimProfileTest, EnableProfile) {
  HermesEuiccClient::TestInterface* euicc_test =
      HermesEuiccClient::Get()->GetTestInterface();
  dbus::ObjectPath inactive_profile_path = euicc_test->AddFakeCarrierProfile(
      dbus::ObjectPath(kTestEuiccPath), hermes::profile::State::kInactive, "",
      HermesEuiccClient::TestInterface::AddCarrierProfileBehavior::
          kAddProfileWithService);
  dbus::ObjectPath pending_profile_path = euicc_test->AddFakeCarrierProfile(
      dbus::ObjectPath(kTestEuiccPath), hermes::profile::State::kPending, "",
      HermesEuiccClient::TestInterface::AddCarrierProfileBehavior::
          kAddProfileWithService);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(1u, observer()->profile_list_change_calls().size());
  observer()->Reset();
  HermesProfileClient::Properties* pending_profile_dbus_properties =
      HermesProfileClient::Get()->GetProperties(pending_profile_path);
  HermesProfileClient::Properties* inactive_profile_dbus_properties =
      HermesProfileClient::Get()->GetProperties(inactive_profile_path);

  // Verify that pending profiles cannot be enabled.
  mojo::Remote<mojom::ESimProfile> pending_esim_profile =
      GetESimProfileForIccid(ESimTestBase::kTestEid,
                             pending_profile_dbus_properties->iccid().value());
  ASSERT_TRUE(pending_esim_profile.is_bound());
  mojom::ESimOperationResult result = EnableProfile(pending_esim_profile);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(mojom::ESimOperationResult::kFailure, result);
  EXPECT_EQ(0u, observer()->profile_change_calls().size());

  // Verify that enabling profile returns result properly.
  mojo::Remote<mojom::ESimProfile> inactive_esim_profile =
      GetESimProfileForIccid(ESimTestBase::kTestEid,
                             inactive_profile_dbus_properties->iccid().value());
  ASSERT_TRUE(inactive_esim_profile.is_bound());
  result = EnableProfile(inactive_esim_profile);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(mojom::ESimOperationResult::kSuccess, result);

  mojom::ESimProfilePropertiesPtr inactive_profile_mojo_properties =
      GetESimProfileProperties(inactive_esim_profile);
  EXPECT_EQ(inactive_profile_dbus_properties->iccid().value(),
            inactive_profile_mojo_properties->iccid);
  EXPECT_EQ(mojom::ProfileState::kActive,
            inactive_profile_mojo_properties->state);
}

TEST_F(ESimProfileTest, DisableProfile) {
  HermesEuiccClient::TestInterface* euicc_test =
      HermesEuiccClient::Get()->GetTestInterface();
  dbus::ObjectPath active_profile_path = euicc_test->AddFakeCarrierProfile(
      dbus::ObjectPath(kTestEuiccPath), hermes::profile::State::kActive, "",
      HermesEuiccClient::TestInterface::AddCarrierProfileBehavior::
          kAddProfileWithService);
  dbus::ObjectPath pending_profile_path = euicc_test->AddFakeCarrierProfile(
      dbus::ObjectPath(kTestEuiccPath), hermes::profile::State::kPending, "",
      HermesEuiccClient::TestInterface::AddCarrierProfileBehavior::
          kAddProfileWithService);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(1u, observer()->profile_list_change_calls().size());
  observer()->Reset();
  HermesProfileClient::Properties* pending_profile_dbus_properties =
      HermesProfileClient::Get()->GetProperties(pending_profile_path);
  HermesProfileClient::Properties* active_profile_dbus_properties =
      HermesProfileClient::Get()->GetProperties(active_profile_path);

  // Verify that pending profiles cannot be disabled.
  mojo::Remote<mojom::ESimProfile> pending_esim_profile =
      GetESimProfileForIccid(ESimTestBase::kTestEid,
                             pending_profile_dbus_properties->iccid().value());
  ASSERT_TRUE(pending_esim_profile.is_bound());
  mojom::ESimOperationResult result = DisableProfile(pending_esim_profile);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(mojom::ESimOperationResult::kFailure, result);
  EXPECT_EQ(0u, observer()->profile_change_calls().size());

  // Verify that disabling profile returns result properly.
  mojo::Remote<mojom::ESimProfile> active_esim_profile = GetESimProfileForIccid(
      ESimTestBase::kTestEid, active_profile_dbus_properties->iccid().value());
  ASSERT_TRUE(active_esim_profile.is_bound());
  result = DisableProfile(active_esim_profile);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(mojom::ESimOperationResult::kSuccess, result);

  mojom::ESimProfilePropertiesPtr active_profile_mojo_properties =
      GetESimProfileProperties(active_esim_profile);
  EXPECT_EQ(active_profile_dbus_properties->iccid().value(),
            active_profile_mojo_properties->iccid);
  EXPECT_EQ(mojom::ProfileState::kInactive,
            active_profile_mojo_properties->state);
}

TEST_F(ESimProfileTest, SetProfileNickName) {
  const std::u16string test_nickname = u"Test nickname";
  base::HistogramTester histogram_tester;

  HermesEuiccClient::TestInterface* euicc_test =
      HermesEuiccClient::Get()->GetTestInterface();
  dbus::ObjectPath active_profile_path = euicc_test->AddFakeCarrierProfile(
      dbus::ObjectPath(kTestEuiccPath), hermes::profile::State::kActive, "",
      HermesEuiccClient::TestInterface::AddCarrierProfileBehavior::
          kAddProfileWithService);
  dbus::ObjectPath pending_profile_path = euicc_test->AddFakeCarrierProfile(
      dbus::ObjectPath(kTestEuiccPath), hermes::profile::State::kPending, "",
      HermesEuiccClient::TestInterface::AddCarrierProfileBehavior::
          kAddProfileWithService);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(1u, observer()->profile_list_change_calls().size());
  observer()->Reset();
  HermesProfileClient::Properties* pending_profile_dbus_properties =
      HermesProfileClient::Get()->GetProperties(pending_profile_path);
  HermesProfileClient::Properties* active_profile_dbus_properties =
      HermesProfileClient::Get()->GetProperties(active_profile_path);

  // Verify that pending profiles cannot be modified.
  mojo::Remote<mojom::ESimProfile> pending_esim_profile =
      GetESimProfileForIccid(ESimTestBase::kTestEid,
                             pending_profile_dbus_properties->iccid().value());
  ASSERT_TRUE(pending_esim_profile.is_bound());
  mojom::ESimOperationResult result =
      SetProfileNickname(pending_esim_profile, test_nickname);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(mojom::ESimOperationResult::kFailure, result);
  EXPECT_EQ(0u, observer()->profile_change_calls().size());

  // Verify that nickname can be set on active profiles.
  histogram_tester.ExpectTotalCount(kProfileRenameResultHistogram, 0);
  mojo::Remote<mojom::ESimProfile> active_esim_profile = GetESimProfileForIccid(
      ESimTestBase::kTestEid, active_profile_dbus_properties->iccid().value());
  ASSERT_TRUE(active_esim_profile.is_bound());
  result = SetProfileNickname(active_esim_profile, test_nickname);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(mojom::ESimOperationResult::kSuccess, result);
  histogram_tester.ExpectTotalCount(kProfileRenameResultHistogram, 1);
  histogram_tester.ExpectBucketCount(kProfileRenameResultHistogram, true, 1);

  mojom::ESimProfilePropertiesPtr active_profile_mojo_properties =
      GetESimProfileProperties(active_esim_profile);
  EXPECT_EQ(test_nickname, active_profile_mojo_properties->nickname);
}

}  // namespace cellular_setup
}  // namespace chromeos
