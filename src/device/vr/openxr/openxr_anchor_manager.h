// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_VR_OPENXR_OPENXR_ANCHOR_MANAGER_H_
#define DEVICE_VR_OPENXR_OPENXR_ANCHOR_MANAGER_H_

#include <map>

#include "base/numerics/checked_math.h"
#include "base/numerics/math_constants.h"
#include "base/optional.h"
#include "device/vr/openxr/openxr_anchor_request.h"
#include "device/vr/openxr/openxr_util.h"
#include "device/vr/public/mojom/vr_service.mojom.h"

namespace device {

class OpenXrApiWrapper;

class OpenXrAnchorManager {
 public:
  OpenXrAnchorManager(const OpenXrExtensionHelper& extension_helper,
                      XrSession session,
                      XrSpace mojo_space);
  ~OpenXrAnchorManager();

  void AddCreateAnchorRequest(
      const mojom::XRNativeOriginInformation& native_origin_information,
      const device::Pose& native_origin_from_anchor,
      CreateAnchorCallback callback);

  device::mojom::XRAnchorsDataPtr ProcessAnchorsForFrame(
      OpenXrApiWrapper* openxr,
      const mojom::VRStageParametersPtr& current_stage_parameters,
      const std::vector<mojom::XRInputSourceStatePtr>& input_state,
      XrTime predicted_display_time);

  void DetachAnchor(AnchorId anchor_id);

 private:
  void DisposeActiveAnchorCallbacks();
  AnchorId CreateAnchor(XrPosef pose,
                        XrSpace space,
                        XrTime predicted_display_time);
  XrSpace GetAnchorSpace(AnchorId anchor_id) const;
  void ProcessCreateAnchorRequests(
      OpenXrApiWrapper* openxr,
      const mojom::VRStageParametersPtr& current_stage_parameters,
      const std::vector<mojom::XRInputSourceStatePtr>& input_state);
  device::mojom::XRAnchorsDataPtr GetCurrentAnchorsData(
      XrTime predicted_display_time) const;

  // An XrPosef with the space it is relative to
  struct XrLocation {
    XrPosef pose;
    XrSpace space;
  };
  base::Optional<XrLocation> GetXrLocationFromNativeOriginInformation(
      OpenXrApiWrapper* openxr,
      const mojom::VRStageParametersPtr& current_stage_parametersm,
      const mojom::XRNativeOriginInformation& native_origin_information,
      const gfx::Transform& native_origin_from_anchor,
      const std::vector<mojom::XRInputSourceStatePtr>& input_state) const;

  base::Optional<XrLocation> GetXrLocationFromReferenceSpace(
      OpenXrApiWrapper* openxr,
      const mojom::VRStageParametersPtr& current_stage_parameters,
      const mojom::XRNativeOriginInformation& native_origin_information,
      const gfx::Transform& native_origin_from_anchor) const;

  const OpenXrExtensionHelper& extension_helper_;
  XrSession session_;
  XrSpace mojo_space_;  // The intermediate space that mojom poses are
                        // represented in (currently defined as local space)

  // Each OpenXR anchor produces a space handle which tracks the location of the
  // anchor. We create and cache this space here in order to avoid complex
  // resource tracking.
  struct AnchorData {
    XrSpatialAnchorMSFT anchor;
    XrSpace
        space;  // The XrSpace tracking this anchor relative to other XrSpaces
  };

  void DestroyAnchorData(const AnchorData& anchor_data) const;

  std::vector<CreateAnchorRequest> create_anchor_requests_;

  AnchorId::Generator anchor_id_generator_;  // 0 is not a valid anchor ID
  std::map<AnchorId, AnchorData> openxr_anchors_;
  DISALLOW_COPY_AND_ASSIGN(OpenXrAnchorManager);
};

}  // namespace device

#endif  // DEVICE_VR_OPENXR_OPENXR_ANCHOR_MANAGER_H_
