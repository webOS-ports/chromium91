// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/services/storage/service_worker/service_worker_storage.h"

#include <stddef.h>

#include <memory>
#include <utility>

#include "base/callback_helpers.h"
#include "base/files/file_util.h"
#include "base/memory/ptr_util.h"
#include "base/metrics/histogram_functions.h"
#include "base/run_loop.h"
#include "base/sequenced_task_runner.h"
#include "base/stl_util.h"
#include "base/task/post_task.h"
#include "base/task/thread_pool.h"
#include "base/task_runner_util.h"
#include "base/trace_event/trace_event.h"
#include "components/services/storage/public/cpp/constants.h"
#include "components/services/storage/service_worker/service_worker_disk_cache.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "net/base/completion_once_callback.h"
#include "net/base/net_errors.h"
#include "third_party/blink/public/common/service_worker/service_worker_scope_match.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_object.mojom.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_registration.mojom.h"

namespace storage {

namespace {

void RunSoon(const base::Location& from_here, base::OnceClosure closure) {
  base::ThreadTaskRunnerHandle::Get()->PostTask(from_here, std::move(closure));
}

const base::FilePath::CharType kDatabaseName[] = FILE_PATH_LITERAL("Database");
const base::FilePath::CharType kDiskCacheName[] =
    FILE_PATH_LITERAL("ScriptCache");

// Used for UMA. Append-only.
enum class DeleteAndStartOverResult {
  kDeleteOk = 0,
  kDeleteDatabaseError = 1,
  kDeleteDiskCacheError = 2,
  kMaxValue = kDeleteDiskCacheError,
};

void RecordDeleteAndStartOverResult(DeleteAndStartOverResult result) {
  base::UmaHistogramEnumeration(
      "ServiceWorker.Storage.DeleteAndStartOverResult", result);
}

}  // namespace

ServiceWorkerStorage::InitialData::InitialData()
    : next_registration_id(blink::mojom::kInvalidServiceWorkerRegistrationId),
      next_version_id(blink::mojom::kInvalidServiceWorkerVersionId),
      next_resource_id(blink::mojom::kInvalidServiceWorkerResourceId) {}

ServiceWorkerStorage::InitialData::~InitialData() = default;

ServiceWorkerStorage::DidDeleteRegistrationParams::DidDeleteRegistrationParams(
    int64_t registration_id,
    GURL origin,
    DeleteRegistrationCallback callback)
    : registration_id(registration_id),
      origin(origin),
      callback(std::move(callback)) {}

ServiceWorkerStorage::DidDeleteRegistrationParams::
    ~DidDeleteRegistrationParams() {}

ServiceWorkerStorage::~ServiceWorkerStorage() {
  ClearSessionOnlyOrigins();
  weak_factory_.InvalidateWeakPtrs();
  database_task_runner_->DeleteSoon(FROM_HERE, std::move(database_));
}

// static
std::unique_ptr<ServiceWorkerStorage> ServiceWorkerStorage::Create(
    const base::FilePath& user_data_directory,
    scoped_refptr<base::SequencedTaskRunner> database_task_runner) {
  return base::WrapUnique(new ServiceWorkerStorage(
      user_data_directory, std::move(database_task_runner)));
}

void ServiceWorkerStorage::GetRegisteredOrigins(
    GetRegisteredOriginsCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run(/*origins=*/std::vector<url::Origin>());
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(&ServiceWorkerStorage::GetRegisteredOrigins,
                                    weak_factory_.GetWeakPtr(),
                                    std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  std::vector<url::Origin> origins;
  for (const auto& origin : registered_origins_)
    origins.push_back(origin);
  std::move(callback).Run(std::move(origins));
}

void ServiceWorkerStorage::FindRegistrationForClientUrl(
    const GURL& client_url,
    FindRegistrationDataCallback callback) {
  DCHECK(!client_url.has_ref());
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run(
          /*data=*/nullptr, /*resources=*/nullptr,
          ServiceWorkerDatabase::Status::kErrorDisabled);
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(
          &ServiceWorkerStorage::FindRegistrationForClientUrl,
          weak_factory_.GetWeakPtr(), client_url, std::move(callback)));
      TRACE_EVENT_INSTANT1(
          "ServiceWorker",
          "ServiceWorkerStorage::FindRegistrationForClientUrl:LazyInitialize",
          TRACE_EVENT_SCOPE_THREAD, "URL", client_url.spec());
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  // Bypass database lookup when there is no stored registration.
  if (!base::Contains(registered_origins_, url::Origin::Create(client_url))) {
    std::move(callback).Run(
        /*data=*/nullptr, /*resources=*/nullptr,
        ServiceWorkerDatabase::Status::kErrorNotFound);
    return;
  }

  database_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&FindForClientUrlInDB, database_.get(),
                                base::ThreadTaskRunnerHandle::Get(), client_url,
                                std::move(callback)));
}

void ServiceWorkerStorage::FindRegistrationForScope(
    const GURL& scope,
    FindRegistrationDataCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             /*data=*/nullptr, /*resources=*/nullptr,
                             ServiceWorkerDatabase::Status::kErrorDisabled));
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(
          &ServiceWorkerStorage::FindRegistrationForScope,
          weak_factory_.GetWeakPtr(), scope, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  // Bypass database lookup when there is no stored registration.
  if (!base::Contains(registered_origins_, url::Origin::Create(scope))) {
    RunSoon(FROM_HERE,
            base::BindOnce(std::move(callback),
                           /*data=*/nullptr, /*resources=*/nullptr,
                           ServiceWorkerDatabase::Status::kErrorNotFound));
    return;
  }

  database_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&FindForScopeInDB, database_.get(),
                                base::ThreadTaskRunnerHandle::Get(), scope,
                                std::move(callback)));
}

void ServiceWorkerStorage::FindRegistrationForId(
    int64_t registration_id,
    const url::Origin& origin,
    FindRegistrationDataCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run(
          /*data=*/nullptr, /*resources=*/nullptr,
          ServiceWorkerDatabase::Status::kErrorDisabled);
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(
          base::BindOnce(&ServiceWorkerStorage::FindRegistrationForId,
                         weak_factory_.GetWeakPtr(), registration_id, origin,
                         std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  // Bypass database lookup when there is no stored registration.
  if (!base::Contains(registered_origins_, origin)) {
    std::move(callback).Run(
        /*data=*/nullptr, /*resources=*/nullptr,
        ServiceWorkerDatabase::Status::kErrorNotFound);
    return;
  }

  database_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&FindForIdInDB, database_.get(),
                                base::ThreadTaskRunnerHandle::Get(),
                                registration_id, origin, std::move(callback)));
}

void ServiceWorkerStorage::FindRegistrationForIdOnly(
    int64_t registration_id,
    FindRegistrationDataCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run(
          /*data=*/nullptr, /*resources=*/nullptr,
          ServiceWorkerDatabase::Status::kErrorDisabled);
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(
          &ServiceWorkerStorage::FindRegistrationForIdOnly,
          weak_factory_.GetWeakPtr(), registration_id, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  database_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&FindForIdOnlyInDB, database_.get(),
                                base::ThreadTaskRunnerHandle::Get(),
                                registration_id, std::move(callback)));
}

void ServiceWorkerStorage::GetRegistrationsForOrigin(
    const url::Origin& origin,
    GetRegistrationsDataCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             ServiceWorkerDatabase::Status::kErrorDisabled,
                             /*registrations=*/nullptr,
                             /*resource_lists=*/nullptr));
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(
          &ServiceWorkerStorage::GetRegistrationsForOrigin,
          weak_factory_.GetWeakPtr(), origin, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  auto registrations = std::make_unique<RegistrationList>();
  auto resource_lists = std::make_unique<std::vector<ResourceList>>();
  RegistrationList* registrations_ptr = registrations.get();
  std::vector<ResourceList>* resource_lists_ptr = resource_lists.get();

  base::PostTaskAndReplyWithResult(
      database_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ServiceWorkerDatabase::GetRegistrationsForOrigin,
                     base::Unretained(database_.get()), origin,
                     registrations_ptr, resource_lists_ptr),
      base::BindOnce(&ServiceWorkerStorage::DidGetRegistrationsForOrigin,
                     weak_factory_.GetWeakPtr(), std::move(callback),
                     std::move(registrations), std::move(resource_lists)));
}

void ServiceWorkerStorage::GetUsageForOrigin(
    const url::Origin& origin,
    GetUsageForOriginCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             ServiceWorkerDatabase::Status::kErrorDisabled,
                             /*usage=*/0));
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(&ServiceWorkerStorage::GetUsageForOrigin,
                                    weak_factory_.GetWeakPtr(), origin,
                                    std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  database_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&ServiceWorkerStorage::GetUsageForOriginInDB,
                     database_.get(), base::ThreadTaskRunnerHandle::Get(),
                     origin, std::move(callback)));
}

void ServiceWorkerStorage::GetAllRegistrations(
    GetAllRegistrationsCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             ServiceWorkerDatabase::Status::kErrorDisabled,
                             /*registrations=*/nullptr));
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(&ServiceWorkerStorage::GetAllRegistrations,
                                    weak_factory_.GetWeakPtr(),
                                    std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  auto registrations = std::make_unique<RegistrationList>();
  RegistrationList* registrations_ptr = registrations.get();

  base::PostTaskAndReplyWithResult(
      database_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ServiceWorkerDatabase::GetAllRegistrations,
                     base::Unretained(database_.get()), registrations_ptr),
      base::BindOnce(&ServiceWorkerStorage::DidGetAllRegistrations,
                     weak_factory_.GetWeakPtr(), std::move(callback),
                     std::move(registrations)));
}

void ServiceWorkerStorage::StoreRegistrationData(
    mojom::ServiceWorkerRegistrationDataPtr registration_data,
    ResourceList resources,
    StoreRegistrationDataCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run(
          ServiceWorkerDatabase::Status::kErrorDisabled,
          /*deleted_version=*/blink::mojom::kInvalidServiceWorkerVersionId,
          /*deleted_resources_size=*/0,
          /*newly_purgeable_resources=*/{});
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(
          &ServiceWorkerStorage::StoreRegistrationData,
          weak_factory_.GetWeakPtr(), std::move(registration_data),
          std::move(resources), std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  if (!has_checked_for_stale_resources_)
    DeleteStaleResources();

  uint64_t resources_total_size_bytes =
      registration_data->resources_total_size_bytes;
  database_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &WriteRegistrationInDB, database_.get(),
          base::ThreadTaskRunnerHandle::Get(), std::move(registration_data),
          std::move(resources),
          base::BindOnce(&ServiceWorkerStorage::DidStoreRegistrationData,
                         weak_factory_.GetWeakPtr(), std::move(callback),
                         resources_total_size_bytes)));
}

void ServiceWorkerStorage::UpdateToActiveState(
    int64_t registration_id,
    const GURL& origin,
    DatabaseStatusCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run(ServiceWorkerDatabase::Status::kErrorDisabled);
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(&ServiceWorkerStorage::UpdateToActiveState,
                                    weak_factory_.GetWeakPtr(), registration_id,
                                    origin, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  base::PostTaskAndReplyWithResult(
      database_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ServiceWorkerDatabase::UpdateVersionToActive,
                     base::Unretained(database_.get()), registration_id,
                     origin),
      std::move(callback));
}

void ServiceWorkerStorage::UpdateLastUpdateCheckTime(
    int64_t registration_id,
    const GURL& origin,
    base::Time last_update_check_time,
    DatabaseStatusCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run(ServiceWorkerDatabase::Status::kErrorDisabled);
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(
          base::BindOnce(&ServiceWorkerStorage::UpdateLastUpdateCheckTime,
                         weak_factory_.GetWeakPtr(), registration_id, origin,
                         last_update_check_time, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  base::PostTaskAndReplyWithResult(
      database_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ServiceWorkerDatabase::UpdateLastCheckTime,
                     base::Unretained(database_.get()), registration_id, origin,
                     last_update_check_time),
      std::move(callback));
}

void ServiceWorkerStorage::UpdateNavigationPreloadEnabled(
    int64_t registration_id,
    const GURL& origin,
    bool enable,
    DatabaseStatusCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run(ServiceWorkerDatabase::Status::kErrorDisabled);
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(
          base::BindOnce(&ServiceWorkerStorage::UpdateNavigationPreloadEnabled,
                         weak_factory_.GetWeakPtr(), registration_id, origin,
                         enable, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  base::PostTaskAndReplyWithResult(
      database_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ServiceWorkerDatabase::UpdateNavigationPreloadEnabled,
                     base::Unretained(database_.get()), registration_id, origin,
                     enable),
      std::move(callback));
}

void ServiceWorkerStorage::UpdateNavigationPreloadHeader(
    int64_t registration_id,
    const GURL& origin,
    const std::string& value,
    DatabaseStatusCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run(ServiceWorkerDatabase::Status::kErrorDisabled);
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(
          base::BindOnce(&ServiceWorkerStorage::UpdateNavigationPreloadHeader,
                         weak_factory_.GetWeakPtr(), registration_id, origin,
                         value, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  base::PostTaskAndReplyWithResult(
      database_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ServiceWorkerDatabase::UpdateNavigationPreloadHeader,
                     base::Unretained(database_.get()), registration_id, origin,
                     value),
      std::move(callback));
}

void ServiceWorkerStorage::DeleteRegistration(
    int64_t registration_id,
    const GURL& origin,
    DeleteRegistrationCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run(
          ServiceWorkerDatabase::Status::kErrorDisabled,
          mojom::ServiceWorkerStorageOriginState::kKeep,
          /*deleted_version_id=*/blink::mojom::kInvalidServiceWorkerVersionId,
          /*deleted_resources_size=*/0,
          /*newly_purgeable_resources=*/std::vector<int64_t>());
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(&ServiceWorkerStorage::DeleteRegistration,
                                    weak_factory_.GetWeakPtr(), registration_id,
                                    origin, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  if (!has_checked_for_stale_resources_)
    DeleteStaleResources();

  auto params = std::make_unique<DidDeleteRegistrationParams>(
      registration_id, origin, std::move(callback));

  database_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &DeleteRegistrationFromDB, database_.get(),
          base::ThreadTaskRunnerHandle::Get(), registration_id, origin,
          base::BindOnce(&ServiceWorkerStorage::DidDeleteRegistration,
                         weak_factory_.GetWeakPtr(), std::move(params))));
}

void ServiceWorkerStorage::PerformStorageCleanup(base::OnceClosure callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run();
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(
          base::BindOnce(&ServiceWorkerStorage::PerformStorageCleanup,
                         weak_factory_.GetWeakPtr(), std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  if (!has_checked_for_stale_resources_)
    DeleteStaleResources();

  database_task_runner_->PostTaskAndReply(
      FROM_HERE, base::BindOnce(&PerformStorageCleanupInDB, database_.get()),
      std::move(callback));
}

void ServiceWorkerStorage::CreateResourceReader(
    int64_t resource_id,
    mojo::PendingReceiver<mojom::ServiceWorkerResourceReader> receiver) {
  DCHECK_NE(resource_id, blink::mojom::kInvalidServiceWorkerResourceId);
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      return;
    case STORAGE_STATE_INITIALIZING:
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(&ServiceWorkerStorage::CreateResourceReader,
                                    weak_factory_.GetWeakPtr(), resource_id,
                                    std::move(receiver)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  uint64_t resource_operation_id = GetNextResourceOperationId();
  DCHECK(!base::Contains(resource_readers_, resource_operation_id));
  resource_readers_[resource_operation_id] =
      std::make_unique<ServiceWorkerResourceReaderImpl>(
          resource_id, disk_cache()->GetWeakPtr(), std::move(receiver),
          base::BindOnce(&ServiceWorkerStorage::OnResourceReaderDisconnected,
                         weak_factory_.GetWeakPtr(), resource_operation_id));
}

void ServiceWorkerStorage::CreateResourceWriter(
    int64_t resource_id,
    mojo::PendingReceiver<mojom::ServiceWorkerResourceWriter> receiver) {
  DCHECK_NE(resource_id, blink::mojom::kInvalidServiceWorkerResourceId);
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      return;
    case STORAGE_STATE_INITIALIZING:
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(&ServiceWorkerStorage::CreateResourceWriter,
                                    weak_factory_.GetWeakPtr(), resource_id,
                                    std::move(receiver)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  uint64_t resource_operation_id = GetNextResourceOperationId();
  DCHECK(!base::Contains(resource_writers_, resource_operation_id));
  resource_writers_[resource_operation_id] =
      std::make_unique<ServiceWorkerResourceWriterImpl>(
          resource_id, disk_cache()->GetWeakPtr(), std::move(receiver),
          base::BindOnce(&ServiceWorkerStorage::OnResourceWriterDisconnected,
                         weak_factory_.GetWeakPtr(), resource_operation_id));
}

void ServiceWorkerStorage::CreateResourceMetadataWriter(
    int64_t resource_id,
    mojo::PendingReceiver<mojom::ServiceWorkerResourceMetadataWriter>
        receiver) {
  DCHECK_NE(resource_id, blink::mojom::kInvalidServiceWorkerResourceId);
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      return;
    case STORAGE_STATE_INITIALIZING:
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(
          &ServiceWorkerStorage::CreateResourceMetadataWriter,
          weak_factory_.GetWeakPtr(), resource_id, std::move(receiver)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  uint64_t resource_operation_id = GetNextResourceOperationId();
  DCHECK(!base::Contains(resource_metadata_writers_, resource_operation_id));
  resource_metadata_writers_[resource_operation_id] =
      std::make_unique<ServiceWorkerResourceMetadataWriterImpl>(
          resource_id, disk_cache()->GetWeakPtr(), std::move(receiver),
          base::BindOnce(
              &ServiceWorkerStorage::OnResourceMetadataWriterDisconnected,
              weak_factory_.GetWeakPtr(), resource_operation_id));
}

void ServiceWorkerStorage::StoreUncommittedResourceId(
    int64_t resource_id,
    DatabaseStatusCallback callback) {
  DCHECK_NE(blink::mojom::kInvalidServiceWorkerResourceId, resource_id);
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run(ServiceWorkerDatabase::Status::kErrorDisabled);
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(
          &ServiceWorkerStorage::StoreUncommittedResourceId,
          weak_factory_.GetWeakPtr(), resource_id, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  if (!has_checked_for_stale_resources_)
    DeleteStaleResources();

  std::vector<int64_t> resource_ids = {resource_id};
  base::PostTaskAndReplyWithResult(
      database_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ServiceWorkerDatabase::WriteUncommittedResourceIds,
                     base::Unretained(database_.get()), resource_ids),
      std::move(callback));
}

void ServiceWorkerStorage::DoomUncommittedResources(
    const std::vector<int64_t>& resource_ids,
    DatabaseStatusCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run(ServiceWorkerDatabase::Status::kErrorDisabled);
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(
          &ServiceWorkerStorage::DoomUncommittedResources,
          weak_factory_.GetWeakPtr(), resource_ids, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  base::PostTaskAndReplyWithResult(
      database_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ServiceWorkerDatabase::PurgeUncommittedResourceIds,
                     base::Unretained(database_.get()), resource_ids),
      base::BindOnce(&ServiceWorkerStorage::DidDoomUncommittedResourceIds,
                     weak_factory_.GetWeakPtr(), resource_ids,
                     std::move(callback)));
}

void ServiceWorkerStorage::StoreUserData(
    int64_t registration_id,
    const url::Origin& origin,
    std::vector<mojom::ServiceWorkerUserDataPtr> user_data,
    DatabaseStatusCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             ServiceWorkerDatabase::Status::kErrorDisabled));
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(
          &ServiceWorkerStorage::StoreUserData, weak_factory_.GetWeakPtr(),
          registration_id, origin, std::move(user_data), std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  if (registration_id == blink::mojom::kInvalidServiceWorkerRegistrationId ||
      user_data.empty()) {
    RunSoon(FROM_HERE,
            base::BindOnce(std::move(callback),
                           ServiceWorkerDatabase::Status::kErrorFailed));
    return;
  }

  for (const auto& entry : user_data) {
    if (entry->key.empty()) {
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             ServiceWorkerDatabase::Status::kErrorFailed));
      return;
    }
  }

  base::PostTaskAndReplyWithResult(
      database_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ServiceWorkerDatabase::WriteUserData,
                     base::Unretained(database_.get()), registration_id, origin,
                     std::move(user_data)),
      std::move(callback));
}

void ServiceWorkerStorage::GetUserData(int64_t registration_id,
                                       const std::vector<std::string>& keys,
                                       GetUserDataInDBCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             ServiceWorkerDatabase::Status::kErrorDisabled,
                             std::vector<std::string>()));
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(&ServiceWorkerStorage::GetUserData,
                                    weak_factory_.GetWeakPtr(), registration_id,
                                    keys, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  if (registration_id == blink::mojom::kInvalidServiceWorkerRegistrationId ||
      keys.empty()) {
    RunSoon(FROM_HERE,
            base::BindOnce(std::move(callback),
                           ServiceWorkerDatabase::Status::kErrorFailed,
                           std::vector<std::string>()));
    return;
  }

  for (const std::string& key : keys) {
    if (key.empty()) {
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             ServiceWorkerDatabase::Status::kErrorFailed,
                             std::vector<std::string>()));
      return;
    }
  }

  database_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&ServiceWorkerStorage::GetUserDataInDB, database_.get(),
                     base::ThreadTaskRunnerHandle::Get(), registration_id, keys,
                     std::move(callback)));
}

void ServiceWorkerStorage::GetUserDataByKeyPrefix(
    int64_t registration_id,
    const std::string& key_prefix,
    GetUserDataInDBCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             ServiceWorkerDatabase::Status::kErrorDisabled,
                             std::vector<std::string>()));
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(
          base::BindOnce(&ServiceWorkerStorage::GetUserDataByKeyPrefix,
                         weak_factory_.GetWeakPtr(), registration_id,
                         key_prefix, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  if (registration_id == blink::mojom::kInvalidServiceWorkerRegistrationId ||
      key_prefix.empty()) {
    RunSoon(FROM_HERE,
            base::BindOnce(std::move(callback),
                           ServiceWorkerDatabase::Status::kErrorFailed,
                           std::vector<std::string>()));
    return;
  }

  database_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&ServiceWorkerStorage::GetUserDataByKeyPrefixInDB,
                     database_.get(), base::ThreadTaskRunnerHandle::Get(),
                     registration_id, key_prefix, std::move(callback)));
}

void ServiceWorkerStorage::GetUserKeysAndDataByKeyPrefix(
    int64_t registration_id,
    const std::string& key_prefix,
    GetUserKeysAndDataInDBCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             ServiceWorkerDatabase::Status::kErrorDisabled,
                             base::flat_map<std::string, std::string>()));
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(
          base::BindOnce(&ServiceWorkerStorage::GetUserKeysAndDataByKeyPrefix,
                         weak_factory_.GetWeakPtr(), registration_id,
                         key_prefix, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  if (registration_id == blink::mojom::kInvalidServiceWorkerRegistrationId ||
      key_prefix.empty()) {
    RunSoon(FROM_HERE,
            base::BindOnce(std::move(callback),
                           ServiceWorkerDatabase::Status::kErrorFailed,
                           base::flat_map<std::string, std::string>()));
    return;
  }

  database_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&ServiceWorkerStorage::GetUserKeysAndDataByKeyPrefixInDB,
                     database_.get(), base::ThreadTaskRunnerHandle::Get(),
                     registration_id, key_prefix, std::move(callback)));
}

void ServiceWorkerStorage::ClearUserData(int64_t registration_id,
                                         const std::vector<std::string>& keys,
                                         DatabaseStatusCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             ServiceWorkerDatabase::Status::kErrorDisabled));
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(&ServiceWorkerStorage::ClearUserData,
                                    weak_factory_.GetWeakPtr(), registration_id,
                                    keys, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  if (registration_id == blink::mojom::kInvalidServiceWorkerRegistrationId ||
      keys.empty()) {
    RunSoon(FROM_HERE,
            base::BindOnce(std::move(callback),
                           ServiceWorkerDatabase::Status::kErrorFailed));
    return;
  }

  for (const std::string& key : keys) {
    if (key.empty()) {
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             ServiceWorkerDatabase::Status::kErrorFailed));
      return;
    }
  }

  base::PostTaskAndReplyWithResult(
      database_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ServiceWorkerDatabase::DeleteUserData,
                     base::Unretained(database_.get()), registration_id, keys),
      std::move(callback));
}

void ServiceWorkerStorage::ClearUserDataByKeyPrefixes(
    int64_t registration_id,
    const std::vector<std::string>& key_prefixes,
    DatabaseStatusCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             ServiceWorkerDatabase::Status::kErrorDisabled));
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(
          base::BindOnce(&ServiceWorkerStorage::ClearUserDataByKeyPrefixes,
                         weak_factory_.GetWeakPtr(), registration_id,
                         key_prefixes, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  if (registration_id == blink::mojom::kInvalidServiceWorkerRegistrationId ||
      key_prefixes.empty()) {
    RunSoon(FROM_HERE,
            base::BindOnce(std::move(callback),
                           ServiceWorkerDatabase::Status::kErrorFailed));
    return;
  }

  for (const std::string& key_prefix : key_prefixes) {
    if (key_prefix.empty()) {
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             ServiceWorkerDatabase::Status::kErrorFailed));
      return;
    }
  }

  base::PostTaskAndReplyWithResult(
      database_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ServiceWorkerDatabase::DeleteUserDataByKeyPrefixes,
                     base::Unretained(database_.get()), registration_id,
                     key_prefixes),
      std::move(callback));
}

void ServiceWorkerStorage::GetUserDataForAllRegistrations(
    const std::string& key,
    GetUserDataForAllRegistrationsInDBCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             ServiceWorkerDatabase::Status::kErrorDisabled,
                             std::vector<mojom::ServiceWorkerUserDataPtr>()));
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(
          base::BindOnce(&ServiceWorkerStorage::GetUserDataForAllRegistrations,
                         weak_factory_.GetWeakPtr(), key, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  if (key.empty()) {
    RunSoon(FROM_HERE,
            base::BindOnce(std::move(callback),
                           ServiceWorkerDatabase::Status::kErrorFailed,
                           std::vector<mojom::ServiceWorkerUserDataPtr>()));
    return;
  }

  database_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&ServiceWorkerStorage::GetUserDataForAllRegistrationsInDB,
                     database_.get(), base::ThreadTaskRunnerHandle::Get(), key,
                     std::move(callback)));
}

void ServiceWorkerStorage::GetUserDataForAllRegistrationsByKeyPrefix(
    const std::string& key_prefix,
    GetUserDataForAllRegistrationsInDBCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             ServiceWorkerDatabase::Status::kErrorDisabled,
                             std::vector<mojom::ServiceWorkerUserDataPtr>()));
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(
          &ServiceWorkerStorage::GetUserDataForAllRegistrationsByKeyPrefix,
          weak_factory_.GetWeakPtr(), key_prefix, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  if (key_prefix.empty()) {
    RunSoon(FROM_HERE,
            base::BindOnce(std::move(callback),
                           ServiceWorkerDatabase::Status::kErrorFailed,
                           std::vector<mojom::ServiceWorkerUserDataPtr>()));
    return;
  }

  database_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &ServiceWorkerStorage::GetUserDataForAllRegistrationsByKeyPrefixInDB,
          database_.get(), base::ThreadTaskRunnerHandle::Get(), key_prefix,
          std::move(callback)));
}

void ServiceWorkerStorage::ClearUserDataForAllRegistrationsByKeyPrefix(
    const std::string& key_prefix,
    DatabaseStatusCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      RunSoon(FROM_HERE,
              base::BindOnce(std::move(callback),
                             ServiceWorkerDatabase::Status::kErrorDisabled));
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(
          &ServiceWorkerStorage::ClearUserDataForAllRegistrationsByKeyPrefix,
          weak_factory_.GetWeakPtr(), key_prefix, std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  if (key_prefix.empty()) {
    RunSoon(FROM_HERE,
            base::BindOnce(std::move(callback),
                           ServiceWorkerDatabase::Status::kErrorFailed));
    return;
  }

  base::PostTaskAndReplyWithResult(
      database_task_runner_.get(), FROM_HERE,
      base::BindOnce(
          &ServiceWorkerDatabase::DeleteUserDataForAllRegistrationsByKeyPrefix,
          base::Unretained(database_.get()), key_prefix),
      std::move(callback));
}

void ServiceWorkerStorage::DeleteAndStartOver(DatabaseStatusCallback callback) {
  Disable();

  // Will be used in DiskCacheImplDoneWithDisk()
  delete_and_start_over_callback_ = std::move(callback);

  // Won't get a callback about cleanup being done, so call it ourselves.
  if (!expecting_done_with_disk_on_disable_)
    DiskCacheImplDoneWithDisk();
}

void ServiceWorkerStorage::DiskCacheImplDoneWithDisk() {
  expecting_done_with_disk_on_disable_ = false;
  if (!delete_and_start_over_callback_.is_null()) {
    // Delete the database on the database thread.
    base::PostTaskAndReplyWithResult(
        database_task_runner_.get(), FROM_HERE,
        base::BindOnce(&ServiceWorkerDatabase::DestroyDatabase,
                       base::Unretained(database_.get())),
        base::BindOnce(&ServiceWorkerStorage::DidDeleteDatabase,
                       weak_factory_.GetWeakPtr(),
                       std::move(delete_and_start_over_callback_)));
  }
}

void ServiceWorkerStorage::GetNewRegistrationId(
    base::OnceCallback<void(int64_t registration_id)> callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run(
          blink::mojom::kInvalidServiceWorkerRegistrationId);
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(&ServiceWorkerStorage::GetNewRegistrationId,
                                    weak_factory_.GetWeakPtr(),
                                    std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }
  int64_t registration_id = next_registration_id_;
  ++next_registration_id_;
  std::move(callback).Run(registration_id);
}

void ServiceWorkerStorage::GetNewVersionId(
    base::OnceCallback<void(int64_t version_id)> callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run(blink::mojom::kInvalidServiceWorkerVersionId);
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(&ServiceWorkerStorage::GetNewVersionId,
                                    weak_factory_.GetWeakPtr(),
                                    std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }
  int64_t version_id = next_version_id_;
  ++next_version_id_;
  std::move(callback).Run(version_id);
}

void ServiceWorkerStorage::GetNewResourceId(
    base::OnceCallback<void(int64_t resource_id)> callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run(blink::mojom::kInvalidServiceWorkerResourceId);
      return;
    case STORAGE_STATE_INITIALIZING:
      // Fall-through.
    case STORAGE_STATE_UNINITIALIZED:
      LazyInitialize(base::BindOnce(&ServiceWorkerStorage::GetNewResourceId,
                                    weak_factory_.GetWeakPtr(),
                                    std::move(callback)));
      return;
    case STORAGE_STATE_INITIALIZED:
      break;
  }
  int64_t resource_id = next_resource_id_;
  ++next_resource_id_;
  std::move(callback).Run(resource_id);
}

void ServiceWorkerStorage::Disable() {
  state_ = STORAGE_STATE_DISABLED;
  if (disk_cache_)
    disk_cache_->Disable();
}

void ServiceWorkerStorage::PurgeResources(
    const std::vector<int64_t>& resource_ids) {
  if (!has_checked_for_stale_resources_)
    DeleteStaleResources();
  StartPurgingResources(resource_ids);
}

void ServiceWorkerStorage::ApplyPolicyUpdates(
    const std::vector<mojom::StoragePolicyUpdatePtr>& policy_updates,
    DatabaseStatusCallback callback) {
  switch (state_) {
    case STORAGE_STATE_DISABLED:
      std::move(callback).Run(ServiceWorkerDatabase::Status::kErrorDisabled);
      return;
    case STORAGE_STATE_INITIALIZING:
    case STORAGE_STATE_UNINITIALIZED: {
      // An explicit clone is needed to pass `policy_updates` to LazyInitialize.
      std::vector<mojom::StoragePolicyUpdatePtr> cloned_policy_updates;
      for (const auto& entry : policy_updates)
        cloned_policy_updates.push_back(entry.Clone());

      LazyInitialize(base::BindOnce(
          &ServiceWorkerStorage::ApplyPolicyUpdates, weak_factory_.GetWeakPtr(),
          std::move(cloned_policy_updates), std::move(callback)));
      return;
    }
    case STORAGE_STATE_INITIALIZED:
      break;
  }

  for (const auto& update : policy_updates) {
    GURL url = update->origin.GetURL();
    if (!update->purge_on_shutdown)
      origins_to_purge_on_shutdown_.erase(url);
    else
      origins_to_purge_on_shutdown_.insert(std::move(url));
  }

  std::move(callback).Run(ServiceWorkerDatabase::Status::kOk);
}

ServiceWorkerStorage::ServiceWorkerStorage(
    const base::FilePath& user_data_directory,
    scoped_refptr<base::SequencedTaskRunner> database_task_runner)
    : next_registration_id_(blink::mojom::kInvalidServiceWorkerRegistrationId),
      next_version_id_(blink::mojom::kInvalidServiceWorkerVersionId),
      next_resource_id_(blink::mojom::kInvalidServiceWorkerResourceId),
      state_(STORAGE_STATE_UNINITIALIZED),
      expecting_done_with_disk_on_disable_(false),
      user_data_directory_(user_data_directory),
      database_task_runner_(std::move(database_task_runner)),
      is_purge_pending_(false),
      has_checked_for_stale_resources_(false) {
  database_ = std::make_unique<ServiceWorkerDatabase>(GetDatabasePath());
}

base::FilePath ServiceWorkerStorage::GetDatabasePath() {
  if (user_data_directory_.empty())
    return base::FilePath();
  return user_data_directory_.Append(storage::kServiceWorkerDirectory)
      .Append(kDatabaseName);
}

base::FilePath ServiceWorkerStorage::GetDiskCachePath() {
  if (user_data_directory_.empty())
    return base::FilePath();
  return user_data_directory_.Append(storage::kServiceWorkerDirectory)
      .Append(kDiskCacheName);
}

void ServiceWorkerStorage::LazyInitializeForTest() {
  DCHECK_NE(state_, STORAGE_STATE_DISABLED);

  if (state_ == STORAGE_STATE_INITIALIZED)
    return;
  base::RunLoop loop;
  LazyInitialize(loop.QuitClosure());
  loop.Run();
}

void ServiceWorkerStorage::SetPurgingCompleteCallbackForTest(
    base::OnceClosure callback) {
  DCHECK(!purging_complete_callback_for_test_);
  purging_complete_callback_for_test_ = std::move(callback);
}

void ServiceWorkerStorage::GetPurgingResourceIdsForTest(
    ResourceIdsCallback callback) {
  std::move(callback).Run(ServiceWorkerDatabase::Status::kOk,
                          std::vector<int64_t>(purgeable_resource_ids_.begin(),
                                               purgeable_resource_ids_.end()));
}

void ServiceWorkerStorage::GetPurgeableResourceIdsForTest(
    ResourceIdsCallback callback) {
  database_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&GetPurgeableResourceIdsFromDB, database_.get(),
                     base::ThreadTaskRunnerHandle::Get(), std::move(callback)));
}

void ServiceWorkerStorage::GetUncommittedResourceIdsForTest(
    ResourceIdsCallback callback) {
  database_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&GetUncommittedResourceIdsFromDB, database_.get(),
                     base::ThreadTaskRunnerHandle::Get(), std::move(callback)));
}

void ServiceWorkerStorage::LazyInitialize(base::OnceClosure callback) {
  DCHECK(state_ == STORAGE_STATE_UNINITIALIZED ||
         state_ == STORAGE_STATE_INITIALIZING)
      << state_;
  pending_tasks_.push_back(std::move(callback));
  if (state_ == STORAGE_STATE_INITIALIZING) {
    return;
  }

  state_ = STORAGE_STATE_INITIALIZING;
  database_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&ReadInitialDataFromDB, database_.get(),
                     base::ThreadTaskRunnerHandle::Get(),
                     base::BindOnce(&ServiceWorkerStorage::DidReadInitialData,
                                    weak_factory_.GetWeakPtr())));
}

void ServiceWorkerStorage::DidReadInitialData(
    std::unique_ptr<InitialData> data,
    ServiceWorkerDatabase::Status status) {
  DCHECK(data);
  DCHECK_EQ(STORAGE_STATE_INITIALIZING, state_);

  if (status == ServiceWorkerDatabase::Status::kOk) {
    next_registration_id_ = data->next_registration_id;
    next_version_id_ = data->next_version_id;
    next_resource_id_ = data->next_resource_id;
    registered_origins_.swap(data->origins);
    state_ = STORAGE_STATE_INITIALIZED;
    base::UmaHistogramCounts1M("ServiceWorker.RegisteredOriginCount",
                               registered_origins_.size());
  } else {
    DVLOG(2) << "Failed to initialize: "
             << ServiceWorkerDatabase::StatusToString(status);
    Disable();
  }

  for (base::OnceClosure& task : pending_tasks_)
    RunSoon(FROM_HERE, std::move(task));
  pending_tasks_.clear();
}

void ServiceWorkerStorage::DidGetRegistrationsForOrigin(
    GetRegistrationsDataCallback callback,
    std::unique_ptr<RegistrationList> registration_data_list,
    std::unique_ptr<std::vector<ResourceList>> resource_lists,
    ServiceWorkerDatabase::Status status) {
  std::move(callback).Run(status, std::move(registration_data_list),
                          std::move(resource_lists));
}

void ServiceWorkerStorage::DidGetAllRegistrations(
    GetAllRegistrationsCallback callback,
    std::unique_ptr<RegistrationList> registration_data_list,
    ServiceWorkerDatabase::Status status) {
  std::move(callback).Run(status, std::move(registration_data_list));
}

void ServiceWorkerStorage::DidStoreRegistrationData(
    StoreRegistrationDataCallback callback,
    uint64_t new_resources_total_size_bytes,
    const GURL& origin,
    const ServiceWorkerDatabase::DeletedVersion& deleted_version,
    ServiceWorkerDatabase::Status status) {
  if (status != ServiceWorkerDatabase::Status::kOk) {
    std::move(callback).Run(status, deleted_version.version_id,
                            deleted_version.resources_total_size_bytes,
                            deleted_version.newly_purgeable_resources);
    return;
  }
  registered_origins_.insert(url::Origin::Create(origin));

  std::move(callback).Run(ServiceWorkerDatabase::Status::kOk,
                          deleted_version.version_id,
                          deleted_version.resources_total_size_bytes,
                          deleted_version.newly_purgeable_resources);
}

void ServiceWorkerStorage::DidDeleteRegistration(
    std::unique_ptr<DidDeleteRegistrationParams> params,
    OriginState origin_state,
    const ServiceWorkerDatabase::DeletedVersion& deleted_version,
    ServiceWorkerDatabase::Status status) {
  if (status != ServiceWorkerDatabase::Status::kOk) {
    std::move(params->callback)
        .Run(status, origin_state, deleted_version.version_id,
             deleted_version.resources_total_size_bytes,
             deleted_version.newly_purgeable_resources);
    return;
  }

  if (origin_state == OriginState::kDelete)
    registered_origins_.erase(url::Origin::Create(params->origin));

  std::move(params->callback)
      .Run(ServiceWorkerDatabase::Status::kOk, origin_state,
           deleted_version.version_id,
           deleted_version.resources_total_size_bytes,
           deleted_version.newly_purgeable_resources);
}

void ServiceWorkerStorage::DidDoomUncommittedResourceIds(
    const std::vector<int64_t>& resource_ids,
    DatabaseStatusCallback callback,
    ServiceWorkerDatabase::Status status) {
  if (status == ServiceWorkerDatabase::Status::kOk)
    PurgeResources(resource_ids);
  std::move(callback).Run(status);
}

ServiceWorkerDiskCache* ServiceWorkerStorage::disk_cache() {
  DCHECK(STORAGE_STATE_INITIALIZED == state_ ||
         STORAGE_STATE_DISABLED == state_)
      << state_;
  if (disk_cache_)
    return disk_cache_.get();
  disk_cache_ = std::make_unique<ServiceWorkerDiskCache>();

  if (IsDisabled()) {
    disk_cache_->Disable();
    return disk_cache_.get();
  }

  base::FilePath path = GetDiskCachePath();
  if (path.empty()) {
    int rv = disk_cache_->InitWithMemBackend(0, net::CompletionOnceCallback());
    DCHECK_EQ(net::OK, rv);
    return disk_cache_.get();
  }

  InitializeDiskCache();
  return disk_cache_.get();
}

void ServiceWorkerStorage::InitializeDiskCache() {
  disk_cache_->set_is_waiting_to_initialize(false);
  expecting_done_with_disk_on_disable_ = true;
  int rv = disk_cache_->InitWithDiskBackend(
      GetDiskCachePath(),
      base::BindOnce(&ServiceWorkerStorage::DiskCacheImplDoneWithDisk,
                     weak_factory_.GetWeakPtr()),
      base::BindOnce(&ServiceWorkerStorage::OnDiskCacheInitialized,
                     weak_factory_.GetWeakPtr()));
  if (rv != net::ERR_IO_PENDING)
    OnDiskCacheInitialized(rv);
}

void ServiceWorkerStorage::OnDiskCacheInitialized(int rv) {
  if (rv != net::OK) {
    LOG(ERROR) << "Failed to open the serviceworker diskcache: "
               << net::ErrorToString(rv);
    Disable();
  }
  base::UmaHistogramBoolean("ServiceWorker.DiskCache.InitResult",
                            rv == net::OK);
}

void ServiceWorkerStorage::StartPurgingResources(
    const std::vector<int64_t>& resource_ids) {
  DCHECK(has_checked_for_stale_resources_);
  for (int64_t resource_id : resource_ids)
    purgeable_resource_ids_.push_back(resource_id);
  ContinuePurgingResources();
}

void ServiceWorkerStorage::StartPurgingResources(
    const ResourceList& resources) {
  DCHECK(has_checked_for_stale_resources_);
  for (const auto& resource : resources)
    purgeable_resource_ids_.push_back(resource->resource_id);
  ContinuePurgingResources();
}

void ServiceWorkerStorage::ContinuePurgingResources() {
  if (is_purge_pending_)
    return;
  if (purgeable_resource_ids_.empty()) {
    if (purging_complete_callback_for_test_)
      std::move(purging_complete_callback_for_test_).Run();
    return;
  }

  // Do one at a time until we're done, use RunSoon to avoid recursion when
  // DoomEntry returns immediately.
  is_purge_pending_ = true;
  int64_t id = purgeable_resource_ids_.front();
  purgeable_resource_ids_.pop_front();
  RunSoon(FROM_HERE, base::BindOnce(&ServiceWorkerStorage::PurgeResource,
                                    weak_factory_.GetWeakPtr(), id));
}

void ServiceWorkerStorage::PurgeResource(int64_t id) {
  DCHECK(is_purge_pending_);
  disk_cache()->DoomEntry(
      id, base::BindOnce(&ServiceWorkerStorage::OnResourcePurged,
                         weak_factory_.GetWeakPtr(), id));
}

void ServiceWorkerStorage::OnResourcePurged(int64_t id, int rv) {
  DCHECK(is_purge_pending_);
  is_purge_pending_ = false;

  base::UmaHistogramSparse("ServiceWorker.Storage.PurgeResourceResult",
                           std::abs(rv));

  // TODO(falken): Is it always OK to ClearPurgeableResourceIds if |rv| is
  // failure? The disk cache entry might still remain and once we remove its
  // purgeable id, we will never retry deleting it.
  std::vector<int64_t> ids = {id};
  database_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          base::IgnoreResult(&ServiceWorkerDatabase::ClearPurgeableResourceIds),
          base::Unretained(database_.get()), ids));

  // Continue purging resources regardless of the previous result.
  ContinuePurgingResources();
}

void ServiceWorkerStorage::DeleteStaleResources() {
  DCHECK(!has_checked_for_stale_resources_);
  has_checked_for_stale_resources_ = true;
  database_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &ServiceWorkerStorage::CollectStaleResourcesFromDB, database_.get(),
          base::ThreadTaskRunnerHandle::Get(),
          base::BindOnce(&ServiceWorkerStorage::DidCollectStaleResources,
                         weak_factory_.GetWeakPtr())));
}

void ServiceWorkerStorage::DidCollectStaleResources(
    const std::vector<int64_t>& stale_resource_ids,
    ServiceWorkerDatabase::Status status) {
  if (status != ServiceWorkerDatabase::Status::kOk) {
    DCHECK_NE(ServiceWorkerDatabase::Status::kErrorNotFound, status);
    Disable();
    return;
  }
  StartPurgingResources(stale_resource_ids);
}

void ServiceWorkerStorage::ClearSessionOnlyOrigins() {
  database_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&DeleteAllDataForOriginsFromDB, database_.get(),
                                origins_to_purge_on_shutdown_));
}

void ServiceWorkerStorage::OnResourceReaderDisconnected(
    uint64_t resource_operation_id) {
  DCHECK(base::Contains(resource_readers_, resource_operation_id));
  resource_readers_.erase(resource_operation_id);
}

void ServiceWorkerStorage::OnResourceWriterDisconnected(
    uint64_t resource_operation_id) {
  DCHECK(base::Contains(resource_writers_, resource_operation_id));
  resource_writers_.erase(resource_operation_id);
}

void ServiceWorkerStorage::OnResourceMetadataWriterDisconnected(
    uint64_t resource_operation_id) {
  DCHECK(base::Contains(resource_metadata_writers_, resource_operation_id));
  resource_metadata_writers_.erase(resource_operation_id);
}

// static
void ServiceWorkerStorage::CollectStaleResourcesFromDB(
    ServiceWorkerDatabase* database,
    scoped_refptr<base::SequencedTaskRunner> original_task_runner,
    GetResourcesCallback callback) {
  std::vector<int64_t> ids;
  ServiceWorkerDatabase::Status status =
      database->GetUncommittedResourceIds(&ids);
  if (status != ServiceWorkerDatabase::Status::kOk) {
    original_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback),
                       std::vector<int64_t>(ids.begin(), ids.end()), status));
    return;
  }

  status = database->PurgeUncommittedResourceIds(ids);
  if (status != ServiceWorkerDatabase::Status::kOk) {
    original_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback),
                       std::vector<int64_t>(ids.begin(), ids.end()), status));
    return;
  }

  ids.clear();
  status = database->GetPurgeableResourceIds(&ids);
  original_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback),
                     std::vector<int64_t>(ids.begin(), ids.end()), status));
}

// static
void ServiceWorkerStorage::ReadInitialDataFromDB(
    ServiceWorkerDatabase* database,
    scoped_refptr<base::SequencedTaskRunner> original_task_runner,
    InitializeCallback callback) {
  DCHECK(database);
  std::unique_ptr<ServiceWorkerStorage::InitialData> data(
      new ServiceWorkerStorage::InitialData());

  ServiceWorkerDatabase::Status status = database->GetNextAvailableIds(
      &data->next_registration_id, &data->next_version_id,
      &data->next_resource_id);
  if (status != ServiceWorkerDatabase::Status::kOk) {
    original_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), std::move(data), status));
    return;
  }

  status = database->GetOriginsWithRegistrations(&data->origins);
  if (status != ServiceWorkerDatabase::Status::kOk) {
    original_task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), std::move(data), status));
    return;
  }

  original_task_runner->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), std::move(data), status));
}

void ServiceWorkerStorage::DeleteRegistrationFromDB(
    ServiceWorkerDatabase* database,
    scoped_refptr<base::SequencedTaskRunner> original_task_runner,
    int64_t registration_id,
    const GURL& origin,
    DeleteRegistrationInDBCallback callback) {
  DCHECK(database);

  ServiceWorkerDatabase::DeletedVersion deleted_version;
  ServiceWorkerDatabase::Status status =
      database->DeleteRegistration(registration_id, origin, &deleted_version);
  if (status != ServiceWorkerDatabase::Status::kOk) {
    original_task_runner->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), OriginState::kKeep,
                                  deleted_version, status));
    return;
  }

  // TODO(nhiroki): Add convenient method to ServiceWorkerDatabase to check the
  // unique origin list.
  RegistrationList registrations;
  status = database->GetRegistrationsForOrigin(url::Origin::Create(origin),
                                               &registrations, nullptr);
  if (status != ServiceWorkerDatabase::Status::kOk) {
    original_task_runner->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), OriginState::kKeep,
                                  deleted_version, status));
    return;
  }

  OriginState origin_state =
      registrations.empty() ? OriginState::kDelete : OriginState::kKeep;
  original_task_runner->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), origin_state,
                                deleted_version, status));
}

void ServiceWorkerStorage::WriteRegistrationInDB(
    ServiceWorkerDatabase* database,
    scoped_refptr<base::SequencedTaskRunner> original_task_runner,
    mojom::ServiceWorkerRegistrationDataPtr registration,
    ResourceList resources,
    WriteRegistrationCallback callback) {
  DCHECK(database);
  ServiceWorkerDatabase::DeletedVersion deleted_version;
  ServiceWorkerDatabase::Status status =
      database->WriteRegistration(*registration, resources, &deleted_version);
  original_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback), registration->script.GetOrigin(),
                     deleted_version, status));
}

// static
void ServiceWorkerStorage::FindForClientUrlInDB(
    ServiceWorkerDatabase* database,
    scoped_refptr<base::SequencedTaskRunner> original_task_runner,
    const GURL& client_url,
    FindInDBCallback callback) {
  GURL origin = client_url.GetOrigin();
  RegistrationList registration_data_list;
  ServiceWorkerDatabase::Status status = database->GetRegistrationsForOrigin(
      url::Origin::Create(origin), &registration_data_list, nullptr);
  if (status != ServiceWorkerDatabase::Status::kOk) {
    original_task_runner->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback),
                                  /*data=*/nullptr,
                                  /*resources=*/nullptr, status));
    return;
  }

  mojom::ServiceWorkerRegistrationDataPtr data;
  auto resources = std::make_unique<ResourceList>();
  status = ServiceWorkerDatabase::Status::kErrorNotFound;

  // Find one with a scope match.
  blink::ServiceWorkerLongestScopeMatcher matcher(client_url);
  int64_t match = blink::mojom::kInvalidServiceWorkerRegistrationId;
  for (const auto& registration_data : registration_data_list)
    if (matcher.MatchLongest(registration_data->scope))
      match = registration_data->registration_id;
  if (match != blink::mojom::kInvalidServiceWorkerRegistrationId)
    status = database->ReadRegistration(match, origin, &data, resources.get());

  original_task_runner->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), std::move(data),
                                std::move(resources), status));
}

// static
void ServiceWorkerStorage::FindForScopeInDB(
    ServiceWorkerDatabase* database,
    scoped_refptr<base::SequencedTaskRunner> original_task_runner,
    const GURL& scope,
    FindInDBCallback callback) {
  GURL origin = scope.GetOrigin();
  RegistrationList registration_data_list;
  ServiceWorkerDatabase::Status status = database->GetRegistrationsForOrigin(
      url::Origin::Create(origin), &registration_data_list, nullptr);
  if (status != ServiceWorkerDatabase::Status::kOk) {
    original_task_runner->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback),
                                  /*data=*/nullptr,
                                  /*resources=*/nullptr, status));
    return;
  }

  // Find one with an exact matching scope.
  mojom::ServiceWorkerRegistrationDataPtr data;
  auto resources = std::make_unique<ResourceList>();
  status = ServiceWorkerDatabase::Status::kErrorNotFound;
  for (const auto& registration_data : registration_data_list) {
    if (scope != registration_data->scope)
      continue;
    status = database->ReadRegistration(registration_data->registration_id,
                                        origin, &data, resources.get());
    break;  // We're done looping.
  }

  original_task_runner->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), std::move(data),
                                std::move(resources), status));
}

// static
void ServiceWorkerStorage::FindForIdInDB(
    ServiceWorkerDatabase* database,
    scoped_refptr<base::SequencedTaskRunner> original_task_runner,
    int64_t registration_id,
    const url::Origin& origin,
    FindInDBCallback callback) {
  mojom::ServiceWorkerRegistrationDataPtr data;
  auto resources = std::make_unique<ResourceList>();
  ServiceWorkerDatabase::Status status = database->ReadRegistration(
      registration_id, origin.GetURL(), &data, resources.get());
  original_task_runner->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), std::move(data),
                                std::move(resources), status));
}

// static
void ServiceWorkerStorage::FindForIdOnlyInDB(
    ServiceWorkerDatabase* database,
    scoped_refptr<base::SequencedTaskRunner> original_task_runner,
    int64_t registration_id,
    FindInDBCallback callback) {
  GURL origin;
  ServiceWorkerDatabase::Status status =
      database->ReadRegistrationOrigin(registration_id, &origin);
  if (status != ServiceWorkerDatabase::Status::kOk) {
    original_task_runner->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback),
                                  /*data=*/nullptr,
                                  /*resources=*/nullptr, status));
    return;
  }
  FindForIdInDB(database, original_task_runner, registration_id,
                url::Origin::Create(origin), std::move(callback));
}

// static
void ServiceWorkerStorage::GetUsageForOriginInDB(
    ServiceWorkerDatabase* database,
    scoped_refptr<base::SequencedTaskRunner> original_task_runner,
    url::Origin origin,
    GetUsageForOriginCallback callback) {
  int64_t usage = 0;
  ServiceWorkerDatabase::Status status =
      database->GetUsageForOrigin(origin, usage);
  original_task_runner->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), status, usage));
}

void ServiceWorkerStorage::GetUserDataInDB(
    ServiceWorkerDatabase* database,
    scoped_refptr<base::SequencedTaskRunner> original_task_runner,
    int64_t registration_id,
    const std::vector<std::string>& keys,
    GetUserDataInDBCallback callback) {
  std::vector<std::string> values;
  ServiceWorkerDatabase::Status status =
      database->ReadUserData(registration_id, keys, &values);
  original_task_runner->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), status, values));
}

void ServiceWorkerStorage::GetUserDataByKeyPrefixInDB(
    ServiceWorkerDatabase* database,
    scoped_refptr<base::SequencedTaskRunner> original_task_runner,
    int64_t registration_id,
    const std::string& key_prefix,
    GetUserDataInDBCallback callback) {
  std::vector<std::string> values;
  ServiceWorkerDatabase::Status status =
      database->ReadUserDataByKeyPrefix(registration_id, key_prefix, &values);
  original_task_runner->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), status, values));
}

void ServiceWorkerStorage::GetUserKeysAndDataByKeyPrefixInDB(
    ServiceWorkerDatabase* database,
    scoped_refptr<base::SequencedTaskRunner> original_task_runner,
    int64_t registration_id,
    const std::string& key_prefix,
    GetUserKeysAndDataInDBCallback callback) {
  base::flat_map<std::string, std::string> data_map;
  ServiceWorkerDatabase::Status status =
      database->ReadUserKeysAndDataByKeyPrefix(registration_id, key_prefix,
                                               &data_map);
  original_task_runner->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), status, data_map));
}

void ServiceWorkerStorage::GetUserDataForAllRegistrationsInDB(
    ServiceWorkerDatabase* database,
    scoped_refptr<base::SequencedTaskRunner> original_task_runner,
    const std::string& key,
    GetUserDataForAllRegistrationsInDBCallback callback) {
  std::vector<mojom::ServiceWorkerUserDataPtr> user_data;
  ServiceWorkerDatabase::Status status =
      database->ReadUserDataForAllRegistrations(key, &user_data);
  original_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback), status, std::move(user_data)));
}

void ServiceWorkerStorage::GetUserDataForAllRegistrationsByKeyPrefixInDB(
    ServiceWorkerDatabase* database,
    scoped_refptr<base::SequencedTaskRunner> original_task_runner,
    const std::string& key_prefix,
    GetUserDataForAllRegistrationsInDBCallback callback) {
  std::vector<mojom::ServiceWorkerUserDataPtr> user_data;
  ServiceWorkerDatabase::Status status =
      database->ReadUserDataForAllRegistrationsByKeyPrefix(key_prefix,
                                                           &user_data);
  original_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback), status, std::move(user_data)));
}

void ServiceWorkerStorage::DeleteAllDataForOriginsFromDB(
    ServiceWorkerDatabase* database,
    const std::set<GURL>& origins) {
  DCHECK(database);

  std::vector<int64_t> newly_purgeable_resources;
  database->DeleteAllDataForOrigins(origins, &newly_purgeable_resources);
}

void ServiceWorkerStorage::PerformStorageCleanupInDB(
    ServiceWorkerDatabase* database) {
  DCHECK(database);
  database->RewriteDB();
}

// static
void ServiceWorkerStorage::GetPurgeableResourceIdsFromDB(
    ServiceWorkerDatabase* database,
    scoped_refptr<base::SequencedTaskRunner> original_task_runner,
    ServiceWorkerStorage::ResourceIdsCallback callback) {
  std::vector<int64_t> resource_ids;
  ServiceWorkerDatabase::Status status =
      database->GetPurgeableResourceIds(&resource_ids);
  original_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback), status, std::move(resource_ids)));
}

// static
void ServiceWorkerStorage::GetUncommittedResourceIdsFromDB(
    ServiceWorkerDatabase* database,
    scoped_refptr<base::SequencedTaskRunner> original_task_runner,
    ServiceWorkerStorage::ResourceIdsCallback callback) {
  std::vector<int64_t> resource_ids;
  ServiceWorkerDatabase::Status status =
      database->GetUncommittedResourceIds(&resource_ids);
  original_task_runner->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback), status, std::move(resource_ids)));
}

void ServiceWorkerStorage::DidDeleteDatabase(
    DatabaseStatusCallback callback,
    ServiceWorkerDatabase::Status status) {
  DCHECK_EQ(STORAGE_STATE_DISABLED, state_);
  if (status != ServiceWorkerDatabase::Status::kOk) {
    // Give up the corruption recovery until the browser restarts.
    LOG(ERROR) << "Failed to delete the database: "
               << ServiceWorkerDatabase::StatusToString(status);
    RecordDeleteAndStartOverResult(
        DeleteAndStartOverResult::kDeleteDatabaseError);
    std::move(callback).Run(status);
    return;
  }
  DVLOG(1) << "Deleted ServiceWorkerDatabase successfully.";

  // Delete the disk cache. Use BLOCK_SHUTDOWN to try to avoid things being
  // half-deleted.
  // TODO(falken): Investigate if BLOCK_SHUTDOWN is needed, as the next startup
  // is expected to cleanup the disk cache anyway. Also investigate whether
  // ClearSessionOnlyOrigins() should try to delete relevant entries from the
  // disk cache before shutdown.

  // TODO(nhiroki): What if there is a bunch of files in the cache directory?
  // Deleting the directory could take a long time and restart could be delayed.
  // We should probably rename the directory and delete it later.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      base::BindOnce(&base::DeletePathRecursively, GetDiskCachePath()),
      base::BindOnce(&ServiceWorkerStorage::DidDeleteDiskCache,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void ServiceWorkerStorage::DidDeleteDiskCache(DatabaseStatusCallback callback,
                                              bool result) {
  DCHECK_EQ(STORAGE_STATE_DISABLED, state_);
  if (!result) {
    // Give up the corruption recovery until the browser restarts.
    LOG(ERROR) << "Failed to delete the diskcache.";
    RecordDeleteAndStartOverResult(
        DeleteAndStartOverResult::kDeleteDiskCacheError);
    std::move(callback).Run(ServiceWorkerDatabase::Status::kErrorFailed);
    return;
  }
  DVLOG(1) << "Deleted ServiceWorkerDiskCache successfully.";
  RecordDeleteAndStartOverResult(DeleteAndStartOverResult::kDeleteOk);
  std::move(callback).Run(ServiceWorkerDatabase::Status::kOk);
}

}  // namespace storage
