// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/browser/api/storage/storage_frontend.h"

#include <memory>
#include <utility>

#include "base/bind.h"
#include "base/callback_helpers.h"
#include "base/files/file_path.h"
#include "base/json/json_reader.h"
#include "base/lazy_instance.h"
#include "base/memory/ptr_util.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/trace_event/trace_event.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "extensions/browser/api/extensions_api_client.h"
#include "extensions/browser/api/storage/backend_task_runner.h"
#include "extensions/browser/api/storage/local_value_store_cache.h"
#include "extensions/browser/event_router.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/value_store/value_store_factory.h"
#include "extensions/common/api/storage.h"

using content::BrowserContext;
using content::BrowserThread;

namespace extensions {

namespace {

base::LazyInstance<BrowserContextKeyedAPIFactory<StorageFrontend>>::
    DestructorAtExit g_factory = LAZY_INSTANCE_INITIALIZER;

events::HistogramValue NamespaceToEventHistogram(
    settings_namespace::Namespace settings_namespace) {
  switch (settings_namespace) {
    case settings_namespace::LOCAL:
      return events::STORAGE_LOCAL_ON_CHANGE;
    case settings_namespace::SYNC:
      return events::STORAGE_SYNC_ON_CHANGE;
    case settings_namespace::MANAGED:
      return events::STORAGE_MANAGED_ON_CHANGE;
    case settings_namespace::INVALID:
      break;
  }
  NOTREACHED();
  return events::UNKNOWN;
}

// Settings change Observer which forwards changes on to the extension
// processes for |context| and its incognito partner if it exists.
class DefaultObserver : public SettingsObserver {
 public:
  explicit DefaultObserver(BrowserContext* context)
      : browser_context_(context) {}

  // SettingsObserver implementation.
  void OnSettingsChanged(const std::string& extension_id,
                         settings_namespace::Namespace settings_namespace,
                         const base::Value& changes) override {
    TRACE_EVENT1("browser", "SettingsObserver:OnSettingsChanged",
                 "extension_id", extension_id);

    // Alias extension_id for investigation of shutdown hangs. crbug.com/1154997
    // Extension IDs are exactly 32 characters in length.
    constexpr size_t kExtensionsIdLength = 32;
    char extension_id_str[kExtensionsIdLength + 1];
    base::strlcpy(extension_id_str, extension_id.c_str(),
                  base::size(extension_id_str));
    base::debug::Alias(extension_id_str);

    const std::string namespace_string =
        settings_namespace::ToString(settings_namespace);
    EventRouter* event_router = EventRouter::Get(browser_context_);

    // We only dispatch the events if there's a valid listener (even though
    // EventRouter would handle the no-listener case) since copying `changes`
    // can be expensive.
    // Event for each storage(sync, local, managed).
    if (event_router->ExtensionHasEventListener(
            extension_id, api::storage::OnChanged::kEventName)) {
      std::unique_ptr<base::ListValue> args(new base::ListValue());
      args->Append(changes.Clone());
      args->AppendString(namespace_string);
      std::unique_ptr<Event> event(
          new Event(events::STORAGE_ON_CHANGED,
                    api::storage::OnChanged::kEventName, std::move(args)));
      event_router->DispatchEventToExtension(extension_id, std::move(event));
    }

    // Event for StorageArea.
    auto area_event_name =
        base::StringPrintf("storage.%s.onChanged", namespace_string.c_str());
    if (event_router->ExtensionHasEventListener(extension_id,
                                                area_event_name)) {
      auto args = std::make_unique<base::ListValue>();
      args->Append(changes.Clone());
      auto event =
          std::make_unique<Event>(NamespaceToEventHistogram(settings_namespace),
                                  area_event_name, std::move(args));
      event_router->DispatchEventToExtension(extension_id, std::move(event));
    }
  }

 private:
  BrowserContext* const browser_context_;
};

}  // namespace

// static
StorageFrontend* StorageFrontend::Get(BrowserContext* context) {
  return BrowserContextKeyedAPIFactory<StorageFrontend>::Get(context);
}

// static
std::unique_ptr<StorageFrontend> StorageFrontend::CreateForTesting(
    scoped_refptr<ValueStoreFactory> storage_factory,
    BrowserContext* context) {
  return base::WrapUnique(
      new StorageFrontend(std::move(storage_factory), context));
}

StorageFrontend::StorageFrontend(BrowserContext* context)
    : StorageFrontend(ExtensionSystem::Get(context)->store_factory(), context) {
}

StorageFrontend::StorageFrontend(scoped_refptr<ValueStoreFactory> factory,
                                 BrowserContext* context)
    : browser_context_(context) {
  Init(std::move(factory));
}

void StorageFrontend::Init(scoped_refptr<ValueStoreFactory> factory) {
  TRACE_EVENT0("browser,startup", "StorageFrontend::Init");

  observers_ = new SettingsObserverList();
  browser_context_observer_ =
      std::make_unique<DefaultObserver>(browser_context_);
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  DCHECK(!browser_context_->IsOffTheRecord());

  observers_->AddObserver(browser_context_observer_.get());

  caches_[settings_namespace::LOCAL] = new LocalValueStoreCache(factory);

  // Add any additional caches the embedder supports (for example, caches
  // for chrome.storage.managed and chrome.storage.sync).
  ExtensionsAPIClient::Get()->AddAdditionalValueStoreCaches(
      browser_context_, factory, observers_, &caches_);
}

StorageFrontend::~StorageFrontend() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  observers_->RemoveObserver(browser_context_observer_.get());
  for (auto it = caches_.begin(); it != caches_.end(); ++it) {
    ValueStoreCache* cache = it->second;
    cache->ShutdownOnUI();
    GetBackendTaskRunner()->DeleteSoon(FROM_HERE, cache);
  }
}

ValueStoreCache* StorageFrontend::GetValueStoreCache(
    settings_namespace::Namespace settings_namespace) const {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  auto it = caches_.find(settings_namespace);
  if (it != caches_.end())
    return it->second;
  return NULL;
}

bool StorageFrontend::IsStorageEnabled(
    settings_namespace::Namespace settings_namespace) const {
  return caches_.find(settings_namespace) != caches_.end();
}

void StorageFrontend::RunWithStorage(
    scoped_refptr<const Extension> extension,
    settings_namespace::Namespace settings_namespace,
    ValueStoreCache::StorageCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  CHECK(extension.get());

  ValueStoreCache* cache = caches_[settings_namespace];
  CHECK(cache);

  GetBackendTaskRunner()->PostTask(
      FROM_HERE,
      base::BindOnce(&ValueStoreCache::RunWithValueStoreForExtension,
                     base::Unretained(cache), std::move(callback), extension));
}

void StorageFrontend::DeleteStorageSoon(const std::string& extension_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  for (auto it = caches_.begin(); it != caches_.end(); ++it) {
    ValueStoreCache* cache = it->second;
    GetBackendTaskRunner()->PostTask(
        FROM_HERE, base::BindOnce(&ValueStoreCache::DeleteStorageSoon,
                                  base::Unretained(cache), extension_id));
  }
}

scoped_refptr<SettingsObserverList> StorageFrontend::GetObservers() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  return observers_;
}

void StorageFrontend::DisableStorageForTesting(
    settings_namespace::Namespace settings_namespace) {
  auto it = caches_.find(settings_namespace);
  if (it != caches_.end()) {
    ValueStoreCache* cache = it->second;
    cache->ShutdownOnUI();
    GetBackendTaskRunner()->DeleteSoon(FROM_HERE, cache);
    caches_.erase(it);
  }
}

// BrowserContextKeyedAPI implementation.

// static
BrowserContextKeyedAPIFactory<StorageFrontend>*
StorageFrontend::GetFactoryInstance() {
  return g_factory.Pointer();
}

// static
const char* StorageFrontend::service_name() { return "StorageFrontend"; }

}  // namespace extensions
