// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/arc/session/arc_vm_client_adapter.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/callback_helpers.h"
#include "base/command_line.h"
#include "base/containers/contains.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/guid.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/posix/safe_strerror.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "base/task/current_thread.h"
#include "base/task/post_task.h"
#include "base/test/bind.h"
#include "base/test/scoped_chromeos_version_info.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/time/time.h"
#include "chromeos/cryptohome/cryptohome_parameters.h"
#include "chromeos/dbus/dbus_thread_manager.h"
#include "chromeos/dbus/debug_daemon/fake_debug_daemon_client.h"
#include "chromeos/dbus/fake_concierge_client.h"
#include "chromeos/dbus/session_manager/fake_session_manager_client.h"
#include "chromeos/dbus/upstart/fake_upstart_client.h"
#include "components/arc/arc_features.h"
#include "components/arc/arc_util.h"
#include "components/arc/session/arc_session.h"
#include "components/arc/session/file_system_status.h"
#include "components/user_manager/user_names.h"
#include "content/public/test/browser_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace arc {
namespace {

constexpr const char kArcVmPerBoardFeaturesJobName[] =
    "arcvm_2dper_2dboard_2dfeatures";
constexpr const char kArcVmBootNotificationServerAddressPrefix[] =
    "\0test_arcvm_boot_notification_server";
constexpr char kArcVmPreLoginServicesJobName[] =
    "arcvm_2dpre_2dlogin_2dservices";
constexpr char kArcVmPostLoginServicesJobName[] =
    "arcvm_2dpost_2dlogin_2dservices";
constexpr char kArcVmPostVmStartServicesJobName[] =
    "arcvm_2dpost_2dvm_2dstart_2dservices";
constexpr const char kArcVmDefaultOwner[] = "ARCVM_DEFAULT_OWNER";

constexpr const char kUserIdHash[] = "this_is_a_valid_user_id_hash";
constexpr const char kSerialNumber[] = "AAAABBBBCCCCDDDD1234";
constexpr int64_t kCid = 123;

StartParams GetPopulatedStartParams() {
  StartParams params;
  params.native_bridge_experiment = false;
  params.lcd_density = 240;
  params.arc_file_picker_experiment = true;
  params.play_store_auto_update =
      StartParams::PlayStoreAutoUpdate::AUTO_UPDATE_ON;
  params.arc_custom_tabs_experiment = true;
  params.num_cores_disabled = 2;
  return params;
}

UpgradeParams GetPopulatedUpgradeParams() {
  UpgradeParams params;
  params.account_id = "fee1dead";
  params.skip_boot_completed_broadcast = true;
  params.packages_cache_mode = UpgradeParams::PackageCacheMode::COPY_ON_INIT;
  params.skip_gms_core_cache = true;
  params.supervision_transition = ArcSupervisionTransition::CHILD_TO_REGULAR;
  params.locale = "en-US";
  params.preferred_languages = {"en_US", "en", "ja"};
  params.is_demo_session = true;
  params.demo_session_apps_path = base::FilePath("/pato/to/demo.apk");
  return params;
}

std::string GenerateAbstractAddress() {
  std::string address(kArcVmBootNotificationServerAddressPrefix,
                      sizeof(kArcVmBootNotificationServerAddressPrefix) - 1);
  return address.append("-" +
                        base::GUID::GenerateRandomV4().AsLowercaseString());
}

// A debugd client that can fail to start Concierge.
// TODO(yusukes): Merge the feature to FakeDebugDaemonClient.
class TestDebugDaemonClient : public chromeos::FakeDebugDaemonClient {
 public:
  TestDebugDaemonClient() = default;
  ~TestDebugDaemonClient() override = default;

  void BackupArcBugReport(const cryptohome::AccountIdentifier& id,
                          chromeos::VoidDBusMethodCallback callback) override {
    backup_arc_bug_report_called_ = true;
    std::move(callback).Run(backup_arc_bug_report_result_);
  }

  bool backup_arc_bug_report_called() const {
    return backup_arc_bug_report_called_;
  }
  void set_backup_arc_bug_report_result(bool result) {
    backup_arc_bug_report_result_ = result;
  }

 private:
  bool backup_arc_bug_report_called_ = false;
  bool backup_arc_bug_report_result_ = true;

  DISALLOW_COPY_AND_ASSIGN(TestDebugDaemonClient);
};

// A concierge that remembers the parameter passed to StartArcVm.
// TODO(yusukes): Merge the feature to FakeConciergeClient.
class TestConciergeClient : public chromeos::FakeConciergeClient {
 public:
  TestConciergeClient() = default;
  ~TestConciergeClient() override = default;

  void StopVm(const vm_tools::concierge::StopVmRequest& request,
              chromeos::DBusMethodCallback<vm_tools::concierge::StopVmResponse>
                  callback) override {
    ++stop_vm_call_count_;
    stop_vm_request_ = request;
    chromeos::FakeConciergeClient::StopVm(request, std::move(callback));
    if (on_stop_vm_callback_ && (stop_vm_call_count_ == callback_count_))
      std::move(on_stop_vm_callback_).Run();
  }

  void StartArcVm(
      const vm_tools::concierge::StartArcVmRequest& request,
      chromeos::DBusMethodCallback<vm_tools::concierge::StartVmResponse>
          callback) override {
    start_arc_vm_request_ = request;
    chromeos::FakeConciergeClient::StartArcVm(request, std::move(callback));
  }

  int stop_vm_call_count() const { return stop_vm_call_count_; }

  const vm_tools::concierge::StartArcVmRequest& start_arc_vm_request() const {
    return start_arc_vm_request_;
  }

  const vm_tools::concierge::StopVmRequest& stop_vm_request() const {
    return stop_vm_request_;
  }

  // Set a callback to be run when stop_vm_call_count() == count.
  void set_on_stop_vm_callback(base::OnceClosure callback, int count) {
    on_stop_vm_callback_ = std::move(callback);
    DCHECK_NE(0, count);
    callback_count_ = count;
  }

 private:
  int stop_vm_call_count_ = 0;
  // When callback_count_ == 0, the on_stop_vm_callback_ is not run.
  int callback_count_ = 0;
  vm_tools::concierge::StartArcVmRequest start_arc_vm_request_;
  vm_tools::concierge::StopVmRequest stop_vm_request_;
  base::OnceClosure on_stop_vm_callback_;

  DISALLOW_COPY_AND_ASSIGN(TestConciergeClient);
};

// A fake ArcVmBootNotificationServer that listens on an UDS and records
// connections and the data sent to it.
class TestArcVmBootNotificationServer
    : public base::MessagePumpForUI::FdWatcher {
 public:
  TestArcVmBootNotificationServer() = default;
  ~TestArcVmBootNotificationServer() override { Stop(); }
  TestArcVmBootNotificationServer(const TestArcVmBootNotificationServer&) =
      delete;
  TestArcVmBootNotificationServer& operator=(
      const TestArcVmBootNotificationServer&) = delete;

  // Creates a socket and binds it to a name in the abstract namespace, then
  // starts listening to the socket on another thread.
  void Start(const std::string& abstract_addr) {
    fd_.reset(socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0));
    ASSERT_TRUE(fd_.is_valid())
        << "open failed with " << base::safe_strerror(errno);

    sockaddr_un addr{.sun_family = AF_UNIX};
    ASSERT_LT(abstract_addr.size(), sizeof(addr.sun_path))
        << "abstract_addr is too long: " << abstract_addr;
    ASSERT_EQ('\0', abstract_addr[0])
        << "abstract_addr is not abstract: " << abstract_addr;
    memset(addr.sun_path, 0, sizeof(addr.sun_path));
    memcpy(addr.sun_path, abstract_addr.data(), abstract_addr.size());
    LOG(INFO) << "Abstract address: \\0" << &(addr.sun_path[1]);

    ASSERT_EQ(HANDLE_EINTR(bind(fd_.get(), reinterpret_cast<sockaddr*>(&addr),
                                sizeof(sockaddr_un))),
              0)
        << "bind failed with " << base::safe_strerror(errno);
    ASSERT_EQ(HANDLE_EINTR(listen(fd_.get(), 5)), 0)
        << "listen failed with " << base::safe_strerror(errno);
    controller_.reset(new base::MessagePumpForUI::FdWatchController(FROM_HERE));
    ASSERT_TRUE(base::CurrentUIThread::Get()->WatchFileDescriptor(
        fd_.get(), true, base::MessagePumpForUI::WATCH_READ, controller_.get(),
        this));
  }

  // Release the socket.
  void Stop() {
    controller_.reset(nullptr);
    fd_.reset(-1);
  }

  // Sets a callback to be run immediately after the next connection.
  void SetConnectionCallback(base::OnceClosure callback) {
    callback_ = std::move(callback);
  }

  int connection_count() { return num_connections_; }

  std::string received_data() { return received_; }

  // base::MessagePumpForUI::FdWatcher overrides
  void OnFileCanReadWithoutBlocking(int fd) override {
    base::ScopedFD client_fd(HANDLE_EINTR(accept(fd_.get(), nullptr, nullptr)));
    ASSERT_TRUE(client_fd.is_valid());

    ++num_connections_;

    // Attempt to read from connection until EOF
    std::string out;
    char buf[256];
    while (true) {
      ssize_t len = HANDLE_EINTR(read(client_fd.get(), buf, sizeof(buf)));
      if (len <= 0)
        break;
      out.append(buf, len);
    }
    received_.append(out);

    if (callback_)
      std::move(callback_).Run();
  }

  void OnFileCanWriteWithoutBlocking(int fd) override {}

 private:
  base::ScopedFD fd_;
  std::unique_ptr<base::MessagePumpForUI::FdWatchController> controller_;
  int num_connections_ = 0;
  std::string received_;
  base::OnceClosure callback_;
};

class FakeDemoModeDelegate : public ArcClientAdapter::DemoModeDelegate {
 public:
  FakeDemoModeDelegate() = default;
  ~FakeDemoModeDelegate() override = default;
  FakeDemoModeDelegate(const FakeDemoModeDelegate&) = delete;
  FakeDemoModeDelegate& operator=(const FakeDemoModeDelegate&) = delete;

  void EnsureOfflineResourcesLoaded(base::OnceClosure callback) override {
    std::move(callback).Run();
  }

  base::FilePath GetDemoAppsPath() override { return base::FilePath(); }
};

class ArcVmClientAdapterTest : public testing::Test,
                               public ArcClientAdapter::Observer {
 public:
  ArcVmClientAdapterTest() {
    // Use the same VLOG() level as production. Note that session_manager sets
    // "--vmodule=*arc/*=1" in src/platform2/login_manager/chrome_setup.cc.
    logging::SetMinLogLevel(-1);

    // Create and set new fake clients every time to reset clients' status.
    chromeos::DBusThreadManager::GetSetterForTesting()->SetDebugDaemonClient(
        std::make_unique<TestDebugDaemonClient>());
    chromeos::DBusThreadManager::GetSetterForTesting()->SetConciergeClient(
        std::make_unique<TestConciergeClient>());
    chromeos::UpstartClient::InitializeFake();
  }

  ~ArcVmClientAdapterTest() override {
    chromeos::DBusThreadManager::GetSetterForTesting()->SetConciergeClient(
        nullptr);
    chromeos::DBusThreadManager::GetSetterForTesting()->SetDebugDaemonClient(
        nullptr);
  }

  void SetUp() override {
    run_loop_ = std::make_unique<base::RunLoop>();
    adapter_ = CreateArcVmClientAdapterForTesting(base::BindRepeating(
        &ArcVmClientAdapterTest::RewriteStatus, base::Unretained(this)));
    arc_instance_stopped_called_ = false;
    adapter_->AddObserver(this);
    ASSERT_TRUE(dir_.CreateUniqueTempDir());

    host_rootfs_writable_ = false;
    system_image_ext_format_ = false;

    // The fake client returns VM_STATUS_STARTING by default. Change it
    // to VM_STATUS_RUNNING which is used by ARCVM.
    vm_tools::concierge::StartVmResponse start_vm_response;
    start_vm_response.set_status(vm_tools::concierge::VM_STATUS_RUNNING);
    auto* vm_info = start_vm_response.mutable_vm_info();
    vm_info->set_cid(kCid);
    GetTestConciergeClient()->set_start_vm_response(start_vm_response);

    // Reset to the original behavior.
    RemoveUpstartStartStopJobFailures();
    SetArcVmBootNotificationServerFdForTesting(base::nullopt);

    const std::string abstract_addr(GenerateAbstractAddress());
    boot_server_ = std::make_unique<TestArcVmBootNotificationServer>();
    boot_server_->Start(abstract_addr);
    SetArcVmBootNotificationServerAddressForTesting(
        abstract_addr,
        // connect_timeout_limit
        base::TimeDelta::FromMilliseconds(100),
        // connect_sleep_duration_initial
        base::TimeDelta::FromMilliseconds(20));

    chromeos::SessionManagerClient::InitializeFake();

    adapter_->SetDemoModeDelegate(&demo_mode_delegate_);
  }

  void TearDown() override {
    chromeos::SessionManagerClient::Shutdown();
    adapter_->RemoveObserver(this);
    adapter_.reset();
    run_loop_.reset();
  }

  // ArcClientAdapter::Observer:
  void ArcInstanceStopped() override {
    arc_instance_stopped_called_ = true;
    run_loop()->Quit();
  }

  void ExpectTrueThenQuit(bool result) {
    EXPECT_TRUE(result);
    run_loop()->Quit();
  }

  void ExpectFalseThenQuit(bool result) {
    EXPECT_FALSE(result);
    run_loop()->Quit();
  }

  void ExpectTrue(bool result) { EXPECT_TRUE(result); }

  void ExpectFalse(bool result) { EXPECT_FALSE(result); }

 protected:
  void SetValidUserInfo() { SetUserInfo(kUserIdHash, kSerialNumber); }

  void SetUserInfo(const std::string& hash, const std::string& serial) {
    adapter()->SetUserInfo(
        cryptohome::Identification(user_manager::StubAccountId()), hash,
        serial);
  }

  void StartMiniArcWithParams(bool expect_success, StartParams params) {
    adapter()->StartMiniArc(
        std::move(params),
        base::BindOnce(expect_success
                           ? &ArcVmClientAdapterTest::ExpectTrueThenQuit
                           : &ArcVmClientAdapterTest::ExpectFalseThenQuit,
                       base::Unretained(this)));

    run_loop()->Run();
    RecreateRunLoop();
  }

  void UpgradeArcWithParams(bool expect_success, UpgradeParams params) {
    adapter()->UpgradeArc(
        std::move(params),
        base::BindOnce(expect_success
                           ? &ArcVmClientAdapterTest::ExpectTrueThenQuit
                           : &ArcVmClientAdapterTest::ExpectFalseThenQuit,
                       base::Unretained(this)));
    run_loop()->Run();
    RecreateRunLoop();
  }

  void UpgradeArcWithParamsAndStopVmCount(bool expect_success,
                                          UpgradeParams params,
                                          int run_until_stop_vm_count) {
    GetTestConciergeClient()->set_on_stop_vm_callback(run_loop()->QuitClosure(),
                                                      run_until_stop_vm_count);
    adapter()->UpgradeArc(
        std::move(params),
        base::BindOnce(expect_success ? &ArcVmClientAdapterTest::ExpectTrue
                                      : &ArcVmClientAdapterTest::ExpectFalse,
                       base::Unretained(this)));
    run_loop()->Run();
    RecreateRunLoop();
  }

  // Starts mini instance with the default StartParams.
  void StartMiniArc() { StartMiniArcWithParams(true, {}); }

  // Upgrades the instance with the default UpgradeParams.
  void UpgradeArc(bool expect_success) {
    UpgradeArcWithParams(expect_success, {});
  }

  void SendVmStartedSignal() {
    vm_tools::concierge::VmStartedSignal signal;
    signal.set_name(kArcVmName);
    for (auto& observer : GetTestConciergeClient()->vm_observer_list())
      observer.OnVmStarted(signal);
  }

  void SendVmStartedSignalNotForArcVm() {
    vm_tools::concierge::VmStartedSignal signal;
    signal.set_name("penguin");
    for (auto& observer : GetTestConciergeClient()->vm_observer_list())
      observer.OnVmStarted(signal);
  }

  void SendVmStoppedSignalForCid(int64_t cid) {
    vm_tools::concierge::VmStoppedSignal signal;
    signal.set_name(kArcVmName);
    signal.set_cid(cid);
    for (auto& observer : GetTestConciergeClient()->vm_observer_list())
      observer.OnVmStopped(signal);
  }

  void SendVmStoppedSignal() { SendVmStoppedSignalForCid(kCid); }

  void SendVmStoppedSignalNotForArcVm() {
    vm_tools::concierge::VmStoppedSignal signal;
    signal.set_name("penguin");
    signal.set_cid(kCid);
    for (auto& observer : GetTestConciergeClient()->vm_observer_list())
      observer.OnVmStopped(signal);
  }

  void SendNameOwnerChangedSignal() {
    for (auto& observer : GetTestConciergeClient()->observer_list())
      observer.ConciergeServiceStopped();
  }

  void InjectUpstartStartJobFailure(const std::string& job_name_to_fail) {
    auto* upstart_client = chromeos::FakeUpstartClient::Get();
    upstart_client->set_start_job_cb(base::BindLambdaForTesting(
        [job_name_to_fail](const std::string& job_name,
                           const std::vector<std::string>& env) {
          // Return success unless |job_name| is |job_name_to_fail|.
          return job_name != job_name_to_fail;
        }));
  }

  void InjectUpstartStopJobFailure(const std::string& job_name_to_fail) {
    auto* upstart_client = chromeos::FakeUpstartClient::Get();
    upstart_client->set_stop_job_cb(base::BindLambdaForTesting(
        [job_name_to_fail](const std::string& job_name,
                           const std::vector<std::string>& env) {
          // Return success unless |job_name| is |job_name_to_fail|.
          return job_name != job_name_to_fail;
        }));
  }

  void StartRecordingUpstartOperations() {
    auto* upstart_client = chromeos::FakeUpstartClient::Get();
    upstart_client->set_start_job_cb(
        base::BindLambdaForTesting([this](const std::string& job_name,
                                          const std::vector<std::string>& env) {
          upstart_operations_.emplace_back(job_name, true);
          return true;
        }));
    upstart_client->set_stop_job_cb(
        base::BindLambdaForTesting([this](const std::string& job_name,
                                          const std::vector<std::string>& env) {
          upstart_operations_.emplace_back(job_name, false);
          return true;
        }));
  }

  void RemoveUpstartStartStopJobFailures() {
    auto* upstart_client = chromeos::FakeUpstartClient::Get();
    upstart_client->set_start_job_cb(
        chromeos::FakeUpstartClient::StartStopJobCallback());
    upstart_client->set_stop_job_cb(
        chromeos::FakeUpstartClient::StartStopJobCallback());
  }

  // Calls ArcVmClientAdapter::StopArcInstance().
  // If |arc_upgraded| is false, we expect ConciergeClient::StopVm to have been
  // called two times, once to clear a stale mini-VM in StartMiniArc(), and
  // another on this call to StopArcInstance().
  // If |arc_upgraded| is true, we expect StopVm() to have been called three
  // times, to clear a stale mini-VM in StartMiniArc(), to clear a stale
  // full-VM in UpgradeArc, and finally on this call to StopArcInstance();
  void StopArcInstance(bool arc_upgraded) {
    adapter()->StopArcInstance(/*on_shutdown=*/false,
                               /*should_backup_log=*/false);
    run_loop()->RunUntilIdle();
    EXPECT_EQ(arc_upgraded ? 3 : 2,
              GetTestConciergeClient()->stop_vm_call_count());
    EXPECT_FALSE(arc_instance_stopped_called());

    RecreateRunLoop();
    SendVmStoppedSignal();
    run_loop()->Run();
    EXPECT_TRUE(arc_instance_stopped_called());
  }

  // Checks that ArcVmClientAdapter has requested to stop the VM (after an
  // error in UpgradeArc).
  // If |stale_full_vm_stopped| is false, we expect ConciergeClient::StopVm to
  // have been called two times, once to clear a stale mini-VM in
  // StartMiniArc(), and another after some error condition. If
  // |stale_full_vm_stopped| is true, we expect StopVm() to have been called
  // three times, to clear a stale mini-VM in StartMiniArc(), to clear a stale
  // full-VM in UpgradeArc, and finally after some error condition.
  void ExpectArcStopped(bool stale_full_vm_stopped) {
    EXPECT_EQ(stale_full_vm_stopped ? 3 : 2,
              GetTestConciergeClient()->stop_vm_call_count());
    EXPECT_FALSE(arc_instance_stopped_called());
    RecreateRunLoop();
    SendVmStoppedSignal();
    run_loop()->Run();
    EXPECT_TRUE(arc_instance_stopped_called());
  }

  void RecreateRunLoop() { run_loop_ = std::make_unique<base::RunLoop>(); }

  base::RunLoop* run_loop() { return run_loop_.get(); }
  ArcClientAdapter* adapter() { return adapter_.get(); }

  bool arc_instance_stopped_called() const {
    return arc_instance_stopped_called_;
  }
  void reset_arc_instance_stopped_called() {
    arc_instance_stopped_called_ = false;
  }
  const std::vector<std::pair<std::string, bool>>& upstart_operations() const {
    return upstart_operations_;
  }
  TestConciergeClient* GetTestConciergeClient() {
    return static_cast<TestConciergeClient*>(
        chromeos::DBusThreadManager::Get()->GetConciergeClient());
  }

  TestDebugDaemonClient* GetTestDebugDaemonClient() {
    return static_cast<TestDebugDaemonClient*>(
        chromeos::DBusThreadManager::Get()->GetDebugDaemonClient());
  }

  TestArcVmBootNotificationServer* boot_notification_server() {
    return boot_server_.get();
  }

  void set_host_rootfs_writable(bool host_rootfs_writable) {
    host_rootfs_writable_ = host_rootfs_writable;
  }

  void set_system_image_ext_format(bool system_image_ext_format) {
    system_image_ext_format_ = system_image_ext_format;
  }

 private:
  void RewriteStatus(FileSystemStatus* status) {
    status->set_host_rootfs_writable_for_testing(host_rootfs_writable_);
    status->set_system_image_ext_format_for_testing(system_image_ext_format_);
  }

  std::unique_ptr<base::RunLoop> run_loop_;
  std::unique_ptr<ArcClientAdapter> adapter_;
  bool arc_instance_stopped_called_;

  content::BrowserTaskEnvironment browser_task_environment_;
  base::ScopedTempDir dir_;

  // Variables to override the value in FileSystemStatus.
  bool host_rootfs_writable_;
  bool system_image_ext_format_;

  // List of upstart operations recorded. When it's "start" the boolean is set
  // to true.
  std::vector<std::pair<std::string, bool>> upstart_operations_;

  std::unique_ptr<TestArcVmBootNotificationServer> boot_server_;

  FakeDemoModeDelegate demo_mode_delegate_;

  DISALLOW_COPY_AND_ASSIGN(ArcVmClientAdapterTest);
};

// Tests that SetUserInfo() doesn't crash.
TEST_F(ArcVmClientAdapterTest, SetUserInfo) {
  SetUserInfo(kUserIdHash, kSerialNumber);
}

// Tests that SetUserInfo() doesn't crash even when empty strings are passed.
// Currently, ArcSessionRunner's tests call SetUserInfo() that way.
// TODO(yusukes): Once ASR's tests are fixed, remove this test and use DCHECKs
// in SetUserInfo().
TEST_F(ArcVmClientAdapterTest, SetUserInfoEmpty) {
  adapter()->SetUserInfo(cryptohome::Identification(), std::string(),
                         std::string());
}

// Tests that StartMiniArc() succeeds by default.
TEST_F(ArcVmClientAdapterTest, StartMiniArc) {
  StartMiniArc();
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());

  StopArcInstance(/*arc_upgraded=*/false);
}

// Tests that StartMiniArc() still succeeds even when Upstart fails to stop
// the arcvm-post-login-services job.
TEST_F(ArcVmClientAdapterTest, StartMiniArc_StopArcVmPostLoginServicesJobFail) {
  // Inject failure to FakeUpstartClient.
  InjectUpstartStopJobFailure(kArcVmPostLoginServicesJobName);

  StartMiniArc();
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());

  StopArcInstance(/*arc_upgraded=*/false);
}

// Tests that StartMiniArc() fails when Upstart fails to start the job.
TEST_F(ArcVmClientAdapterTest, StartMiniArc_StartArcVmPerBoardFeaturesJobFail) {
  // Inject failure to FakeUpstartClient.
  InjectUpstartStartJobFailure(kArcVmPerBoardFeaturesJobName);

  StartMiniArcWithParams(false, {});

  // Confirm that no VM is started.
  EXPECT_FALSE(GetTestConciergeClient()->start_arc_vm_called());
}

// Tests that StartMiniArc() fails if Upstart fails to start
// arcvm-pre-login-services.
TEST_F(ArcVmClientAdapterTest, StartMiniArc_StartArcVmPreLoginServicesJobFail) {
  // Inject failure to FakeUpstartClient.
  InjectUpstartStartJobFailure(kArcVmPreLoginServicesJobName);

  StartMiniArcWithParams(false, {});
  EXPECT_FALSE(GetTestConciergeClient()->start_arc_vm_called());
}

// Tests that StartMiniArc() succeeds if Upstart fails to stop
// arcvm-pre-login-services.
TEST_F(ArcVmClientAdapterTest, StartMiniArc_StopArcVmPreLoginServicesJobFail) {
  // Inject failure to FakeUpstartClient.
  InjectUpstartStopJobFailure(kArcVmPreLoginServicesJobName);

  StartMiniArc();
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());

  StopArcInstance(/*arc_upgraded=*/false);
}

// Tests that StartMiniArc()'s JOB_STOP_AND_START for
// |kArcVmPreLoginServicesJobName| is properly implemented.
TEST_F(ArcVmClientAdapterTest, StartMiniArc_JobRestart) {
  StartRecordingUpstartOperations();
  StartMiniArc();

  const auto& ops = upstart_operations();
  // Find the STOP operation for the job.
  auto it = std::find(
      ops.begin(), ops.end(),
      std::make_pair(std::string(kArcVmPreLoginServicesJobName), false));
  ASSERT_NE(ops.end(), it);
  ++it;
  ASSERT_NE(ops.end(), it);
  // The next operation must be START for the job.
  EXPECT_EQ(it->first, kArcVmPreLoginServicesJobName);
  EXPECT_TRUE(it->second);  // true means START.
}

// Tests that StopArcInstance() eventually notifies the observer.
TEST_F(ArcVmClientAdapterTest, StopArcInstance) {
  SetValidUserInfo();
  StartMiniArc();
  UpgradeArc(true);

  adapter()->StopArcInstance(/*on_shutdown=*/false,
                             /*should_backup_log=*/false);
  run_loop()->RunUntilIdle();
  EXPECT_EQ(3, GetTestConciergeClient()->stop_vm_call_count());
  // The callback for StopVm D-Bus reply does NOT call ArcInstanceStopped when
  // the D-Bus call result is successful.
  EXPECT_FALSE(arc_instance_stopped_called());

  // Instead, vm_concierge explicitly notifies Chrome of the VM termination.
  RecreateRunLoop();
  SendVmStoppedSignal();
  run_loop()->Run();
  // ..and that calls ArcInstanceStopped.
  EXPECT_TRUE(arc_instance_stopped_called());
}

// b/164816080 This test ensures that a new vm instance that is
// created while handling the shutting down of the previous instance,
// doesn't incorrectly receive the shutdown event as well.
TEST_F(ArcVmClientAdapterTest, DoesNotGetArcInstanceStoppedOnNestedInstance) {
  using RunLoopFactory = base::RepeatingCallback<base::RunLoop*()>;

  class Observer : public ArcClientAdapter::Observer {
   public:
    Observer(RunLoopFactory run_loop_factory, Observer* child_observer)
        : run_loop_factory_(run_loop_factory),
          child_observer_(child_observer) {}
    Observer(const Observer&) = delete;
    Observer& operator=(const Observer&) = delete;

    ~Observer() override {
      if (child_observer_ && nested_adapter_)
        nested_adapter_->RemoveObserver(child_observer_);
    }

    bool stopped_called() const { return stopped_called_; }

    // ArcClientAdapter::Observer:
    void ArcInstanceStopped() override {
      stopped_called_ = true;

      if (child_observer_) {
        nested_adapter_ = CreateArcVmClientAdapterForTesting(base::DoNothing());
        nested_adapter_->AddObserver(child_observer_);
        nested_adapter_->SetUserInfo(
            cryptohome::Identification(user_manager::StubAccountId()),
            kUserIdHash, kSerialNumber);
        nested_adapter_->SetDemoModeDelegate(&demo_mode_delegate_);

        base::RunLoop* run_loop = run_loop_factory_.Run();
        nested_adapter_->StartMiniArc({}, QuitClosure(run_loop));
        run_loop->Run();

        run_loop = run_loop_factory_.Run();
        nested_adapter_->UpgradeArc({}, QuitClosure(run_loop));
        run_loop->Run();
      }
    }

   private:
    base::OnceCallback<void(bool)> QuitClosure(base::RunLoop* run_loop) {
      return base::BindOnce(
          [](base::RunLoop* run_loop, bool result) { run_loop->Quit(); },
          run_loop);
    }

    base::RepeatingCallback<base::RunLoop*()> const run_loop_factory_;
    Observer* const child_observer_;
    std::unique_ptr<ArcClientAdapter> nested_adapter_;
    FakeDemoModeDelegate demo_mode_delegate_;
    bool stopped_called_ = false;
  };

  SetValidUserInfo();
  StartMiniArc();
  UpgradeArc(true);

  RunLoopFactory run_loop_factory = base::BindLambdaForTesting([this]() {
    RecreateRunLoop();
    return run_loop();
  });

  Observer child_observer(run_loop_factory, nullptr);
  Observer parent_observer(run_loop_factory, &child_observer);
  adapter()->AddObserver(&parent_observer);
  base::ScopedClosureRunner teardown(base::BindOnce(
      [](ArcClientAdapter* adapter, Observer* parent_observer) {
        adapter->RemoveObserver(parent_observer);
      },
      adapter(), &parent_observer));

  SendVmStoppedSignal();

  EXPECT_TRUE(parent_observer.stopped_called());
  EXPECT_FALSE(child_observer.stopped_called());
}

// Tests that StopArcInstance() initiates ARC log backup.
TEST_F(ArcVmClientAdapterTest, StopArcInstance_WithLogBackup) {
  SetValidUserInfo();
  StartMiniArc();
  UpgradeArc(true);

  adapter()->StopArcInstance(/*on_shutdown=*/false, /*should_backup_log=*/true);
  run_loop()->RunUntilIdle();
  EXPECT_EQ(3, GetTestConciergeClient()->stop_vm_call_count());
  // The callback for StopVm D-Bus reply does NOT call ArcInstanceStopped when
  // the D-Bus call result is successful.
  EXPECT_FALSE(arc_instance_stopped_called());

  // Instead, vm_concierge explicitly notifies Chrome of the VM termination.
  RecreateRunLoop();
  SendVmStoppedSignal();
  run_loop()->Run();
  // ..and that calls ArcInstanceStopped.
  EXPECT_TRUE(arc_instance_stopped_called());
}

TEST_F(ArcVmClientAdapterTest, StopArcInstance_WithLogBackup_BackupFailed) {
  SetValidUserInfo();
  StartMiniArc();
  UpgradeArc(true);

  EXPECT_FALSE(GetTestDebugDaemonClient()->backup_arc_bug_report_called());
  GetTestDebugDaemonClient()->set_backup_arc_bug_report_result(false);

  adapter()->StopArcInstance(/*on_shutdown=*/false, /*should_backup_log=*/true);
  run_loop()->RunUntilIdle();
  EXPECT_EQ(3, GetTestConciergeClient()->stop_vm_call_count());
  // The callback for StopVm D-Bus reply does NOT call ArcInstanceStopped when
  // the D-Bus call result is successful.
  EXPECT_FALSE(arc_instance_stopped_called());

  // Instead, vm_concierge explicitly notifies Chrome of the VM termination.
  RecreateRunLoop();
  SendVmStoppedSignal();
  run_loop()->Run();

  EXPECT_TRUE(GetTestDebugDaemonClient()->backup_arc_bug_report_called());
  // ..and that calls ArcInstanceStopped.
  EXPECT_TRUE(arc_instance_stopped_called());
}

// Tests that StopArcInstance() called during shutdown doesn't do anything.
TEST_F(ArcVmClientAdapterTest, StopArcInstance_OnShutdown) {
  SetValidUserInfo();
  StartMiniArc();
  UpgradeArc(true);

  adapter()->StopArcInstance(/*on_shutdown=*/true, /*should_backup_log=*/false);
  run_loop()->RunUntilIdle();
  EXPECT_EQ(2, GetTestConciergeClient()->stop_vm_call_count());
  EXPECT_FALSE(arc_instance_stopped_called());
}

// Tests that StopArcInstance() immediately notifies the observer on failure.
TEST_F(ArcVmClientAdapterTest, StopArcInstance_Fail) {
  SetValidUserInfo();
  StartMiniArc();
  UpgradeArc(true);

  // Inject failure.
  vm_tools::concierge::StopVmResponse response;
  response.set_success(false);
  GetTestConciergeClient()->set_stop_vm_response(response);

  adapter()->StopArcInstance(/*on_shutdown=*/false,
                             /*should_backup_log=*/false);

  run_loop()->Run();
  EXPECT_EQ(3, GetTestConciergeClient()->stop_vm_call_count());

  // The callback for StopVm D-Bus reply does call ArcInstanceStopped when
  // the D-Bus call result is NOT successful.
  EXPECT_TRUE(arc_instance_stopped_called());
}

// Test that StopArcInstance() stops the mini-VM if it cannot find a VM with
// the current user ID hash.
TEST_F(ArcVmClientAdapterTest, StopArcInstance_StopMiniVm) {
  StartMiniArc();

  SetValidUserInfo();

  vm_tools::concierge::GetVmInfoResponse response;
  response.set_success(false);
  GetTestConciergeClient()->set_get_vm_info_response(response);

  adapter()->StopArcInstance(/*on_shutdown=*/false,
                             /*should_backup_log*/ false);
  run_loop()->RunUntilIdle();

  EXPECT_TRUE(GetTestConciergeClient()->get_vm_info_called());
  // Expect StopVm() to be called twice; once in StartMiniArc to clear stale
  // mini-VM, and again on StopArcInstance().
  EXPECT_EQ(2, GetTestConciergeClient()->stop_vm_call_count());
  EXPECT_EQ(kArcVmDefaultOwner,
            GetTestConciergeClient()->stop_vm_request().owner_id());
}

// Tests that UpgradeArc() handles arcvm-post-login-services startup failures
// properly.
TEST_F(ArcVmClientAdapterTest, UpgradeArc_StartArcVmPostLoginServicesFailure) {
  SetValidUserInfo();
  StartMiniArc();

  // Inject failure to FakeUpstartClient.
  InjectUpstartStartJobFailure(kArcVmPostLoginServicesJobName);

  UpgradeArcWithParamsAndStopVmCount(false, {}, /*run_until_stop_vm_count=*/3);

  ExpectArcStopped(/*stale_full_vm_stopped=*/true);
}

// Tests that StartMiniArc() handles arcvm-post-vm-start-services stop failures
// properly.
TEST_F(ArcVmClientAdapterTest,
       StartMiniArc_StopArcVmPostVmStartServicesFailure) {
  SetValidUserInfo();
  // Inject failure to FakeUpstartClient.
  InjectUpstartStopJobFailure(kArcVmPostVmStartServicesJobName);

  // StartMiniArc should still succeed.
  StartMiniArc();

  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());

  // Make sure StopVm() is called only once, to stop existing VMs on
  // StartMiniArc().
  EXPECT_EQ(1, GetTestConciergeClient()->stop_vm_call_count());
}

// Tests that UpgradeArc() handles arcvm-post-vm-start-services startup
// failures properly.
TEST_F(ArcVmClientAdapterTest,
       UpgradeArc_StartArcVmPostVmStartServicesFailure) {
  SetValidUserInfo();
  StartMiniArc();

  // Inject failure to FakeUpstartClient.
  InjectUpstartStartJobFailure(kArcVmPostVmStartServicesJobName);
  // UpgradeArc should fail and the VM should be stoppped.
  UpgradeArcWithParamsAndStopVmCount(false, {}, /*run_until_stop_vm_count=*/3);
  ExpectArcStopped(/*stale_full_vm_stopped=*/true);
}

// Tests that "no user ID hash" failure is handled properly.
TEST_F(ArcVmClientAdapterTest, UpgradeArc_NoUserId) {
  // Don't set the user id hash.
  SetUserInfo(std::string(), kSerialNumber);
  StartMiniArc();
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());

  UpgradeArcWithParamsAndStopVmCount(false, {}, /*run_until_stop_vm_count=*/2);
  ExpectArcStopped(/*stale_full_vm_stopped=*/false);
}

// Tests that a "Failed Adb Sideload response" case is handled properly.
TEST_F(ArcVmClientAdapterTest, UpgradeArc_FailedAdbResponse) {
  SetValidUserInfo();
  StartMiniArc();

  // Ask the Fake Session Manager to return a failed Adb Sideload response.
  chromeos::FakeSessionManagerClient::Get()->set_adb_sideload_response(
      chromeos::FakeSessionManagerClient::AdbSideloadResponseCode::FAILED);

  UpgradeArcWithParamsAndStopVmCount(false, {}, /*run_until_stop_vm_count=*/3);
  ExpectArcStopped(/*stale_full_vm_stopped=*/true);
}

// Tests that a "Need_Powerwash Adb Sideload response" case is handled properly.
TEST_F(ArcVmClientAdapterTest, UpgradeArc_NeedPowerwashAdbResponse) {
  SetValidUserInfo();
  StartMiniArc();

  // Ask the Fake Session Manager to return a Need_Powerwash Adb Sideload
  // response.
  chromeos::FakeSessionManagerClient::Get()->set_adb_sideload_response(
      chromeos::FakeSessionManagerClient::AdbSideloadResponseCode::
          NEED_POWERWASH);
  UpgradeArc(true);
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
  EXPECT_TRUE(base::Contains(boot_notification_server()->received_data(),
                             "ro.boot.enable_adb_sideloading=0"));
}

// Tests that adb sideloading is disabled by default.
TEST_F(ArcVmClientAdapterTest, UpgradeArc_AdbSideloadingPropertyDefault) {
  SetValidUserInfo();
  StartMiniArc();

  UpgradeArc(true);
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
  EXPECT_TRUE(base::Contains(boot_notification_server()->received_data(),
                             "ro.boot.enable_adb_sideloading=0"));
}

// Tests that adb sideloading can be controlled via session_manager.
TEST_F(ArcVmClientAdapterTest, UpgradeArc_AdbSideloadingPropertyEnabled) {
  SetValidUserInfo();
  StartMiniArc();

  chromeos::FakeSessionManagerClient::Get()->set_adb_sideload_enabled(true);
  UpgradeArc(true);
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
  EXPECT_TRUE(base::Contains(boot_notification_server()->received_data(),
                             "ro.boot.enable_adb_sideloading=1"));
}

TEST_F(ArcVmClientAdapterTest, UpgradeArc_AdbSideloadingPropertyDisabled) {
  SetValidUserInfo();
  StartMiniArc();

  chromeos::FakeSessionManagerClient::Get()->set_adb_sideload_enabled(false);
  UpgradeArc(true);
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
  EXPECT_TRUE(base::Contains(boot_notification_server()->received_data(),
                             "ro.boot.enable_adb_sideloading=0"));
}

// Tests that "no serial" failure is handled properly.
TEST_F(ArcVmClientAdapterTest, UpgradeArc_NoSerial) {
  // Don't set the serial number.
  SetUserInfo(kUserIdHash, std::string());
  StartMiniArc();
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());

  UpgradeArcWithParamsAndStopVmCount(false, {}, /*run_until_stop_vm_count=*/2);
  ExpectArcStopped(/*stale_full_vm_stopped=*/false);
}

// Test that ConciergeClient::SetVmId() empty reply is handled properly.
TEST_F(ArcVmClientAdapterTest, UpgradeArc_SetVmIdEmptyReply) {
  SetValidUserInfo();
  StartMiniArc();

  // Inject failure
  GetTestConciergeClient()->set_set_vm_id_response(base::nullopt);

  UpgradeArcWithParamsAndStopVmCount(false, {}, /*run_until_stop_vm_count=*/3);
  ExpectArcStopped(/*stale_full_vm_stopped=*/true);
}

// Test that ConciergeClient::SetVmId() unsuccessful reply is handled properly.
TEST_F(ArcVmClientAdapterTest, UpgradeArc_SetVmIdFailure) {
  SetValidUserInfo();
  StartMiniArc();

  // Inject failure
  vm_tools::concierge::SetVmIdResponse response;
  response.set_success(false);
  GetTestConciergeClient()->set_set_vm_id_response(response);

  UpgradeArcWithParamsAndStopVmCount(false, {}, /*run_until_stop_vm_count=*/3);
  ExpectArcStopped(/*stale_full_vm_stopped=*/true);
}

TEST_F(ArcVmClientAdapterTest, StartMiniArc_StopExistingVmFailure) {
  // Inject failure.
  vm_tools::concierge::StopVmResponse response;
  response.set_success(false);
  GetTestConciergeClient()->set_stop_vm_response(response);

  StartMiniArcWithParams(false, {});

  EXPECT_FALSE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
}

TEST_F(ArcVmClientAdapterTest, StartMiniArc_StopExistingVmFailureEmptyReply) {
  // Inject failure.
  GetTestConciergeClient()->set_stop_vm_response(base::nullopt);

  StartMiniArcWithParams(false, {});

  EXPECT_FALSE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
}

TEST_F(ArcVmClientAdapterTest, UpgradeArc_StopExistingVmFailure) {
  SetValidUserInfo();
  StartMiniArc();

  // Inject failure.
  vm_tools::concierge::StopVmResponse response;
  response.set_success(false);
  GetTestConciergeClient()->set_stop_vm_response(response);

  UpgradeArcWithParamsAndStopVmCount(false, {}, /*run_until_stop_vm_count=*/3);
  ExpectArcStopped(/*stale_full_vm_stopped=*/true);
}

TEST_F(ArcVmClientAdapterTest, UpgradeArc_StopExistingVmFailureEmptyReply) {
  SetValidUserInfo();
  StartMiniArc();

  // Inject failure.
  GetTestConciergeClient()->set_stop_vm_response(base::nullopt);

  UpgradeArcWithParamsAndStopVmCount(false, {}, /*run_until_stop_vm_count=*/3);
  ExpectArcStopped(/*stale_full_vm_stopped=*/true);
}

// Tests that ConciergeClient::WaitForServiceToBeAvailable() failure is handled
// properly.
TEST_F(ArcVmClientAdapterTest, StartMiniArc_WaitForConciergeAvailableFailure) {
  // Inject failure.
  GetTestConciergeClient()->set_wait_for_service_to_be_available_response(
      false);

  StartMiniArcWithParams(false, {});
  EXPECT_FALSE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
}

// Tests that StartArcVm() failure is handled properly.
TEST_F(ArcVmClientAdapterTest, StartMiniArc_StartArcVmFailure) {
  SetValidUserInfo();
  // Inject failure to StartArcVm().
  vm_tools::concierge::StartVmResponse start_vm_response;
  start_vm_response.set_status(vm_tools::concierge::VM_STATUS_UNKNOWN);
  GetTestConciergeClient()->set_start_vm_response(start_vm_response);

  StartMiniArcWithParams(false, {});

  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
}

TEST_F(ArcVmClientAdapterTest, StartMiniArc_StartArcVmFailureEmptyReply) {
  SetValidUserInfo();
  // Inject failure to StartArcVm(). This emulates D-Bus timeout situations.
  GetTestConciergeClient()->set_start_vm_response(base::nullopt);

  StartMiniArcWithParams(false, {});

  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
}

// Tests that successful StartArcVm() call is handled properly.
TEST_F(ArcVmClientAdapterTest, UpgradeArc_Success) {
  SetValidUserInfo();
  StartMiniArc();
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
  UpgradeArc(true);

  StopArcInstance(/*arc_upgraded=*/true);
}

// Try to start and upgrade the instance with more params.
TEST_F(ArcVmClientAdapterTest, StartUpgradeArc_VariousParams) {
  StartParams start_params(GetPopulatedStartParams());
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));

  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());

  UpgradeParams params(GetPopulatedUpgradeParams());
  UpgradeArcWithParams(true, std::move(params));
}

// Try to start and upgrade the instance with slightly different params
// than StartUpgradeArc_VariousParams for better code coverage.
TEST_F(ArcVmClientAdapterTest, StartUpgradeArc_VariousParams2) {
  StartParams start_params(GetPopulatedStartParams());
  // Use slightly different params than StartUpgradeArc_VariousParams.
  start_params.play_store_auto_update =
      StartParams::PlayStoreAutoUpdate::AUTO_UPDATE_OFF;

  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));

  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());

  UpgradeParams params(GetPopulatedUpgradeParams());
  // Use slightly different params than StartUpgradeArc_VariousParams.
  params.packages_cache_mode =
      UpgradeParams::PackageCacheMode::SKIP_SETUP_COPY_ON_INIT;
  params.supervision_transition = ArcSupervisionTransition::REGULAR_TO_CHILD;
  params.preferred_languages = {"en_US"};

  UpgradeArcWithParams(true, std::move(params));
}

// Try to start and upgrade the instance with demo mode enabled.
TEST_F(ArcVmClientAdapterTest, StartUpgradeArc_DemoMode) {
  constexpr char kDemoImage[] =
      "/run/imageloader/demo-mode-resources/0.0.1.7/android_demo_apps.squash";
  base::FilePath apps_path = base::FilePath(kDemoImage);

  class TestDemoDelegate : public ArcClientAdapter::DemoModeDelegate {
   public:
    explicit TestDemoDelegate(base::FilePath apps_path)
        : apps_path_(apps_path) {}
    ~TestDemoDelegate() override = default;

    void EnsureOfflineResourcesLoaded(base::OnceClosure callback) override {
      std::move(callback).Run();
    }

    base::FilePath GetDemoAppsPath() override { return apps_path_; }

   private:
    base::FilePath apps_path_;
  };

  TestDemoDelegate delegate(apps_path);
  adapter()->SetDemoModeDelegate(&delegate);
  StartMiniArc();

  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());

  // Verify the request.
  auto request = GetTestConciergeClient()->start_arc_vm_request();
  // Make sure disks have the squashfs image.
  EXPECT_TRUE(([&kDemoImage, &request]() {
    for (const auto& disk : request.disks()) {
      if (disk.path() == kDemoImage)
        return true;
    }
    return false;
  }()));

  SetValidUserInfo();
  UpgradeParams params(GetPopulatedUpgradeParams());
  // Enable demo mode.
  params.is_demo_session = true;

  UpgradeArcWithParams(true, std::move(params));
  EXPECT_TRUE(base::Contains(boot_notification_server()->received_data(),
                             "ro.boot.arc_demo_mode=1"));
}

TEST_F(ArcVmClientAdapterTest, StartMiniArc_DisableSystemDefaultApp) {
  StartParams start_params(GetPopulatedStartParams());
  start_params.arc_disable_system_default_app = true;
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.disable_system_default_app=1"));
}

TEST_F(ArcVmClientAdapterTest, StartUpgradeArc_DisableMediaStoreMaintenance) {
  StartParams start_params(GetPopulatedStartParams());
  start_params.disable_media_store_maintenance = true;
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  UpgradeParams params(GetPopulatedUpgradeParams());
  UpgradeArcWithParams(true, std::move(params));
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.disable_media_store_maintenance=1"));
}

TEST_F(ArcVmClientAdapterTest, StartUpgradeArc_ArcVmUreadaheadModeReadahead) {
  StartParams start_params(GetPopulatedStartParams());
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  UpgradeParams params(GetPopulatedUpgradeParams());
  UpgradeArcWithParams(true, std::move(params));
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.arcvm_ureadahead_mode=readahead"));
}

TEST_F(ArcVmClientAdapterTest, StartMiniArc_EnablePaiGeneration) {
  StartParams start_params(GetPopulatedStartParams());
  start_params.arc_generate_play_auto_install = true;
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.arc_generate_pai=1"));
}

TEST_F(ArcVmClientAdapterTest, StartMiniArc_PaiGenerationDefaultDisabled) {
  SetValidUserInfo();
  StartMiniArcWithParams(true, GetPopulatedStartParams());
  // No androidboot property should be generated.
  EXPECT_FALSE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.arc_generate_pai=1"));
  EXPECT_FALSE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.arc_generate_pai=0"));
}

// Tests that StartArcVm() is called with valid parameters.
TEST_F(ArcVmClientAdapterTest, StartMiniArc_StartArcVmParams) {
  SetValidUserInfo();
  StartMiniArc();
  ASSERT_TRUE(GetTestConciergeClient()->start_arc_vm_called());

  // Verify parameters
  const auto& params = GetTestConciergeClient()->start_arc_vm_request();
  EXPECT_EQ("arcvm", params.name());
  EXPECT_LT(0u, params.cpus());
  EXPECT_FALSE(params.vm().kernel().empty());
  // Make sure system.raw.img is passed.
  EXPECT_FALSE(params.vm().rootfs().empty());
  // Make sure vendor.raw.img is passed.
  EXPECT_LE(1, params.disks_size());
  EXPECT_LT(0, params.params_size());
}

// Tests that crosvm crash is handled properly.
TEST_F(ArcVmClientAdapterTest, CrosvmCrash) {
  SetValidUserInfo();
  StartMiniArc();
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
  UpgradeArc(true);

  // Kill crosvm and verify StopArcInstance is called.
  SendVmStoppedSignal();
  run_loop()->Run();
  EXPECT_TRUE(arc_instance_stopped_called());
}

// Tests that vm_concierge crash is handled properly.
TEST_F(ArcVmClientAdapterTest, ConciergeCrash) {
  SetValidUserInfo();
  StartMiniArc();
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
  UpgradeArc(true);

  // Kill vm_concierge and verify StopArcInstance is called.
  SendNameOwnerChangedSignal();
  run_loop()->Run();
  EXPECT_TRUE(arc_instance_stopped_called());
}

// Tests the case where crosvm crashes, then vm_concierge crashes too.
TEST_F(ArcVmClientAdapterTest, CrosvmAndConciergeCrashes) {
  SetValidUserInfo();
  StartMiniArc();
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
  UpgradeArc(true);

  // Kill crosvm and verify StopArcInstance is called.
  SendVmStoppedSignal();
  run_loop()->Run();
  EXPECT_TRUE(arc_instance_stopped_called());

  // Kill vm_concierge and verify StopArcInstance is NOT called since
  // the observer has already been called.
  RecreateRunLoop();
  reset_arc_instance_stopped_called();
  SendNameOwnerChangedSignal();
  run_loop()->RunUntilIdle();
  EXPECT_FALSE(arc_instance_stopped_called());
}

// Tests the case where a unknown VmStopped signal is sent to Chrome.
TEST_F(ArcVmClientAdapterTest, VmStoppedSignal_UnknownCid) {
  SetValidUserInfo();
  StartMiniArc();
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());
  EXPECT_FALSE(arc_instance_stopped_called());
  UpgradeArc(true);

  SendVmStoppedSignalForCid(42);  // unknown CID
  run_loop()->RunUntilIdle();
  EXPECT_FALSE(arc_instance_stopped_called());
}

// Tests the case where a stale VmStopped signal is sent to Chrome.
TEST_F(ArcVmClientAdapterTest, VmStoppedSignal_Stale) {
  SendVmStoppedSignalForCid(42);
  run_loop()->RunUntilIdle();
  EXPECT_FALSE(arc_instance_stopped_called());
}

// Tests the case where a VmStopped signal not for ARCVM (e.g. Termina) is sent
// to Chrome.
TEST_F(ArcVmClientAdapterTest, VmStoppedSignal_Termina) {
  SendVmStoppedSignalNotForArcVm();
  run_loop()->RunUntilIdle();
  EXPECT_FALSE(arc_instance_stopped_called());
}

// Tests that receiving VmStarted signal is no-op.
TEST_F(ArcVmClientAdapterTest, VmStartedSignal) {
  SendVmStartedSignal();
  run_loop()->RunUntilIdle();
  RecreateRunLoop();
  SendVmStartedSignalNotForArcVm();
  run_loop()->RunUntilIdle();
}

// Tests that ConciergeServiceStarted() doesn't crash.
TEST_F(ArcVmClientAdapterTest, TestConciergeServiceStarted) {
  GetTestConciergeClient()->NotifyConciergeStarted();
}

// Tests that the kernel parameter does not include "rw" by default.
TEST_F(ArcVmClientAdapterTest, KernelParam_RO) {
  set_host_rootfs_writable(false);
  set_system_image_ext_format(false);
  StartMiniArc();
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());

  // Check "rw" is not in |params|.
  auto request = GetTestConciergeClient()->start_arc_vm_request();
  EXPECT_FALSE(base::Contains(request.params(), "rw"));
}

// Tests that the kernel parameter does include "rw" when '/' is writable and
// the image is in ext4.
TEST_F(ArcVmClientAdapterTest, KernelParam_RW) {
  set_host_rootfs_writable(true);
  set_system_image_ext_format(true);
  StartMiniArc();
  EXPECT_TRUE(GetTestConciergeClient()->start_arc_vm_called());

  // Check "rw" is in |params|.
  auto request = GetTestConciergeClient()->start_arc_vm_request();
  EXPECT_TRUE(base::Contains(request.params(), "rw"));
}

// Tests that CreateArcVmClientAdapter() doesn't crash.
TEST_F(ArcVmClientAdapterTest, TestCreateArcVmClientAdapter) {
  CreateArcVmClientAdapter();
}

TEST_F(ArcVmClientAdapterTest, ChromeOsChannelStable) {
  base::test::ScopedChromeOSVersionInfo info(
      "CHROMEOS_RELEASE_TRACK=stable-channel", base::Time::Now());

  StartParams start_params(GetPopulatedStartParams());
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.chromeos_channel=stable"));
}

TEST_F(ArcVmClientAdapterTest, ChromeOsChannelUnknown) {
  base::test::ScopedChromeOSVersionInfo info("CHROMEOS_RELEASE_TRACK=invalid",
                                             base::Time::Now());

  StartParams start_params(GetPopulatedStartParams());
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.chromeos_channel=unknown"));
}

TEST_F(ArcVmClientAdapterTest, VshdForTest) {
  base::test::ScopedChromeOSVersionInfo info(
      "CHROMEOS_RELEASE_TRACK=testimage-channel", base::Time::Now());

  StartParams start_params(GetPopulatedStartParams());
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  UpgradeArc(true);
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.vshd_service_override=vshd_for_test"));
}

TEST_F(ArcVmClientAdapterTest, VshdForRelease) {
  base::test::ScopedChromeOSVersionInfo info(
      "CHROMEOS_RELEASE_TRACK=stable-channel", base::Time::Now());

  StartParams start_params(GetPopulatedStartParams());
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  UpgradeArc(true);
  EXPECT_FALSE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.vshd_service_override=vshd_for_test"));
}

TEST_F(ArcVmClientAdapterTest, VshdForUnknownChannel) {
  base::test::ScopedChromeOSVersionInfo info("CHROMEOS_RELEASE_TRACK=unknown",
                                             base::Time::Now());

  StartParams start_params(GetPopulatedStartParams());
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  UpgradeArc(true);
  EXPECT_FALSE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.vshd_service_override=vshd_for_test"));
}

// Tests that the binary translation type is set to None when no library is
// enabled by USE flags.
TEST_F(ArcVmClientAdapterTest, BintaryTranslationTypeNone) {
  StartParams start_params(GetPopulatedStartParams());
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.native_bridge=0"));
}

// Tests that the binary translation type is set to Houdini when only 32-bit
// Houdini library is enabled by USE flags.
TEST_F(ArcVmClientAdapterTest, BintaryTranslationTypeHoudini) {
  base::CommandLine::ForCurrentProcess()->InitFromArgv(
      {"", "--enable-houdini"});
  StartParams start_params(GetPopulatedStartParams());
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.native_bridge=libhoudini.so"));
}

// Tests that the binary translation type is set to Houdini when only 64-bit
// Houdini library is enabled by USE flags.
TEST_F(ArcVmClientAdapterTest, BintaryTranslationTypeHoudini64) {
  base::CommandLine::ForCurrentProcess()->InitFromArgv(
      {"", "--enable-houdini64"});
  StartParams start_params(GetPopulatedStartParams());
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.native_bridge=libhoudini.so"));
}

// Tests that the binary translation type is set to NDK translation when only
// 32-bit NDK translation library is enabled by USE flags.
TEST_F(ArcVmClientAdapterTest, BintaryTranslationTypeNdkTranslation) {
  base::CommandLine::ForCurrentProcess()->InitFromArgv(
      {"", "--enable-ndk-translation"});
  StartParams start_params(GetPopulatedStartParams());
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.native_bridge=libndk_translation.so"));
}

// Tests that the binary translation type is set to NDK translation when only
// 64-bit NDK translation library is enabled by USE flags.
TEST_F(ArcVmClientAdapterTest, BintaryTranslationTypeNdkTranslation64) {
  base::CommandLine::ForCurrentProcess()->InitFromArgv(
      {"", "--enable-ndk-translation64"});
  StartParams start_params(GetPopulatedStartParams());
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.native_bridge=libndk_translation.so"));
}

// Tests that the binary translation type is set to NDK translation when both
// Houdini and NDK translation libraries are enabled by USE flags, and the
// parameter start_params.native_bridge_experiment is set to true.
TEST_F(ArcVmClientAdapterTest, BintaryTranslationTypeNativeBridgeExperiment) {
  base::CommandLine::ForCurrentProcess()->InitFromArgv(
      {"", "--enable-houdini", "--enable-ndk-translation"});
  StartParams start_params(GetPopulatedStartParams());
  start_params.native_bridge_experiment = true;
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.native_bridge=libndk_translation.so"));
}

// Tests that the binary translation type is set to Houdini when both Houdini
// and NDK translation libraries are enabled by USE flags, and the parameter
// start_params.native_bridge_experiment is set to false.
TEST_F(ArcVmClientAdapterTest, BintaryTranslationTypeNoNativeBridgeExperiment) {
  base::CommandLine::ForCurrentProcess()->InitFromArgv(
      {"", "--enable-houdini", "--enable-ndk-translation"});
  StartParams start_params(GetPopulatedStartParams());
  start_params.native_bridge_experiment = false;
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.native_bridge=libhoudini.so"));
}

// Tests that "readahead" mode is used by default.
TEST_F(ArcVmClientAdapterTest, TestGetArcVmUreadaheadModeDefault) {
  StartParams start_params(GetPopulatedStartParams());
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.arcvm_ureadahead_mode=readahead"));
  EXPECT_FALSE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.arcvm_ureadahead_mode=generate"));
}

// Tests that the "generate" command line switches the mode.
TEST_F(ArcVmClientAdapterTest, TestGetArcVmUreadaheadModeGenerate) {
  base::CommandLine::ForCurrentProcess()->InitFromArgv(
      {"", "--arcvm-ureadahead-mode=generate"});
  StartParams start_params(GetPopulatedStartParams());
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  EXPECT_FALSE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.arcvm_ureadahead_mode=readahead"));
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.arcvm_ureadahead_mode=generate"));
}

// Tests that the "disabled" command line disables both readahead and generate.
TEST_F(ArcVmClientAdapterTest, TestGetArcVmUreadaheadModeDisabled) {
  base::CommandLine::ForCurrentProcess()->InitFromArgv(
      {"", "--arcvm-ureadahead-mode=disabled"});
  StartParams start_params(GetPopulatedStartParams());
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  EXPECT_FALSE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.arcvm_ureadahead_mode=readahead"));
  EXPECT_FALSE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.arcvm_ureadahead_mode=generate"));
}

// Tests that ArcVmClientAdapter connects to the boot notification server
// twice: once in StartMiniArc to check that it is listening, and the second
// time in UpgradeArc to send props.
TEST_F(ArcVmClientAdapterTest, TestConnectToBootNotificationServer) {
  // Stop the RunLoop after a connection to the server.
  boot_notification_server()->SetConnectionCallback(run_loop()->QuitClosure());
  SetValidUserInfo();
  adapter()->StartMiniArc(
      {}, base::BindOnce([](bool result) { EXPECT_TRUE(result); }));
  run_loop()->Run();

  EXPECT_EQ(boot_notification_server()->connection_count(), 1);
  EXPECT_TRUE(boot_notification_server()->received_data().empty());

  RecreateRunLoop();
  boot_notification_server()->SetConnectionCallback(run_loop()->QuitClosure());
  adapter()->UpgradeArc(
      GetPopulatedUpgradeParams(),
      base::BindOnce([](bool result) { EXPECT_TRUE(result); }));
  run_loop()->Run();

  EXPECT_EQ(boot_notification_server()->connection_count(), 2);
  EXPECT_FALSE(boot_notification_server()->received_data().empty());
  // Compare received data to expected output
  std::string expected_props = base::JoinString(
      GenerateUpgradePropsForTesting(GetPopulatedUpgradeParams(), kSerialNumber,
                                     "ro.boot"),
      "\n");
  EXPECT_EQ(boot_notification_server()->received_data(), expected_props);
}

// Tests that StartMiniArc fails when the boot notification server is not
// listening.
TEST_F(ArcVmClientAdapterTest, TestBootNotificationServerIsNotListening) {
  boot_notification_server()->Stop();
  // Change timeout to 26 seconds to allow for exponential backoff.
  base::test::ScopedRunLoopTimeout timeout(FROM_HERE,
                                           base::TimeDelta::FromSeconds(26));

  StartMiniArcWithParams(false, {});
}

// Tests that UpgradeArc() fails when sending the upgrade props
// to the boot notification server fails.
TEST_F(ArcVmClientAdapterTest, UpgradeArc_SendPropFail) {
  SetValidUserInfo();
  StartMiniArc();

  // Let ConnectToArcVmBootNotificationServer() return an invalid FD.
  SetArcVmBootNotificationServerFdForTesting(-1);

  UpgradeArcWithParamsAndStopVmCount(false, {}, /*run_until_stop_vm_count=*/3);
  ExpectArcStopped(/*stale_full_vm_stopped=*/true);
}

// Tests that UpgradeArc() fails when sending the upgrade props
// to the boot notification server fails.
TEST_F(ArcVmClientAdapterTest, UpgradeArc_SendPropFailNotWritable) {
  SetValidUserInfo();
  StartMiniArc();

  // Let ConnectToArcVmBootNotificationServer() return dup(STDIN_FILENO) which
  // is not writable.
  SetArcVmBootNotificationServerFdForTesting(STDIN_FILENO);

  UpgradeArcWithParamsAndStopVmCount(false, {}, /*run_until_stop_vm_count=*/3);
  ExpectArcStopped(/*stale_full_vm_stopped=*/true);
}

TEST_F(ArcVmClientAdapterTest, DisableDownloadProviderDefault) {
  StartParams start_params(GetPopulatedStartParams());
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  auto request = GetTestConciergeClient()->start_arc_vm_request();
  // Not expected arc_disable_download_provider in properties.
  for (const auto& param : request.params())
    EXPECT_EQ(std::string::npos, param.find("disable_download_provider"));
}

TEST_F(ArcVmClientAdapterTest, DisableDownloadProviderEnforced) {
  StartParams start_params(GetPopulatedStartParams());
  start_params.disable_download_provider = true;
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  auto request = GetTestConciergeClient()->start_arc_vm_request();
  EXPECT_TRUE(
      base::Contains(GetTestConciergeClient()->start_arc_vm_request().params(),
                     "androidboot.disable_download_provider=1"));
}

struct DalvikMemoryProfileTestParam {
  // Requested profile.
  StartParams::DalvikMemoryProfile profile;
  // Name of profile that is expected.
  const char* profile_name;
};

constexpr DalvikMemoryProfileTestParam kDalvikMemoryProfileTestCases[] = {
    {StartParams::DalvikMemoryProfile::DEFAULT, nullptr},
    {StartParams::DalvikMemoryProfile::M4G, "4G"},
    {StartParams::DalvikMemoryProfile::M8G, "8G"},
    {StartParams::DalvikMemoryProfile::M16G, "16G"}};

class ArcVmClientAdapterDalvikMemoryProfileTest
    : public ArcVmClientAdapterTest,
      public testing::WithParamInterface<DalvikMemoryProfileTestParam> {};

INSTANTIATE_TEST_SUITE_P(All,
                         ArcVmClientAdapterDalvikMemoryProfileTest,
                         ::testing::ValuesIn(kDalvikMemoryProfileTestCases));

TEST_P(ArcVmClientAdapterDalvikMemoryProfileTest, Profile) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitWithFeatureState(arc::kUseHighMemoryDalvikProfile,
                                    true /* use */);

  const auto& test_param = GetParam();
  StartParams start_params(GetPopulatedStartParams());
  start_params.dalvik_memory_profile = test_param.profile;
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  auto request = GetTestConciergeClient()->start_arc_vm_request();
  if (test_param.profile_name) {
    EXPECT_TRUE(base::Contains(
        GetTestConciergeClient()->start_arc_vm_request().params(),
        std::string("androidboot.arc_dalvik_memory_profile=") +
            test_param.profile_name));
  } else {
    // Not expected any arc_dalvik_memory_profile.
    for (const auto& param : request.params())
      EXPECT_EQ(std::string::npos, param.find("arc_dalvik_memory_profile"));
  }
}

struct UsapProfileTestParam {
  // Requested profile.
  StartParams::UsapProfile profile;
  // Name of profile that is expected.
  const char* profile_name;
};

constexpr UsapProfileTestParam kUsapProfileTestCases[] = {
    {StartParams::UsapProfile::DEFAULT, nullptr},
    {StartParams::UsapProfile::M4G, "4G"},
    {StartParams::UsapProfile::M8G, "8G"},
    {StartParams::UsapProfile::M16G, "16G"}};

class ArcVmClientAdapterUsapProfileTest
    : public ArcVmClientAdapterTest,
      public testing::WithParamInterface<UsapProfileTestParam> {};

INSTANTIATE_TEST_SUITE_P(All,
                         ArcVmClientAdapterUsapProfileTest,
                         ::testing::ValuesIn(kUsapProfileTestCases));

TEST_P(ArcVmClientAdapterUsapProfileTest, Profile) {
  const auto& test_param = GetParam();
  StartParams start_params(GetPopulatedStartParams());
  start_params.usap_profile = test_param.profile;
  SetValidUserInfo();
  StartMiniArcWithParams(true, std::move(start_params));
  auto request = GetTestConciergeClient()->start_arc_vm_request();
  if (test_param.profile_name) {
    EXPECT_TRUE(base::Contains(
        GetTestConciergeClient()->start_arc_vm_request().params(),
        std::string("androidboot.usap_profile=") + test_param.profile_name));
  } else {
    // Not expected any arc_dalvik_memory_profile.
    for (const auto& param : request.params())
      EXPECT_EQ(std::string::npos, param.find("usap_profile"));
  }
}

}  // namespace
}  // namespace arc
