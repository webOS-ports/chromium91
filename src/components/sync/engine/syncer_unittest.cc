// Copyright 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/sync/engine/syncer.h"

#include <stddef.h>

#include <algorithm>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "base/bind.h"
#include "base/callback_helpers.h"
#include "base/compiler_specific.h"
#include "base/location.h"
#include "base/stl_util.h"
#include "base/strings/stringprintf.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "components/sync/base/client_tag_hash.h"
#include "components/sync/base/extensions_activity.h"
#include "components/sync/base/time.h"
#include "components/sync/engine/backoff_delay_provider.h"
#include "components/sync/engine/cancelation_signal.h"
#include "components/sync/engine/cycle/mock_debug_info_getter.h"
#include "components/sync/engine/cycle/sync_cycle_context.h"
#include "components/sync/engine/data_type_activation_response.h"
#include "components/sync/engine/forwarding_model_type_processor.h"
#include "components/sync/engine/net/server_connection_manager.h"
#include "components/sync/engine/nigori/keystore_keys_handler.h"
#include "components/sync/engine/sync_scheduler_impl.h"
#include "components/sync/engine/syncer_proto_util.h"
#include "components/sync/protocol/bookmark_specifics.pb.h"
#include "components/sync/protocol/preference_specifics.pb.h"
#include "components/sync/test/engine/mock_connection_manager.h"
#include "components/sync/test/engine/mock_model_type_processor.h"
#include "components/sync/test/engine/mock_nudge_handler.h"
#include "components/sync/test/fake_sync_encryption_handler.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace syncer {

namespace {

using testing::IsEmpty;
using testing::UnorderedElementsAre;

sync_pb::EntitySpecifics MakeSpecifics(ModelType model_type) {
  sync_pb::EntitySpecifics specifics;
  AddDefaultFieldValue(model_type, &specifics);
  return specifics;
}

}  // namespace

// Syncer unit tests. Unfortunately a lot of these tests
// are outdated and need to be reworked and updated.
class SyncerTest : public testing::Test,
                   public SyncCycle::Delegate,
                   public SyncEngineEventListener {
 protected:
  SyncerTest()
      : extensions_activity_(new ExtensionsActivity),
        syncer_(nullptr),
        last_client_invalidation_hint_buffer_size_(10) {}

  // SyncCycle::Delegate implementation.
  void OnThrottled(const base::TimeDelta& throttle_duration) override {
    FAIL() << "Should not get silenced.";
  }
  void OnTypesThrottled(ModelTypeSet types,
                        const base::TimeDelta& throttle_duration) override {
    scheduler_->OnTypesThrottled(types, throttle_duration);
  }
  void OnTypesBackedOff(ModelTypeSet types) override {
    scheduler_->OnTypesBackedOff(types);
  }
  bool IsAnyThrottleOrBackoff() override { return false; }
  void OnReceivedPollIntervalUpdate(
      const base::TimeDelta& new_interval) override {
    last_poll_interval_received_ = new_interval;
  }
  void OnReceivedCustomNudgeDelays(
      const std::map<ModelType, base::TimeDelta>& delay_map) override {
    auto iter = delay_map.find(SESSIONS);
    if (iter != delay_map.end() && iter->second > base::TimeDelta())
      last_sessions_commit_delay_ = iter->second;
    iter = delay_map.find(BOOKMARKS);
    if (iter != delay_map.end() && iter->second > base::TimeDelta())
      last_bookmarks_commit_delay_ = iter->second;
  }
  void OnReceivedClientInvalidationHintBufferSize(int size) override {
    last_client_invalidation_hint_buffer_size_ = size;
  }
  void OnReceivedGuRetryDelay(const base::TimeDelta& delay) override {}
  void OnReceivedMigrationRequest(ModelTypeSet types) override {}
  void OnProtocolEvent(const ProtocolEvent& event) override {}
  void OnSyncProtocolError(const SyncProtocolError& error) override {}

  void OnSyncCycleEvent(const SyncCycleEvent& event) override {
    DVLOG(1) << "HandleSyncEngineEvent in unittest " << event.what_happened;
  }

  void OnActionableError(const SyncProtocolError& error) override {}
  void OnRetryTimeChanged(base::Time retry_time) override {}
  void OnThrottledTypesChanged(ModelTypeSet throttled_types) override {}
  void OnBackedOffTypesChanged(ModelTypeSet backed_off_types) override {}
  void OnMigrationRequested(ModelTypeSet types) override {}

  void ResetCycle() {
    cycle_ = std::make_unique<SyncCycle>(context_.get(), this);
  }

  bool SyncShareNudge() {
    ResetCycle();

    // Pretend we've seen a local change, to make the nudge_tracker look normal.
    nudge_tracker_.RecordLocalChange(ModelTypeSet(BOOKMARKS));

    return syncer_->NormalSyncShare(context_->GetEnabledTypes(),
                                    &nudge_tracker_, cycle_.get());
  }

  bool SyncShareConfigure() {
    return SyncShareConfigureTypes(context_->GetEnabledTypes());
  }

  bool SyncShareConfigureTypes(ModelTypeSet types) {
    ResetCycle();
    return syncer_->ConfigureSyncShare(
        types, sync_pb::SyncEnums::RECONFIGURATION, cycle_.get());
  }

  void SetUp() override {
    mock_server_ = std::make_unique<MockConnectionManager>();
    debug_info_getter_ = std::make_unique<MockDebugInfoGetter>();
    std::vector<SyncEngineEventListener*> listeners;
    listeners.push_back(this);

    model_type_registry_ = std::make_unique<ModelTypeRegistry>(
        &mock_nudge_handler_, &cancelation_signal_, &encryption_handler_);

    EnableDatatype(BOOKMARKS);
    EnableDatatype(EXTENSIONS);
    EnableDatatype(NIGORI);
    EnableDatatype(PREFERENCES);

    context_ = std::make_unique<SyncCycleContext>(
        mock_server_.get(), extensions_activity_.get(), listeners,
        debug_info_getter_.get(), model_type_registry_.get(),
        "fake_invalidator_client_id", local_cache_guid(),
        mock_server_->store_birthday(), "fake_bag_of_chips",
        /*poll_interval=*/base::TimeDelta::FromMinutes(30));
    auto syncer = std::make_unique<Syncer>(&cancelation_signal_);
    // The syncer is destroyed with the scheduler that owns it.
    syncer_ = syncer.get();
    scheduler_ = std::make_unique<SyncSchedulerImpl>(
        "TestSyncScheduler", BackoffDelayProvider::FromDefaults(),
        context_.get(), std::move(syncer), false);

    mock_server_->SetKeystoreKey("encryption_key");
  }

  void TearDown() override {
    mock_server_.reset();
    scheduler_.reset();
  }

  void VerifyNoHierarchyConflictsReported(
      const sync_pb::ClientToServerMessage& message) {
    // Our request should have reported no hierarchy conflicts detected.
    const sync_pb::ClientStatus& client_status = message.client_status();
    EXPECT_TRUE(client_status.has_hierarchy_conflict_detected());
    EXPECT_FALSE(client_status.hierarchy_conflict_detected());
  }

  const std::string local_cache_guid() { return "lD16ebCGCZh+zkiZ68gWDw=="; }

  const std::string foreign_cache_guid() { return "kqyg7097kro6GSUod+GSg=="; }

  MockModelTypeProcessor* GetProcessor(ModelType model_type) {
    return &mock_model_type_processors_[model_type];
  }

  std::unique_ptr<DataTypeActivationResponse> MakeFakeActivationResponse(
      ModelType model_type) {
    auto response = std::make_unique<DataTypeActivationResponse>();
    response->model_type_state.set_initial_sync_done(true);
    response->model_type_state.mutable_progress_marker()->set_data_type_id(
        GetSpecificsFieldNumberFromModelType(model_type));
    response->type_processor = std::make_unique<ForwardingModelTypeProcessor>(
        GetProcessor(model_type));
    return response;
  }

  void EnableDatatype(ModelType model_type) {
    enabled_datatypes_.Put(model_type);
    model_type_registry_->ConnectDataType(
        model_type, MakeFakeActivationResponse(model_type));
    mock_server_->ExpectGetUpdatesRequestTypes(enabled_datatypes_);
  }

  void DisableDatatype(ModelType model_type) {
    enabled_datatypes_.Remove(model_type);
    model_type_registry_->DisconnectDataType(model_type);
    mock_server_->ExpectGetUpdatesRequestTypes(enabled_datatypes_);
  }

  // Configures SyncCycleContext and NudgeTracker so Syncer won't call
  // GetUpdates prior to Commit. This method can be used to ensure a Commit is
  // not preceeded by GetUpdates.
  void ConfigureNoGetUpdatesRequired() {
    nudge_tracker_.OnInvalidationsEnabled();
    nudge_tracker_.RecordSuccessfulSyncCycle(ProtocolTypes());

    ASSERT_FALSE(nudge_tracker_.IsGetUpdatesRequired(ProtocolTypes()));
  }

  base::test::SingleThreadTaskEnvironment task_environment_;

  FakeSyncEncryptionHandler encryption_handler_;
  scoped_refptr<ExtensionsActivity> extensions_activity_;
  std::unique_ptr<MockConnectionManager> mock_server_;
  CancelationSignal cancelation_signal_;
  std::map<ModelType, MockModelTypeProcessor> mock_model_type_processors_;

  Syncer* syncer_;

  std::unique_ptr<SyncCycle> cycle_;
  MockNudgeHandler mock_nudge_handler_;
  std::unique_ptr<ModelTypeRegistry> model_type_registry_;
  std::unique_ptr<SyncSchedulerImpl> scheduler_;
  std::unique_ptr<SyncCycleContext> context_;
  base::TimeDelta last_poll_interval_received_;
  base::TimeDelta last_sessions_commit_delay_;
  base::TimeDelta last_bookmarks_commit_delay_;
  int last_client_invalidation_hint_buffer_size_;

  ModelTypeSet enabled_datatypes_;
  NudgeTracker nudge_tracker_;
  std::unique_ptr<MockDebugInfoGetter> debug_info_getter_;

 private:
  DISALLOW_COPY_AND_ASSIGN(SyncerTest);
};

TEST_F(SyncerTest, CommitFiltersThrottledEntries) {
  const ModelTypeSet throttled_types(BOOKMARKS);

  GetProcessor(BOOKMARKS)->AppendCommitRequest(
      ClientTagHash::FromHashed("tag1"), MakeSpecifics(BOOKMARKS), "id1");

  // Sync without enabling bookmarks.
  mock_server_->ExpectGetUpdatesRequestTypes(
      Difference(context_->GetEnabledTypes(), throttled_types));
  ResetCycle();
  syncer_->NormalSyncShare(
      Difference(context_->GetEnabledTypes(), throttled_types), &nudge_tracker_,
      cycle_.get());

  // Nothing should have been committed as bookmarks is throttled.
  EXPECT_EQ(0, GetProcessor(BOOKMARKS)->GetLocalChangesCallCount());

  // Sync again with bookmarks enabled.
  mock_server_->ExpectGetUpdatesRequestTypes(context_->GetEnabledTypes());
  EXPECT_TRUE(SyncShareNudge());
  EXPECT_EQ(1, GetProcessor(BOOKMARKS)->GetLocalChangesCallCount());
}

TEST_F(SyncerTest, GetUpdatesPartialThrottled) {
  const sync_pb::EntitySpecifics bookmark = MakeSpecifics(BOOKMARKS);
  const sync_pb::EntitySpecifics pref = MakeSpecifics(PREFERENCES);

  // Normal sync, all the data types should get synced.
  mock_server_->AddUpdateSpecifics("1", "0", "A", 10, 10, true, 0, bookmark,
                                   foreign_cache_guid(), "-1");
  mock_server_->AddUpdateSpecifics("2", "1", "B", 10, 10, false, 2, bookmark,
                                   foreign_cache_guid(), "-2");
  mock_server_->AddUpdateSpecifics("3", "1", "C", 10, 10, false, 1, bookmark,
                                   foreign_cache_guid(), "-3");
  mock_server_->AddUpdateSpecifics("4", "0", "D", 10, 10, false, 0, pref);

  EXPECT_TRUE(SyncShareNudge());
  // Initial state. Everything is normal.
  ASSERT_EQ(1U, GetProcessor(BOOKMARKS)->GetNumUpdateResponses());
  ASSERT_EQ(3U, GetProcessor(BOOKMARKS)->GetNthUpdateResponse(0).size());
  ASSERT_EQ(1U, GetProcessor(PREFERENCES)->GetNumUpdateResponses());
  ASSERT_EQ(1U, GetProcessor(PREFERENCES)->GetNthUpdateResponse(0).size());

  // Set BOOKMARKS throttled but PREFERENCES not,
  // then BOOKMARKS should not get synced but PREFERENCES should.
  ModelTypeSet throttled_types(BOOKMARKS);
  mock_server_->set_throttling(true);
  mock_server_->SetPartialFailureTypes(throttled_types);

  mock_server_->AddUpdateSpecifics("1", "0", "E", 20, 20, true, 0, bookmark,
                                   foreign_cache_guid(), "-1");
  mock_server_->AddUpdateSpecifics("2", "1", "F", 20, 20, false, 2, bookmark,
                                   foreign_cache_guid(), "-2");
  mock_server_->AddUpdateSpecifics("3", "1", "G", 20, 20, false, 1, bookmark,
                                   foreign_cache_guid(), "-3");
  mock_server_->AddUpdateSpecifics("4", "0", "H", 20, 20, false, 0, pref);
  EXPECT_TRUE(SyncShareNudge());

  // PREFERENCES continues to work normally (not throttled).
  ASSERT_EQ(2U, GetProcessor(PREFERENCES)->GetNumUpdateResponses());
  // BOOKMARKS throttled.
  EXPECT_EQ(1U, GetProcessor(BOOKMARKS)->GetNumUpdateResponses());

  // Unthrottled BOOKMARKS, then BOOKMARKS should get synced now.
  mock_server_->set_throttling(false);

  mock_server_->AddUpdateSpecifics("1", "0", "E", 30, 30, true, 0, bookmark,
                                   foreign_cache_guid(), "-1");
  mock_server_->AddUpdateSpecifics("2", "1", "F", 30, 30, false, 2, bookmark,
                                   foreign_cache_guid(), "-2");
  mock_server_->AddUpdateSpecifics("3", "1", "G", 30, 30, false, 1, bookmark,
                                   foreign_cache_guid(), "-3");
  mock_server_->AddUpdateSpecifics("4", "0", "H", 30, 30, false, 0, pref);
  EXPECT_TRUE(SyncShareNudge());
  // BOOKMARKS unthrottled.
  EXPECT_EQ(2U, GetProcessor(BOOKMARKS)->GetNumUpdateResponses());
}

TEST_F(SyncerTest, GetUpdatesPartialFailure) {
  const sync_pb::EntitySpecifics bookmark = MakeSpecifics(BOOKMARKS);
  const sync_pb::EntitySpecifics pref = MakeSpecifics(PREFERENCES);

  // Normal sync, all the data types should get synced.
  mock_server_->AddUpdateSpecifics("1", "0", "A", 10, 10, true, 0, bookmark,
                                   foreign_cache_guid(), "-1");
  mock_server_->AddUpdateSpecifics("2", "1", "B", 10, 10, false, 2, bookmark,
                                   foreign_cache_guid(), "-2");
  mock_server_->AddUpdateSpecifics("3", "1", "C", 10, 10, false, 1, bookmark,
                                   foreign_cache_guid(), "-3");
  mock_server_->AddUpdateSpecifics("4", "0", "D", 10, 10, false, 0, pref);

  EXPECT_TRUE(SyncShareNudge());
  // Initial state. Everything is normal.
  ASSERT_EQ(1U, GetProcessor(BOOKMARKS)->GetNumUpdateResponses());
  ASSERT_EQ(3U, GetProcessor(BOOKMARKS)->GetNthUpdateResponse(0).size());
  ASSERT_EQ(1U, GetProcessor(PREFERENCES)->GetNumUpdateResponses());
  ASSERT_EQ(1U, GetProcessor(PREFERENCES)->GetNthUpdateResponse(0).size());

  // Set BOOKMARKS failure but PREFERENCES not,
  // then BOOKMARKS should not get synced but PREFERENCES should.
  ModelTypeSet failed_types(BOOKMARKS);
  mock_server_->set_partial_failure(true);
  mock_server_->SetPartialFailureTypes(failed_types);

  mock_server_->AddUpdateSpecifics("1", "0", "E", 20, 20, true, 0, bookmark,
                                   foreign_cache_guid(), "-1");
  mock_server_->AddUpdateSpecifics("2", "1", "F", 20, 20, false, 2, bookmark,
                                   foreign_cache_guid(), "-2");
  mock_server_->AddUpdateSpecifics("3", "1", "G", 20, 20, false, 1, bookmark,
                                   foreign_cache_guid(), "-3");
  mock_server_->AddUpdateSpecifics("4", "0", "H", 20, 20, false, 0, pref);
  EXPECT_TRUE(SyncShareNudge());

  // PREFERENCES continues to work normally (not throttled).
  ASSERT_EQ(2U, GetProcessor(PREFERENCES)->GetNumUpdateResponses());
  // BOOKMARKS failed.
  EXPECT_EQ(1U, GetProcessor(BOOKMARKS)->GetNumUpdateResponses());

  // Set BOOKMARKS not partial failed, then BOOKMARKS should get synced now.
  mock_server_->set_partial_failure(false);

  mock_server_->AddUpdateSpecifics("1", "0", "E", 30, 30, true, 0, bookmark,
                                   foreign_cache_guid(), "-1");
  mock_server_->AddUpdateSpecifics("2", "1", "F", 30, 30, false, 2, bookmark,
                                   foreign_cache_guid(), "-2");
  mock_server_->AddUpdateSpecifics("3", "1", "G", 30, 30, false, 1, bookmark,
                                   foreign_cache_guid(), "-3");
  mock_server_->AddUpdateSpecifics("4", "0", "H", 30, 30, false, 0, pref);
  EXPECT_TRUE(SyncShareNudge());
  // BOOKMARKS not failed.
  EXPECT_EQ(2U, GetProcessor(BOOKMARKS)->GetNumUpdateResponses());
}

TEST_F(SyncerTest, TestSimpleCommit) {
  const std::string kSyncId1 = "id1";
  const std::string kSyncId2 = "id2";

  GetProcessor(PREFERENCES)
      ->AppendCommitRequest(ClientTagHash::FromHashed("tag1"),
                            MakeSpecifics(PREFERENCES), kSyncId1);
  GetProcessor(PREFERENCES)
      ->AppendCommitRequest(ClientTagHash::FromHashed("tag2"),
                            MakeSpecifics(PREFERENCES), kSyncId2);

  EXPECT_TRUE(SyncShareNudge());
  EXPECT_THAT(mock_server_->committed_ids(),
              UnorderedElementsAre(kSyncId1, kSyncId2));
}

TEST_F(SyncerTest, TestSimpleGetUpdates) {
  std::string id = "some_id";
  std::string parent_id = "0";
  std::string name = "in_root";
  int64_t version = 10;
  int64_t timestamp = 10;
  mock_server_->AddUpdateDirectory(id, parent_id, name, version, timestamp,
                                   foreign_cache_guid(), "-1");

  EXPECT_TRUE(SyncShareNudge());

  ASSERT_EQ(1U, GetProcessor(BOOKMARKS)->GetNumUpdateResponses());
  std::vector<const UpdateResponseData*> updates_list =
      GetProcessor(BOOKMARKS)->GetNthUpdateResponse(0);
  EXPECT_EQ(1U, updates_list.size());

  const UpdateResponseData& update = *updates_list.back();
  const EntityData& entity = update.entity;

  EXPECT_EQ(id, entity.id);
  EXPECT_EQ(version, update.response_version);
  // Creation time hardcoded in MockConnectionManager::AddUpdateMeta().
  EXPECT_EQ(ProtoTimeToTime(1), entity.creation_time);
  EXPECT_EQ(ProtoTimeToTime(timestamp), entity.modification_time);
  EXPECT_EQ(name, entity.name);
  EXPECT_FALSE(entity.is_deleted());
}

// Committing more than kDefaultMaxCommitBatchSize items requires that
// we post more than one commit command to the server.  This test makes
// sure that scenario works as expected.
TEST_F(SyncerTest, CommitManyItemsInOneGo_Success) {
  int num_batches = 3;
  int items_to_commit = kDefaultMaxCommitBatchSize * num_batches;

  for (int i = 0; i < items_to_commit; i++) {
    GetProcessor(PREFERENCES)
        ->AppendCommitRequest(
            ClientTagHash::FromHashed(base::StringPrintf("tag%d", i)),
            MakeSpecifics(PREFERENCES));
  }

  EXPECT_TRUE(SyncShareNudge());
  EXPECT_EQ(static_cast<size_t>(num_batches),
            mock_server_->commit_messages().size());

  ASSERT_EQ(static_cast<size_t>(num_batches),
            GetProcessor(PREFERENCES)->GetNumCommitResponses());
  EXPECT_EQ(static_cast<size_t>(kDefaultMaxCommitBatchSize),
            GetProcessor(PREFERENCES)->GetNthCommitResponse(0).size());
  EXPECT_EQ(static_cast<size_t>(kDefaultMaxCommitBatchSize),
            GetProcessor(PREFERENCES)->GetNthCommitResponse(1).size());
  EXPECT_EQ(static_cast<size_t>(kDefaultMaxCommitBatchSize),
            GetProcessor(PREFERENCES)->GetNthCommitResponse(2).size());
}

// Test that a single failure to contact the server will cause us to exit the
// commit loop immediately.
TEST_F(SyncerTest, CommitManyItemsInOneGo_PostBufferFail) {
  int num_batches = 3;
  int items_to_commit = kDefaultMaxCommitBatchSize * num_batches;

  for (int i = 0; i < items_to_commit; i++) {
    GetProcessor(PREFERENCES)
        ->AppendCommitRequest(
            ClientTagHash::FromHashed(base::StringPrintf("tag%d", i)),
            MakeSpecifics(PREFERENCES));
  }

  // The second commit should fail.  It will be preceded by one successful
  // GetUpdate and one succesful commit.
  mock_server_->FailNthPostBufferToPathCall(3);
  base::HistogramTester histogram_tester;
  EXPECT_FALSE(SyncShareNudge());

  EXPECT_EQ(1U, mock_server_->commit_messages().size());
  EXPECT_EQ(
      SyncerError::SYNC_SERVER_ERROR,
      cycle_->status_controller().model_neutral_state().commit_result.value());

  // Since the second batch fails, the third one should not even be gathered.
  EXPECT_EQ(2, GetProcessor(PREFERENCES)->GetLocalChangesCallCount());

  histogram_tester.ExpectBucketCount("Sync.CommitResponse.PREFERENCE",
                                     SyncerError::SYNC_SERVER_ERROR,
                                     /*count=*/1);
  histogram_tester.ExpectBucketCount("Sync.CommitResponse",
                                     SyncerError::SYNC_SERVER_ERROR,
                                     /*count=*/1);
}

// Test that a single conflict response from the server will cause us to exit
// the commit loop immediately.
TEST_F(SyncerTest, CommitManyItemsInOneGo_CommitConflict) {
  int num_batches = 2;
  int items_to_commit = kDefaultMaxCommitBatchSize * num_batches;

  for (int i = 0; i < items_to_commit; i++) {
    GetProcessor(PREFERENCES)
        ->AppendCommitRequest(
            ClientTagHash::FromHashed(base::StringPrintf("tag%d", i)),
            MakeSpecifics(PREFERENCES));
  }

  // Return a CONFLICT response for the first item.
  mock_server_->set_conflict_n_commits(1);
  EXPECT_FALSE(SyncShareNudge());

  // We should stop looping at the first sign of trouble.
  EXPECT_EQ(1U, mock_server_->commit_messages().size());
  EXPECT_EQ(1, GetProcessor(PREFERENCES)->GetLocalChangesCallCount());
}

// Tests that sending debug info events works.
TEST_F(SyncerTest, SendDebugInfoEventsOnGetUpdates_HappyCase) {
  debug_info_getter_->AddDebugEvent();
  debug_info_getter_->AddDebugEvent();

  EXPECT_TRUE(SyncShareNudge());

  // Verify we received one GetUpdates request with two debug info events.
  EXPECT_EQ(1U, mock_server_->requests().size());
  ASSERT_TRUE(mock_server_->last_request().has_get_updates());
  EXPECT_EQ(2, mock_server_->last_request().debug_info().events_size());

  EXPECT_TRUE(SyncShareNudge());

  // See that we received another GetUpdates request, but that it contains no
  // debug info events.
  EXPECT_EQ(2U, mock_server_->requests().size());
  ASSERT_TRUE(mock_server_->last_request().has_get_updates());
  EXPECT_EQ(0, mock_server_->last_request().debug_info().events_size());

  debug_info_getter_->AddDebugEvent();

  EXPECT_TRUE(SyncShareNudge());

  // See that we received another GetUpdates request and it contains one debug
  // info event.
  EXPECT_EQ(3U, mock_server_->requests().size());
  ASSERT_TRUE(mock_server_->last_request().has_get_updates());
  EXPECT_EQ(1, mock_server_->last_request().debug_info().events_size());
}

// Tests that debug info events are dropped on server error.
TEST_F(SyncerTest, SendDebugInfoEventsOnGetUpdates_PostFailsDontDrop) {
  debug_info_getter_->AddDebugEvent();
  debug_info_getter_->AddDebugEvent();

  mock_server_->FailNextPostBufferToPathCall();
  EXPECT_FALSE(SyncShareNudge());

  // Verify we attempted to send one GetUpdates request with two debug info
  // events.
  EXPECT_EQ(1U, mock_server_->requests().size());
  ASSERT_TRUE(mock_server_->last_request().has_get_updates());
  EXPECT_EQ(2, mock_server_->last_request().debug_info().events_size());

  EXPECT_TRUE(SyncShareNudge());

  // See that the client resent the two debug info events.
  EXPECT_EQ(2U, mock_server_->requests().size());
  ASSERT_TRUE(mock_server_->last_request().has_get_updates());
  EXPECT_EQ(2, mock_server_->last_request().debug_info().events_size());

  // The previous send was successful so this next one shouldn't generate any
  // debug info events.
  EXPECT_TRUE(SyncShareNudge());
  EXPECT_EQ(3U, mock_server_->requests().size());
  ASSERT_TRUE(mock_server_->last_request().has_get_updates());
  EXPECT_EQ(0, mock_server_->last_request().debug_info().events_size());
}

// Tests that commit failure with conflict will trigger GetUpdates for next
// cycle of sync
TEST_F(SyncerTest, CommitFailureWithConflict) {
  ConfigureNoGetUpdatesRequired();

  GetProcessor(PREFERENCES)
      ->AppendCommitRequest(ClientTagHash::FromHashed("tag1"),
                            MakeSpecifics(PREFERENCES), "id1");

  EXPECT_FALSE(nudge_tracker_.IsGetUpdatesRequired(ProtocolTypes()));

  EXPECT_TRUE(SyncShareNudge());
  EXPECT_FALSE(nudge_tracker_.IsGetUpdatesRequired(ProtocolTypes()));

  GetProcessor(PREFERENCES)
      ->AppendCommitRequest(ClientTagHash::FromHashed("tag1"),
                            MakeSpecifics(PREFERENCES), "id1");

  mock_server_->set_conflict_n_commits(1);
  EXPECT_FALSE(SyncShareNudge());
  EXPECT_TRUE(nudge_tracker_.IsGetUpdatesRequired(ProtocolTypes()));

  nudge_tracker_.RecordSuccessfulSyncCycle(ProtocolTypes());
  EXPECT_FALSE(nudge_tracker_.IsGetUpdatesRequired(ProtocolTypes()));
}

// Tests that sending debug info events on Commit works.
TEST_F(SyncerTest, SendDebugInfoEventsOnCommit_HappyCase) {
  // Make sure GetUpdate isn't call as it would "steal" debug info events before
  // Commit has a chance to send them.
  ConfigureNoGetUpdatesRequired();

  // Generate a debug info event and trigger a commit.
  debug_info_getter_->AddDebugEvent();
  GetProcessor(PREFERENCES)
      ->AppendCommitRequest(ClientTagHash::FromHashed("tag1"),
                            MakeSpecifics(PREFERENCES), "id1");
  EXPECT_TRUE(SyncShareNudge());

  // Verify that the last request received is a Commit and that it contains a
  // debug info event.
  EXPECT_EQ(1U, mock_server_->requests().size());
  ASSERT_TRUE(mock_server_->last_request().has_commit());
  EXPECT_EQ(1, mock_server_->last_request().debug_info().events_size());

  // Generate another commit, but no debug info event.
  GetProcessor(PREFERENCES)
      ->AppendCommitRequest(ClientTagHash::FromHashed("tag2"),
                            MakeSpecifics(PREFERENCES), "id2");
  EXPECT_TRUE(SyncShareNudge());

  // See that it was received and contains no debug info events.
  EXPECT_EQ(2U, mock_server_->requests().size());
  ASSERT_TRUE(mock_server_->last_request().has_commit());
  EXPECT_EQ(0, mock_server_->last_request().debug_info().events_size());
}

// Tests that debug info events are not dropped on server error.
TEST_F(SyncerTest, SendDebugInfoEventsOnCommit_PostFailsDontDrop) {
  // Make sure GetUpdate isn't call as it would "steal" debug info events before
  // Commit has a chance to send them.
  ConfigureNoGetUpdatesRequired();

  mock_server_->FailNextPostBufferToPathCall();

  // Generate a debug info event and trigger a commit.
  debug_info_getter_->AddDebugEvent();
  GetProcessor(PREFERENCES)
      ->AppendCommitRequest(ClientTagHash::FromHashed("tag1"),
                            MakeSpecifics(PREFERENCES), "id1");
  EXPECT_FALSE(SyncShareNudge());

  // Verify that the last request sent is a Commit and that it contains a debug
  // info event.
  EXPECT_EQ(1U, mock_server_->requests().size());
  ASSERT_TRUE(mock_server_->last_request().has_commit());
  EXPECT_EQ(1, mock_server_->last_request().debug_info().events_size());

  // Try again. Because of how MockModelTypeProcessor works, commit data needs
  // to be provided again.
  GetProcessor(PREFERENCES)
      ->AppendCommitRequest(ClientTagHash::FromHashed("tag1"),
                            MakeSpecifics(PREFERENCES), "id1");
  EXPECT_TRUE(SyncShareNudge());

  // Verify that we've received another Commit and that it contains a debug info
  // event (just like the previous one).
  EXPECT_EQ(2U, mock_server_->requests().size());
  ASSERT_TRUE(mock_server_->last_request().has_commit());
  EXPECT_EQ(1, mock_server_->last_request().debug_info().events_size());

  // Generate another commit and try again.
  GetProcessor(PREFERENCES)
      ->AppendCommitRequest(ClientTagHash::FromHashed("tag2"),
                            MakeSpecifics(PREFERENCES), "id2");
  EXPECT_TRUE(SyncShareNudge());

  // See that it was received and contains no debug info events.
  EXPECT_EQ(3U, mock_server_->requests().size());
  ASSERT_TRUE(mock_server_->last_request().has_commit());
  EXPECT_EQ(0, mock_server_->last_request().debug_info().events_size());
}

TEST_F(SyncerTest, TestClientCommandDuringUpdate) {
  using sync_pb::ClientCommand;

  auto command = std::make_unique<ClientCommand>();
  command->set_set_sync_poll_interval(8);
  command->set_set_sync_long_poll_interval(800);
  command->set_sessions_commit_delay_seconds(3141);
  sync_pb::CustomNudgeDelay* bookmark_delay =
      command->add_custom_nudge_delays();
  bookmark_delay->set_datatype_id(
      GetSpecificsFieldNumberFromModelType(BOOKMARKS));
  bookmark_delay->set_delay_ms(950);
  command->set_client_invalidation_hint_buffer_size(11);
  mock_server_->AddUpdateDirectory("1", "0", "in_root", 1, 1,
                                   foreign_cache_guid(), "-1");
  mock_server_->SetGUClientCommand(std::move(command));
  EXPECT_TRUE(SyncShareNudge());

  EXPECT_EQ(base::TimeDelta::FromSeconds(8), last_poll_interval_received_);
  EXPECT_EQ(base::TimeDelta::FromSeconds(3141), last_sessions_commit_delay_);
  EXPECT_EQ(base::TimeDelta::FromMilliseconds(950),
            last_bookmarks_commit_delay_);
  EXPECT_EQ(11, last_client_invalidation_hint_buffer_size_);

  command = std::make_unique<ClientCommand>();
  command->set_set_sync_poll_interval(180);
  command->set_set_sync_long_poll_interval(190);
  command->set_sessions_commit_delay_seconds(2718);
  bookmark_delay = command->add_custom_nudge_delays();
  bookmark_delay->set_datatype_id(
      GetSpecificsFieldNumberFromModelType(BOOKMARKS));
  bookmark_delay->set_delay_ms(1050);
  command->set_client_invalidation_hint_buffer_size(9);
  mock_server_->AddUpdateDirectory("1", "0", "in_root", 1, 1,
                                   foreign_cache_guid(), "-1");
  mock_server_->SetGUClientCommand(std::move(command));
  EXPECT_TRUE(SyncShareNudge());

  EXPECT_EQ(base::TimeDelta::FromSeconds(180), last_poll_interval_received_);
  EXPECT_EQ(base::TimeDelta::FromSeconds(2718), last_sessions_commit_delay_);
  EXPECT_EQ(base::TimeDelta::FromMilliseconds(1050),
            last_bookmarks_commit_delay_);
  EXPECT_EQ(9, last_client_invalidation_hint_buffer_size_);
}

TEST_F(SyncerTest, TestClientCommandDuringCommit) {
  using sync_pb::ClientCommand;

  auto command = std::make_unique<ClientCommand>();
  command->set_set_sync_poll_interval(8);
  command->set_set_sync_long_poll_interval(800);
  command->set_sessions_commit_delay_seconds(3141);
  sync_pb::CustomNudgeDelay* bookmark_delay =
      command->add_custom_nudge_delays();
  bookmark_delay->set_datatype_id(
      GetSpecificsFieldNumberFromModelType(BOOKMARKS));
  bookmark_delay->set_delay_ms(950);
  command->set_client_invalidation_hint_buffer_size(11);
  GetProcessor(BOOKMARKS)->AppendCommitRequest(
      ClientTagHash::FromHashed("tag1"), MakeSpecifics(BOOKMARKS), "id1");
  mock_server_->SetCommitClientCommand(std::move(command));
  EXPECT_TRUE(SyncShareNudge());

  EXPECT_EQ(base::TimeDelta::FromSeconds(8), last_poll_interval_received_);
  EXPECT_EQ(base::TimeDelta::FromSeconds(3141), last_sessions_commit_delay_);
  EXPECT_EQ(base::TimeDelta::FromMilliseconds(950),
            last_bookmarks_commit_delay_);
  EXPECT_EQ(11, last_client_invalidation_hint_buffer_size_);

  command = std::make_unique<ClientCommand>();
  command->set_set_sync_poll_interval(180);
  command->set_set_sync_long_poll_interval(190);
  command->set_sessions_commit_delay_seconds(2718);
  bookmark_delay = command->add_custom_nudge_delays();
  bookmark_delay->set_datatype_id(
      GetSpecificsFieldNumberFromModelType(BOOKMARKS));
  bookmark_delay->set_delay_ms(1050);
  command->set_client_invalidation_hint_buffer_size(9);
  GetProcessor(BOOKMARKS)->AppendCommitRequest(
      ClientTagHash::FromHashed("tag2"), MakeSpecifics(BOOKMARKS), "id2");
  mock_server_->SetCommitClientCommand(std::move(command));
  EXPECT_TRUE(SyncShareNudge());

  EXPECT_EQ(base::TimeDelta::FromSeconds(180), last_poll_interval_received_);
  EXPECT_EQ(base::TimeDelta::FromSeconds(2718), last_sessions_commit_delay_);
  EXPECT_EQ(base::TimeDelta::FromMilliseconds(1050),
            last_bookmarks_commit_delay_);
  EXPECT_EQ(9, last_client_invalidation_hint_buffer_size_);
}

TEST_F(SyncerTest, ClientTagServerCreatedUpdatesWork) {
  mock_server_->AddUpdateDirectory("1", "0", "permitem1", 1, 10,
                                   foreign_cache_guid(), "-1");
  mock_server_->SetLastUpdateClientTag("clienttag");

  EXPECT_TRUE(SyncShareNudge());

  ASSERT_EQ(1U, GetProcessor(BOOKMARKS)->GetNumUpdateResponses());
  std::vector<const UpdateResponseData*> updates_list =
      GetProcessor(BOOKMARKS)->GetNthUpdateResponse(0);
  EXPECT_EQ(1U, updates_list.size());

  const UpdateResponseData& update = *updates_list.back();
  const EntityData& entity = update.entity;

  EXPECT_EQ("permitem1", entity.name);
  EXPECT_EQ(ClientTagHash::FromHashed("clienttag"), entity.client_tag_hash);
  EXPECT_FALSE(entity.is_deleted());
}

TEST_F(SyncerTest, GetUpdatesSetsRequestedTypes) {
  // The expectations of this test happen in the MockConnectionManager's
  // GetUpdates handler.  EnableDatatype sets the expectation value from our
  // set of enabled/disabled datatypes.
  EXPECT_TRUE(SyncShareNudge());
  EXPECT_EQ(1, mock_server_->GetAndClearNumGetUpdatesRequests());

  EnableDatatype(AUTOFILL);
  EXPECT_TRUE(SyncShareNudge());
  EXPECT_EQ(1, mock_server_->GetAndClearNumGetUpdatesRequests());

  DisableDatatype(BOOKMARKS);
  EXPECT_TRUE(SyncShareNudge());
  EXPECT_EQ(1, mock_server_->GetAndClearNumGetUpdatesRequests());

  DisableDatatype(AUTOFILL);
  EXPECT_TRUE(SyncShareNudge());
  EXPECT_EQ(1, mock_server_->GetAndClearNumGetUpdatesRequests());

  DisableDatatype(PREFERENCES);
  EnableDatatype(AUTOFILL);
  EXPECT_TRUE(SyncShareNudge());
  EXPECT_EQ(1, mock_server_->GetAndClearNumGetUpdatesRequests());
}

// A typical scenario: server and client each have one update for the other.
// This is the "happy path" alternative to UpdateFailsThenDontCommit.
TEST_F(SyncerTest, UpdateThenCommit) {
  std::string to_receive = "some_id1";
  std::string to_commit = "some_id2";
  std::string parent_id = "0";
  mock_server_->AddUpdateDirectory(to_receive, parent_id, "x", 1, 10,
                                   foreign_cache_guid(), "-1");
  GetProcessor(BOOKMARKS)->AppendCommitRequest(
      ClientTagHash::FromHashed("tag1"), MakeSpecifics(BOOKMARKS), to_commit);

  EXPECT_TRUE(SyncShareNudge());

  // The sync cycle should have included a GetUpdate, then a commit.
  EXPECT_TRUE(mock_server_->last_request().has_commit());
  EXPECT_THAT(mock_server_->committed_ids(), UnorderedElementsAre(to_commit));

  // The update should have been received.
  ASSERT_EQ(1U, GetProcessor(BOOKMARKS)->GetNumUpdateResponses());
  std::vector<const UpdateResponseData*> updates_list =
      GetProcessor(BOOKMARKS)->GetNthUpdateResponse(0);
  ASSERT_EQ(1U, updates_list.size());
  EXPECT_EQ(to_receive, updates_list[0]->entity.id);
}

// Same as above, but this time we fail to download updates.
// We should not attempt to commit anything unless we successfully downloaded
// updates, otherwise we risk causing a server-side conflict.
TEST_F(SyncerTest, UpdateFailsThenDontCommit) {
  std::string to_receive = "some_id1";
  std::string to_commit = "some_id2";
  std::string parent_id = "0";
  mock_server_->AddUpdateDirectory(to_receive, parent_id, "x", 1, 10,
                                   foreign_cache_guid(), "-1");
  GetProcessor(BOOKMARKS)->AppendCommitRequest(
      ClientTagHash::FromHashed("tag1"), MakeSpecifics(BOOKMARKS), to_commit);

  mock_server_->FailNextPostBufferToPathCall();
  EXPECT_FALSE(SyncShareNudge());

  // We did not receive this update.
  EXPECT_EQ(0U, GetProcessor(BOOKMARKS)->GetNumUpdateResponses());

  // No commit should have been sent.
  EXPECT_FALSE(mock_server_->last_request().has_commit());
  EXPECT_THAT(mock_server_->committed_ids(), IsEmpty());

  // Inform the Mock we won't be fetching all updates.
  mock_server_->ClearUpdatesQueue();
}

// Downloads two updates successfully.
// This is the "happy path" alternative to ConfigureFailsDontApplyUpdates.
TEST_F(SyncerTest, ConfigureDownloadsTwoBatchesSuccess) {
  // Construct the first GetUpdates response.
  mock_server_->AddUpdatePref("id1", "", "one", 1, 10);
  mock_server_->SetChangesRemaining(1);
  mock_server_->NextUpdateBatch();

  // Construct the second GetUpdates response.
  mock_server_->AddUpdatePref("id2", "", "two", 2, 20);

  ASSERT_EQ(0U, GetProcessor(PREFERENCES)->GetNumUpdateResponses());

  SyncShareConfigure();

  // The type should have received the initial updates.
  EXPECT_EQ(1U, GetProcessor(PREFERENCES)->GetNumUpdateResponses());
}

// Same as the above case, but this time the second batch fails to download.
TEST_F(SyncerTest, ConfigureFailsDontApplyUpdates) {
  // The scenario: we have two batches of updates with one update each.  A
  // normal confgure step would download all the updates one batch at a time and
  // apply them.  This configure will succeed in downloading the first batch
  // then fail when downloading the second.
  mock_server_->FailNthPostBufferToPathCall(2);

  // Construct the first GetUpdates response.
  mock_server_->AddUpdatePref("id1", "", "one", 1, 10);
  mock_server_->SetChangesRemaining(1);
  mock_server_->NextUpdateBatch();

  // Construct the second GetUpdates response.
  mock_server_->AddUpdatePref("id2", "", "two", 2, 20);

  ASSERT_EQ(0U, GetProcessor(PREFERENCES)->GetNumUpdateResponses());

  SyncShareConfigure();

  // The processor should not have received the initial sync data.
  EXPECT_EQ(0U, GetProcessor(PREFERENCES)->GetNumUpdateResponses());

  // One update remains undownloaded.
  mock_server_->ClearUpdatesQueue();
}

// Tests that if type is not registered with ModelTypeRegistry (e.g. because
// type's LoadModels failed), Syncer::ConfigureSyncShare runs without triggering
// DCHECK.
TEST_F(SyncerTest, ConfigureFailedUnregisteredType) {
  // Simulate type being unregistered before configuration by including type
  // that isn't registered with ModelTypeRegistry.
  SyncShareConfigureTypes(ModelTypeSet(APPS));

  // No explicit verification, DCHECK shouldn't have been triggered.
}

TEST_F(SyncerTest, GetKeySuccess) {
  KeystoreKeysHandler* keystore_keys_handler =
      model_type_registry_->keystore_keys_handler();
  EXPECT_TRUE(keystore_keys_handler->NeedKeystoreKey());

  SyncShareConfigure();

  EXPECT_EQ(SyncerError::SYNCER_OK,
            cycle_->status_controller().last_get_key_result().value());
  EXPECT_FALSE(keystore_keys_handler->NeedKeystoreKey());
}

TEST_F(SyncerTest, GetKeyEmpty) {
  KeystoreKeysHandler* keystore_keys_handler =
      model_type_registry_->keystore_keys_handler();
  EXPECT_TRUE(keystore_keys_handler->NeedKeystoreKey());

  mock_server_->SetKeystoreKey(std::string());
  SyncShareConfigure();

  EXPECT_NE(SyncerError::SYNCER_OK,
            cycle_->status_controller().last_get_key_result().value());
  EXPECT_TRUE(keystore_keys_handler->NeedKeystoreKey());
}

// Verify that commit only types are never requested in GetUpdates, but still
// make it into the commit messages. Additionally, make sure failing GU types
// are correctly removed before commit.
TEST_F(SyncerTest, CommitOnlyTypes) {
  mock_server_->set_partial_failure(true);
  mock_server_->SetPartialFailureTypes(ModelTypeSet(PREFERENCES));

  EnableDatatype(USER_EVENTS);

  GetProcessor(PREFERENCES)
      ->AppendCommitRequest(ClientTagHash::FromHashed("tag1"),
                            MakeSpecifics(PREFERENCES), "id1");
  GetProcessor(EXTENSIONS)
      ->AppendCommitRequest(ClientTagHash::FromHashed("tag2"),
                            MakeSpecifics(EXTENSIONS), "id2");
  GetProcessor(USER_EVENTS)
      ->AppendCommitRequest(ClientTagHash::FromHashed("tag3"),
                            MakeSpecifics(USER_EVENTS), "id3");

  EXPECT_TRUE(SyncShareNudge());

  ASSERT_EQ(2U, mock_server_->requests().size());
  ASSERT_TRUE(mock_server_->requests()[0].has_get_updates());
  // MockConnectionManager will ensure USER_EVENTS was not included in the GU.
  EXPECT_EQ(
      4, mock_server_->requests()[0].get_updates().from_progress_marker_size());

  ASSERT_TRUE(mock_server_->requests()[1].has_commit());
  const sync_pb::CommitMessage commit = mock_server_->requests()[1].commit();
  EXPECT_EQ(2, commit.entries_size());
  EXPECT_TRUE(commit.entries(0).specifics().has_extension());
  EXPECT_TRUE(commit.entries(1).specifics().has_user_event());
}

enum {
  TEST_PARAM_BOOKMARK_ENABLE_BIT,
  TEST_PARAM_AUTOFILL_ENABLE_BIT,
  TEST_PARAM_BIT_COUNT
};

class MixedResult : public SyncerTest,
                    public ::testing::WithParamInterface<int> {
 protected:
  bool ShouldFailBookmarkCommit() {
    return (GetParam() & (1 << TEST_PARAM_BOOKMARK_ENABLE_BIT)) == 0;
  }
  bool ShouldFailAutofillCommit() {
    return (GetParam() & (1 << TEST_PARAM_AUTOFILL_ENABLE_BIT)) == 0;
  }
};

INSTANTIATE_TEST_SUITE_P(ExtensionsActivity,
                         MixedResult,
                         testing::Range(0, 1 << TEST_PARAM_BIT_COUNT));

TEST_P(MixedResult, ExtensionsActivity) {
  GetProcessor(PREFERENCES)
      ->AppendCommitRequest(ClientTagHash::FromHashed("pref1"),
                            MakeSpecifics(PREFERENCES), "prefid1");
  GetProcessor(BOOKMARKS)->AppendCommitRequest(
      ClientTagHash::FromHashed("bookmark1"), MakeSpecifics(BOOKMARKS),
      "bookmarkid2");

  if (ShouldFailBookmarkCommit()) {
    mock_server_->SetTransientErrorId("bookmarkid2");
  }

  if (ShouldFailAutofillCommit()) {
    mock_server_->SetTransientErrorId("prefid1");
  }

  // Put some extensions activity records into the monitor.
  {
    ExtensionsActivity::Records records;
    records["ABC"].extension_id = "ABC";
    records["ABC"].bookmark_write_count = 2049U;
    records["xyz"].extension_id = "xyz";
    records["xyz"].bookmark_write_count = 4U;
    context_->extensions_activity()->PutRecords(records);
  }

  EXPECT_EQ(!ShouldFailBookmarkCommit() && !ShouldFailAutofillCommit(),
            SyncShareNudge());

  ExtensionsActivity::Records final_monitor_records;
  context_->extensions_activity()->GetAndClearRecords(&final_monitor_records);
  if (ShouldFailBookmarkCommit()) {
    ASSERT_EQ(2U, final_monitor_records.size())
        << "Should restore records after unsuccessful bookmark commit.";
    EXPECT_EQ("ABC", final_monitor_records["ABC"].extension_id);
    EXPECT_EQ("xyz", final_monitor_records["xyz"].extension_id);
    EXPECT_EQ(2049U, final_monitor_records["ABC"].bookmark_write_count);
    EXPECT_EQ(4U, final_monitor_records["xyz"].bookmark_write_count);
  } else {
    EXPECT_TRUE(final_monitor_records.empty())
        << "Should not restore records after successful bookmark commit.";
  }
}

}  // namespace syncer
