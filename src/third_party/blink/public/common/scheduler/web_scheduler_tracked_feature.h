// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_PUBLIC_COMMON_SCHEDULER_WEB_SCHEDULER_TRACKED_FEATURE_H_
#define THIRD_PARTY_BLINK_PUBLIC_COMMON_SCHEDULER_WEB_SCHEDULER_TRACKED_FEATURE_H_

#include <stdint.h>
#include <string>
#include "base/optional.h"
#include "third_party/blink/public/common/common_export.h"

namespace blink {
namespace scheduler {

// A list of features which influence scheduling behaviour (throttling /
// freezing / back-forward cache) and which might be sent to the browser process
// for metrics-related purposes.
//
// Please keep in sync with WebSchedulerTrackedFeature in
// tools/metrics/histograms/enums.xml. These values should not be renumbered.
enum class WebSchedulerTrackedFeature : uint32_t {
  kWebSocket = 0,
  kWebRTC = 1,

  // TODO(rakina): Move tracking of cache-control usage from
  // WebSchedulerTrackedFeature to RenderFrameHost.
  kMainResourceHasCacheControlNoCache = 2,
  kMainResourceHasCacheControlNoStore = 3,
  kSubresourceHasCacheControlNoCache = 4,
  kSubresourceHasCacheControlNoStore = 5,

  kPageShowEventListener = 6,
  kPageHideEventListener = 7,
  kBeforeUnloadEventListener = 8,
  kUnloadEventListener = 9,
  kFreezeEventListener = 10,
  kResumeEventListener = 11,

  kContainsPlugins = 12,
  kDocumentLoaded = 13,
  kDedicatedWorkerOrWorklet = 14,

  // There are some other values defined for specific request context types
  // (e.g., XHR). This value corresponds to a network requests not covered by
  // specific context types down below.
  kOutstandingNetworkRequestOthers = 15,

  // kServiceWorkerControlledPage = 16. Removed after implementing ServiceWorker
  // support.

  kOutstandingIndexedDBTransaction = 17,

  // Whether the page tried to request a permission regardless of the outcome.
  // TODO(altimin): Track this more accurately depending on the data.
  // See permission.mojom for more details.
  kRequestedGeolocationPermission = 19,
  kRequestedNotificationsPermission = 20,
  kRequestedMIDIPermission = 21,
  kRequestedAudioCapturePermission = 22,
  kRequestedVideoCapturePermission = 23,
  kRequestedBackForwardCacheBlockedSensors = 24,
  // This covers all background-related permissions, including background sync,
  // background fetch and others.
  kRequestedBackgroundWorkPermission = 26,

  kBroadcastChannel = 27,

  kIndexedDBConnection = 28,

  // kWebGL = 29. Removed after implementing WebGL support.
  kWebVR = 30,
  kWebXR = 31,

  kSharedWorker = 32,

  kWebLocks = 33,
  kWebHID = 34,

  // kWakeLock = 35, Removed because clean-up is done upon visibility change.

  kWebShare = 36,

  kRequestedStorageAccessGrant = 37,
  kWebNfc = 38,
  kWebFileSystem = 39,

  kOutstandingNetworkRequestFetch = 40,
  kOutstandingNetworkRequestXHR = 41,

  kAppBanner = 42,
  kPrinting = 43,
  kWebDatabase = 44,
  kPictureInPicture = 45,
  kPortal = 46,
  kSpeechRecognizer = 47,
  kIdleManager = 48,
  kPaymentManager = 49,
  kSpeechSynthesis = 50,
  kKeyboardLock = 51,
  kWebOTPService = 52,
  kOutstandingNetworkRequestDirectSocket = 53,
  kMediaSessionImplOnServiceCreated = 56,

  // NB: This enum is used in a bitmask, so kMaxValue must be less than 64.
  kMaxValue = kMediaSessionImplOnServiceCreated,
};

static_assert(static_cast<uint32_t>(WebSchedulerTrackedFeature::kMaxValue) < 64,
              "This enum is used in a bitmask, so the values should fit into a"
              "64-bit integer");

BLINK_COMMON_EXPORT std::string FeatureToHumanReadableString(
    WebSchedulerTrackedFeature feature);

BLINK_COMMON_EXPORT base::Optional<WebSchedulerTrackedFeature> StringToFeature(
    const std::string& str);

// Converts a WebSchedulerTrackedFeature to a bit for use in a bitmask.
BLINK_COMMON_EXPORT constexpr uint64_t FeatureToBit(
    WebSchedulerTrackedFeature feature) {
  return 1ull << static_cast<uint32_t>(feature);
}

// Sticky features can't be unregistered and remain active for the rest of the
// lifetime of the page.
BLINK_COMMON_EXPORT bool IsFeatureSticky(WebSchedulerTrackedFeature feature);

// All the sticky features in bitmask form.
BLINK_COMMON_EXPORT uint64_t StickyFeaturesBitmask();

}  // namespace scheduler
}  // namespace blink

#endif  // THIRD_PARTY_BLINK_PUBLIC_COMMON_SCHEDULER_WEB_SCHEDULER_TRACKED_FEATURE_H_
