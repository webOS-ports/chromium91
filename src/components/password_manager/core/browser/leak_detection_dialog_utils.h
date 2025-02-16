// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PASSWORD_MANAGER_CORE_BROWSER_LEAK_DETECTION_DIALOG_UTILS_H_
#define COMPONENTS_PASSWORD_MANAGER_CORE_BROWSER_LEAK_DETECTION_DIALOG_UTILS_H_

#include <string>
#include <type_traits>

#include "base/macros.h"
#include "base/types/strong_alias.h"
#include "components/password_manager/core/browser/password_manager_metrics_util.h"
#include "ui/gfx/range/range.h"
#include "url/gurl.h"

namespace password_manager {

// Defines possible scenarios for leaked credentials.
enum CredentialLeakFlags {
  // Password is saved for current site.
  kPasswordSaved = 1 << 0,
  // Password is reused on other sites.
  kPasswordUsedOnOtherSites = 1 << 1,
  // User is syncing passwords with normal encryption.
  kSyncingPasswordsNormally = 1 << 2,
};

enum class PasswordCheckupReferrer {
  // Corresponds to the leak detection dialog shown on Desktop and Mobile.
  kLeakDetectionDialog = 0,
  // Corresponds to Chrome's password check page on Desktop.
  kPasswordCheck = 1,
};

// Contains combination of CredentialLeakFlags values.
using CredentialLeakType = std::underlying_type_t<CredentialLeakFlags>;

using IsSaved = base::StrongAlias<class IsSavedTag, bool>;
using IsReused = base::StrongAlias<class IsReusedTag, bool>;
using IsSyncing = base::StrongAlias<class IsSyncingTag, bool>;
// Creates CredentialLeakType from strong booleans.
CredentialLeakType CreateLeakType(IsSaved is_saved,
                                  IsReused is_reused,
                                  IsSyncing is_syncing);

// Checks whether password is saved in chrome.
bool IsPasswordSaved(CredentialLeakType leak_type);

// Checks whether password is reused on other sites.
bool IsPasswordUsedOnOtherSites(CredentialLeakType leak_type);

// Checks whether user is syncing passwords with normal encryption.
bool IsSyncingPasswordsNormally(CredentialLeakType leak_type);

// Returns the label for the leak dialog accept button.
std::u16string GetAcceptButtonLabel(
    password_manager::CredentialLeakType leak_type);

// Returns the label for the leak dialog cancel button.
std::u16string GetCancelButtonLabel();

// Returns the leak dialog message based on leak type.
std::u16string GetDescription(password_manager::CredentialLeakType leak_type,
                              const GURL& origin);

// Returns the leak dialog title based on leak type.
std::u16string GetTitle(password_manager::CredentialLeakType leak_type);

// Returns the leak dialog tooltip shown on (?) click.
std::u16string GetLeakDetectionTooltip();

// Checks whether the leak dialog should prompt user to password checkup.
bool ShouldCheckPasswords(password_manager::CredentialLeakType leak_type);

// Checks whether the leak dialog should show change password button.
bool ShouldShowChangePasswordButton(CredentialLeakType leak_type);

// Checks whether the leak dialog should show cancel button.
bool ShouldShowCancelButton(password_manager::CredentialLeakType leak_type);

// Returns the LeakDialogType corresponding to |leak_type|.
password_manager::metrics_util::LeakDialogType GetLeakDialogType(
    password_manager::CredentialLeakType leak_type);

// Returns the URL used to launch the password checkup.
GURL GetPasswordCheckupURL(PasswordCheckupReferrer referrer =
                               PasswordCheckupReferrer::kLeakDetectionDialog);

}  // namespace password_manager

#endif  // COMPONENTS_PASSWORD_MANAGER_CORE_BROWSER_LEAK_DETECTION_DIALOG_UTILS_H_
