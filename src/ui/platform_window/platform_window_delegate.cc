// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/platform_window/platform_window_delegate.h"

#include "third_party/skia/include/core/SkPath.h"
#include "ui/gfx/geometry/size.h"

namespace ui {

PlatformWindowDelegate::BoundsChange::BoundsChange() = default;

PlatformWindowDelegate::BoundsChange::BoundsChange(const gfx::Rect& bounds)
    : bounds(bounds) {}

PlatformWindowDelegate::BoundsChange::~BoundsChange() = default;

PlatformWindowDelegate::PlatformWindowDelegate() = default;

PlatformWindowDelegate::~PlatformWindowDelegate() = default;

base::Optional<gfx::Size> PlatformWindowDelegate::GetMinimumSizeForWindow() {
  return base::nullopt;
}

base::Optional<gfx::Size> PlatformWindowDelegate::GetMaximumSizeForWindow() {
  return base::nullopt;
}

///@name USE_NEVA_APPRUNTIME
///@{
LinuxInputMethodContext* PlatformWindowDelegate::GetInputMethodContext() {
  return nullptr;
}
///@}

SkPath PlatformWindowDelegate::GetWindowMaskForWindowShapeInPixels() {
  return SkPath();
}

void PlatformWindowDelegate::OnSurfaceFrameLockingChanged(bool lock) {}

}  // namespace ui
