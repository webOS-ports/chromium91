// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_DIGITAL_ASSET_LINKS_DIGITAL_ASSET_LINKS_HANDLER_H_
#define COMPONENTS_DIGITAL_ASSET_LINKS_DIGITAL_ASSET_LINKS_HANDLER_H_

#include <map>
#include <set>
#include <string>

#include "base/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/optional.h"
#include "base/time/time.h"
#include "base/values.h"
#include "content/public/browser/web_contents_observer.h"
#include "services/data_decoder/public/cpp/data_decoder.h"

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
}  // namespace network

namespace digital_asset_links {

extern const char kDigitalAssetLinksCheckResponseKeyLinked[];

// GENERATED_JAVA_ENUM_PACKAGE: org.chromium.components.digital_asset_links
enum class RelationshipCheckResult {
  kSuccess = 0,
  kFailure,
  kNoConnection,
};

using RelationshipCheckResultCallback =
    base::OnceCallback<void(RelationshipCheckResult)>;

// A handler class for sending REST API requests to DigitalAssetLinks web
// end point. See
// https://developers.google.com/digital-asset-links/v1/getting-started
// for details of usage and APIs. These APIs are used to verify declared
// relationships between different asset types like web domains or Android apps.
// The lifecycle of this handler will be governed by the owner.
// The WebContents are used for logging console messages.
class DigitalAssetLinksHandler : public content::WebContentsObserver {
 public:
  // Optionally include |web_contents| for logging error messages to DevTools.
  explicit DigitalAssetLinksHandler(
      scoped_refptr<network::SharedURLLoaderFactory> factory,
      content::WebContents* web_contents = nullptr);
  ~DigitalAssetLinksHandler() override;

  // Checks whether the given "relationship" has been declared by the target
  // |web_domain| for the source Android app which is uniquely defined by the
  // |package| and SHA256 |fingerprint| (a string with 32 hexadecimals with :
  // between) given. Any error in the string params here will result in a bad
  // request and a nullptr response to the callback.
  //
  // Calling this multiple times on the same handler will cancel the previous
  // checks.
  // See
  // https://developers.google.com/digital-asset-links/reference/rest/v1/assetlinks/check
  // for details.
  bool CheckDigitalAssetLinkRelationshipForAndroidApp(
      const std::string& web_domain,
      const std::string& relationship,
      const std::string& fingerprint,
      const std::string& package,
      RelationshipCheckResultCallback callback);

  // Checks if the asset links for |web_domain| allow pages controlled by
  // |manifest_url| to query for WebAPKs generated by |web_domain|.
  // TODO(rayankans): Link to the developer blog when published.
  bool CheckDigitalAssetLinkRelationshipForWebApk(
      const std::string& web_domain,
      const std::string& manifest_url,
      RelationshipCheckResultCallback callback);

  // Generic DAL verifier. Checks whether the given |relationship| has been
  // declared by the target |web_domain| using the values in |target_values|.
  // We require a match for every entry in the |target_values| map, but within
  // the entry, we require a match only for one value in the set.
  // For example, |target_values| may contain an entry with key="site" and
  // values={"https://example1.com", "https://example2.com"}. In order to
  // validate, the manifest must have an entry with key="site" with one or more
  // of the URLs as the value.
  bool CheckDigitalAssetLinkRelationship(
      const std::string& web_domain,
      const std::string& relationship,
      const base::Optional<std::string>& fingerprint,
      const std::map<std::string, std::set<std::string>>& target_values,
      RelationshipCheckResultCallback callback);

  // The amount of time to wait before giving up on a given network request and
  // considering it an error. If not set, then the request is allowed to take
  // as much time as it wants. Passed directly to the URL loader.
  void SetTimeoutDuration(base::TimeDelta timeout_duration);

 private:
  void OnURLLoadComplete(
      std::string relationship,
      base::Optional<std::string> fingerprint,
      std::map<std::string, std::set<std::string>> target_values,
      std::unique_ptr<std::string> response_body);

  // Callback for the DataDecoder.
  void OnJSONParseResult(
      std::string relationship,
      base::Optional<std::string> fingerprint,
      std::map<std::string, std::set<std::string>> target_values,
      data_decoder::DataDecoder::ValueOrError result);

  scoped_refptr<network::SharedURLLoaderFactory> shared_url_loader_factory_;

  std::unique_ptr<network::SimpleURLLoader> url_loader_;

  // The per request callback for receiving a URLFetcher result. This gets
  // reset every time we get a new CheckDigitalAssetLinkRelationship call.
  RelationshipCheckResultCallback callback_;

  base::TimeDelta timeout_duration_ = base::TimeDelta();

  base::WeakPtrFactory<DigitalAssetLinksHandler> weak_ptr_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(DigitalAssetLinksHandler);
};

}  // namespace digital_asset_links

#endif  // COMPONENTS_DIGITAL_ASSET_LINKS_DIGITAL_ASSET_LINKS_HANDLER_H_
