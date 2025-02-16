// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/autofill_assistant/browser/script_parameters.h"

#include "components/autofill_assistant/browser/test_util.h"
#include "testing/gmock/include/gmock/gmock.h"

namespace autofill_assistant {

using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Pair;
using ::testing::UnorderedElementsAreArray;

TEST(ScriptParametersTest, Create) {
  ScriptParameters parameters = {{{"key_a", "value_a"}, {"key_b", "value_b"}}};
  EXPECT_THAT(parameters.ToProto(),
              UnorderedElementsAreArray(std::map<std::string, std::string>(
                  {{"key_a", "value_a"}, {"key_b", "value_b"}})));
}

TEST(ScriptParametersTest, MergeEmpty) {
  ScriptParameters merged;
  EXPECT_THAT(merged.ToProto(), IsEmpty());
  merged.MergeWith(ScriptParameters());
  EXPECT_THAT(merged.ToProto(), IsEmpty());
}

TEST(ScriptParametersTest, MergeEmptyWithNonEmpty) {
  ScriptParameters empty;
  empty.MergeWith({{{"key_a", "value_a"}}});
  EXPECT_THAT(empty.ToProto(),
              UnorderedElementsAreArray(
                  std::map<std::string, std::string>({{"key_a", "value_a"}})));
}

TEST(ScriptParametersTest, MergeNonEmptyWithEmpty) {
  ScriptParameters parameters = {{{"key_a", "value_a"}}};
  parameters.MergeWith(ScriptParameters());
  EXPECT_THAT(parameters.ToProto(),
              UnorderedElementsAreArray(
                  std::map<std::string, std::string>({{"key_a", "value_a"}})));
}

TEST(ScriptParametersTest, MergeNonEmptyWithNonEmpty) {
  ScriptParameters parameters_a = {{{"key_a", "value_a"}}};
  ScriptParameters parameters_b = {
      {{"key_a", "value_a_changed"}, {"key_b", "value_b"}}};

  parameters_a.MergeWith(parameters_b);
  EXPECT_THAT(parameters_a.ToProto(),
              UnorderedElementsAreArray(std::map<std::string, std::string>(
                  {{"key_a", "value_a"}, {"key_b", "value_b"}})));
}

TEST(ScriptParametersTest, TriggerScriptAllowList) {
  ScriptParameters parameters = {{{"DEBUG_BUNDLE_ID", "12345"},
                                  {"key_a", "value_a"},
                                  {"DEBUG_BUNDLE_VERSION", "version"},
                                  {"DEBUG_SOCKET_ID", "678"},
                                  {"FALLBACK_BUNDLE_ID", "fallback_id"},
                                  {"key_b", "value_b"},
                                  {"FALLBACK_BUNDLE_VERSION", "fallback_ver"}}};

  EXPECT_THAT(parameters.ToProto(/* only_trigger_script_allowlisted = */ false),
              UnorderedElementsAreArray(std::map<std::string, std::string>(
                  {{"DEBUG_BUNDLE_ID", "12345"},
                   {"key_a", "value_a"},
                   {"DEBUG_BUNDLE_VERSION", "version"},
                   {"DEBUG_SOCKET_ID", "678"},
                   {"FALLBACK_BUNDLE_ID", "fallback_id"},
                   {"key_b", "value_b"},
                   {"FALLBACK_BUNDLE_VERSION", "fallback_ver"}})));

  EXPECT_THAT(parameters.ToProto(/* only_trigger_script_allowlisted = */ true),
              UnorderedElementsAreArray(std::map<std::string, std::string>(
                  {{"DEBUG_BUNDLE_ID", "12345"},
                   {"DEBUG_BUNDLE_VERSION", "version"},
                   {"DEBUG_SOCKET_ID", "678"},
                   {"FALLBACK_BUNDLE_ID", "fallback_id"},
                   {"FALLBACK_BUNDLE_VERSION", "fallback_ver"}})));
}

TEST(ScriptParametersTest, SpecialScriptParameters) {
  ScriptParameters parameters = {
      {{"ENABLED", "true"},
       {"USER_EMAIL", "example@chromium.org"},
       {"ORIGINAL_DEEPLINK", "https://www.example.com"},
       {"TRIGGER_SCRIPT_EXPERIMENT", "true"},
       {"START_IMMEDIATELY", "false"},
       {"REQUEST_TRIGGER_SCRIPT", "true"},
       {"TRIGGER_SCRIPTS_BASE64", "abc123"},
       {"PASSWORD_CHANGE_USERNAME", "fake_username"},
       {"OVERLAY_COLORS", "#123456"},
       {"DETAILS_SHOW_INITIAL", "true"},
       {"DETAILS_TITLE", "title"},
       {"DETAILS_DESCRIPTION_LINE_1", "line1"},
       {"DETAILS_DESCRIPTION_LINE_2", "line2"},
       {"DETAILS_DESCRIPTION_LINE_3", "line3"},
       {"DETAILS_IMAGE_URL", "image"},
       {"DETAILS_IMAGE_ACCESSIBILITY_HINT", "hint"},
       {"DETAILS_IMAGE_CLICKTHROUGH_URL", "clickthrough"},
       {"DETAILS_TOTAL_PRICE_LABEL", "total"},
       {"DETAILS_TOTAL_PRICE", "12"}}};

  EXPECT_THAT(parameters.GetEnabled(), Eq(true));
  EXPECT_THAT(parameters.GetCallerEmail(), Eq("example@chromium.org"));
  EXPECT_THAT(parameters.GetOriginalDeeplink(), Eq("https://www.example.com"));
  EXPECT_THAT(parameters.GetTriggerScriptExperiment(), Eq(true));
  EXPECT_THAT(parameters.GetStartImmediately(), Eq(false));
  EXPECT_THAT(parameters.GetRequestsTriggerScript(), Eq(true));
  EXPECT_THAT(parameters.GetBase64TriggerScriptsResponseProto(), Eq("abc123"));
  EXPECT_THAT(parameters.GetPasswordChangeUsername(), Eq("fake_username"));
  EXPECT_THAT(parameters.GetOverlayColors(), Eq("#123456"));
  EXPECT_THAT(parameters.GetDetailsShowInitial(), Eq(true));
  EXPECT_THAT(parameters.GetDetailsTitle(), Eq("title"));
  EXPECT_THAT(parameters.GetDetailsDescriptionLine1(), Eq("line1"));
  EXPECT_THAT(parameters.GetDetailsDescriptionLine2(), Eq("line2"));
  EXPECT_THAT(parameters.GetDetailsDescriptionLine3(), Eq("line3"));
  EXPECT_THAT(parameters.GetDetailsImageUrl(), Eq("image"));
  EXPECT_THAT(parameters.GetDetailsImageAccessibilityHint(), Eq("hint"));
  EXPECT_THAT(parameters.GetDetailsImageClickthroughUrl(), Eq("clickthrough"));
  EXPECT_THAT(parameters.GetDetailsTotalPriceLabel(), Eq("total"));
  EXPECT_THAT(parameters.GetDetailsTotalPrice(), Eq("12"));
}

TEST(ScriptParametersTest, ScriptParameterMatch) {
  ScriptParameters parameters = {{{"must_exist_and_exists", "exists"},
                                  {"must_not_exist_and_exists", "exists"},
                                  {"must_match", "matching_value"},
                                  {"must_match_empty", ""}}};

  ScriptParameterMatchProto must_exist;
  must_exist.set_name("must_exist_and_exists");
  must_exist.set_exists(true);
  EXPECT_TRUE(parameters.Matches(must_exist));

  must_exist.set_name("must_exist_and_doesnt_exist");
  EXPECT_FALSE(parameters.Matches(must_exist));

  ScriptParameterMatchProto must_not_exist;
  must_not_exist.set_name("must_not_exist_and_doesnt_exist");
  must_not_exist.set_exists(false);
  EXPECT_TRUE(parameters.Matches(must_not_exist));

  must_not_exist.set_name("must_not_exist_and_exists");
  EXPECT_FALSE(parameters.Matches(must_not_exist));

  ScriptParameterMatchProto must_match;
  must_match.set_name("must_match");
  must_match.set_value_equals("matching_value");
  EXPECT_TRUE(parameters.Matches(must_match));

  must_match.set_value_equals("not_matching_value");
  EXPECT_FALSE(parameters.Matches(must_match));

  must_match.set_value_equals("");
  EXPECT_FALSE(parameters.Matches(must_match));

  must_match.set_name("must_match_doesnt_exist");
  EXPECT_FALSE(parameters.Matches(must_match));

  ScriptParameterMatchProto must_match_empty;
  must_match_empty.set_name("must_match_empty");
  must_match_empty.set_value_equals("");
  EXPECT_TRUE(parameters.Matches(must_match_empty));

  must_match_empty.set_value_equals("not_empty");
  EXPECT_FALSE(parameters.Matches(must_match_empty));
}

TEST(ScriptParametersTest, ToProtoRemovesEnabled) {
  ScriptParameters parameters = {{{"key_a", "value_a"}, {"ENABLED", "true"}}};

  EXPECT_THAT(parameters.ToProto(/* only_trigger_script_allowlisted = */ false),
              UnorderedElementsAreArray(
                  std::map<std::string, std::string>({{"key_a", "value_a"}})));

  EXPECT_THAT(parameters.ToProto(/* only_trigger_script_allowlisted = */ true),
              IsEmpty());
}

}  // namespace autofill_assistant
