// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/views/bubble/tooltip_icon.h"

#include "base/timer/timer.h"
#include "components/vector_icons/vector_icons.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/views/bubble/bubble_frame_view.h"
#include "ui/views/bubble/info_bubble.h"
#include "ui/views/metadata/metadata_impl_macros.h"
#include "ui/views/mouse_watcher_view_host.h"

namespace views {

TooltipIcon::TooltipIcon(const std::u16string& tooltip, int tooltip_icon_size)
    : tooltip_(tooltip),
      tooltip_icon_size_(tooltip_icon_size),
      mouse_inside_(false),
      bubble_(nullptr),
      preferred_width_(0) {
  SetDrawAsHovered(false);
}

TooltipIcon::~TooltipIcon() {
  for (auto& observer : observers_)
    observer.OnTooltipIconDestroying(this);
  HideBubble();
}

void TooltipIcon::OnMouseEntered(const ui::MouseEvent& event) {
  mouse_inside_ = true;
  show_timer_.Start(FROM_HERE, base::TimeDelta::FromMilliseconds(150), this,
                    &TooltipIcon::ShowBubble);
}

void TooltipIcon::OnMouseExited(const ui::MouseEvent& event) {
  show_timer_.Stop();
}

bool TooltipIcon::OnMousePressed(const ui::MouseEvent& event) {
  // Swallow the click so that the parent doesn't process it.
  return true;
}

void TooltipIcon::OnGestureEvent(ui::GestureEvent* event) {
  if (event->type() == ui::ET_GESTURE_TAP) {
    ShowBubble();
    event->SetHandled();
  }
}

void TooltipIcon::GetAccessibleNodeData(ui::AXNodeData* node_data) {
  node_data->role = ax::mojom::Role::kTooltip;
  node_data->SetName(tooltip_);
}

void TooltipIcon::MouseMovedOutOfHost() {
  if (IsMouseHovered()) {
    mouse_watcher_->Start(GetWidget()->GetNativeWindow());
    return;
  }

  mouse_inside_ = false;
  HideBubble();
}

void TooltipIcon::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void TooltipIcon::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void TooltipIcon::SetDrawAsHovered(bool hovered) {
  SetImage(gfx::CreateVectorIcon(
      vector_icons::kInfoOutlineIcon, tooltip_icon_size_,
      GetNativeTheme()->GetSystemColor(
          hovered ? ui::NativeTheme::kColorId_TooltipIconHovered
                  : ui::NativeTheme::kColorId_TooltipIcon)));
}

void TooltipIcon::ShowBubble() {
  if (bubble_)
    return;

  SetDrawAsHovered(true);

  bubble_ = new InfoBubble(this, tooltip_);
  bubble_->set_preferred_width(preferred_width_);
  bubble_->SetArrow(anchor_point_arrow_);
  // When shown due to a gesture event, close on deactivate (i.e. don't use
  // "focusless").
  bubble_->SetCanActivate(!mouse_inside_);

  bubble_->Show();
  observation_.Observe(bubble_->GetWidget());

  if (mouse_inside_) {
    View* frame = bubble_->GetWidget()->non_client_view()->frame_view();
    mouse_watcher_ = std::make_unique<MouseWatcher>(
        std::make_unique<MouseWatcherViewHost>(frame, gfx::Insets()), this);
    mouse_watcher_->Start(GetWidget()->GetNativeWindow());
  }

  for (auto& observer : observers_)
    observer.OnTooltipBubbleShown(this);
}

void TooltipIcon::HideBubble() {
  if (bubble_)
    bubble_->Hide();
}

void TooltipIcon::OnWidgetDestroyed(Widget* widget) {
  DCHECK(observation_.IsObservingSource(widget));
  observation_.Reset();

  SetDrawAsHovered(false);
  mouse_watcher_.reset();
  bubble_ = nullptr;
}

BEGIN_METADATA(TooltipIcon, ImageView)
END_METADATA

}  // namespace views
