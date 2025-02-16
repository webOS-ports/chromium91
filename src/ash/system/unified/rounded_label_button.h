// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_SYSTEM_UNIFIED_ROUNDED_LABEL_BUTTON_H_
#define ASH_SYSTEM_UNIFIED_ROUNDED_LABEL_BUTTON_H_

#include <string>

#include "base/macros.h"
#include "ui/views/controls/button/label_button.h"

namespace ash {

// LabelButton that has a rounded shape with a Material Design ink drop.
class RoundedLabelButton : public views::LabelButton {
 public:
  RoundedLabelButton(PressedCallback callback, const std::u16string& text);
  ~RoundedLabelButton() override;

  // views::LabelButton:
  gfx::Size CalculatePreferredSize() const override;
  int GetHeightForWidth(int width) const override;
  std::unique_ptr<views::InkDrop> CreateInkDrop() override;
  std::unique_ptr<views::InkDropRipple> CreateInkDropRipple() const override;
  std::unique_ptr<views::InkDropHighlight> CreateInkDropHighlight()
      const override;
  const char* GetClassName() const override;
  void OnThemeChanged() override;

 private:
  DISALLOW_COPY_AND_ASSIGN(RoundedLabelButton);
};

}  // namespace ash

#endif  // ASH_SYSTEM_UNIFIED_ROUNDED_LABEL_BUTTON_H_
