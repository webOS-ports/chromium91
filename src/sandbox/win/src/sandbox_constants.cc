// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sandbox/win/src/sandbox_constants.h"

namespace sandbox {
// Strings used as keys in base::Value snapshots of Policies for WebUI.
extern const char kAppContainerCapabilities[] = "appContainerCapabilities";
extern const char kAppContainerInitialCapabilities[] =
    "appContainerInitialCapabilities";
extern const char kAppContainerSid[] = "appContainerSid";
extern const char kDesiredIntegrityLevel[] = "desiredIntegrityLevel";
extern const char kDesiredMitigations[] = "desiredMitigations";
extern const char kDisconnectCsrss[] = "disconnectCsrss";
extern const char kHandlesToClose[] = "handlesToClose";
extern const char kJobLevel[] = "jobLevel";
extern const char kLockdownLevel[] = "lockdownLevel";
extern const char kLowboxSid[] = "lowboxSid";
extern const char kPlatformMitigations[] = "platformMitigations";
extern const char kPolicyRules[] = "policyRules";
extern const char kProcessIds[] = "processIds";

// Strings used as values in snapshots of Policies.
extern const char kDisabled[] = "disabled";
extern const char kEnabled[] = "enabled";
}  // namespace sandbox
