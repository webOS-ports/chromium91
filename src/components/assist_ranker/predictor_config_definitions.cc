// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/assist_ranker/predictor_config_definitions.h"
#include "components/assist_ranker/base_predictor.h"

namespace assist_ranker {

#if defined(OS_ANDROID)
const base::Feature kContextualSearchRankerQuery{
    "ContextualSearchRankerQuery", base::FEATURE_DISABLED_BY_DEFAULT};

namespace {

const char kContextualSearchModelName[] = "contextual_search_model";
const char kContextualSearchLoggingName[] = "ContextualSearch";
const char kContextualSearchUmaPrefixName[] = "Search.ContextualSearch.Ranker";

const char kContextualSearchDefaultModelUrl[] =
    "https://www.gstatic.com/chrome/intelligence/assist/ranker/models/"
    "contextual_search/test_ranker_model_20171109_short_words_v2.pb.bin";

const base::FeatureParam<std::string>*
GetContextualSearchRankerUrlFeatureParam() {
  static auto* kContextualSearchRankerUrl = new base::FeatureParam<std::string>(
      &kContextualSearchRankerQuery, "contextual-search-ranker-model-url",
      kContextualSearchDefaultModelUrl);
  return kContextualSearchRankerUrl;
}

float GetContextualSearchRankerThresholdFeatureParam() {
  static auto* kContextualSearchRankerThreshold =
      new base::FeatureParam<double>(
          &kContextualSearchRankerQuery,
          "contextual-search-ranker-predict-threshold",
          kNoPredictThresholdReplacement);
  return static_cast<float>(kContextualSearchRankerThreshold->Get());
}

// NOTE: This list needs to be kept in sync with tools/metrics/ukm/ukm.xml!
// Only features within this list will be logged to UKM.
// TODO(chrome-ranker-team) Deprecate the allowlist once it is available through
// the UKM generated API.
const base::flat_set<std::string>* GetContextualSearchFeatureAllowlist() {
  static auto* kContextualSearchFeatureAllowlist =
      new base::flat_set<std::string>({"DidOptIn",
                                       "DurationAfterScrollMs",
                                       "EntityImpressionsCount",
                                       "EntityOpensCount",
                                       "FontSize",
                                       "IsEntity",
                                       "IsEntityEligible",
                                       "IsHttp",
                                       "IsLanguageMismatch",
                                       "IsLongWord",
                                       "IsSecondTapOverride",
                                       "IsShortWord",
                                       "IsWordEdge",
                                       "OpenCount",
                                       "OutcomeRankerDidPredict",
                                       "OutcomeRankerPrediction",
                                       "OutcomeRankerPredictionScore",
                                       "OutcomeWasCardsDataShown",
                                       "OutcomeWasPanelOpened",
                                       "OutcomeWasQuickActionClicked",
                                       "OutcomeWasQuickAnswerSeen",
                                       "PortionOfElement",
                                       "Previous28DayCtrPercent",
                                       "Previous28DayImpressionsCount",
                                       "PreviousWeekCtrPercent",
                                       "PreviousWeekImpressionsCount",
                                       "QuickActionImpressionsCount",
                                       "QuickActionsIgnored",
                                       "QuickActionsTaken",
                                       "QuickAnswerCount",
                                       "ScreenTopDps",
                                       "TapCount",
                                       "TapDurationMs",
                                       "WasScreenBottom"});
  return kContextualSearchFeatureAllowlist;
}

}  // namespace

const PredictorConfig GetContextualSearchPredictorConfig() {
  static auto kContextualSearchPredictorConfig = *(new PredictorConfig(
      kContextualSearchModelName, kContextualSearchLoggingName,
      kContextualSearchUmaPrefixName, LOG_UKM,
      GetContextualSearchFeatureAllowlist(), &kContextualSearchRankerQuery,
      GetContextualSearchRankerUrlFeatureParam(),
      GetContextualSearchRankerThresholdFeatureParam()));
  return kContextualSearchPredictorConfig;
}
#endif  // OS_ANDROID

}  // namespace assist_ranker
