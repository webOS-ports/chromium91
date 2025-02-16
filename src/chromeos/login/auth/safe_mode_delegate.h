// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_LOGIN_AUTH_SAFE_MODE_DELEGATE_H_
#define CHROMEOS_LOGIN_AUTH_SAFE_MODE_DELEGATE_H_

#include "base/callback.h"
#include "base/component_export.h"

namespace chromeos {

class UserContext;

// In case when DeviceSettings get corrupted, it is not possible to rely
// on them to determine if particular user can sign in. In that case
// device enters "Safe Mode" where only owner can sign in (to recreate
// DeviceSettings).
// As we can not rely on DeviceSettings to determine an owner, the only
// option is to correct signing key is present inside user's cryptohome,
// and to abort sign-in process if there is no valid key.
// This inteface abstracts interface between code that performs cryptohome
// mounting and code that has access to DeviceSettings.

class COMPONENT_EXPORT(CHROMEOS_LOGIN_AUTH) SafeModeDelegate {
 public:
  SafeModeDelegate() = default;
  virtual ~SafeModeDelegate() = default;

  // Not copyable or movable.
  SafeModeDelegate(const SafeModeDelegate&) = delete;
  SafeModeDelegate& operator=(const SafeModeDelegate&) = delete;

  using IsOwnerCallback = base::OnceCallback<void(bool is_owner)>;

  // Method to be implemented in child. Return |true| if device is running
  // in safe mode.
  virtual bool IsSafeMode() = 0;

  // Method to be implemented in child. Have to call |callback| with boolean
  // parameter that indicates if user in |context| can act as an owner in
  // safe mode.
  virtual void CheckSafeModeOwnership(const UserContext& context,
                                      IsOwnerCallback callback) = 0;
};

}  // namespace chromeos

#endif  // CHROMEOS_LOGIN_AUTH_SAFE_MODE_DELEGATE_H_
