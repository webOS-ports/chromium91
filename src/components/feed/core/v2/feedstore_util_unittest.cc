// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/feed/core/v2/feedstore_util.h"

#include <string>
#include "base/test/task_environment.h"
#include "components/feed/core/v2/config.h"
#include "components/feed/core/v2/test/test_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace feedstore {
namespace {
base::Time kTestTimeEpoch = base::Time::UnixEpoch();
const base::Time kExpiryTime1 = kTestTimeEpoch + base::TimeDelta::FromHours(2);

const std::string Token1() {
  return "token1";
}
const std::string Token2() {
  return "token2";
}

TEST(feedstore_util_test, SetSessionId) {
  Metadata metadata;

  // Verify that directly calling SetSessionId works as expected.
  SetSessionId(metadata, Token1(), kExpiryTime1);

  EXPECT_EQ(Token1(), metadata.session_id().token());
  EXPECT_TIME_EQ(kExpiryTime1, GetSessionIdExpiryTime(metadata));
}

TEST(feedstore_util_test, MaybeUpdateSessionId) {
  base::test::TaskEnvironment task_environment{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};

  feedstore::Metadata metadata;
  SetSessionId(metadata, Token1(), kExpiryTime1);

  // Updating the token with nullopt is a NOP.
  EXPECT_FALSE(MaybeUpdateSessionId(metadata, base::nullopt));

  // Updating the token with the same value is a NOP.
  EXPECT_FALSE(MaybeUpdateSessionId(metadata, Token1()));

  // Updating the token with a different value resets the token and assigns a
  // new expiry time.
  base::Optional<Metadata> metadata2 = MaybeUpdateSessionId(metadata, Token2());
  ASSERT_TRUE(metadata2);
  EXPECT_EQ(Token2(), metadata2->session_id().token());
  EXPECT_TIME_EQ(base::Time::Now() + feed::GetFeedConfig().session_id_max_age,
                 GetSessionIdExpiryTime(*metadata2));

  // Updating the token with the empty string clears its value.
  base::Optional<Metadata> metadata3 = MaybeUpdateSessionId(*metadata2, "");
  EXPECT_TRUE(metadata3->session_id().token().empty());
  EXPECT_TRUE(GetSessionIdExpiryTime(*metadata3).is_null());
}

TEST(feedstore_util_test, GetNextActionId) {
  Metadata metadata;

  EXPECT_EQ(feed::LocalActionId(1), GetNextActionId(metadata));
  EXPECT_EQ(feed::LocalActionId(2), GetNextActionId(metadata));
}

}  // namespace
}  // namespace feedstore
