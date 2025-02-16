// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_VIEWS_COREWM_TOOLTIP_AURA_H_
#define UI_VIEWS_COREWM_TOOLTIP_AURA_H_

#include <memory>

#include "base/macros.h"
#include "ui/views/corewm/tooltip.h"
#include "ui/views/widget/widget_observer.h"

namespace gfx {
class RenderText;
class Size;
}  // namespace gfx

namespace ui {
struct AXNodeData;
}  // namespace ui

namespace views {

class Widget;

namespace corewm {
namespace test {
class TooltipAuraTestApi;
}

// Implementation of Tooltip that shows the tooltip using a Widget and Label.
class VIEWS_EXPORT TooltipAura : public Tooltip, public WidgetObserver {
 public:
  // FIXME: get cursor offset from actual cursor size.
  static constexpr int kCursorOffsetX = 10;
  static constexpr int kCursorOffsetY = 15;

  TooltipAura() = default;
  ~TooltipAura() override;

 private:
  class TooltipWidget;

  friend class test::TooltipAuraTestApi;
  gfx::RenderText* GetRenderTextForTest();
  void GetAccessibleNodeDataForTest(ui::AXNodeData* node_data);

  // Adjusts the bounds given by the arguments to fit inside the desktop
  // and returns the adjusted bounds.
  gfx::Rect GetTooltipBounds(const gfx::Size& tooltip_size,
                             const TooltipPosition& position);

  // Sets |widget_| to a new instance of TooltipWidget.
  void CreateTooltipWidget(const gfx::Rect& bounds);

  // Destroys |widget_|.
  void DestroyWidget();

  // Tooltip:
  int GetMaxWidth(const gfx::Point& location) const override;
  void Update(aura::Window* window,
              const std::u16string& tooltip_text,
              const TooltipPosition& position) override;
  void Show() override;
  void Hide() override;
  bool IsVisible() override;

  // WidgetObserver:
  void OnWidgetDestroying(Widget* widget) override;

  // The widget containing the tooltip. May be NULL.
  TooltipWidget* widget_ = nullptr;

  // The window we're showing the tooltip for. Never NULL and valid while
  // showing.
  aura::Window* tooltip_window_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(TooltipAura);
};

}  // namespace corewm
}  // namespace views

#endif  // UI_VIEWS_COREWM_TOOLTIP_AURA_H_
