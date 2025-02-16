// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/callback_helpers.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/metrics/user_action_tester.h"
#include "components/feed/core/v2/api_test/feed_api_test.h"
#include "components/feed/core/v2/config.h"
#include "components/feed/core/v2/feed_stream.h"
#include "components/feed/core/v2/test/callback_receiver.h"
#include "components/feed/feed_feature_list.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace feed {
namespace test {
namespace {

class FeedStreamConditionalActionsUploadTest : public FeedApiTest {
  void SetupFeatures() override {
    scoped_feature_list_.InitAndEnableFeature(
        feed::kInterestFeedV2ClicksAndViewsConditionalUpload);
  }
};

TEST_F(FeedApiTest, LoadStreamSendsNoticeCardAcknowledgement) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitAndEnableFeature(
      feed::kInterestFeedNoticeCardAutoDismiss);

  store_->OverwriteStream(kForYouStream, MakeTypicalInitialModelState(),
                          base::DoNothing());
  TestForYouSurface surface(stream_.get());
  WaitForIdleTaskQueue();

  // Generate 3 view actions and 1 click action to trigger the acknowledgement
  // of the notice card.
  const int notice_card_index = 0;
  std::string slice_id =
      surface.initial_state->updated_slices(notice_card_index)
          .slice()
          .slice_id();
  stream_->ReportSliceViewed(surface.GetSurfaceId(), surface.GetStreamType(),
                             slice_id);
  stream_->ReportSliceViewed(surface.GetSurfaceId(), surface.GetStreamType(),
                             slice_id);
  stream_->ReportSliceViewed(surface.GetSurfaceId(), surface.GetStreamType(),
                             slice_id);
  stream_->ReportOpenAction(surface.GetStreamType(), slice_id);

  response_translator_.InjectResponse(MakeTypicalInitialModelState());
  stream_->UnloadModel(surface.GetStreamType());
  stream_->ExecuteRefreshTask(RefreshTaskId::kRefreshForYouFeed);
  WaitForIdleTaskQueue();

  EXPECT_TRUE(network_.query_request_sent->feed_request()
                  .feed_query()
                  .chrome_fulfillment_info()
                  .notice_card_acknowledged());
}

TEST_F(FeedStreamConditionalActionsUploadTest,
       NoActionsUploadUntilReachedConditions) {
  // Tests the flow where we:
  //   (1) Perform a ThereAndBackAgain action and a View action while upload
  //   isn't enabled => (2) Attempt an upload while the upload conditions aren't
  //   reached => (3) Reach upload conditions => (4) Perform another View action
  //   that should be dropped => (5) Simulate the backgrounding of the app to
  //   enable actions upload => (6) Trigger an upload which will upload the
  //   stored ThereAndBackAgain action.
  response_translator_.InjectResponse(MakeTypicalInitialModelState());
  TestForYouSurface surface(stream_.get());
  WaitForIdleTaskQueue();

  // Process the view action and the ThereAndBackAgain action while the upload
  // conditions aren't reached.
  stream_->ProcessViewAction(MakeFeedAction(42ul).SerializeAsString());
  WaitForIdleTaskQueue();
  // Verify that the view action was dropped.
  ASSERT_EQ(0ul, ReadStoredActions(stream_->GetStore()).size());

  stream_->ProcessThereAndBackAgain(
      MakeThereAndBackAgainData(42ul).SerializeAsString());
  WaitForIdleTaskQueue();
  // Verify that the ThereAndBackAgain action is in the action store.
  ASSERT_EQ(1ul, ReadStoredActions(stream_->GetStore()).size());

  // Attempt an upload.
  stream_->OnEnterBackground();
  WaitForIdleTaskQueue();
  // Verify that no upload is done because the conditions aren't reached.
  EXPECT_EQ(0, network_.GetActionRequestCount());

  // Reach conditions.
  stream_->ReportSliceViewed(
      surface.GetSurfaceId(), surface.GetStreamType(),
      surface.initial_state->updated_slices(1).slice().slice_id());

  // Verify that the view action is still dropped because we haven't
  // transitioned out of the current surface.
  stream_->ProcessViewAction(MakeFeedAction(42ul).SerializeAsString());
  WaitForIdleTaskQueue();
  ASSERT_EQ(1ul, ReadStoredActions(stream_->GetStore()).size());

  // Enable the upload bit and trigger the upload of actions.
  surface.Detach();
  stream_->OnEnterBackground();
  WaitForIdleTaskQueue();

  // Verify that the ThereAndBackAgain action was uploaded but not the view
  // action.
  ASSERT_EQ(1, network_.GetActionRequestCount());
  EXPECT_EQ(1, network_.GetActionRequestSent()->feed_actions_size());
}

TEST_F(FeedStreamConditionalActionsUploadTest, EnableUploadOnSurfaceAttached) {
  response_translator_.InjectResponse(MakeTypicalInitialModelState());
  TestForYouSurface surface(stream_.get());
  WaitForIdleTaskQueue();

  // Perform a ThereAndBackAgain action.
  stream_->ProcessThereAndBackAgain(
      MakeThereAndBackAgainData(42ul).SerializeAsString());
  WaitForIdleTaskQueue();

  // Reach conditions.
  stream_->ReportSliceViewed(
      surface.GetSurfaceId(), surface.GetStreamType(),
      surface.initial_state->updated_slices(1).slice().slice_id());

  // Attach a new surface to update the bit to enable uploads.
  TestForYouSurface surface2(stream_.get());

  // Trigger an upload through load more to isolate the effect of the on-attach
  // event on enabling uploads.
  response_translator_.InjectResponse(MakeTypicalNextPageState());
  stream_->LoadMore(surface, base::DoNothing());
  WaitForIdleTaskQueue();

  // Verify that the ThereAndBackAgain action was uploaded.
  ASSERT_EQ(1, network_.GetActionRequestCount());
  EXPECT_EQ(1, network_.GetActionRequestSent()->feed_actions_size());
}

TEST_F(FeedStreamConditionalActionsUploadTest, EnableUploadOnEnterBackground) {
  response_translator_.InjectResponse(MakeTypicalInitialModelState());
  TestForYouSurface surface(stream_.get());
  WaitForIdleTaskQueue();

  // Perform a ThereAndBackAgain action.
  stream_->ProcessThereAndBackAgain(
      MakeThereAndBackAgainData(42ul).SerializeAsString());
  WaitForIdleTaskQueue();

  // Reach conditions.
  stream_->ReportSliceViewed(
      surface.GetSurfaceId(), surface.GetStreamType(),
      surface.initial_state->updated_slices(1).slice().slice_id());

  surface.Detach();
  stream_->OnEnterBackground();
  WaitForIdleTaskQueue();

  // Verify that the ThereAndBackAgain action was uploaded.
  ASSERT_EQ(1, network_.GetActionRequestCount());
  EXPECT_EQ(1, network_.GetActionRequestSent()->feed_actions_size());
}

TEST_F(FeedStreamConditionalActionsUploadTest,
       AllowActionsUploadWhenNoticeCardNotPresentRegardlessOfConditions) {
  auto model_state = MakeTypicalInitialModelState();
  model_state->stream_data.set_privacy_notice_fulfilled(false);
  response_translator_.InjectResponse(std::move(model_state));
  TestForYouSurface surface(stream_.get());
  WaitForIdleTaskQueue();

  // Process the view action and the ThereAndBackAgain action while the upload
  // conditions aren't reached.
  stream_->ProcessViewAction(MakeFeedAction(42ul).SerializeAsString());
  WaitForIdleTaskQueue();
  stream_->ProcessThereAndBackAgain(
      MakeThereAndBackAgainData(42ul).SerializeAsString());
  WaitForIdleTaskQueue();

  // Trigger an upload through a query.
  surface.Detach();
  stream_->OnEnterBackground();
  WaitForIdleTaskQueue();

  // Verify the ThereAndBackAgain action and the view action were uploaded.
  ASSERT_EQ(1, network_.GetActionRequestCount());
  EXPECT_EQ(2, network_.GetActionRequestSent()->feed_actions_size());
}

TEST_F(FeedStreamConditionalActionsUploadTest,
       ResetTheUploadEnableBitsOnClearAll) {
  response_translator_.InjectResponse(MakeTypicalInitialModelState());
  TestForYouSurface surface(stream_.get());
  WaitForIdleTaskQueue();

  // Reach conditions.
  stream_->ReportSliceViewed(
      surface.GetSurfaceId(), surface.GetStreamType(),
      surface.initial_state->updated_slices(1).slice().slice_id());
  surface.Detach();
  stream_->OnEnterBackground();
  ASSERT_TRUE(stream_->CanUploadActions());

  // Trigger a ClearAll, and ensure actions cannot be uploaded until conditions
  // are reached again.
  stream_->OnSignedOut();
  WaitForIdleTaskQueue();
  ASSERT_FALSE(stream_->CanUploadActions());
}

TEST_F(FeedApiTest, LoadStreamUpdateNoticeCardFulfillmentHistogram) {
  base::HistogramTester histograms;

  // Trigger a stream refresh that updates the histogram.
  {
    auto model_state = MakeTypicalInitialModelState();
    model_state->stream_data.set_privacy_notice_fulfilled(false);
    response_translator_.InjectResponse(std::move(model_state));

    refresh_scheduler_.Clear();
    stream_->ExecuteRefreshTask(RefreshTaskId::kRefreshForYouFeed);
    WaitForIdleTaskQueue();
  }

  UnloadModel(kForYouStream);

  // Trigger another stream refresh that updates the histogram.
  {
    auto model_state = MakeTypicalInitialModelState();
    model_state->stream_data.set_privacy_notice_fulfilled(true);
    response_translator_.InjectResponse(std::move(model_state));

    refresh_scheduler_.Clear();
    stream_->ExecuteRefreshTask(RefreshTaskId::kRefreshForYouFeed);
    WaitForIdleTaskQueue();
  }

  // Verify that the notice card fulfillment histogram was properly recorded.
  histograms.ExpectBucketCount("ContentSuggestions.Feed.NoticeCardFulfilled2",
                               0, 1);
  histograms.ExpectBucketCount("ContentSuggestions.Feed.NoticeCardFulfilled2",
                               1, 1);
}

TEST_F(FeedStreamConditionalActionsUploadTest,
       DontTriggerActionsUploadWhenWasNotSignedIn) {
  auto update_request = MakeTypicalInitialModelState();
  update_request->stream_data.set_signed_in(false);
  response_translator_.InjectResponse(std::move(update_request));
  TestForYouSurface surface(stream_.get());
  WaitForIdleTaskQueue();

  // Try to reach conditions.
  stream_->ReportSliceViewed(
      surface.GetSurfaceId(), surface.GetStreamType(),
      surface.initial_state->updated_slices(1).slice().slice_id());

  // Try to trigger an upload through a query.
  stream_->OnEnterBackground();
  WaitForIdleTaskQueue();

  // Verify that even if the conditions were reached, the pref that enables the
  // upload wasn't set to true because the latest refresh request wasn't signed
  // in.
  ASSERT_FALSE(stream_->CanUploadActions());
}

TEST_F(FeedStreamConditionalActionsUploadTest,
       LoadMoreDoesntUpdateNoticeCardPrefAndHistogram) {
  // The initial stream load has the notice card.
  response_translator_.InjectResponse(MakeTypicalInitialModelState());
  TestForYouSurface surface(stream_.get());
  WaitForIdleTaskQueue();

  // Inject a response for the LoadMore fetch that doesn't have the notice card.
  // It shouldn't overwrite the notice card pref.
  response_translator_.InjectResponse(MakeTypicalNextPageState(
      /* page_number= */ 0,
      /* last_added_time= */ kTestTimeEpoch,
      /* signed_in= */ true,
      /* logging_enabled= */ false));

  // Start tracking histograms after the initial stream load to isolate the
  // effect of load more.
  base::HistogramTester histograms;

  stream_->LoadMore(surface, base::DoNothing());
  WaitForIdleTaskQueue();

  // Process a view action that should be dropped because the upload of actions
  // is still disabled because there is still a notice card.
  stream_->ProcessViewAction(MakeFeedAction(42ul).SerializeAsString());
  WaitForIdleTaskQueue();

  // Trigger an upload.
  stream_->OnEnterBackground();
  WaitForIdleTaskQueue();

  // Verify that there were no uploads.
  EXPECT_EQ(0, network_.GetActionRequestCount());

  // Verify that the notice card fulfillment histogram isn't recorded for load
  // more.
  histograms.ExpectTotalCount("ContentSuggestions.Feed.NoticeCardFulfilled2",
                              0);
}

}  // namespace
}  // namespace test
}  // namespace feed
