// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_SERVICES_LIBASSISTANT_SERVICE_CONTROLLER_H_
#define CHROMEOS_SERVICES_LIBASSISTANT_SERVICE_CONTROLLER_H_

#include "base/component_export.h"
#include "base/observer_list.h"
#include "base/scoped_observation.h"
#include "chromeos/services/libassistant/assistant_manager_observer.h"
#include "chromeos/services/libassistant/public/mojom/service.mojom.h"
#include "chromeos/services/libassistant/public/mojom/service_controller.mojom.h"
#include "chromeos/services/libassistant/public/mojom/settings_controller.mojom-forward.h"
#include "libassistant/shared/public/assistant_manager.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote_set.h"

namespace assistant_client {
class AssistantManager;
class AssistantManagerInternal;
}  // namespace assistant_client

namespace chromeos {
namespace libassistant {

class ChromiumApiDelegate;
class LibassistantFactory;

// Component managing the lifecycle of Libassistant,
// exposing methods to start/stop and configure Libassistant.
class COMPONENT_EXPORT(LIBASSISTANT_SERVICE) ServiceController
    : public mojom::ServiceController {
 public:
  explicit ServiceController(LibassistantFactory* factory);
  ServiceController(ServiceController&) = delete;
  ServiceController& operator=(ServiceController&) = delete;
  ~ServiceController() override;

  void Bind(mojo::PendingReceiver<mojom::ServiceController> receiver,
            mojom::SettingsController* settings_controller);

  // mojom::ServiceController implementation:
  void Initialize(mojom::BootupConfigPtr libassistant_config,
                  mojo::PendingRemote<network::mojom::URLLoaderFactory>
                      url_loader_factory) override;
  void Start() override;
  void Stop() override;
  void ResetAllDataAndStop() override;
  void AddAndFireStateObserver(
      mojo::PendingRemote<mojom::StateObserver> observer) override;

  void AddAndFireAssistantManagerObserver(AssistantManagerObserver* observer);
  void RemoveAssistantManagerObserver(AssistantManagerObserver* observer);
  void RemoveAllAssistantManagerObservers();

  bool IsInitialized() const;
  // Note this is true even when the service is running (as it is still started
  // at that point).
  bool IsStarted() const;
  bool IsRunning() const;

  // Will return nullptr if the service is stopped.
  assistant_client::AssistantManager* assistant_manager();
  // Will return nullptr if the service is stopped.
  assistant_client::AssistantManagerInternal* assistant_manager_internal();

 private:
  class DeviceStateListener;

  void OnStartFinished();

  void SetStateAndInformObservers(mojom::ServiceState new_state);

  void CreateAndRegisterDeviceStateListener();
  void CreateAndRegisterChromiumApiDelegate(
      mojo::PendingRemote<network::mojom::URLLoaderFactory> url_loader_factory);
  void CreateChromiumApiDelegate(
      mojo::PendingRemote<network::mojom::URLLoaderFactory> url_loader_factory);

  mojom::ServiceState state_ = mojom::ServiceState::kStopped;

  // Called during |Initialize| to apply boot configuration.
  mojom::SettingsController* settings_controller_ = nullptr;

  LibassistantFactory& libassistant_factory_;

  std::unique_ptr<assistant_client::AssistantManager> assistant_manager_;
  assistant_client::AssistantManagerInternal* assistant_manager_internal_ =
      nullptr;
  std::unique_ptr<ChromiumApiDelegate> chromium_api_delegate_;
  std::unique_ptr<DeviceStateListener> device_state_listener_;

  mojo::Receiver<mojom::ServiceController> receiver_{this};
  mojo::RemoteSet<mojom::StateObserver> state_observers_;
  base::ObserverList<AssistantManagerObserver> assistant_manager_observers_;
};

using ScopedAssistantManagerObserver = base::ScopedObservation<
    ServiceController,
    AssistantManagerObserver,
    &ServiceController::AddAndFireAssistantManagerObserver,
    &ServiceController::RemoveAssistantManagerObserver>;

}  // namespace libassistant
}  // namespace chromeos

#endif  // CHROMEOS_SERVICES_LIBASSISTANT_SERVICE_CONTROLLER_H_
