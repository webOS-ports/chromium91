// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_DEVTOOLS_PROTOCOL_EMULATION_HANDLER_H_
#define CONTENT_BROWSER_DEVTOOLS_PROTOCOL_EMULATION_HANDLER_H_

#include "base/macros.h"
#include "content/browser/devtools/protocol/devtools_domain_handler.h"
#include "content/browser/devtools/protocol/emulation.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "third_party/blink/public/common/widget/device_emulation_params.h"

namespace net {
class HttpRequestHeaders;
}  // namespace net

namespace content {

class DevToolsAgentHostImpl;
class RenderFrameHostImpl;
class RenderWidgetHostImpl;
class WebContentsImpl;

namespace protocol {

class EmulationHandler : public DevToolsDomainHandler,
                         public Emulation::Backend {
 public:
  EmulationHandler();
  ~EmulationHandler() override;

  static std::vector<EmulationHandler*> ForAgentHost(
      DevToolsAgentHostImpl* host);

  void Wire(UberDispatcher* dispatcher) override;
  void SetRenderer(int process_host_id,
                   RenderFrameHostImpl* frame_host) override;

  Response Disable() override;

  Response SetIdleOverride(bool is_user_active,
                           bool is_screen_unlocked) override;
  Response ClearIdleOverride() override;

  Response SetGeolocationOverride(Maybe<double> latitude,
                                  Maybe<double> longitude,
                                  Maybe<double> accuracy) override;
  Response ClearGeolocationOverride() override;

  Response SetEmitTouchEventsForMouse(
      bool enabled,
      Maybe<std::string> configuration) override;

  Response SetUserAgentOverride(
      const std::string& user_agent,
      Maybe<std::string> accept_language,
      Maybe<std::string> platform,
      Maybe<Emulation::UserAgentMetadata> ua_metadata_override) override;

  Response CanEmulate(bool* result) override;
  Response SetDeviceMetricsOverride(
      int width,
      int height,
      double device_scale_factor,
      bool mobile,
      Maybe<double> scale,
      Maybe<int> screen_widget,
      Maybe<int> screen_height,
      Maybe<int> position_x,
      Maybe<int> position_y,
      Maybe<bool> dont_set_visible_size,
      Maybe<Emulation::ScreenOrientation> screen_orientation,
      Maybe<protocol::Page::Viewport> viewport,
      Maybe<protocol::Emulation::DisplayFeature> displayFeature) override;
  Response ClearDeviceMetricsOverride() override;

  Response SetVisibleSize(int width, int height) override;

  Response SetFocusEmulationEnabled(bool) override;

  blink::DeviceEmulationParams GetDeviceEmulationParams();
  void SetDeviceEmulationParams(const blink::DeviceEmulationParams& params);

  bool device_emulation_enabled() { return device_emulation_enabled_; }

  void ApplyOverrides(net::HttpRequestHeaders* headers);
  bool ApplyUserAgentMetadataOverrides(
      base::Optional<blink::UserAgentMetadata>* override_out);

 private:
  WebContentsImpl* GetWebContents();
  void UpdateTouchEventEmulationState();
  void UpdateDeviceEmulationState();
  void UpdateDeviceEmulationStateForHost(
      RenderWidgetHostImpl* render_widget_host);

  bool touch_emulation_enabled_;
  std::string touch_emulation_configuration_;
  bool device_emulation_enabled_;
  bool focus_emulation_enabled_;
  blink::DeviceEmulationParams device_emulation_params_;
  std::string user_agent_;

  // |user_agent_metadata_| is meaningful if |user_agent_| is non-empty.
  // In that case nullopt will disable sending of client hints, and a
  // non-nullopt value will be sent.
  base::Optional<blink::UserAgentMetadata> user_agent_metadata_;
  std::string accept_language_;

  RenderFrameHostImpl* host_;

  base::ScopedClosureRunner capture_handle_;

  DISALLOW_COPY_AND_ASSIGN(EmulationHandler);
};

}  // namespace protocol
}  // namespace content

#endif  // CONTENT_BROWSER_DEVTOOLS_PROTOCOL_EMULATION_HANDLER_H_
