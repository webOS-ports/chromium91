// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/password_manager/core/browser/ui/saved_passwords_presenter.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/ranges/algorithm.h"
#include "base/stl_util.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/browser/password_list_sorter.h"
#include "components/password_manager/core/browser/password_manager_metrics_util.h"

namespace {
using password_manager::metrics_util::IsPasswordChanged;
using password_manager::metrics_util::IsUsernameChanged;
using SavedPasswordsView =
    password_manager::SavedPasswordsPresenter::SavedPasswordsView;

bool IsUsernameAlreadyUsed(SavedPasswordsView all_forms,
                           SavedPasswordsView forms_to_check,
                           const std::u16string& new_username) {
  // In case the username changed, make sure that there exists no other
  // credential with the same signon_realm and username in the same store.
  auto has_conflicting_username = [&forms_to_check,
                                   &new_username](const auto& form) {
    return new_username == form.username_value &&
           base::ranges::any_of(forms_to_check, [&form](const auto& old_form) {
             return form.signon_realm == old_form.signon_realm &&
                    form.IsUsingAccountStore() ==
                        old_form.IsUsingAccountStore();
           });
  };
  return base::ranges::any_of(all_forms, has_conflicting_username);
}

}  // namespace

namespace password_manager {

SavedPasswordsPresenter::SavedPasswordsPresenter(
    scoped_refptr<PasswordStore> profile_store,
    scoped_refptr<PasswordStore> account_store)
    : profile_store_(std::move(profile_store)),
      account_store_(std::move(account_store)) {
  DCHECK(profile_store_);
  profile_store_->AddObserver(this);
  if (account_store_)
    account_store_->AddObserver(this);
}

SavedPasswordsPresenter::~SavedPasswordsPresenter() {
  if (account_store_)
    account_store_->RemoveObserver(this);
  profile_store_->RemoveObserver(this);
}

void SavedPasswordsPresenter::Init() {
  profile_store_->GetAllLoginsWithAffiliationAndBrandingInformation(this);
  if (account_store_)
    account_store_->GetAllLoginsWithAffiliationAndBrandingInformation(this);
}

void SavedPasswordsPresenter::RemovePassword(const PasswordForm& form) {
  std::string current_form_key = CreateSortKey(form);
  for (const auto& saved_form : passwords_) {
    if (CreateSortKey(saved_form) == current_form_key) {
      PasswordStore& store =
          saved_form.IsUsingAccountStore() ? *account_store_ : *profile_store_;
      store.RemoveLogin(saved_form);
    }
  }
}

bool SavedPasswordsPresenter::EditPassword(const PasswordForm& form,
                                           std::u16string new_password) {
  auto is_equal = [&form](const PasswordForm& form_to_check) {
    return ArePasswordFormUniqueKeysEqual(form, form_to_check);
  };
  auto found = base::ranges::find_if(passwords_, is_equal);
  if (found == passwords_.end())
    return false;

  found->password_value = std::move(new_password);
  PasswordStore& store =
      form.IsUsingAccountStore() ? *account_store_ : *profile_store_;
  store.UpdateLogin(*found);
  NotifyEdited(*found);
  return true;
}

bool SavedPasswordsPresenter::EditSavedPasswords(
    const PasswordForm& form,
    const std::u16string& new_username,
    const std::u16string& new_password) {
  // TODO(crbug.com/1184691): Adapt this code to support credentials
  // coming from both account and profile store, then change desktop
  // settings and maybe iOS to use this presenter for updating the duplicates.
  std::vector<PasswordForm> forms_to_change;
  std::string current_form_key = CreateSortKey(form);
  for (const auto& saved_form : passwords_) {
    if (CreateSortKey(saved_form) == current_form_key)
      forms_to_change.push_back(saved_form);
  }
  return EditSavedPasswords(forms_to_change, new_username, new_password);
}

bool SavedPasswordsPresenter::EditSavedPasswords(
    const SavedPasswordsView forms,
    const std::u16string& new_username,
    const std::u16string& new_password) {
  IsUsernameChanged username_changed(new_username != forms[0].username_value);
  IsPasswordChanged password_changed(new_password != forms[0].password_value);

  if (new_password.empty())
    return false;
  if (username_changed &&
      IsUsernameAlreadyUsed(passwords_, forms, new_username))
    return false;

  // An updated username implies a change in the primary key, thus we need to
  // make sure to call the right API. Update every entry in the equivalence
  // class.
  if (username_changed || password_changed) {
    for (const auto& old_form : forms) {
      PasswordStore& store =
          old_form.IsUsingAccountStore() ? *account_store_ : *profile_store_;

      PasswordForm new_form = old_form;
      new_form.username_value = new_username;
      new_form.password_value = new_password;

      if (username_changed) {
        // Changing username requires deleting old form and adding new one. So
        // the different API should be called.
        store.UpdateLoginWithPrimaryKey(new_form, old_form);
      } else {
        store.UpdateLogin(new_form);
      }
      NotifyEdited(new_form);
    }
  }

  password_manager::metrics_util::LogPasswordEditResult(username_changed,
                                                        password_changed);
  return true;
}

SavedPasswordsPresenter::SavedPasswordsView
SavedPasswordsPresenter::GetSavedPasswords() const {
  return passwords_;
}

std::vector<std::u16string> SavedPasswordsPresenter::GetUsernamesForRealm(
    const std::string& signon_realm,
    bool is_using_account_store) {
  std::vector<std::u16string> usernames;
  for (const auto& form : passwords_) {
    if (form.signon_realm == signon_realm &&
        form.IsUsingAccountStore() == is_using_account_store) {
      usernames.push_back(form.username_value);
    }
  }
  return usernames;
}

void SavedPasswordsPresenter::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void SavedPasswordsPresenter::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void SavedPasswordsPresenter::NotifyEdited(const PasswordForm& password) {
  for (auto& observer : observers_)
    observer.OnEdited(password);
}

void SavedPasswordsPresenter::NotifySavedPasswordsChanged() {
  for (auto& observer : observers_)
    observer.OnSavedPasswordsChanged(passwords_);
}

void SavedPasswordsPresenter::OnLoginsChanged(
    const PasswordStoreChangeList& changes) {
  // This class overrides OnLoginsChangedIn() (the version of this
  // method that also receives the originating store), so the store-less version
  // never gets called.
  NOTREACHED();
}

void SavedPasswordsPresenter::OnLoginsChangedIn(
    PasswordStore* store,
    const PasswordStoreChangeList& changes) {
  store->GetAllLoginsWithAffiliationAndBrandingInformation(this);
}

void SavedPasswordsPresenter::OnGetPasswordStoreResults(
    std::vector<std::unique_ptr<PasswordForm>> results) {
  // This class overrides OnGetPasswordStoreResultsFrom() (the version of this
  // method that also receives the originating store), so the store-less version
  // never gets called.
  NOTREACHED();
}

void SavedPasswordsPresenter::OnGetPasswordStoreResultsFrom(
    PasswordStore* store,
    std::vector<std::unique_ptr<PasswordForm>> results) {
  // Ignore blocked or federated credentials.
  base::EraseIf(results, [](const auto& form) {
    return form->blocked_by_user || form->IsFederatedCredential();
  });

  // Profile store passwords are always stored first in `passwords_`.
  auto account_passwords_it = base::ranges::partition_point(
      passwords_,
      [](auto& password) { return !password.IsUsingAccountStore(); });
  if (store == profile_store_) {
    // Old profile store passwords are in front. Create a temporary buffer for
    // the new passwords and replace existing passwords.
    std::vector<PasswordForm> new_passwords;
    new_passwords.reserve(results.size() + passwords_.end() -
                          account_passwords_it);
    auto new_passwords_back_inserter = std::back_inserter(new_passwords);
    base::ranges::transform(results, new_passwords_back_inserter,
                            [](auto& result) { return std::move(*result); });
    std::move(account_passwords_it, passwords_.end(),
              new_passwords_back_inserter);
    passwords_ = std::move(new_passwords);
  } else {
    // Need to replace existing account passwords at the end. Can re-use
    // existing `passwords_` vector.
    passwords_.erase(account_passwords_it, passwords_.end());
    if (passwords_.capacity() < passwords_.size() + results.size())
      passwords_.reserve(passwords_.size() + results.size());
    base::ranges::transform(results, std::back_inserter(passwords_),
                            [](auto& result) { return std::move(*result); });
  }
  NotifySavedPasswordsChanged();
}

}  // namespace password_manager
