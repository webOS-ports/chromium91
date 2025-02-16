// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/public/cpp/network_isolation_key_mojom_traits.h"

#include "base/stl_util.h"
#include "mojo/public/cpp/test_support/test_utils.h"
#include "services/network/public/mojom/network_isolation_key.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace mojo {

TEST(NetworkIsolationKeyMojomTraitsTest, SerializeAndDeserialize) {
  std::vector<net::NetworkIsolationKey> keys = {
      net::NetworkIsolationKey(), net::NetworkIsolationKey::CreateTransient(),
      net::NetworkIsolationKey::CreateOpaqueAndNonTransient(),
      net::NetworkIsolationKey(url::Origin::Create(GURL("http://a.test/")),
                               url::Origin::Create(GURL("http://b.test/"))),
      net::NetworkIsolationKey(
          url::Origin::Create(GURL("http://foo.a.test/")),
          url::Origin::Create(GURL("http://bar.b.test/")))};

  for (auto original : keys) {
    SCOPED_TRACE(original.ToDebugString());
    net::NetworkIsolationKey copied;
    EXPECT_TRUE(mojo::test::SerializeAndDeserialize<
                network::mojom::NetworkIsolationKey>(original, copied));
    EXPECT_EQ(original, copied);
    EXPECT_EQ(original.GetTopFrameSite(), copied.GetTopFrameSite());
    EXPECT_EQ(original.GetFrameSite(), copied.GetFrameSite());
    EXPECT_EQ(original.IsTransient(), copied.IsTransient());
  }
}

}  // namespace mojo
