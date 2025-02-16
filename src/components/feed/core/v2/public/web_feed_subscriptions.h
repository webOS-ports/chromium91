// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_FEED_CORE_V2_PUBLIC_WEB_FEED_SUBSCRIPTIONS_H_
#define COMPONENTS_FEED_CORE_V2_PUBLIC_WEB_FEED_SUBSCRIPTIONS_H_

#include <string>
#include "base/callback.h"
#include "components/feed/core/v2/public/types.h"

namespace feed {

// API to access Web Feed subscriptions.
class WebFeedSubscriptions {
 public:
  struct FollowWebFeedResult {
    WebFeedSubscriptionRequestStatus request_status =
        WebFeedSubscriptionRequestStatus::kUnknown;
    // If followed, the metadata for the followed feed.
    WebFeedMetadata web_feed_metadata;
  };
  // Follow a web feed given information about a web page. Calls `callback` when
  // complete. The callback parameter reports whether the url is now considered
  // followed.
  virtual void FollowWebFeed(
      const WebFeedPageInformation& page_info,
      base::OnceCallback<void(FollowWebFeedResult)> callback) = 0;

  // Follow a web feed given a web feed ID.
  virtual void FollowWebFeed(
      const std::string& web_feed_id,
      base::OnceCallback<void(FollowWebFeedResult)> callback) = 0;

  struct UnfollowWebFeedResult {
    WebFeedSubscriptionRequestStatus request_status =
        WebFeedSubscriptionRequestStatus::kUnknown;
  };

  // Follow a web feed given a URL. Calls `callback` when complete. The callback
  // parameter reports whether the url is now considered followed.
  virtual void UnfollowWebFeed(
      const std::string& web_feed_id,
      base::OnceCallback<void(UnfollowWebFeedResult)> callback) = 0;

  // Web Feed lookup for pages. These functions fetch `WebFeedMetadata` for any
  // web feed which is recommended by the server, currently subscribed, or was
  // recently subscribed. `callback` is given a nullptr if no web feed data is
  // found.

  // Look up web feed information for a web page.
  virtual void FindWebFeedInfoForPage(
      const WebFeedPageInformation& page_info,
      base::OnceCallback<void(WebFeedMetadata)> callback) = 0;

  // Look up web feed information for a web page given the `web_feed_id`.
  virtual void FindWebFeedInfoForWebFeedId(
      const std::string& web_feed_id,
      base::OnceCallback<void(WebFeedMetadata)> callback) = 0;

  // Returns all current subscriptions.
  virtual void GetAllSubscriptions(
      base::OnceCallback<void(std::vector<WebFeedMetadata>)> callback) = 0;
};

}  // namespace feed
#endif  // COMPONENTS_FEED_CORE_V2_PUBLIC_WEB_FEED_SUBSCRIPTIONS_H_
