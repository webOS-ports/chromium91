// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_CHILD_WEBTHEMEENGINE_IMPL_ANDROID_H_
#define CONTENT_CHILD_WEBTHEMEENGINE_IMPL_ANDROID_H_

#include "third_party/blink/public/platform/web_theme_engine.h"

namespace content {

class WebThemeEngineAndroid : public blink::WebThemeEngine {
 public:
  // WebThemeEngine methods:
  ~WebThemeEngineAndroid() override;
  gfx::Size GetSize(blink::WebThemeEngine::Part) override;
  void GetOverlayScrollbarStyle(
      blink::WebThemeEngine::ScrollbarStyle*) override;
  void Paint(cc::PaintCanvas* canvas,
             blink::WebThemeEngine::Part part,
             blink::WebThemeEngine::State state,
             const gfx::Rect& rect,
             const blink::WebThemeEngine::ExtraParams* extra_params,
             blink::mojom::ColorScheme color_scheme,
             const base::Optional<SkColor>& accent_color) override;
  blink::ForcedColors GetForcedColors() const override;
  void SetForcedColors(const blink::ForcedColors forced_colors) override;
};

}  // namespace content

#endif  // CONTENT_CHILD_WEBTHEMEENGINE_IMPL_ANDROID_H_
