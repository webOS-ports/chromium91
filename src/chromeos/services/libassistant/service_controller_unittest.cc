// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/services/libassistant/service_controller.h"

#include <memory>

#include "base/base_paths.h"
#include "base/json/json_reader.h"
#include "base/run_loop.h"
#include "base/test/gtest_util.h"
#include "base/test/scoped_path_override.h"
#include "base/test/task_environment.h"
#include "chromeos/assistant/internal/test_support/fake_assistant_manager.h"
#include "chromeos/assistant/internal/test_support/fake_assistant_manager_internal.h"
#include "chromeos/services/libassistant/assistant_manager_observer.h"
#include "chromeos/services/libassistant/public/mojom/service_controller.mojom.h"
#include "chromeos/services/libassistant/public/mojom/settings_controller.mojom.h"
#include "chromeos/services/libassistant/settings_controller.h"
#include "chromeos/services/libassistant/test_support/fake_libassistant_factory.h"
#include "libassistant/shared/internal_api/assistant_manager_internal.h"
#include "libassistant/shared/public/device_state_listener.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chromeos {
namespace libassistant {

namespace {

using mojom::ServiceState;
using ::testing::StrictMock;

#define EXPECT_NO_CALLS(args...) EXPECT_CALL(args).Times(0)

// Tests if the JSON string contains the given path with the given value
#define EXPECT_HAS_PATH_WITH_VALUE(config_string, path, expected_value)    \
  ({                                                                       \
    base::Optional<base::Value> config =                                   \
        base::JSONReader::Read(config_string);                             \
    ASSERT_TRUE(config.has_value());                                       \
    const base::Value* actual = config->FindPath(path);                    \
    base::Value expected = base::Value(expected_value);                    \
    ASSERT_NE(actual, nullptr)                                             \
        << "Path '" << path << "' not found in config: " << config_string; \
    EXPECT_EQ(*actual, expected);                                          \
  })

std::vector<mojom::AuthenticationTokenPtr> ToVector(
    mojom::AuthenticationTokenPtr token) {
  std::vector<mojom::AuthenticationTokenPtr> result;
  result.push_back(std::move(token));
  return result;
}

class StateObserverMock : public mojom::StateObserver {
 public:
  StateObserverMock() : receiver_(this) {}

  MOCK_METHOD(void, OnStateChanged, (ServiceState));

  mojo::PendingRemote<mojom::StateObserver> BindAndPassReceiver() {
    return receiver_.BindNewPipeAndPassRemote();
  }

 private:
  mojo::Receiver<mojom::StateObserver> receiver_;
};

class AssistantManagerObserverMock : public AssistantManagerObserver {
 public:
  AssistantManagerObserverMock() = default;
  AssistantManagerObserverMock(AssistantManagerObserverMock&) = delete;
  AssistantManagerObserverMock& operator=(AssistantManagerObserverMock&) =
      delete;
  ~AssistantManagerObserverMock() override = default;

  // AssistantManagerObserver implementation:
  MOCK_METHOD(
      void,
      OnAssistantManagerCreated,
      (assistant_client::AssistantManager * assistant_manager,
       assistant_client::AssistantManagerInternal* assistant_manager_internal));
  MOCK_METHOD(
      void,
      OnAssistantManagerStarted,
      (assistant_client::AssistantManager * assistant_manager,
       assistant_client::AssistantManagerInternal* assistant_manager_internal));
  MOCK_METHOD(
      void,
      OnAssistantManagerRunning,
      (assistant_client::AssistantManager * assistant_manager,
       assistant_client::AssistantManagerInternal* assistant_manager_internal));
  MOCK_METHOD(
      void,
      OnDestroyingAssistantManager,
      (assistant_client::AssistantManager * assistant_manager,
       assistant_client::AssistantManagerInternal* assistant_manager_internal));
  MOCK_METHOD(void, OnAssistantManagerDestroyed, ());
};

class SettingsControllerMock : public mojom::SettingsController {
 public:
  SettingsControllerMock() = default;
  SettingsControllerMock(const SettingsControllerMock&) = delete;
  SettingsControllerMock& operator=(const SettingsControllerMock&) = delete;
  ~SettingsControllerMock() override = default;

  // mojom::SettingsController implementation:
  MOCK_METHOD(void,
              SetAuthenticationTokens,
              (std::vector<mojom::AuthenticationTokenPtr> tokens));
  MOCK_METHOD(void, SetListeningEnabled, (bool value));
  MOCK_METHOD(void, SetLocale, (const std::string& value));
  MOCK_METHOD(void, SetSpokenFeedbackEnabled, (bool value));
  MOCK_METHOD(void, SetHotwordEnabled, (bool value));
  MOCK_METHOD(void,
              GetSettings,
              (const std::string& selector, GetSettingsCallback callback));
  MOCK_METHOD(void,
              UpdateSettings,
              (const std::string& settings, UpdateSettingsCallback callback));
};

class AssistantServiceControllerTest : public testing::Test {
 public:
  AssistantServiceControllerTest()
      : service_controller_(
            std::make_unique<ServiceController>(&libassistant_factory_)) {
    service_controller_->Bind(client_.BindNewPipeAndPassReceiver(),
                              &settings_controller_);
  }

  mojo::Remote<mojom::ServiceController>& client() { return client_; }
  ServiceController& service_controller() {
    DCHECK(service_controller_);
    return *service_controller_;
  }

  void RunUntilIdle() { base::RunLoop().RunUntilIdle(); }

  // Add the state observer. Will expect the call that follows immediately after
  // adding the observer.
  void AddStateObserver(StateObserverMock* observer) {
    EXPECT_CALL(*observer, OnStateChanged);
    service_controller().AddAndFireStateObserver(
        observer->BindAndPassReceiver());
    RunUntilIdle();
  }

  void AddAndFireAssistantManagerObserver(AssistantManagerObserver* observer) {
    service_controller().AddAndFireAssistantManagerObserver(observer);
  }

  void RemoveAssistantManagerObserver(AssistantManagerObserver* observer) {
    service_controller().RemoveAssistantManagerObserver(observer);
  }

  void AddAndFireStateObserver(StateObserverMock* observer) {
    service_controller().AddAndFireStateObserver(
        observer->BindAndPassReceiver());
    RunUntilIdle();
  }

  void Initialize(mojom::BootupConfigPtr config = mojom::BootupConfig::New()) {
    service_controller().Initialize(std::move(config), BindURLLoaderFactory());
  }

  void Start() {
    service_controller().Start();
    RunUntilIdle();
  }

  void SendOnStartFinished() {
    auto* device_state_listener =
        libassistant_factory_.assistant_manager().device_state_listener();
    ASSERT_NE(device_state_listener, nullptr);
    device_state_listener->OnStartFinished();
    RunUntilIdle();
  }

  void Stop() {
    service_controller().Stop();
    RunUntilIdle();
  }

  void DestroyServiceController() { service_controller_.reset(); }

  std::string libassistant_config() {
    return libassistant_factory_.libassistant_config();
  }

  SettingsControllerMock& settings_controller_mock() {
    return settings_controller_;
  }

 private:
  mojo::PendingRemote<network::mojom::URLLoaderFactory> BindURLLoaderFactory() {
    mojo::PendingRemote<network::mojom::URLLoaderFactory> pending_remote;
    url_loader_factory_.Clone(pending_remote.InitWithNewPipeAndPassReceiver());
    return pending_remote;
  }

  base::test::SingleThreadTaskEnvironment environment_;
  base::ScopedPathOverride home_override{base::DIR_HOME};

  network::TestURLLoaderFactory url_loader_factory_;

  FakeLibassistantFactory libassistant_factory_;
  testing::NiceMock<SettingsControllerMock> settings_controller_;
  mojo::Remote<mojom::ServiceController> client_;
  std::unique_ptr<ServiceController> service_controller_;
};

}  // namespace

namespace mojom {

void PrintTo(const ServiceState state, std::ostream* stream) {
  switch (state) {
    case ServiceState::kRunning:
      *stream << "kRunning";
      return;
    case ServiceState::kStarted:
      *stream << "kStarted";
      return;
    case ServiceState::kStopped:
      *stream << "kStopped";
      return;
  }
  *stream << "INVALID ServiceState (" << static_cast<int>(state) << ")";
}

}  // namespace mojom

TEST_F(AssistantServiceControllerTest, StateShouldStartAsStopped) {
  Initialize();
  StateObserverMock observer;

  EXPECT_CALL(observer, OnStateChanged(ServiceState::kStopped));

  AddAndFireStateObserver(&observer);
}

TEST_F(AssistantServiceControllerTest,
       StateShouldChangeToStartedAfterCallingStart) {
  Initialize();
  StateObserverMock observer;
  AddStateObserver(&observer);

  EXPECT_CALL(observer, OnStateChanged(ServiceState::kStarted));

  Start();
}

TEST_F(AssistantServiceControllerTest,
       StateShouldChangeToStoppedAfterCallingStop) {
  Initialize();
  Start();

  StateObserverMock observer;
  AddStateObserver(&observer);

  EXPECT_CALL(observer, OnStateChanged(ServiceState::kStopped));

  Stop();
}

TEST_F(AssistantServiceControllerTest,
       StateShouldChangeToRunningAfterLibassistantSignalsItsDone) {
  Initialize();
  Start();

  StateObserverMock observer;
  AddStateObserver(&observer);

  EXPECT_CALL(observer, OnStateChanged(ServiceState::kRunning));

  SendOnStartFinished();
}

TEST_F(AssistantServiceControllerTest,
       ShouldSendCurrentStateWhenAddingObserver) {
  Initialize();

  {
    StateObserverMock observer;

    EXPECT_CALL(observer, OnStateChanged(ServiceState::kStopped));
    AddAndFireStateObserver(&observer);
  }

  Start();

  {
    StateObserverMock observer;

    EXPECT_CALL(observer, OnStateChanged(ServiceState::kStarted));
    AddAndFireStateObserver(&observer);
  }

  Stop();

  {
    StateObserverMock observer;

    EXPECT_CALL(observer, OnStateChanged(ServiceState::kStopped));
    AddAndFireStateObserver(&observer);
  }
}

TEST_F(AssistantServiceControllerTest,
       ShouldCreateAssistantManagerWhenCallingInitialize) {
  EXPECT_EQ(nullptr, service_controller().assistant_manager());

  Initialize();

  EXPECT_NE(nullptr, service_controller().assistant_manager());
}

TEST_F(AssistantServiceControllerTest, ShouldBeNoopWhenCallingStartTwice) {
  // Note: This is the preferred behavior for services exposed through mojom.
  Initialize();
  Start();

  StateObserverMock observer;
  AddStateObserver(&observer);

  EXPECT_NO_CALLS(observer, OnStateChanged);

  Start();
}

TEST_F(AssistantServiceControllerTest, CallingStopTwiceShouldBeANoop) {
  Initialize();
  Stop();

  StateObserverMock observer;
  AddStateObserver(&observer);

  EXPECT_NO_CALLS(observer, OnStateChanged);

  Stop();
}

TEST_F(AssistantServiceControllerTest, ShouldAllowStartAfterStop) {
  Initialize();
  Start();
  Stop();

  // The second Initialize() call should create the AssistantManager.

  Initialize();
  EXPECT_NE(nullptr, service_controller().assistant_manager());

  // The second Start() call should send out a state update.

  StateObserverMock observer;
  AddStateObserver(&observer);

  EXPECT_CALL(observer, OnStateChanged(ServiceState::kStarted));

  Start();

  EXPECT_NE(nullptr, service_controller().assistant_manager());
}

TEST_F(AssistantServiceControllerTest,
       ShouldDestroyAssistantManagerWhenCallingStop) {
  Initialize();
  Start();
  EXPECT_NE(nullptr, service_controller().assistant_manager());

  Stop();

  EXPECT_EQ(nullptr, service_controller().assistant_manager());
}

TEST_F(AssistantServiceControllerTest,
       StateShouldChangeToStoppedWhenBeingDestroyed) {
  Initialize();
  Start();

  StateObserverMock observer;
  AddStateObserver(&observer);

  EXPECT_CALL(observer, OnStateChanged(ServiceState::kStopped));

  DestroyServiceController();
  RunUntilIdle();
}

TEST_F(AssistantServiceControllerTest,
       ShouldCreateButNotPublishAssistantManagerInternalWhenCallingInitialize) {
  EXPECT_EQ(nullptr, service_controller().assistant_manager_internal());

  Initialize();

  EXPECT_NE(nullptr, service_controller().assistant_manager_internal());
}

TEST_F(AssistantServiceControllerTest,
       ShouldPublishAssistantManagerInternalWhenCallingStart) {
  Initialize();
  Start();

  EXPECT_NE(nullptr, service_controller().assistant_manager_internal());
}

TEST_F(AssistantServiceControllerTest,
       ShouldDestroyAssistantManagerInternalWhenCallingStop) {
  Initialize();

  EXPECT_NE(nullptr, service_controller().assistant_manager_internal());

  Start();
  Stop();

  EXPECT_EQ(nullptr, service_controller().assistant_manager_internal());
}

TEST_F(AssistantServiceControllerTest,
       ShouldPassS3ServerUriOverrideToMojomService) {
  auto bootup_config = mojom::BootupConfig::New();
  bootup_config->s3_server_uri_override = "the-s3-server-uri-override";
  Initialize(std::move(bootup_config));

  EXPECT_HAS_PATH_WITH_VALUE(libassistant_config(),
                             "testing.s3_grpc_server_uri",
                             "the-s3-server-uri-override");
}

TEST_F(AssistantServiceControllerTest,
       ShouldCallOnAssistantManagerCreatedWhenCallingInitialize) {
  StrictMock<AssistantManagerObserverMock> observer;
  AddAndFireAssistantManagerObserver(&observer);

  EXPECT_CALL(observer, OnAssistantManagerCreated)
      .WillOnce([&controller = service_controller()](
                    assistant_client::AssistantManager* assistant_manager,
                    assistant_client::AssistantManagerInternal*
                        assistant_manager_internal) {
        EXPECT_EQ(assistant_manager, controller.assistant_manager());
        EXPECT_EQ(assistant_manager_internal,
                  controller.assistant_manager_internal());
      });

  Initialize();

  RemoveAssistantManagerObserver(&observer);
}

TEST_F(AssistantServiceControllerTest,
       ShouldCallOnAssistantManagerCreatedWhenAddingObserver) {
  Initialize();

  StrictMock<AssistantManagerObserverMock> observer;

  EXPECT_CALL(observer, OnAssistantManagerCreated)
      .WillOnce([&controller = service_controller()](
                    assistant_client::AssistantManager* assistant_manager,
                    assistant_client::AssistantManagerInternal*
                        assistant_manager_internal) {
        EXPECT_EQ(assistant_manager, controller.assistant_manager());
        EXPECT_EQ(assistant_manager_internal,
                  controller.assistant_manager_internal());
      });

  AddAndFireAssistantManagerObserver(&observer);

  RemoveAssistantManagerObserver(&observer);
}

TEST_F(AssistantServiceControllerTest,
       ShouldCallOnAssistantManagerStartedWhenCallingStart) {
  StrictMock<AssistantManagerObserverMock> observer;
  AddAndFireAssistantManagerObserver(&observer);

  EXPECT_CALL(observer, OnAssistantManagerCreated);
  Initialize();

  EXPECT_CALL(observer, OnAssistantManagerStarted)
      .WillOnce([&controller = service_controller()](
                    assistant_client::AssistantManager* assistant_manager,
                    assistant_client::AssistantManagerInternal*
                        assistant_manager_internal) {
        EXPECT_EQ(assistant_manager, controller.assistant_manager());
        EXPECT_EQ(assistant_manager_internal,
                  controller.assistant_manager_internal());
      });

  Start();

  RemoveAssistantManagerObserver(&observer);
}

TEST_F(AssistantServiceControllerTest,
       ShouldCallOnAssistantManagerInitializedAndCreatedWhenAddingObserver) {
  Initialize();
  Start();

  StrictMock<AssistantManagerObserverMock> observer;

  EXPECT_CALL(observer, OnAssistantManagerCreated)
      .WillOnce([&controller = service_controller()](
                    assistant_client::AssistantManager* assistant_manager,
                    assistant_client::AssistantManagerInternal*
                        assistant_manager_internal) {
        EXPECT_EQ(assistant_manager, controller.assistant_manager());
        EXPECT_EQ(assistant_manager_internal,
                  controller.assistant_manager_internal());
      });

  EXPECT_CALL(observer, OnAssistantManagerStarted)
      .WillOnce([&controller = service_controller()](
                    assistant_client::AssistantManager* assistant_manager,
                    assistant_client::AssistantManagerInternal*
                        assistant_manager_internal) {
        EXPECT_EQ(assistant_manager, controller.assistant_manager());
        EXPECT_EQ(assistant_manager_internal,
                  controller.assistant_manager_internal());
      });

  AddAndFireAssistantManagerObserver(&observer);

  RemoveAssistantManagerObserver(&observer);
}

TEST_F(AssistantServiceControllerTest,
       ShouldCallOnAssistantManagerRunningWhenCallingOnStartFinished) {
  StrictMock<AssistantManagerObserverMock> observer;
  AddAndFireAssistantManagerObserver(&observer);

  EXPECT_CALL(observer, OnAssistantManagerCreated);
  Initialize();
  EXPECT_CALL(observer, OnAssistantManagerStarted);
  Start();

  EXPECT_CALL(observer, OnAssistantManagerRunning)
      .WillOnce([&controller = service_controller()](
                    assistant_client::AssistantManager* assistant_manager,
                    assistant_client::AssistantManagerInternal*
                        assistant_manager_internal) {
        EXPECT_EQ(assistant_manager, controller.assistant_manager());
        EXPECT_EQ(assistant_manager_internal,
                  controller.assistant_manager_internal());
      });

  SendOnStartFinished();

  RemoveAssistantManagerObserver(&observer);
}

TEST_F(AssistantServiceControllerTest,
       ShouldCallOnAssistantManagerRunningWhenAddingObserver) {
  Initialize();
  Start();
  SendOnStartFinished();

  StrictMock<AssistantManagerObserverMock> observer;

  EXPECT_CALL(observer, OnAssistantManagerCreated)
      .WillOnce([&controller = service_controller()](
                    assistant_client::AssistantManager* assistant_manager,
                    assistant_client::AssistantManagerInternal*
                        assistant_manager_internal) {
        EXPECT_EQ(assistant_manager, controller.assistant_manager());
        EXPECT_EQ(assistant_manager_internal,
                  controller.assistant_manager_internal());
      });

  EXPECT_CALL(observer, OnAssistantManagerStarted)
      .WillOnce([&controller = service_controller()](
                    assistant_client::AssistantManager* assistant_manager,
                    assistant_client::AssistantManagerInternal*
                        assistant_manager_internal) {
        EXPECT_EQ(assistant_manager, controller.assistant_manager());
        EXPECT_EQ(assistant_manager_internal,
                  controller.assistant_manager_internal());
      });

  EXPECT_CALL(observer, OnAssistantManagerRunning)
      .WillOnce([&controller = service_controller()](
                    assistant_client::AssistantManager* assistant_manager,
                    assistant_client::AssistantManagerInternal*
                        assistant_manager_internal) {
        EXPECT_EQ(assistant_manager, controller.assistant_manager());
        EXPECT_EQ(assistant_manager_internal,
                  controller.assistant_manager_internal());
      });

  AddAndFireAssistantManagerObserver(&observer);

  RemoveAssistantManagerObserver(&observer);
}
TEST_F(AssistantServiceControllerTest,
       ShouldCallOnDestroyingAssistantManagerWhenCallingStop) {
  StrictMock<AssistantManagerObserverMock> observer;
  AddAndFireAssistantManagerObserver(&observer);

  EXPECT_CALL(observer, OnAssistantManagerCreated);
  EXPECT_CALL(observer, OnAssistantManagerStarted);
  Initialize();
  Start();

  const auto* expected_assistant_manager =
      service_controller().assistant_manager();
  const auto* expected_assistant_manager_internal =
      service_controller().assistant_manager_internal();

  EXPECT_CALL(observer, OnDestroyingAssistantManager)
      .WillOnce([&](assistant_client::AssistantManager* assistant_manager,
                    assistant_client::AssistantManagerInternal*
                        assistant_manager_internal) {
        EXPECT_EQ(assistant_manager, expected_assistant_manager);
        EXPECT_EQ(assistant_manager_internal,
                  expected_assistant_manager_internal);
      });
  EXPECT_CALL(observer, OnAssistantManagerDestroyed);

  Stop();

  RemoveAssistantManagerObserver(&observer);
}

TEST_F(AssistantServiceControllerTest,
       ShouldNotCallAssistantManagerObserverWhenItHasBeenRemoved) {
  StrictMock<AssistantManagerObserverMock> observer;
  AddAndFireAssistantManagerObserver(&observer);
  RemoveAssistantManagerObserver(&observer);

  EXPECT_NO_CALLS(observer, OnAssistantManagerCreated);
  EXPECT_NO_CALLS(observer, OnAssistantManagerStarted);
  EXPECT_NO_CALLS(observer, OnDestroyingAssistantManager);
  EXPECT_NO_CALLS(observer, OnAssistantManagerDestroyed);

  Initialize();
  Start();
  Stop();

  RemoveAssistantManagerObserver(&observer);
}

TEST_F(AssistantServiceControllerTest,
       ShouldCallOnDestroyingAssistantManagerWhenBeingDestroyed) {
  Initialize();
  Start();

  StrictMock<AssistantManagerObserverMock> observer;
  EXPECT_CALL(observer, OnAssistantManagerCreated);
  EXPECT_CALL(observer, OnAssistantManagerStarted);
  AddAndFireAssistantManagerObserver(&observer);

  EXPECT_CALL(observer, OnDestroyingAssistantManager);
  EXPECT_CALL(observer, OnAssistantManagerDestroyed);
  DestroyServiceController();
}

TEST_F(AssistantServiceControllerTest,
       ShouldPassBootupConfigToSettingsController) {
  const bool hotword_enabled = true;
  const bool spoken_feedback_enabled = false;

  EXPECT_CALL(settings_controller_mock(), SetLocale("locale"));
  EXPECT_CALL(settings_controller_mock(), SetHotwordEnabled(hotword_enabled));
  EXPECT_CALL(settings_controller_mock(),
              SetSpokenFeedbackEnabled(spoken_feedback_enabled));
  EXPECT_CALL(settings_controller_mock(), SetAuthenticationTokens);

  auto bootup_config = mojom::BootupConfig::New();
  bootup_config->locale = "locale";
  bootup_config->hotword_enabled = hotword_enabled;
  bootup_config->spoken_feedback_enabled = spoken_feedback_enabled;
  bootup_config->authentication_tokens =
      ToVector(mojom::AuthenticationToken::New("user", "token"));

  Initialize(std::move(bootup_config));
}

}  // namespace libassistant
}  // namespace chromeos
