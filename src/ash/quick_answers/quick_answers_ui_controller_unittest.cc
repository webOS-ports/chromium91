// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/quick_answers/ui/quick_answers_view.h"

#include "ash/constants/ash_features.h"
#include "ash/quick_answers/quick_answers_controller_impl.h"
#include "ash/quick_answers/quick_answers_ui_controller.h"
#include "ash/test/ash_test_base.h"
#include "base/test/scoped_feature_list.h"

namespace ash {

namespace {

constexpr gfx::Rect kDefaultAnchorBoundsInScreen =
    gfx::Rect(gfx::Point(500, 250), gfx::Size(80, 140));

}  // namespace

class QuickAnswersUiControllerTest : public AshTestBase {
 protected:
  QuickAnswersUiControllerTest() {
    scoped_feature_list_.InitAndEnableFeature(
        chromeos::features::kQuickAnswers);
  }
  QuickAnswersUiControllerTest(const QuickAnswersUiControllerTest&) = delete;
  QuickAnswersUiControllerTest& operator=(const QuickAnswersUiControllerTest&) =
      delete;
  ~QuickAnswersUiControllerTest() override = default;

  // AshTestBase:
  void SetUp() override {
    AshTestBase::SetUp();

    ui_controller_ =
        static_cast<QuickAnswersControllerImpl*>(QuickAnswersController::Get())
            ->quick_answers_ui_controller();
  }

  // Currently instantiated QuickAnswersView instance.
  QuickAnswersUiController* ui_controller() { return ui_controller_; }

 private:
  QuickAnswersUiController* ui_controller_ = nullptr;
  base::test::ScopedFeatureList scoped_feature_list_;
};

TEST_F(QuickAnswersUiControllerTest, TearDownWhileQuickAnswersViewShowing) {
  EXPECT_FALSE(ui_controller()->is_showing_quick_answers_view());
  ui_controller()->CreateQuickAnswersView(kDefaultAnchorBoundsInScreen,
                                          "default_title", "default_query");
  EXPECT_TRUE(ui_controller()->is_showing_quick_answers_view());
}

TEST_F(QuickAnswersUiControllerTest, TearDownWhileNoticeViewShowing) {
  EXPECT_FALSE(ui_controller()->is_showing_user_notice_view());
  ui_controller()->CreateUserNoticeView(kDefaultAnchorBoundsInScreen,
                                        std::u16string(), std::u16string());
  EXPECT_TRUE(ui_controller()->is_showing_user_notice_view());
}

}  // namespace ash
