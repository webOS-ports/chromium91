// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/public/cpp/link_header_parser.h"

#include "base/memory/scoped_refptr.h"
#include "net/http/http_response_headers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace network {

namespace {

const GURL kBaseUrl = GURL("https://example.com/foo?bar=baz");

}  // namespace

TEST(LinkHeaderParserTest, NoLinkHeader) {
  auto headers =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/2 200 OK\n");

  std::vector<mojom::LinkHeaderPtr> parsed_headers =
      ParseLinkHeaders(*headers, kBaseUrl);
  ASSERT_EQ(parsed_headers.size(), 0UL);
}

TEST(LinkHeaderParserTest, InvalidValue) {
  auto headers =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/2 200 OK\n");
  // The value is invalid because it misses a semicolon after `rel=preload`.
  headers->AddHeader("link", "</script.js>; rel=preload as=script");

  std::vector<mojom::LinkHeaderPtr> parsed_headers =
      ParseLinkHeaders(*headers, kBaseUrl);
  ASSERT_EQ(parsed_headers.size(), 0UL);
}

TEST(LinkHeaderParserTest, UndefinedAttribute) {
  auto headers =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/2 200 OK\n");
  // `unknownattr` is not pre-defined.
  headers->AddHeader("link",
                     "</style.css>; rel=preload; as=stylesheet; unknownattr");

  std::vector<mojom::LinkHeaderPtr> parsed_headers =
      ParseLinkHeaders(*headers, kBaseUrl);
  ASSERT_EQ(parsed_headers.size(), 0UL);
}

TEST(LinkHeaderParserTest, UndefinedAttributeValue) {
  auto headers =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/2 200 OK\n");
  headers->AddHeader("link", "</foo>; rel=preload; as=unknown-as");

  std::vector<mojom::LinkHeaderPtr> parsed_headers =
      ParseLinkHeaders(*headers, kBaseUrl);
  ASSERT_EQ(parsed_headers.size(), 0UL);
}

TEST(LinkHeaderParserTest, UnknownMimeType) {
  auto headers =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/2 200 OK\n");
  headers->AddHeader("link", "</foo>; rel=preload; type=unknown-type");

  std::vector<mojom::LinkHeaderPtr> parsed_headers =
      ParseLinkHeaders(*headers, kBaseUrl);
  ASSERT_EQ(parsed_headers.size(), 0UL);
}

TEST(LinkHeaderParserTest, NoRelAttribute) {
  auto headers =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/2 200 OK\n");
  // `rel` must be present.
  headers->AddHeader("link", "</foo>");

  std::vector<mojom::LinkHeaderPtr> parsed_headers =
      ParseLinkHeaders(*headers, kBaseUrl);
  ASSERT_EQ(parsed_headers.size(), 0UL);
}

TEST(LinkHeaderParserTest, AttributesAppearTwice) {
  auto headers =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/2 200 OK\n");
  headers->AddHeader("link", "</foo>; rel=preload; rel=prefetch");

  std::vector<mojom::LinkHeaderPtr> parsed_headers =
      ParseLinkHeaders(*headers, kBaseUrl);
  ASSERT_EQ(parsed_headers.size(), 1UL);
  // The parser should use the first one.
  EXPECT_EQ(parsed_headers[0]->rel, mojom::LinkRelAttribute::kPreload);

  // TODO(crbug.com/1182567): Add tests for other attributes if the behavior is
  // reasonable.
}

TEST(LinkHeaderParserTest, LinkAsAttribute) {
  auto headers =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/2 200 OK\n");
  headers->AddHeader("link", "</foo>; rel=preload");
  headers->AddHeader("link", "</font.woff2>; rel=preload; as=font");
  headers->AddHeader("link", "</image.jpg>; rel=preload; as=image");
  headers->AddHeader("link", "</script.js>; rel=preload; as=script");
  headers->AddHeader("link", "</style.css>; rel=preload; as=stylesheet");

  std::vector<mojom::LinkHeaderPtr> parsed_headers =
      ParseLinkHeaders(*headers, kBaseUrl);
  ASSERT_EQ(parsed_headers.size(), 5UL);
  EXPECT_EQ(parsed_headers[0]->as, mojom::LinkAsAttribute::kUnspecified);
  EXPECT_EQ(parsed_headers[1]->as, mojom::LinkAsAttribute::kFont);
  EXPECT_EQ(parsed_headers[2]->as, mojom::LinkAsAttribute::kImage);
  EXPECT_EQ(parsed_headers[3]->as, mojom::LinkAsAttribute::kScript);
  EXPECT_EQ(parsed_headers[4]->as, mojom::LinkAsAttribute::kStyleSheet);
}

TEST(LinkHeaderParserTest, CrossOriginAttribute) {
  auto headers =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/2 200 OK\n");
  headers->AddHeader("link", "<https://cross.example.com/>; rel=preload");
  headers->AddHeader("link",
                     "<https://cross.example.com/>; rel=preload; crossorigin");
  headers->AddHeader(
      "link",
      "<https://cross.example.com/>; rel=preload; crossorigin=anonymous");
  headers->AddHeader(
      "link",
      "<https://cross.example.com/>; rel=preload; crossorigin=use-credentials");

  std::vector<mojom::LinkHeaderPtr> parsed_headers =
      ParseLinkHeaders(*headers, kBaseUrl);
  ASSERT_EQ(parsed_headers.size(), 4UL);
  EXPECT_EQ(parsed_headers[0]->cross_origin,
            mojom::CrossOriginAttribute::kUnspecified);
  EXPECT_EQ(parsed_headers[1]->cross_origin,
            mojom::CrossOriginAttribute::kAnonymous);
  EXPECT_EQ(parsed_headers[2]->cross_origin,
            mojom::CrossOriginAttribute::kAnonymous);
  EXPECT_EQ(parsed_headers[3]->cross_origin,
            mojom::CrossOriginAttribute::kUseCredentials);
}

TEST(LinkHeaderParserTest, TwoHeaders) {
  auto headers =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/2 200 OK\n");
  headers->AddHeader("link", "</image.jpg>; rel=preload; as=image");
  headers->AddHeader("link",
                     "<https://cross.example.com/font.woff2>; rel=preload; "
                     "as=font; crossorigin; type=font/woff2");

  std::vector<mojom::LinkHeaderPtr> parsed_headers =
      ParseLinkHeaders(*headers, kBaseUrl);
  ASSERT_EQ(parsed_headers.size(), 2UL);

  EXPECT_EQ(parsed_headers[0]->href, kBaseUrl.Resolve("/image.jpg"));
  EXPECT_EQ(parsed_headers[0]->rel, mojom::LinkRelAttribute::kPreload);
  EXPECT_EQ(parsed_headers[0]->as, mojom::LinkAsAttribute::kImage);
  EXPECT_EQ(parsed_headers[0]->cross_origin,
            mojom::CrossOriginAttribute::kUnspecified);
  EXPECT_FALSE(parsed_headers[0]->mime_type.has_value());

  EXPECT_EQ(parsed_headers[1]->href,
            GURL("https://cross.example.com/font.woff2"));
  EXPECT_EQ(parsed_headers[1]->rel, mojom::LinkRelAttribute::kPreload);
  EXPECT_EQ(parsed_headers[1]->as, mojom::LinkAsAttribute::kFont);
  EXPECT_EQ(parsed_headers[1]->cross_origin,
            mojom::CrossOriginAttribute::kAnonymous);
  EXPECT_EQ(parsed_headers[1]->mime_type, "font/woff2");
}

TEST(LinkHeaderParserTest, UpperCaseCharacters) {
  auto headers =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/2 200 OK\n");
  headers->AddHeader("link", "</image.jpg>; REL=preload; as=IMAGE");
  headers->AddHeader("link",
                     "<https://cross.example.com/font.woff2>; rel=PRELOAD; "
                     "AS=font; CROSSORIGIN=USE-CREDENTIALS; TYPE=font/woff2");

  std::vector<mojom::LinkHeaderPtr> parsed_headers =
      ParseLinkHeaders(*headers, kBaseUrl);
  ASSERT_EQ(parsed_headers.size(), 2UL);

  EXPECT_EQ(parsed_headers[0]->href, kBaseUrl.Resolve("/image.jpg"));
  EXPECT_EQ(parsed_headers[0]->rel, mojom::LinkRelAttribute::kPreload);
  EXPECT_EQ(parsed_headers[0]->as, mojom::LinkAsAttribute::kImage);

  EXPECT_EQ(parsed_headers[1]->href,
            GURL("https://cross.example.com/font.woff2"));
  EXPECT_EQ(parsed_headers[1]->rel, mojom::LinkRelAttribute::kPreload);
  EXPECT_EQ(parsed_headers[1]->as, mojom::LinkAsAttribute::kFont);
  EXPECT_EQ(parsed_headers[1]->cross_origin,
            mojom::CrossOriginAttribute::kUseCredentials);
  EXPECT_EQ(parsed_headers[1]->mime_type, "font/woff2");
}

}  // namespace network
