// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/public/cpp/holding_space/holding_space_prefs.h"

#include "ash/public/cpp/ash_features.h"
#include "base/time/time.h"
#include "base/util/values/values_util.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"

namespace ash {
namespace holding_space_prefs {

namespace {

// Boolean preference storing if holding space previews are enabled.
constexpr char kPreviewsEnabled[] = "ash.holding_space.previews_enabled";

// Time preference storing when an item was first added to holding space.
constexpr char kTimeOfFirstAdd[] = "ash.holding_space.time_of_first_add";

// Time preference storing when holding space first became available.
constexpr char kTimeOfFirstAvailability[] =
    "ash.holding_space.time_of_first_availability";

// Time preference storing when holding space was first entered.
constexpr char kTimeOfFirstEntry[] = "ash.holding_space.time_of_first_entry";

// Time preference storing when the first pin to holding space occurred.
constexpr char kTimeOfFirstPin[] = "ash.holding_space.time_of_first_pin";

}  // namespace

void RegisterProfilePrefs(PrefRegistrySimple* registry) {
  registry->RegisterBooleanPref(
      kPreviewsEnabled, features::IsTemporaryHoldingSpacePreviewsEnabled());
  registry->RegisterTimePref(kTimeOfFirstAdd, base::Time::UnixEpoch());
  registry->RegisterTimePref(kTimeOfFirstAvailability, base::Time::UnixEpoch());
  registry->RegisterTimePref(kTimeOfFirstEntry, base::Time::UnixEpoch());
  registry->RegisterTimePref(kTimeOfFirstPin, base::Time::UnixEpoch());
}

void AddPreviewsEnabledChangedCallback(PrefChangeRegistrar* registrar,
                                       base::RepeatingClosure callback) {
  registrar->Add(kPreviewsEnabled, std::move(callback));
}

bool IsPreviewsEnabled(PrefService* prefs) {
  return prefs->GetBoolean(kPreviewsEnabled);
}

void SetPreviewsEnabled(PrefService* prefs, bool enabled) {
  prefs->SetBoolean(kPreviewsEnabled, enabled);
}

base::Optional<base::Time> GetTimeOfFirstAdd(PrefService* prefs) {
  // The `kTimeOfFirstAdd` preference was added after the `kTimeOfFirstPin`
  // preference, so if the `kTimeOfFirstAdd` has not yet been marked it's
  // possible that the user may still have pinned a file at an earlier time.
  auto* pref = prefs->FindPreference(kTimeOfFirstAdd);
  return pref->IsDefaultValue() ? GetTimeOfFirstPin(prefs)
                                : util::ValueToTime(pref->GetValue());
}

bool MarkTimeOfFirstAdd(PrefService* prefs) {
  if (prefs->FindPreference(kTimeOfFirstAdd)->IsDefaultValue()) {
    // The `kTimeOfFirstAdd` preference was added after the `kTimeOfFirstPin`
    // preference, so it's possible that this is not actually the first time an
    // item has been added to holding space. If `kTimeOfFirstPin` was previously
    // recorded, that will be more accurate than using `base::Time::Now()`.
    prefs->SetTime(kTimeOfFirstAdd,
                   GetTimeOfFirstPin(prefs).value_or(base::Time::Now()));
    return true;
  }
  return false;
}

base::Optional<base::Time> GetTimeOfFirstAvailability(PrefService* prefs) {
  auto* pref = prefs->FindPreference(kTimeOfFirstAvailability);
  return pref->IsDefaultValue() ? base::nullopt
                                : util::ValueToTime(pref->GetValue());
}

bool MarkTimeOfFirstAvailability(PrefService* prefs) {
  if (prefs->FindPreference(kTimeOfFirstAvailability)->IsDefaultValue()) {
    prefs->SetTime(kTimeOfFirstAvailability, base::Time::Now());
    return true;
  }
  return false;
}

base::Optional<base::Time> GetTimeOfFirstEntry(PrefService* prefs) {
  auto* pref = prefs->FindPreference(kTimeOfFirstEntry);
  return pref->IsDefaultValue() ? base::nullopt
                                : util::ValueToTime(pref->GetValue());
}

bool MarkTimeOfFirstEntry(PrefService* prefs) {
  if (prefs->FindPreference(kTimeOfFirstEntry)->IsDefaultValue()) {
    prefs->SetTime(kTimeOfFirstEntry, base::Time::Now());
    return true;
  }
  return false;
}

base::Optional<base::Time> GetTimeOfFirstPin(PrefService* prefs) {
  auto* pref = prefs->FindPreference(kTimeOfFirstPin);
  return pref->IsDefaultValue() ? base::nullopt
                                : util::ValueToTime(pref->GetValue());
}

bool MarkTimeOfFirstPin(PrefService* prefs) {
  if (prefs->FindPreference(kTimeOfFirstPin)->IsDefaultValue()) {
    prefs->SetTime(kTimeOfFirstPin, base::Time::Now());
    return true;
  }
  return false;
}

}  // namespace holding_space_prefs
}  // namespace ash
