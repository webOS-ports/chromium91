// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/assistant/ui/main_stage/assistant_zero_state_view.h"

#include <memory>

#include "ash/assistant/model/assistant_ui_model.h"
#include "ash/assistant/ui/assistant_ui_constants.h"
#include "ash/assistant/ui/assistant_view_delegate.h"
#include "ash/assistant/ui/assistant_view_ids.h"
#include "ash/assistant/ui/main_stage/assistant_onboarding_view.h"
#include "ash/public/cpp/assistant/assistant_state.h"
#include "ash/public/cpp/assistant/controller/assistant_ui_controller.h"
#include "ash/strings/grit/ash_strings.h"
#include "chromeos/services/assistant/public/cpp/features.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/fill_layout.h"

namespace ash {

namespace {

using chromeos::assistant::features::IsBetterOnboardingEnabled;

// Appearance.
constexpr int kGreetingLabelTopMarginDip = 28;
constexpr int kOnboardingViewTopMarginDip = 48;

}  // namespace

AssistantZeroStateView::AssistantZeroStateView(AssistantViewDelegate* delegate)
    : delegate_(delegate) {
  SetID(AssistantViewID::kZeroStateView);

  InitLayout();
  UpdateLayout();

  assistant_controller_observation_.Observe(AssistantController::Get());
  AssistantUiController::Get()->GetModel()->AddObserver(this);
}

AssistantZeroStateView::~AssistantZeroStateView() {
  if (AssistantUiController::Get())
    AssistantUiController::Get()->GetModel()->RemoveObserver(this);
}

const char* AssistantZeroStateView::GetClassName() const {
  return "AssistantZeroStateView";
}

gfx::Size AssistantZeroStateView::CalculatePreferredSize() const {
  return gfx::Size(INT_MAX, GetHeightForWidth(INT_MAX));
}

void AssistantZeroStateView::ChildPreferredSizeChanged(views::View* child) {
  PreferredSizeChanged();
}

void AssistantZeroStateView::OnAssistantControllerDestroying() {
  AssistantUiController::Get()->GetModel()->RemoveObserver(this);
  DCHECK(assistant_controller_observation_.IsObservingSource(
      AssistantController::Get()));
  assistant_controller_observation_.Reset();
}

void AssistantZeroStateView::OnUiVisibilityChanged(
    AssistantVisibility new_visibility,
    AssistantVisibility old_visibility,
    base::Optional<AssistantEntryPoint> entry_point,
    base::Optional<AssistantExitPoint> exit_point) {
  if (new_visibility == AssistantVisibility::kClosed)
    UpdateLayout();
}

void AssistantZeroStateView::InitLayout() {
  // Layout.
  auto* layout = SetLayoutManager(std::make_unique<views::FillLayout>());
  layout->SetIncludeHiddenViews(false);

  // Onboarding.
  if (IsBetterOnboardingEnabled()) {
    onboarding_view_ =
        AddChildView(std::make_unique<AssistantOnboardingView>(delegate_));
    onboarding_view_->SetBorder(
        views::CreateEmptyBorder(kOnboardingViewTopMarginDip, 0, 0, 0));
  }

  // Greeting.
  greeting_label_ = AddChildView(std::make_unique<views::Label>());
  greeting_label_->SetID(AssistantViewID::kGreetingLabel);
  greeting_label_->SetAutoColorReadabilityEnabled(false);
  greeting_label_->SetBackground(views::CreateSolidBackground(SK_ColorWHITE));
  greeting_label_->SetBorder(
      views::CreateEmptyBorder(kGreetingLabelTopMarginDip, 0, 0, 0));
  greeting_label_->SetEnabledColor(kTextColorPrimary);
  greeting_label_->SetFontList(
      assistant::ui::GetDefaultFontList()
          .DeriveWithSizeDelta(8)
          .DeriveWithWeight(gfx::Font::Weight::MEDIUM));
  greeting_label_->SetHorizontalAlignment(
      gfx::HorizontalAlignment::ALIGN_CENTER);
  greeting_label_->SetMultiLine(true);
  greeting_label_->SetText(
      l10n_util::GetStringUTF16(IDS_ASH_ASSISTANT_PROMPT_DEFAULT));
}

void AssistantZeroStateView::UpdateLayout() {
  if (!IsBetterOnboardingEnabled())
    return;

  const bool show_onboarding = delegate_->ShouldShowOnboarding();
  onboarding_view_->SetVisible(show_onboarding);
  greeting_label_->SetVisible(!show_onboarding);
}

}  // namespace ash
