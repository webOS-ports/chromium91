// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/embedder_support/origin_trials/component_updater_utils.h"

#include <string>
#include <utility>

#include "base/files/scoped_temp_dir.h"
#include "base/values.h"
#include "base/version.h"
#include "components/component_updater/installer_policies/origin_trials_component_installer.h"
#include "components/embedder_support/origin_trials/origin_trial_prefs.h"
#include "components/embedder_support/origin_trials/pref_names.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/platform_test.h"

namespace {

// Mirror the constants used in the component installer. Do not share the
// constants, as want to catch inadvertent changes in the tests. The keys will
// will be generated server-side, so any changes need to be intentional and
// coordinated.
static const char kManifestOriginTrialsKey[] = "origin-trials";
static const char kManifestPublicKeyPath[] = "origin-trials.public-key";
static const char kManifestDisabledFeaturesPath[] =
    "origin-trials.disabled-features";
static const char kManifestDisabledTokensPath[] =
    "origin-trials.disabled-tokens";
static const char kManifestDisabledTokenSignaturesPath[] =
    "origin-trials.disabled-tokens.signatures";

static const char kExistingPublicKey[] = "existing public key";
static const char kNewPublicKey[] = "new public key";
static const char kExistingDisabledFeature[] = "already disabled";
static const std::vector<std::string> kExistingDisabledFeatures = {
    kExistingDisabledFeature};
static const char kNewDisabledFeature1[] = "newly disabled 1";
static const char kNewDisabledFeature2[] = "newly disabled 2";
static const std::vector<std::string> kNewDisabledFeatures = {
    kNewDisabledFeature1, kNewDisabledFeature2};
static const char kExistingDisabledToken[] = "already disabled token";
static const std::vector<std::string> kExistingDisabledTokens = {
    kExistingDisabledToken};
static const char kNewDisabledToken1[] = "newly disabled token 1";
static const char kNewDisabledToken2[] = "newly disabled token 2";
static const std::vector<std::string> kNewDisabledTokens = {kNewDisabledToken1,
                                                            kNewDisabledToken2};

}  // namespace

namespace component_updater {

class OriginTrialsComponentInstallerTest : public PlatformTest {
 public:
  OriginTrialsComponentInstallerTest() = default;

  void SetUp() override {
    PlatformTest::SetUp();

    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    embedder_support::OriginTrialPrefs::RegisterPrefs(local_state_.registry());
    policy_ = std::make_unique<OriginTrialsComponentInstallerPolicy>();
  }

  void LoadUpdates(std::unique_ptr<base::DictionaryValue> manifest) {
    if (!manifest) {
      manifest = std::make_unique<base::DictionaryValue>();
      manifest->Set(kManifestOriginTrialsKey, std::make_unique<base::Value>());
    }
    ASSERT_TRUE(policy_->VerifyInstallation(*manifest, temp_dir_.GetPath()));
    embedder_support::ReadOriginTrialsConfigAndPopulateLocalState(
        local_state(), std::move(manifest));
  }

  void AddDisabledFeaturesToPrefs(const std::vector<std::string>& features) {
    base::ListValue disabled_feature_list;
    disabled_feature_list.AppendStrings(features);
    ListPrefUpdate update(
        local_state(), embedder_support::prefs::kOriginTrialDisabledFeatures);
    update->Swap(&disabled_feature_list);
  }

  void CheckDisabledFeaturesPrefs(const std::vector<std::string>& features) {
    ASSERT_FALSE(features.empty());

    ASSERT_TRUE(local_state()->HasPrefPath(
        embedder_support::prefs::kOriginTrialDisabledFeatures));

    const base::ListValue* disabled_feature_list = local_state()->GetList(
        embedder_support::prefs::kOriginTrialDisabledFeatures);
    ASSERT_TRUE(disabled_feature_list);

    ASSERT_EQ(features.size(), disabled_feature_list->GetSize());

    std::string disabled_feature;
    for (size_t i = 0; i < features.size(); ++i) {
      const bool found = disabled_feature_list->GetString(i, &disabled_feature);
      EXPECT_TRUE(found) << "Entry not found or not a string at index " << i;
      if (!found) {
        continue;
      }
      EXPECT_EQ(features[i], disabled_feature)
          << "Feature lists differ at index " << i;
    }
  }

  void AddDisabledTokensToPrefs(const std::vector<std::string>& tokens) {
    base::ListValue disabled_token_list;
    disabled_token_list.AppendStrings(tokens);
    ListPrefUpdate update(local_state(),
                          embedder_support::prefs::kOriginTrialDisabledTokens);
    update->Swap(&disabled_token_list);
  }

  void CheckDisabledTokensPrefs(const std::vector<std::string>& tokens) {
    ASSERT_FALSE(tokens.empty());

    ASSERT_TRUE(local_state()->HasPrefPath(
        embedder_support::prefs::kOriginTrialDisabledTokens));

    const base::ListValue* disabled_token_list = local_state()->GetList(
        embedder_support::prefs::kOriginTrialDisabledTokens);
    ASSERT_TRUE(disabled_token_list);

    ASSERT_EQ(tokens.size(), disabled_token_list->GetSize());

    std::string disabled_token;
    for (size_t i = 0; i < tokens.size(); ++i) {
      const bool found = disabled_token_list->GetString(i, &disabled_token);
      EXPECT_TRUE(found) << "Entry not found or not a string at index " << i;
      if (!found) {
        continue;
      }
      EXPECT_EQ(tokens[i], disabled_token)
          << "Token lists differ at index " << i;
    }
  }

  PrefService* local_state() { return &local_state_; }

 protected:
  base::ScopedTempDir temp_dir_;
  TestingPrefServiceSimple local_state_;
  std::unique_ptr<ComponentInstallerPolicy> policy_;

 private:
  DISALLOW_COPY_AND_ASSIGN(OriginTrialsComponentInstallerTest);
};

TEST_F(OriginTrialsComponentInstallerTest,
       PublicKeyResetToDefaultWhenOverrideMissing) {
  local_state()->SetString(embedder_support::prefs::kOriginTrialPublicKey,
                           kExistingPublicKey);
  ASSERT_EQ(
      kExistingPublicKey,
      local_state()->GetString(embedder_support::prefs::kOriginTrialPublicKey));

  // Load with empty section in manifest
  LoadUpdates(nullptr);

  EXPECT_FALSE(local_state()->HasPrefPath(
      embedder_support::prefs::kOriginTrialPublicKey));
}

TEST_F(OriginTrialsComponentInstallerTest, PublicKeySetWhenOverrideExists) {
  ASSERT_FALSE(local_state()->HasPrefPath(
      embedder_support::prefs::kOriginTrialPublicKey));

  auto manifest = std::make_unique<base::DictionaryValue>();
  manifest->SetString(kManifestPublicKeyPath, kNewPublicKey);
  LoadUpdates(std::move(manifest));

  EXPECT_EQ(kNewPublicKey, local_state()->GetString(
                               embedder_support::prefs::kOriginTrialPublicKey));
}

TEST_F(OriginTrialsComponentInstallerTest,
       DisabledFeaturesResetToDefaultWhenListMissing) {
  AddDisabledFeaturesToPrefs(kExistingDisabledFeatures);
  ASSERT_TRUE(local_state()->HasPrefPath(
      embedder_support::prefs::kOriginTrialDisabledFeatures));

  // Load with empty section in manifest
  LoadUpdates(nullptr);

  EXPECT_FALSE(local_state()->HasPrefPath(
      embedder_support::prefs::kOriginTrialDisabledFeatures));
}

TEST_F(OriginTrialsComponentInstallerTest,
       DisabledFeaturesResetToDefaultWhenListEmpty) {
  AddDisabledFeaturesToPrefs(kExistingDisabledFeatures);
  ASSERT_TRUE(local_state()->HasPrefPath(
      embedder_support::prefs::kOriginTrialDisabledFeatures));

  auto manifest = std::make_unique<base::DictionaryValue>();
  auto disabled_feature_list = std::make_unique<base::ListValue>();
  manifest->Set(kManifestDisabledFeaturesPath,
                std::move(disabled_feature_list));

  LoadUpdates(std::move(manifest));

  EXPECT_FALSE(local_state()->HasPrefPath(
      embedder_support::prefs::kOriginTrialDisabledFeatures));
}

TEST_F(OriginTrialsComponentInstallerTest, DisabledFeaturesSetWhenListExists) {
  ASSERT_FALSE(local_state()->HasPrefPath(
      embedder_support::prefs::kOriginTrialDisabledFeatures));

  auto manifest = std::make_unique<base::DictionaryValue>();
  auto disabled_feature_list = std::make_unique<base::ListValue>();
  disabled_feature_list->AppendString(kNewDisabledFeature1);
  manifest->Set(kManifestDisabledFeaturesPath,
                std::move(disabled_feature_list));

  LoadUpdates(std::move(manifest));

  std::vector<std::string> features = {kNewDisabledFeature1};
  CheckDisabledFeaturesPrefs(features);
}

TEST_F(OriginTrialsComponentInstallerTest,
       DisabledFeaturesReplacedWhenListExists) {
  AddDisabledFeaturesToPrefs(kExistingDisabledFeatures);
  ASSERT_TRUE(local_state()->HasPrefPath(
      embedder_support::prefs::kOriginTrialDisabledFeatures));

  auto manifest = std::make_unique<base::DictionaryValue>();
  auto disabled_feature_list = std::make_unique<base::ListValue>();
  disabled_feature_list->AppendStrings(kNewDisabledFeatures);
  manifest->Set(kManifestDisabledFeaturesPath,
                std::move(disabled_feature_list));

  LoadUpdates(std::move(manifest));

  CheckDisabledFeaturesPrefs(kNewDisabledFeatures);
}

TEST_F(OriginTrialsComponentInstallerTest,
       DisabledTokensResetToDefaultWhenListMissing) {
  AddDisabledTokensToPrefs(kExistingDisabledTokens);
  ASSERT_TRUE(local_state()->HasPrefPath(
      embedder_support::prefs::kOriginTrialDisabledTokens));

  // Load with empty section in manifest
  LoadUpdates(nullptr);

  EXPECT_FALSE(local_state()->HasPrefPath(
      embedder_support::prefs::kOriginTrialDisabledTokens));
}

TEST_F(OriginTrialsComponentInstallerTest,
       DisabledTokensResetToDefaultWhenKeyExistsAndListMissing) {
  AddDisabledTokensToPrefs(kExistingDisabledTokens);
  ASSERT_TRUE(local_state()->HasPrefPath(
      embedder_support::prefs::kOriginTrialDisabledTokens));

  // Load with disabled tokens key in manifest, but no list values
  auto manifest = std::make_unique<base::DictionaryValue>();
  manifest->Set(kManifestDisabledTokensPath, std::make_unique<base::Value>());

  LoadUpdates(std::move(manifest));

  EXPECT_FALSE(local_state()->HasPrefPath(
      embedder_support::prefs::kOriginTrialDisabledTokens));
}

TEST_F(OriginTrialsComponentInstallerTest,
       DisabledTokensResetToDefaultWhenListEmpty) {
  AddDisabledTokensToPrefs(kExistingDisabledTokens);
  ASSERT_TRUE(local_state()->HasPrefPath(
      embedder_support::prefs::kOriginTrialDisabledTokens));

  auto manifest = std::make_unique<base::DictionaryValue>();
  auto disabled_token_list = std::make_unique<base::ListValue>();
  manifest->Set(kManifestDisabledTokenSignaturesPath,
                std::move(disabled_token_list));

  LoadUpdates(std::move(manifest));

  EXPECT_FALSE(local_state()->HasPrefPath(
      embedder_support::prefs::kOriginTrialDisabledTokens));
}

TEST_F(OriginTrialsComponentInstallerTest, DisabledTokensSetWhenListExists) {
  ASSERT_FALSE(local_state()->HasPrefPath(
      embedder_support::prefs::kOriginTrialDisabledTokens));

  auto manifest = std::make_unique<base::DictionaryValue>();
  auto disabled_token_list = std::make_unique<base::ListValue>();
  disabled_token_list->AppendString(kNewDisabledToken1);
  manifest->Set(kManifestDisabledTokenSignaturesPath,
                std::move(disabled_token_list));

  LoadUpdates(std::move(manifest));

  std::vector<std::string> tokens = {kNewDisabledToken1};
  CheckDisabledTokensPrefs(tokens);
}

TEST_F(OriginTrialsComponentInstallerTest,
       DisabledTokensReplacedWhenListExists) {
  AddDisabledTokensToPrefs(kExistingDisabledTokens);
  ASSERT_TRUE(local_state()->HasPrefPath(
      embedder_support::prefs::kOriginTrialDisabledTokens));

  auto manifest = std::make_unique<base::DictionaryValue>();
  auto disabled_token_list = std::make_unique<base::ListValue>();
  disabled_token_list->AppendStrings(kNewDisabledTokens);
  manifest->Set(kManifestDisabledTokenSignaturesPath,
                std::move(disabled_token_list));

  LoadUpdates(std::move(manifest));

  CheckDisabledTokensPrefs(kNewDisabledTokens);
}

}  // namespace component_updater
