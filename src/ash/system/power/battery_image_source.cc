// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/system/power/battery_image_source.h"

#include "ash/resources/vector_icons/vector_icons.h"
#include "ash/style/ash_color_provider.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/skia_util.h"

namespace {

// The minimum height (in dp) of the charged region of the battery icon when the
// battery is present and has a charge greater than 0.
const int kMinVisualChargeLevel = 1;

}  // namespace

namespace ash {

BatteryImageSource::BatteryImageSource(
    const PowerStatus::BatteryImageInfo& info,
    int height,
    SkColor bg_color,
    SkColor fg_color)
    : gfx::CanvasImageSource(gfx::Size(height, height)),
      info_(info),
      bg_color_(bg_color),
      fg_color_(fg_color) {}

BatteryImageSource::~BatteryImageSource() = default;

void BatteryImageSource::Draw(gfx::Canvas* canvas) {
  gfx::ImageSkia icon = CreateVectorIcon(kBatteryIcon, fg_color_);

  // Draw the solid outline of the battery icon.
  canvas->DrawImageInt(icon, 0, 0);

  canvas->Save();

  const float dsf = canvas->UndoDeviceScaleFactor();
  // All constants below are expressed relative to a canvas size of 20. The
  // actual canvas size (i.e. |size()|) may not be 20.
  const float kAssumedCanvasSize = 20;
  const float const_scale = dsf * size().height() / kAssumedCanvasSize;

  // |charge_level| is a value between 0 and the visual height of the icon
  // representing the number of device pixels the battery image should be
  // shown charged. The exception is when |charge_level| is very low; in this
  // case, still draw 1dip of charge.

  SkPath path;
  gfx::RectF fill_rect = gfx::RectF(8, 6, 6, 11);
  fill_rect.Scale(const_scale);
  path.addRect(gfx::RectToSkRect(gfx::ToEnclosingRect(fill_rect)));
  cc::PaintFlags flags;

  SkRect icon_bounds = path.getBounds();
  float charge_level =
      std::floor(info_.charge_percent / 100.0 * icon_bounds.height());
  const float min_charge_level = dsf * kMinVisualChargeLevel;
  charge_level =
      base::ClampToRange(charge_level, min_charge_level, icon_bounds.height());

  const float charge_y = icon_bounds.bottom() - charge_level;
  gfx::RectF clip_rect(0, charge_y, size().width() * dsf,
                       size().height() * dsf);
  canvas->ClipRect(clip_rect);

  auto* color_provider = AshColorProvider::Get();
  const SkColor alert_color = color_provider->GetContentLayerColor(
      AshColorProvider::ContentLayerType::kIconColorAlert);
  const bool use_alert_color =
      charge_level == min_charge_level && info_.alert_if_low;
  flags.setColor(use_alert_color ? alert_color : fg_color_);
  canvas->DrawPath(path, flags);

  canvas->Restore();

  if (info_.badge_outline) {
    const SkColor outline_color =
        info_.charge_percent > 50 ? fg_color_ : bg_color_;
    PaintVectorIcon(canvas, *info_.badge_outline, outline_color);
  }

  // Paint the badge over top of the battery, if applicable.
  if (info_.icon_badge) {
    const SkColor badge_color =
        use_alert_color
            ? alert_color
            : info_.charge_percent > 50
                  ? color_provider->GetContentLayerColor(
                        AshColorProvider::ContentLayerType::kBatteryBadgeColor)
                  : fg_color_;

    PaintVectorIcon(canvas, *info_.icon_badge, badge_color);
  }
}

bool BatteryImageSource::HasRepresentationAtAllScales() const {
  return true;
}

}  // namespace ash
