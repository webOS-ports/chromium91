// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/sync/engine/cycle/sync_cycle_context.h"

#include "components/sync/base/extensions_activity.h"

namespace syncer {

SyncCycleContext::SyncCycleContext(
    ServerConnectionManager* connection_manager,
    ExtensionsActivity* extensions_activity,
    const std::vector<SyncEngineEventListener*>& listeners,
    DebugInfoGetter* debug_info_getter,
    ModelTypeRegistry* model_type_registry,
    const std::string& invalidator_client_id,
    const std::string& cache_guid,
    const std::string& birthday,
    const std::string& bag_of_chips,
    base::TimeDelta poll_interval)
    : connection_manager_(connection_manager),
      extensions_activity_(extensions_activity),
      notifications_enabled_(false),
      cache_guid_(cache_guid),
      birthday_(birthday),
      bag_of_chips_(bag_of_chips),
      max_commit_batch_size_(kDefaultMaxCommitBatchSize),
      debug_info_getter_(debug_info_getter),
      model_type_registry_(model_type_registry),
      invalidator_client_id_(invalidator_client_id),
      cookie_jar_mismatch_(false),
      single_client_(false),
      poll_interval_(poll_interval) {
  DCHECK(!poll_interval.is_zero());
  std::vector<SyncEngineEventListener*>::const_iterator it;
  for (it = listeners.begin(); it != listeners.end(); ++it)
    listeners_.AddObserver(*it);
}

SyncCycleContext::~SyncCycleContext() {}

ModelTypeSet SyncCycleContext::GetEnabledTypes() const {
  return model_type_registry_->GetEnabledTypes();
}

void SyncCycleContext::set_birthday(const std::string& birthday) {
  DCHECK(birthday_.empty());
  birthday_ = birthday;
}

void SyncCycleContext::set_bag_of_chips(const std::string& bag_of_chips) {
  bag_of_chips_ = bag_of_chips;
}

}  // namespace syncer
