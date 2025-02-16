// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_MESSAGE_CENTER_PUBLIC_CPP_MESSAGE_CENTER_CONSTANTS_H_
#define UI_MESSAGE_CENTER_PUBLIC_CPP_MESSAGE_CENTER_CONSTANTS_H_

#include <stddef.h>

#include "build/build_config.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/color_palette.h"

// TODO(estade): many of these constants could be internalized.
namespace message_center {

// Exported values /////////////////////////////////////////////////////////////

// Square image sizes in DIPs.
const int kNotificationButtonIconSize = 16;
const int kNotificationIconSize = 80;
// A border is applied to images that have a non-preferred aspect ratio.
const int kNotificationImageBorderSize = 10;
const int kNotificationPreferredImageWidth = 360;
const int kNotificationPreferredImageHeight = 240;
const int kSmallImageSize = 16;
const int kSmallImageSizeMD = 18;
const int kSmallImagePadding = 4;

// Limits.
const size_t kMaxVisibleMessageCenterNotifications = 100;
const size_t kMaxVisiblePopupNotifications = 3;

// DIP dimension; H size of the whole card.
const int kNotificationWidth = 360;

// Within a notification ///////////////////////////////////////////////////////

// DIP dimensions (H = horizontal, V = vertical).

const int kIconToTextPadding = 16;  // H space between icon & title/message.
const int kTextTopPadding = 12;     // V space between text elements.
const int kIconBottomPadding = 16;  // Minimum non-zero V space between icon
                                    // and frame.
// H space between the context message and the end of the card.
const int kTextRightPadding = 23;
const int kTextLeftPadding = kNotificationIconSize + kIconToTextPadding;
const int kContextMessageViewWidth =
    kNotificationWidth - kTextLeftPadding - kTextRightPadding;
// space between buttons and frame.
const int kControlButtonPadding = 2;
const int kControlButtonBorderSize = 6;

// Text sizes.
const int kTitleFontSize = 14;        // For title only.
const int kEmptyCenterFontSize = 13;  // For empty message only.
const int kTitleLineHeight = 20;      // In DIPs.
const int kMessageFontSize = 12;      // For everything but title.
const int kMessageLineHeight = 18;    // In DIPs.

// For list notifications.
// Not used when --enabled-new-style-notification is set.
const size_t kNotificationMaximumItems = 5;

// This is an experimental short delay for all notification timeouts.
// It is currently only enabled if the kNotificationExperimentalShortTimeouts
// flag is enabled. If disabled the below delays are used as before.
const int kAutocloseShortDelaySeconds = 6;
// Timing. Web Notifications always use high-priority timings except on
// Chrome OS. Given the absence of a notification center on non-Chrome OS
// platforms, this improves users' ability to interact with the toasts.
const int kAutocloseDefaultDelaySeconds = 8;
const int kAutocloseHighPriorityDelaySeconds = 25;

// Buttons.
const int kButtonHeight = 38;              // In DIPs.
const int kButtonHorizontalPadding = 16;   // In DIPs.
const int kButtonIconTopPadding = 11;      // In DIPs.
const int kButtonIconToTitlePadding = 16;  // In DIPs.

// Progress bar.
const int kProgressBarTopPadding = 16;
#if defined(OS_APPLE)
const int kProgressBarThickness = 5;
const int kProgressBarCornerRadius = 3;
#endif

// Line limits.
const int kMaxTitleLines = 2;
const int kMessageCollapsedLineLimit = 2;
const int kMessageExpandedLineLimit = 5;
const int kContextMessageLineLimit = 1;

// Around notifications ////////////////////////////////////////////////////////

// Horizontal & vertical thickness of the border around the notifications in the
// notification list.
constexpr int kNotificationBorderThickness = 1;
// Horizontal & vertical space around & between notifications in the
// notification list.
constexpr int kMarginBetweenItemsInList = 8;

// Horizontal & vertical space around & between popup notifications.
constexpr int kMarginBetweenPopups = 10;

// Radius of the rounded corners of a notification.
// The corners are only rounded in Chrome OS.
constexpr int kNotificationCornerRadius = 2;

}  // namespace message_center

#endif  // UI_MESSAGE_CENTER_PUBLIC_CPP_MESSAGE_CENTER_CONSTANTS_H_
