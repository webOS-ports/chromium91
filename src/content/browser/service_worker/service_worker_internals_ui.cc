// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/service_worker/service_worker_internals_ui.h"

#include <stdint.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/callback_helpers.h"
#include "base/metrics/histogram_functions.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "content/browser/devtools/devtools_agent_host_impl.h"
#include "content/browser/devtools/service_worker_devtools_agent_host.h"
#include "content/browser/devtools/service_worker_devtools_manager.h"
#include "content/browser/service_worker/embedded_worker_status.h"
#include "content/browser/service_worker/service_worker_context_core_observer.h"
#include "content/browser/service_worker/service_worker_context_wrapper.h"
#include "content/browser/service_worker/service_worker_registration.h"
#include "content/browser/service_worker/service_worker_version.h"
#include "content/grit/dev_ui_content_resources.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/console_message.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/child_process_host.h"
#include "content/public/common/url_constants.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_object.mojom.h"

using base::DictionaryValue;
using base::ListValue;
using base::Value;
using base::WeakPtr;

namespace content {

namespace {

using GetRegistrationsCallback =
    base::OnceCallback<void(const std::vector<ServiceWorkerRegistrationInfo>&,
                            const std::vector<ServiceWorkerVersionInfo>&,
                            const std::vector<ServiceWorkerRegistrationInfo>&)>;

void OperationCompleteCallback(WeakPtr<ServiceWorkerInternalsHandler> internals,
                               const std::string& callback_id,
                               blink::ServiceWorkerStatusCode status) {
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE, base::BindOnce(OperationCompleteCallback, internals,
                                  callback_id, status));
    return;
  }
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (internals) {
    internals->OnOperationComplete(static_cast<int>(status), callback_id);
  }
}

base::ProcessId GetRealProcessId(int process_host_id) {
  if (process_host_id == ChildProcessHost::kInvalidUniqueID)
    return base::kNullProcessId;

  RenderProcessHost* rph = RenderProcessHost::FromID(process_host_id);
  if (!rph)
    return base::kNullProcessId;

  base::ProcessHandle handle = rph->GetProcess().Handle();
  if (handle == base::kNullProcessHandle)
    return base::kNullProcessId;
  // TODO(nhiroki): On Windows, |rph->GetHandle()| does not duplicate ownership
  // of the process handle and the render host still retains it. Therefore, we
  // cannot create a base::Process object, which provides a proper way to get a
  // process id, from the handle. For a stopgap, we use this deprecated
  // function that does not require the ownership (http://crbug.com/417532).
  return base::GetProcId(handle);
}

void UpdateVersionInfo(const ServiceWorkerVersionInfo& version, Value* info) {
  switch (version.running_status) {
    case EmbeddedWorkerStatus::STOPPED:
      info->SetStringKey("running_status", "STOPPED");
      break;
    case EmbeddedWorkerStatus::STARTING:
      info->SetStringKey("running_status", "STARTING");
      break;
    case EmbeddedWorkerStatus::RUNNING:
      info->SetStringKey("running_status", "RUNNING");
      break;
    case EmbeddedWorkerStatus::STOPPING:
      info->SetStringKey("running_status", "STOPPING");
      break;
  }

  switch (version.status) {
    case ServiceWorkerVersion::NEW:
      info->SetStringKey("status", "NEW");
      break;
    case ServiceWorkerVersion::INSTALLING:
      info->SetStringKey("status", "INSTALLING");
      break;
    case ServiceWorkerVersion::INSTALLED:
      info->SetStringKey("status", "INSTALLED");
      break;
    case ServiceWorkerVersion::ACTIVATING:
      info->SetStringKey("status", "ACTIVATING");
      break;
    case ServiceWorkerVersion::ACTIVATED:
      info->SetStringKey("status", "ACTIVATED");
      break;
    case ServiceWorkerVersion::REDUNDANT:
      info->SetStringKey("status", "REDUNDANT");
      break;
  }

  switch (version.fetch_handler_existence) {
    case ServiceWorkerVersion::FetchHandlerExistence::UNKNOWN:
      info->SetStringKey("fetch_handler_existence", "UNKNOWN");
      break;
    case ServiceWorkerVersion::FetchHandlerExistence::EXISTS:
      info->SetStringKey("fetch_handler_existence", "EXISTS");
      break;
    case ServiceWorkerVersion::FetchHandlerExistence::DOES_NOT_EXIST:
      info->SetStringKey("fetch_handler_existence", "DOES_NOT_EXIST");
      break;
  }

  info->SetStringKey("script_url", version.script_url.spec());
  info->SetStringKey("version_id", base::NumberToString(version.version_id));
  info->SetIntKey("process_id",
                  static_cast<int>(GetRealProcessId(version.process_id)));
  info->SetIntKey("process_host_id", version.process_id);
  info->SetIntKey("thread_id", version.thread_id);
  info->SetIntKey("devtools_agent_route_id", version.devtools_agent_route_id);

  base::Value::ListStorage clients;
  for (auto& it : version.clients) {
    base::Value client(base::Value::Type::DICTIONARY);
    client.SetStringKey("client_id", it.first);
    if (it.second.type() == blink::mojom::ServiceWorkerClientType::kWindow) {
      WebContents* web_contents =
          WebContents::FromFrameTreeNodeId(it.second.GetFrameTreeNodeId());
      if (web_contents)
        client.SetStringKey("url", web_contents->GetURL().spec());
    }
    clients.emplace_back(std::move(client));
  }
  info->SetKey("clients", base::Value(std::move(clients)));
}

base::Value::ListStorage GetRegistrationListValue(
    const std::vector<ServiceWorkerRegistrationInfo>& registrations) {
  base::Value::ListStorage result;
  for (const auto& registration : registrations) {
    base::Value registration_info(base::Value::Type::DICTIONARY);
    registration_info.SetStringKey("scope", registration.scope.spec());
    registration_info.SetStringKey(
        "registration_id", base::NumberToString(registration.registration_id));
    registration_info.SetBoolKey("navigation_preload_enabled",
                                 registration.navigation_preload_enabled);
    registration_info.SetIntKey("navigation_preload_header_length",
                                registration.navigation_preload_header_length);

    if (registration.active_version.version_id !=
        blink::mojom::kInvalidServiceWorkerVersionId) {
      base::Value active_info(base::Value::Type::DICTIONARY);
      UpdateVersionInfo(registration.active_version, &active_info);
      registration_info.SetKey("active", std::move(active_info));
    }

    if (registration.waiting_version.version_id !=
        blink::mojom::kInvalidServiceWorkerVersionId) {
      base::Value waiting_info(base::Value::Type::DICTIONARY);
      UpdateVersionInfo(registration.waiting_version, &waiting_info);
      registration_info.SetKey("waiting", std::move(waiting_info));
    }

    result.emplace_back(std::move(registration_info));
  }
  return result;
}

base::Value::ListStorage GetVersionListValue(
    const std::vector<ServiceWorkerVersionInfo>& versions) {
  base::Value::ListStorage result;
  for (const auto& version : versions) {
    base::Value info(base::Value::Type::DICTIONARY);
    UpdateVersionInfo(version, &info);
    result.emplace_back(std::move(info));
  }
  return result;
}

void DidGetStoredRegistrationsOnCoreThread(
    scoped_refptr<ServiceWorkerContextWrapper> context,
    GetRegistrationsCallback callback,
    blink::ServiceWorkerStatusCode status,
    const std::vector<ServiceWorkerRegistrationInfo>& stored_registrations) {
  DCHECK_CURRENTLY_ON(ServiceWorkerContext::GetCoreThreadId());
  GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback), context->GetAllLiveRegistrationInfo(),
                     context->GetAllLiveVersionInfo(), stored_registrations));
}

void GetRegistrationsOnCoreThread(
    scoped_refptr<ServiceWorkerContextWrapper> context,
    GetRegistrationsCallback callback) {
  DCHECK_CURRENTLY_ON(ServiceWorkerContext::GetCoreThreadId());
  context->GetAllRegistrations(base::BindOnce(
      DidGetStoredRegistrationsOnCoreThread, context, std::move(callback)));
}

void DidGetRegistrations(
    WeakPtr<ServiceWorkerInternalsHandler> internals,
    int partition_id,
    const base::FilePath& context_path,
    const std::vector<ServiceWorkerRegistrationInfo>& live_registrations,
    const std::vector<ServiceWorkerVersionInfo>& live_versions,
    const std::vector<ServiceWorkerRegistrationInfo>& stored_registrations) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  if (internals) {
    internals->OnDidGetRegistrations(live_registrations, live_versions,
                                     stored_registrations, partition_id,
                                     context_path);
  }
}

// These values are persisted to logs. Entries should not be renumbered and
// numeric values should never be reused.
enum class ServiceWorkerInternalsLinkQuery {
  kDevTools = 0,
  kOther = 1,
  kMaxValue = kOther,
};

}  // namespace

class ServiceWorkerInternalsHandler::PartitionObserver
    : public ServiceWorkerContextCoreObserver {
 public:
  PartitionObserver(int partition_id,
                    WeakPtr<ServiceWorkerInternalsHandler> handler)
      : partition_id_(partition_id), handler_(handler) {}
  ~PartitionObserver() override = default;
  // ServiceWorkerContextCoreObserver overrides:
  void OnStarting(int64_t version_id) override {
    DCHECK_CURRENTLY_ON(BrowserThread::UI);
    if (handler_) {
      handler_->OnRunningStateChanged();
    }
  }
  void OnStarted(int64_t version_id,
                 const GURL& scope,
                 int process_id,
                 const GURL& script_url,
                 const blink::ServiceWorkerToken& token) override {
    DCHECK_CURRENTLY_ON(BrowserThread::UI);
    if (handler_) {
      handler_->OnRunningStateChanged();
    }
  }
  void OnStopping(int64_t version_id) override {
    DCHECK_CURRENTLY_ON(BrowserThread::UI);
    if (handler_) {
      handler_->OnRunningStateChanged();
    }
  }
  void OnStopped(int64_t version_id) override {
    DCHECK_CURRENTLY_ON(BrowserThread::UI);
    if (handler_) {
      handler_->OnRunningStateChanged();
    }
  }
  void OnVersionStateChanged(int64_t version_id,
                             const GURL& scope,
                             ServiceWorkerVersion::Status) override {
    DCHECK_CURRENTLY_ON(BrowserThread::UI);
    if (handler_) {
      handler_->OnVersionStateChanged(partition_id_, version_id);
    }
  }
  void OnErrorReported(
      int64_t version_id,
      const GURL& scope,
      const ServiceWorkerContextObserver::ErrorInfo& info) override {
    DCHECK_CURRENTLY_ON(BrowserThread::UI);
    if (!handler_) {
      return;
    }
    base::Value details(base::Value::Type::DICTIONARY);
    details.SetStringKey("message", info.error_message);
    details.SetIntKey("lineNumber", info.line_number);
    details.SetIntKey("columnNumber", info.column_number);
    details.SetStringKey("sourceURL", info.source_url.spec());

    handler_->OnErrorEvent("error-reported", partition_id_, version_id,
                           std::move(details));
  }
  void OnReportConsoleMessage(int64_t version_id,
                              const GURL& scope,
                              const ConsoleMessage& message) override {
    DCHECK_CURRENTLY_ON(BrowserThread::UI);
    if (!handler_) {
      return;
    }
    base::Value details(base::Value::Type::DICTIONARY);
    details.SetIntKey("sourceIdentifier", static_cast<int>(message.source));
    details.SetIntKey("message_level", static_cast<int>(message.message_level));
    details.SetStringKey("message", message.message);
    details.SetIntKey("lineNumber", message.line_number);
    details.SetStringKey("sourceURL", message.source_url.spec());
    handler_->OnErrorEvent("console-message-reported", partition_id_,
                           version_id, std::move(details));
  }
  void OnRegistrationCompleted(int64_t registration_id,
                               const GURL& scope) override {
    DCHECK_CURRENTLY_ON(BrowserThread::UI);
    if (handler_) {
      handler_->OnRegistrationEvent("registration-completed", scope);
    }
  }
  void OnRegistrationDeleted(int64_t registration_id,
                             const GURL& scope) override {
    DCHECK_CURRENTLY_ON(BrowserThread::UI);
    if (handler_) {
      handler_->OnRegistrationEvent("registration-deleted", scope);
    }
  }
  int partition_id() const { return partition_id_; }

 private:
  const int partition_id_;
  WeakPtr<ServiceWorkerInternalsHandler> handler_;
};

ServiceWorkerInternalsUI::ServiceWorkerInternalsUI(WebUI* web_ui)
    : WebUIController(web_ui) {
  std::string query = web_ui->GetWebContents()->GetURL().query();
  ServiceWorkerInternalsLinkQuery query_type =
      ServiceWorkerInternalsLinkQuery::kOther;
  if (query == "devtools") {
    query_type = ServiceWorkerInternalsLinkQuery::kDevTools;
  }
  base::UmaHistogramEnumeration("ServiceWorker.InternalsPageAccessed",
                                query_type);
  WebUIDataSource* source =
      WebUIDataSource::Create(kChromeUIServiceWorkerInternalsHost);
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ScriptSrc,
      "script-src chrome://resources 'self' 'unsafe-eval';");
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::TrustedTypes,
      "trusted-types jstemplate;");
  source->UseStringsJs();
  source->AddResourcePath("serviceworker_internals.js",
                          IDR_SERVICE_WORKER_INTERNALS_JS);
  source->AddResourcePath("serviceworker_internals.css",
                          IDR_SERVICE_WORKER_INTERNALS_CSS);
  source->SetDefaultResource(IDR_SERVICE_WORKER_INTERNALS_HTML);
  source->DisableDenyXFrameOptions();

  web_ui->AddMessageHandler(std::make_unique<ServiceWorkerInternalsHandler>());
  BrowserContext* browser_context =
      web_ui->GetWebContents()->GetBrowserContext();
  WebUIDataSource::Add(browser_context, source);
}

ServiceWorkerInternalsUI::~ServiceWorkerInternalsUI() = default;

ServiceWorkerInternalsHandler::ServiceWorkerInternalsHandler() = default;

void ServiceWorkerInternalsHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "GetOptions",
      base::BindRepeating(&ServiceWorkerInternalsHandler::HandleGetOptions,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "SetOption",
      base::BindRepeating(&ServiceWorkerInternalsHandler::HandleSetOption,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getAllRegistrations",
      base::BindRepeating(
          &ServiceWorkerInternalsHandler::HandleGetAllRegistrations,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "stop",
      base::BindRepeating(&ServiceWorkerInternalsHandler::HandleStopWorker,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "inspect",
      base::BindRepeating(&ServiceWorkerInternalsHandler::HandleInspectWorker,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "unregister",
      base::BindRepeating(&ServiceWorkerInternalsHandler::HandleUnregister,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "start",
      base::BindRepeating(&ServiceWorkerInternalsHandler::HandleStartWorker,
                          base::Unretained(this)));
}

void ServiceWorkerInternalsHandler::OnJavascriptDisallowed() {
  BrowserContext* browser_context =
      web_ui()->GetWebContents()->GetBrowserContext();
  // Safe to use base::Unretained(this) because
  // ForEachStoragePartition is synchronous.
  BrowserContext::ForEachStoragePartition(
      browser_context,
      base::BindRepeating(
          &ServiceWorkerInternalsHandler::RemoveObserverFromStoragePartition,
          base::Unretained(this)));
  weak_ptr_factory_.InvalidateWeakPtrs();
}

ServiceWorkerInternalsHandler::~ServiceWorkerInternalsHandler() {
  // ServiceWorkerInternalsHandler can be destroyed without
  // OnJavascriptDisallowed() ever being called (https://crbug.com/1199198).
  // Call it to ensure that `this` is removed as an observer.
  OnJavascriptDisallowed();
}

void ServiceWorkerInternalsHandler::OnRunningStateChanged() {
  FireWebUIListener("running-state-changed");
}

void ServiceWorkerInternalsHandler::OnVersionStateChanged(int partition_id,
                                                          int64_t version_id) {
  FireWebUIListener("version-state-changed", base::Value(partition_id),
                    base::Value(base::NumberToString(version_id)));
}

void ServiceWorkerInternalsHandler::OnErrorEvent(const std::string& event_name,
                                                 int partition_id,
                                                 int64_t version_id,
                                                 base::Value details) {
  FireWebUIListener(event_name, base::Value(partition_id),
                    base::Value(base::NumberToString(version_id)), details);
}

void ServiceWorkerInternalsHandler::OnRegistrationEvent(
    const std::string& event_name,
    const GURL& scope) {
  FireWebUIListener(event_name, base::Value(scope.spec()));
}

void ServiceWorkerInternalsHandler::OnDidGetRegistrations(
    const std::vector<ServiceWorkerRegistrationInfo>& live_registrations,
    const std::vector<ServiceWorkerVersionInfo>& live_versions,
    const std::vector<ServiceWorkerRegistrationInfo>& stored_registrations,
    int partition_id,
    const base::FilePath& context_path) {
  base::Value registrations(base::Value::Type::DICTIONARY);
  registrations.SetKey(
      "liveRegistrations",
      base::Value(GetRegistrationListValue(live_registrations)));
  registrations.SetKey("liveVersions",
                       base::Value(GetVersionListValue(live_versions)));
  registrations.SetKey(
      "storedRegistrations",
      base::Value(GetRegistrationListValue(stored_registrations)));
  FireWebUIListener("partition-data", std::move(registrations),
                    base::Value(partition_id),
                    base::Value(context_path.AsUTF8Unsafe()));
}

void ServiceWorkerInternalsHandler::OnOperationComplete(
    int status,
    const std::string& callback_id) {
  ResolveJavascriptCallback(base::Value(callback_id), base::Value(status));
}

void ServiceWorkerInternalsHandler::HandleGetOptions(const ListValue* args) {
  std::string callback_id;
  CHECK(args->GetString(0, &callback_id));
  AllowJavascript();
  DictionaryValue options;
  options.SetBoolean("debug_on_start",
                     ServiceWorkerDevToolsManager::GetInstance()
                         ->debug_service_worker_on_start());
  ResolveJavascriptCallback(base::Value(callback_id), options);
}

void ServiceWorkerInternalsHandler::HandleSetOption(const ListValue* args) {
  std::string option_name;
  bool option_boolean;
  if (!args->GetString(0, &option_name) || option_name != "debug_on_start" ||
      !args->GetBoolean(1, &option_boolean)) {
    return;
  }
  ServiceWorkerDevToolsManager::GetInstance()
      ->set_debug_service_worker_on_start(option_boolean);
}

void ServiceWorkerInternalsHandler::HandleGetAllRegistrations(
    const ListValue* args) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  // Allow Javascript here too, because these messages are sent back to back.
  AllowJavascript();
  BrowserContext* browser_context =
      web_ui()->GetWebContents()->GetBrowserContext();
  // Safe to use base::Unretained(this) because
  // ForEachStoragePartition is synchronous.
  BrowserContext::ForEachStoragePartition(
      browser_context,
      base::BindRepeating(
          &ServiceWorkerInternalsHandler::AddContextFromStoragePartition,
          base::Unretained(this)));
}

void ServiceWorkerInternalsHandler::AddContextFromStoragePartition(
    StoragePartition* partition) {
  int partition_id = 0;
  scoped_refptr<ServiceWorkerContextWrapper> context =
      static_cast<ServiceWorkerContextWrapper*>(
          partition->GetServiceWorkerContext());
  auto it = observers_.find(reinterpret_cast<uintptr_t>(partition));
  if (it != observers_.end()) {
    partition_id = it->second->partition_id();
  } else {
    partition_id = next_partition_id_++;
    auto new_observer = std::make_unique<PartitionObserver>(
        partition_id, weak_ptr_factory_.GetWeakPtr());
    context->AddObserver(new_observer.get());
    observers_[reinterpret_cast<uintptr_t>(partition)] =
        std::move(new_observer);
  }

  RunOrPostTaskOnThread(
      FROM_HERE, ServiceWorkerContext::GetCoreThreadId(),
      base::BindOnce(
          GetRegistrationsOnCoreThread, context,
          base::BindOnce(DidGetRegistrations, weak_ptr_factory_.GetWeakPtr(),
                         partition_id,
                         context->is_incognito() ? base::FilePath()
                                                 : partition->GetPath())));
}

void ServiceWorkerInternalsHandler::RemoveObserverFromStoragePartition(
    StoragePartition* partition) {
  auto it = observers_.find(reinterpret_cast<uintptr_t>(partition));
  if (it == observers_.end())
    return;
  std::unique_ptr<PartitionObserver> observer = std::move(it->second);
  observers_.erase(it);
  scoped_refptr<ServiceWorkerContextWrapper> context =
      static_cast<ServiceWorkerContextWrapper*>(
          partition->GetServiceWorkerContext());
  context->RemoveObserver(observer.get());
}

void ServiceWorkerInternalsHandler::FindContext(
    int partition_id,
    StoragePartition** result_partition,
    StoragePartition* storage_partition) const {
  auto it = observers_.find(reinterpret_cast<uintptr_t>(storage_partition));
  if (it != observers_.end() && partition_id == it->second->partition_id()) {
    *result_partition = storage_partition;
  }
}

bool ServiceWorkerInternalsHandler::GetServiceWorkerContext(
    int partition_id,
    scoped_refptr<ServiceWorkerContextWrapper>* context) const {
  BrowserContext* browser_context =
      web_ui()->GetWebContents()->GetBrowserContext();
  StoragePartition* result_partition(nullptr);
  BrowserContext::ForEachStoragePartition(
      browser_context,
      base::BindRepeating(&ServiceWorkerInternalsHandler::FindContext,
                          base::Unretained(this), partition_id,
                          &result_partition));
  if (!result_partition)
    return false;
  *context = static_cast<ServiceWorkerContextWrapper*>(
      result_partition->GetServiceWorkerContext());
  return true;
}

void ServiceWorkerInternalsHandler::HandleStopWorker(const ListValue* args) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  std::string callback_id;
  const DictionaryValue* cmd_args = nullptr;
  int partition_id;
  scoped_refptr<ServiceWorkerContextWrapper> context;
  std::string version_id_string;
  int64_t version_id = 0;
  if (!args->GetString(0, &callback_id) || !args->GetDictionary(1, &cmd_args) ||
      !cmd_args->GetInteger("partition_id", &partition_id) ||
      !GetServiceWorkerContext(partition_id, &context) ||
      !cmd_args->GetString("version_id", &version_id_string) ||
      !base::StringToInt64(version_id_string, &version_id)) {
    return;
  }

  base::OnceCallback<void(blink::ServiceWorkerStatusCode)> callback =
      base::BindOnce(OperationCompleteCallback, weak_ptr_factory_.GetWeakPtr(),
                     callback_id);
  StopWorkerWithId(context, version_id, std::move(callback));
}

void ServiceWorkerInternalsHandler::HandleInspectWorker(const ListValue* args) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  std::string callback_id;
  const DictionaryValue* cmd_args = nullptr;
  int process_host_id = 0;
  int devtools_agent_route_id = 0;
  if (!args->GetString(0, &callback_id) || !args->GetDictionary(1, &cmd_args) ||
      !cmd_args->GetInteger("process_host_id", &process_host_id) ||
      !cmd_args->GetInteger("devtools_agent_route_id",
                            &devtools_agent_route_id)) {
    return;
  }
  base::OnceCallback<void(blink::ServiceWorkerStatusCode)> callback =
      base::BindOnce(OperationCompleteCallback, weak_ptr_factory_.GetWeakPtr(),
                     callback_id);
  scoped_refptr<ServiceWorkerDevToolsAgentHost> agent_host(
      ServiceWorkerDevToolsManager::GetInstance()
          ->GetDevToolsAgentHostForWorker(process_host_id,
                                          devtools_agent_route_id));
  if (!agent_host.get()) {
    std::move(callback).Run(blink::ServiceWorkerStatusCode::kErrorNotFound);
    return;
  }
  agent_host->Inspect();
  std::move(callback).Run(blink::ServiceWorkerStatusCode::kOk);
}

void ServiceWorkerInternalsHandler::HandleUnregister(const ListValue* args) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  std::string callback_id;
  int partition_id;
  std::string scope_string;
  const DictionaryValue* cmd_args = nullptr;
  scoped_refptr<ServiceWorkerContextWrapper> context;
  if (!args->GetString(0, &callback_id) || !args->GetDictionary(1, &cmd_args) ||
      !cmd_args->GetInteger("partition_id", &partition_id) ||
      !GetServiceWorkerContext(partition_id, &context) ||
      !cmd_args->GetString("scope", &scope_string)) {
    return;
  }

  base::OnceCallback<void(blink::ServiceWorkerStatusCode)> callback =
      base::BindOnce(OperationCompleteCallback, weak_ptr_factory_.GetWeakPtr(),
                     callback_id);
  UnregisterWithScope(context, GURL(scope_string), std::move(callback));
}

void ServiceWorkerInternalsHandler::HandleStartWorker(const ListValue* args) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  std::string callback_id;
  int partition_id;
  std::string scope_string;
  const DictionaryValue* cmd_args = nullptr;
  scoped_refptr<ServiceWorkerContextWrapper> context;
  if (!args->GetString(0, &callback_id) || !args->GetDictionary(1, &cmd_args) ||
      !cmd_args->GetInteger("partition_id", &partition_id) ||
      !GetServiceWorkerContext(partition_id, &context) ||
      !cmd_args->GetString("scope", &scope_string)) {
    return;
  }
  base::OnceCallback<void(blink::ServiceWorkerStatusCode)> callback =
      base::BindOnce(OperationCompleteCallback, weak_ptr_factory_.GetWeakPtr(),
                     callback_id);
  context->StartActiveServiceWorker(GURL(scope_string), std::move(callback));
}

void ServiceWorkerInternalsHandler::StopWorkerWithId(
    scoped_refptr<ServiceWorkerContextWrapper> context,
    int64_t version_id,
    StatusCallback callback) {
  if (!BrowserThread::CurrentlyOn(ServiceWorkerContext::GetCoreThreadId())) {
    BrowserThread::GetTaskRunnerForThread(
        ServiceWorkerContext::GetCoreThreadId())
        ->PostTask(
            FROM_HERE,
            base::BindOnce(&ServiceWorkerInternalsHandler::StopWorkerWithId,
                           base::Unretained(this), context, version_id,
                           std::move(callback)));
    return;
  }

  scoped_refptr<ServiceWorkerVersion> version =
      context->GetLiveVersion(version_id);
  if (!version.get()) {
    std::move(callback).Run(blink::ServiceWorkerStatusCode::kErrorNotFound);
    return;
  }

  // ServiceWorkerVersion::StopWorker() takes a base::OnceClosure for argument,
  // so bind blink::ServiceWorkerStatusCode::kOk to callback here.
  version->StopWorker(
      base::BindOnce(std::move(callback), blink::ServiceWorkerStatusCode::kOk));
}

void ServiceWorkerInternalsHandler::UnregisterWithScope(
    scoped_refptr<ServiceWorkerContextWrapper> context,
    const GURL& scope,
    ServiceWorkerInternalsHandler::StatusCallback callback) const {
  if (!BrowserThread::CurrentlyOn(ServiceWorkerContext::GetCoreThreadId())) {
    BrowserThread::GetTaskRunnerForThread(
        ServiceWorkerContext::GetCoreThreadId())
        ->PostTask(
            FROM_HERE,
            base::BindOnce(&ServiceWorkerInternalsHandler::UnregisterWithScope,
                           base::Unretained(this), context, scope,
                           std::move(callback)));
    return;
  }

  if (!context->context()) {
    std::move(callback).Run(blink::ServiceWorkerStatusCode::kErrorAbort);
    return;
  }

  // ServiceWorkerContextWrapper::UnregisterServiceWorker doesn't work here
  // because that reduces a status code to boolean.
  context->context()->UnregisterServiceWorker(
      scope, /*is_immediate=*/false,
      base::AdaptCallbackForRepeating(std::move(callback)));
}

}  // namespace content
