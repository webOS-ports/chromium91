// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_SYNC_ENGINE_SYNC_SCHEDULER_H_
#define COMPONENTS_SYNC_ENGINE_SYNC_SCHEDULER_H_

#include <memory>
#include <string>

#include "base/callback.h"
#include "base/compiler_specific.h"
#include "base/time/time.h"
#include "components/sync/base/invalidation_interface.h"
#include "components/sync/engine/cycle/sync_cycle.h"
#include "services/network/public/mojom/network_change_manager.mojom.h"

namespace base {
class Location;
}  // namespace base

namespace syncer {

struct ConfigurationParams {
  ConfigurationParams();
  ConfigurationParams(sync_pb::SyncEnums::GetUpdatesOrigin origin,
                      ModelTypeSet types_to_download,
                      base::OnceClosure ready_task);
  ConfigurationParams(const ConfigurationParams&) = delete;
  ConfigurationParams(ConfigurationParams&& other);
  ConfigurationParams& operator=(const ConfigurationParams&) = delete;
  ConfigurationParams& operator=(ConfigurationParams&&);
  ~ConfigurationParams();

  // Origin for the configuration.
  sync_pb::SyncEnums::GetUpdatesOrigin origin;
  // The types that should be downloaded.
  ModelTypeSet types_to_download;
  // Callback to invoke on configuration completion.
  base::OnceClosure ready_task;
};

// A class to schedule syncer tasks intelligently.
class SyncScheduler : public SyncCycle::Delegate {
 public:
  enum Mode {
    // In this mode, the thread only performs configuration tasks.  This is
    // designed to make the case where we want to download updates for a
    // specific type only, and not continue syncing until we are moved into
    // normal mode.
    CONFIGURATION_MODE,
    // Resumes polling and allows nudges, drops configuration tasks.  Runs
    // through entire sync cycle.
    NORMAL_MODE,
  };

  // All methods of SyncScheduler must be called on the same thread
  // (except for RequestEarlyExit()).

  SyncScheduler() = default;
  ~SyncScheduler() override = default;

  // Start the scheduler with the given mode.  If the scheduler is
  // already started, switch to the given mode, although some
  // scheduled tasks from the old mode may still run. |last_poll_time| will
  // be used to decide what the poll timer should be initialized with.
  virtual void Start(Mode mode, base::Time last_poll_time) = 0;

  // Schedules the configuration task specified by |params|. Returns true if
  // the configuration task executed immediately, false if it had to be
  // scheduled for a later attempt. |params.ready_task| is invoked whenever the
  // configuration task executes. |params.retry_task| is invoked once if the
  // configuration task could not execute. |params.ready_task| will still be
  // called when configuration finishes.
  // Note: must already be in CONFIGURATION mode.
  virtual void ScheduleConfiguration(ConfigurationParams params) = 0;

  // Request that the syncer avoid starting any new tasks and prepare for
  // shutdown.
  virtual void Stop() = 0;

  // The meat and potatoes. All three of the following methods will post a
  // delayed task to attempt the actual nudge (see ScheduleNudgeImpl).
  //
  // NOTE: |desired_delay| is best-effort. If a nudge is already scheduled to
  // depart earlier than Now() + delay, the scheduler can and will prefer to
  // batch the two so that only one nudge is sent (at the earlier time). Also,
  // as always with delayed tasks and timers, it's possible the task gets run
  // any time after |desired_delay|.

  // The LocalNudge indicates that we've made a local change, and that the
  // syncer should plan to commit this to the server some time soon.
  virtual void ScheduleLocalNudge(ModelTypeSet types,
                                  const base::Location& nudge_location) = 0;

  // The LocalRefreshRequest occurs when we decide for some reason to manually
  // request updates.  This should be used sparingly.  For example, one of its
  // uses is to fetch the latest tab sync data when it's relevant to the UI on
  // platforms where tab sync is not registered for invalidations.
  virtual void ScheduleLocalRefreshRequest(
      ModelTypeSet types,
      const base::Location& nudge_location) = 0;

  // Invalidations are notifications the server sends to let us know when other
  // clients have committed data.  We need to contact the sync server (being
  // careful to pass along the "hints" delivered with those invalidations) in
  // order to fetch the update.
  virtual void ScheduleInvalidationNudge(
      ModelType type,
      std::unique_ptr<InvalidationInterface> invalidation,
      const base::Location& nudge_location) = 0;

  // Requests a non-blocking initial sync request for the specified type.
  //
  // Many types can only complete initial sync while the scheduler is in
  // configure mode, but a few of them are able to perform their initial sync
  // while the scheduler is in normal mode.  This non-blocking initial sync
  // can be requested through this function.
  virtual void ScheduleInitialSyncNudge(ModelType model_type) = 0;

  // Change status of notifications in the SyncCycleContext.
  virtual void SetNotificationsEnabled(bool notifications_enabled) = 0;

  // Called when credentials are updated by the user.
  virtual void OnCredentialsUpdated() = 0;

  // Called when the network layer detects a connection status change.
  virtual void OnConnectionStatusChange(
      network::mojom::ConnectionType type) = 0;
};

}  // namespace syncer

#endif  // COMPONENTS_SYNC_ENGINE_SYNC_SCHEDULER_H_
