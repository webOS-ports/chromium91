// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_FEED_CORE_V2_TEST_STREAM_BUILDER_H_
#define COMPONENTS_FEED_CORE_V2_TEST_STREAM_BUILDER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "components/feed/core/proto/v2/store.pb.h"
#include "components/feed/core/v2/proto_util.h"
#include "components/feed/core/v2/protocol_translator.h"
#include "components/feed/core/v2/types.h"

// Functions that help build a feedstore::StreamStructure for testing.
namespace feed {
struct StreamModelUpdateRequest;

extern base::Time kTestTimeEpoch;

ContentId MakeContentId(ContentId::Type type,
                        std::string content_domain,
                        int id_number);
ContentId MakeClusterId(int id_number);
ContentId MakeContentContentId(int id_number);
ContentId MakeSharedStateContentId(int id_number);
ContentId MakeRootId(int id_number = 0);
ContentId MakeSharedStateId(int id_number = 0);
feedstore::StreamStructure MakeStream(int id_number = 0);
feedstore::StreamStructure MakeCluster(int id_number, ContentId parent);
feedstore::StreamStructure MakeContentNode(int id_number, ContentId parent);
feedstore::StreamSharedState MakeSharedState(int id_number);
feedstore::StreamStructure MakeRemove(ContentId id);
feedstore::StreamStructure MakeClearAll();
feedstore::Content MakeContent(int id_number);
feedstore::DataOperation MakeOperation(feedstore::StreamStructure structure);
feedstore::DataOperation MakeOperation(feedstore::Content content);
feedstore::Record MakeRecord(feedstore::Content content);
feedstore::Record MakeRecord(
    feedstore::StreamStructureSet stream_structure_set);
feedstore::Record MakeRecord(feedstore::StreamSharedState shared_state);
feedstore::Record MakeRecord(feedstore::StreamData stream_data);

// Helper structure to configure and return RefreshResponseData and
// StreamModelUpdateRequest objects denoting typical initial and next page
// refresh response payloads.
struct StreamModelUpdateRequestGenerator {
  base::Time last_added_time = kTestTimeEpoch;
  bool signed_in = true;
  bool logging_enabled = true;
  bool privacy_notice_fulfilled = true;

  StreamModelUpdateRequestGenerator();
  ~StreamModelUpdateRequestGenerator();

  std::unique_ptr<StreamModelUpdateRequest> MakeFirstPage(
      int first_cluster_id = 0) const;

  std::unique_ptr<StreamModelUpdateRequest> MakeNextPage(
      int page_number = 2,
      StreamModelUpdateRequest::Source source =
          StreamModelUpdateRequest::Source::kInitialLoadFromStore) const;
};

// Returns data operations to create a typical stream:
// Root
// |-Cluster 0
// |  |-Content 0
// |-Cluster 1
//    |-Content 1
std::vector<feedstore::DataOperation> MakeTypicalStreamOperations();
std::unique_ptr<StreamModelUpdateRequest> MakeTypicalInitialModelState(
    int first_cluster_id = 0,
    base::Time last_added_time = kTestTimeEpoch,
    bool signed_in = true,
    bool logging_enabled = true,
    bool privacy_notice_fulfilled = true);
// Root
// |-Cluster 2
// |  |-Content 2
// |-Cluster 3
//    |-Content 3
std::unique_ptr<StreamModelUpdateRequest> MakeTypicalNextPageState(
    int page_number = 2,
    base::Time last_added_time = kTestTimeEpoch,
    bool signed_in = true,
    bool logging_enabled = true,
    bool privacy_notice_fulfilled = true,
    StreamModelUpdateRequest::Source source =
        StreamModelUpdateRequest::Source::kNetworkLoadMore);

feedstore::WebFeedInfo MakeWebFeedInfo(const std::string& name);

}  // namespace feed

#endif  // COMPONENTS_FEED_CORE_V2_TEST_STREAM_BUILDER_H_
