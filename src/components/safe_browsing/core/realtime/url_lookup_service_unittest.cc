// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/safe_browsing/core/realtime/url_lookup_service.h"

#include "base/bind.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/mock_callback.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "build/build_config.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/safe_browsing/buildflags.h"
#include "components/safe_browsing/core/browser/referrer_chain_provider.h"
#include "components/safe_browsing/core/browser/safe_browsing_token_fetcher.h"
#include "components/safe_browsing/core/common/safe_browsing_prefs.h"
#include "components/safe_browsing/core/common/test_task_environment.h"
#include "components/safe_browsing/core/features.h"
#include "components/safe_browsing/core/verdict_cache_manager.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "components/unified_consent/pref_names.h"
#include "components/unified_consent/unified_consent_service.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/platform_test.h"

using ::testing::_;
using testing::DoAll;
using testing::Return;
using testing::SetArgPointee;

namespace safe_browsing {

namespace {
constexpr char kRealTimeLookupUrlPrefix[] =
    "https://safebrowsing.google.com/safebrowsing/clientreport/realtime";

constexpr char kTestUrl[] = "http://example.test/";
constexpr char kTestReferrerUrl[] = "http://example.referrer/";
constexpr char kTestSubframeUrl[] = "http://iframe.example.test/";
constexpr char kTestSubframeReferrerUrl[] = "http://iframe.example.referrer/";

class TestSafeBrowsingTokenFetcher : public SafeBrowsingTokenFetcher {
 public:
  TestSafeBrowsingTokenFetcher() = default;
  ~TestSafeBrowsingTokenFetcher() override = default;

  // SafeBrowsingTokenFetcher:
  void Start(Callback callback) override { callback_ = std::move(callback); }

  void RunAccessTokenCallback(std::string token) {
    std::move(callback_).Run(token);
  }

 private:
  Callback callback_;
};

class MockReferrerChainProvider : public ReferrerChainProvider {
 public:
  virtual ~MockReferrerChainProvider() = default;
  MOCK_METHOD3(IdentifyReferrerChainByWebContents,
               AttributionResult(content::WebContents* web_contents,
                                 int user_gesture_count_limit,
                                 ReferrerChain* out_referrer_chain));
  MOCK_METHOD4(IdentifyReferrerChainByEventURL,
               AttributionResult(const GURL& event_url,
                                 SessionID event_tab_id,
                                 int user_gesture_count_limit,
                                 ReferrerChain* out_referrer_chain));
  MOCK_METHOD3(IdentifyReferrerChainByPendingEventURL,
               AttributionResult(const GURL& event_url,
                                 int user_gesture_count_limit,
                                 ReferrerChain* out_referrer_chain));
};

}  // namespace

class RealTimeUrlLookupServiceTest : public PlatformTest {
 public:
  void SetUp() override {
    HostContentSettingsMap::RegisterProfilePrefs(test_pref_service_.registry());
    safe_browsing::RegisterProfilePrefs(test_pref_service_.registry());
    task_environment_ = CreateTestTaskEnvironment(
        base::test::TaskEnvironment::TimeSource::MOCK_TIME);
    PlatformTest::SetUp();

    test_shared_loader_factory_ =
        base::MakeRefCounted<network::WeakWrapperSharedURLLoaderFactory>(
            &test_url_loader_factory_);

    content_setting_map_ = new HostContentSettingsMap(
        &test_pref_service_, false /* is_off_the_record */,
        false /* store_last_modified */,
        false /* restore_session */);
    cache_manager_ = std::make_unique<VerdictCacheManager>(
        nullptr, content_setting_map_.get());
    referrer_chain_provider_ = std::make_unique<MockReferrerChainProvider>();

    auto token_fetcher = std::make_unique<TestSafeBrowsingTokenFetcher>();
    raw_token_fetcher_ = token_fetcher.get();
    rt_service_ = std::make_unique<RealTimeUrlLookupService>(
        test_shared_loader_factory_, cache_manager_.get(),
        base::BindRepeating(
            [](PrefService* pref_service) {
              ChromeUserPopulation population;
              population.set_user_population(
                  IsEnhancedProtectionEnabled(*pref_service)
                      ? ChromeUserPopulation::ENHANCED_PROTECTION
                      : IsExtendedReportingEnabled(*pref_service)
                            ? ChromeUserPopulation::EXTENDED_REPORTING
                            : ChromeUserPopulation::SAFE_BROWSING);
              population.set_profile_management_status(
                  ChromeUserPopulation::NOT_MANAGED);
              population.set_is_history_sync_enabled(true);
              population.set_is_under_advanced_protection(true);
              population.set_is_incognito(false);
              return population;
            },
            &test_pref_service_),
        &test_pref_service_, std::move(token_fetcher),
        base::BindRepeating(
            &RealTimeUrlLookupServiceTest::AreTokenFetchesConfiguredInClient,
            base::Unretained(this)),
        /*is_off_the_record=*/false, /*variations_service=*/nullptr,
        referrer_chain_provider_.get());
  }

  void TearDown() override {
    cache_manager_.reset();
    content_setting_map_->ShutdownOnUIThread();
  }

  bool CanCheckUrl(const GURL& url) {
    return RealTimeUrlLookupServiceBase::CanCheckUrl(url);
  }
  void HandleLookupError() { rt_service_->HandleLookupError(); }
  void HandleLookupSuccess() { rt_service_->HandleLookupSuccess(); }
  bool IsInBackoffMode() { return rt_service_->IsInBackoffMode(); }
  std::unique_ptr<RTLookupRequest> FillRequestProto(const GURL& url) {
    return rt_service_->FillRequestProto(url);
  }
  std::unique_ptr<RTLookupResponse> GetCachedRealTimeUrlVerdict(
      const GURL& url) {
    return rt_service_->GetCachedRealTimeUrlVerdict(url);
  }

  void MayBeCacheRealTimeUrlVerdict(
      GURL url,
      RTLookupResponse::ThreatInfo::VerdictType verdict_type,
      RTLookupResponse::ThreatInfo::ThreatType threat_type,
      int cache_duration_sec,
      const std::string& cache_expression,
      RTLookupResponse::ThreatInfo::CacheExpressionMatchType
          cache_expression_match_type) {
    RTLookupResponse response;
    RTLookupResponse::ThreatInfo* new_threat_info = response.add_threat_info();
    new_threat_info->set_verdict_type(verdict_type);
    new_threat_info->set_threat_type(threat_type);
    new_threat_info->set_cache_duration_sec(cache_duration_sec);
    new_threat_info->set_cache_expression_using_match_type(cache_expression);
    new_threat_info->set_cache_expression_match_type(
        cache_expression_match_type);
    rt_service_->MayBeCacheRealTimeUrlVerdict(url, response);
  }

  void SetUpRTLookupResponse(
      RTLookupResponse::ThreatInfo::VerdictType verdict_type,
      RTLookupResponse::ThreatInfo::ThreatType threat_type,
      int cache_duration_sec,
      const std::string& cache_expression,
      RTLookupResponse::ThreatInfo::CacheExpressionMatchType
          cache_expression_match_type) {
    RTLookupResponse response;
    RTLookupResponse::ThreatInfo* new_threat_info = response.add_threat_info();
    RTLookupResponse::ThreatInfo threat_info;
    threat_info.set_verdict_type(verdict_type);
    threat_info.set_threat_type(threat_type);
    threat_info.set_cache_duration_sec(cache_duration_sec);
    threat_info.set_cache_expression_using_match_type(cache_expression);
    threat_info.set_cache_expression_match_type(cache_expression_match_type);
    *new_threat_info = threat_info;
    std::string expected_response_str;
    response.SerializeToString(&expected_response_str);
    test_url_loader_factory_.AddResponse(kRealTimeLookupUrlPrefix,
                                         expected_response_str);
  }

  RealTimeUrlLookupService* rt_service() { return rt_service_.get(); }

  void EnableMbb() {
    unified_consent::UnifiedConsentService::RegisterPrefs(
        test_pref_service_.registry());
    test_pref_service_.SetUserPref(
        unified_consent::prefs::kUrlKeyedAnonymizedDataCollectionEnabled,
        std::make_unique<base::Value>(true));
  }

  void EnableRealTimeUrlLookup(
      const std::vector<base::Feature>& enabled_features,
      const std::vector<base::Feature>& disabled_features) {
    EnableMbb();
    feature_list_.InitWithFeatures(enabled_features, disabled_features);
  }

  void EnableRealTimeUrlLookupWithParameters(
      const std::vector<base::test::ScopedFeatureList::FeatureAndParams>&
          enabled_features_and_params,
      const std::vector<base::Feature>& disabled_features) {
    EnableMbb();
    feature_list_.InitWithFeaturesAndParameters(enabled_features_and_params,
                                                disabled_features);
  }

  void EnableTokenFetchesInClient() {
    token_fetches_configured_in_client_ = true;
  }

  void FulfillAccessTokenRequest(std::string token) {
    raw_token_fetcher_->RunAccessTokenCallback(token);
  }

  bool AreTokenFetchesConfiguredInClient(
      bool user_has_enabled_extended_protection) {
    return token_fetches_configured_in_client_;
  }

  std::unique_ptr<ReferrerChainEntry> CreateReferrerChainEntry(
      const std::string& url,
      const std::string& main_frame_url,
      const std::string& referrer_url,
      const std::string& referrer_main_frame_url) {
    std::unique_ptr<ReferrerChainEntry> referrer_chain_entry =
        std::make_unique<ReferrerChainEntry>();
    referrer_chain_entry->set_url(url);
    if (!main_frame_url.empty()) {
      referrer_chain_entry->set_main_frame_url(main_frame_url);
    }
    referrer_chain_entry->set_referrer_url(referrer_url);
    if (!referrer_main_frame_url.empty()) {
      referrer_chain_entry->set_referrer_main_frame_url(
          referrer_main_frame_url);
    }
    return referrer_chain_entry;
  }

  network::TestURLLoaderFactory test_url_loader_factory_;
  scoped_refptr<network::SharedURLLoaderFactory> test_shared_loader_factory_;
  std::unique_ptr<RealTimeUrlLookupService> rt_service_;
  std::unique_ptr<VerdictCacheManager> cache_manager_;
  scoped_refptr<HostContentSettingsMap> content_setting_map_;
  bool token_fetches_configured_in_client_ = false;
  TestSafeBrowsingTokenFetcher* raw_token_fetcher_ = nullptr;
  std::unique_ptr<base::test::TaskEnvironment> task_environment_;
  sync_preferences::TestingPrefServiceSyncable test_pref_service_;
  base::test::ScopedFeatureList feature_list_;
  std::unique_ptr<MockReferrerChainProvider> referrer_chain_provider_;
};

TEST_F(RealTimeUrlLookupServiceTest, TestFillRequestProto) {
  struct SanitizeUrlCase {
    const char* url;
    const char* expected_url;
  } sanitize_url_cases[] = {
      {"http://example.com/", "http://example.com/"},
      {"http://user:pass@example.com/", "http://example.com/"},
      {"http://%123:bar@example.com/", "http://example.com/"},
      {"http://example.com/abc#123", "http://example.com/abc#123"}};
  for (size_t i = 0; i < base::size(sanitize_url_cases); i++) {
    GURL url(sanitize_url_cases[i].url);
    auto result = FillRequestProto(url);
    EXPECT_EQ(sanitize_url_cases[i].expected_url, result->url());
    EXPECT_EQ(RTLookupRequest::NAVIGATION, result->lookup_type());
    EXPECT_EQ(ChromeUserPopulation::SAFE_BROWSING,
              result->population().user_population());

    // The value of is_history_sync_enabled() should reflect that of the
    // callback passed in by the client, which in this case is true.
    EXPECT_TRUE(result->population().is_history_sync_enabled());
    EXPECT_EQ(ChromeUserPopulation::NOT_MANAGED,
              result->population().profile_management_status());
#if BUILDFLAG(FULL_SAFE_BROWSING)
    EXPECT_TRUE(result->population().is_under_advanced_protection());
#endif
  }
}

TEST_F(RealTimeUrlLookupServiceTest, TestBackoffAndTimerReset) {
  // Not in backoff at the beginning.
  ASSERT_FALSE(IsInBackoffMode());

  // Failure 1: No backoff.
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());

  // Failure 2: No backoff.
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());

  // Failure 3: Entered backoff.
  HandleLookupError();
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 1 second.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 299 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(298));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff should have been reset after 300 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_FALSE(IsInBackoffMode());
}

TEST_F(RealTimeUrlLookupServiceTest, TestBackoffAndLookupSuccessReset) {
  // Not in backoff at the beginning.
  ASSERT_FALSE(IsInBackoffMode());

  // Failure 1: No backoff.
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());

  // Lookup success resets the backoff counter.
  HandleLookupSuccess();
  EXPECT_FALSE(IsInBackoffMode());

  // Failure 1: No backoff.
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());

  // Failure 2: No backoff.
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());

  // Lookup success resets the backoff counter.
  HandleLookupSuccess();
  EXPECT_FALSE(IsInBackoffMode());

  // Failure 1: No backoff.
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());

  // Failure 2: No backoff.
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());

  // Failure 3: Entered backoff.
  HandleLookupError();
  EXPECT_TRUE(IsInBackoffMode());

  // Lookup success resets the backoff counter.
  HandleLookupSuccess();
  EXPECT_FALSE(IsInBackoffMode());
}

TEST_F(RealTimeUrlLookupServiceTest, TestExponentialBackoff) {
  ///////////////////////////////
  // Initial backoff: 300 seconds
  ///////////////////////////////

  // Not in backoff at the beginning.
  ASSERT_FALSE(IsInBackoffMode());

  // Failure 1: No backoff.
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());

  // Failure 2: No backoff.
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());

  // Failure 3: Entered backoff.
  HandleLookupError();
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 1 second.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 299 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(298));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff should have been reset after 300 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_FALSE(IsInBackoffMode());

  /////////////////////////////////////
  // Exponential backoff 1: 600 seconds
  /////////////////////////////////////

  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());
  HandleLookupError();
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 1 second.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 599 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(598));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff should have been reset after 600 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_FALSE(IsInBackoffMode());

  //////////////////////////////////////
  // Exponential backoff 2: 1200 seconds
  //////////////////////////////////////

  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());
  HandleLookupError();
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 1 second.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 1199 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1198));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff should have been reset after 1200 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_FALSE(IsInBackoffMode());

  ///////////////////////////////////////////////////
  // Exponential backoff 3: 1800 seconds (30 minutes)
  ///////////////////////////////////////////////////

  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());
  HandleLookupError();
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 1 second.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 1799 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1798));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff should have been reset after 1800 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_FALSE(IsInBackoffMode());

  ///////////////////////////////////////////////////
  // Exponential backoff 4: 1800 seconds (30 minutes)
  ///////////////////////////////////////////////////

  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());
  HandleLookupError();
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 1 second.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 1799 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1798));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff should have been reset after 1800 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_FALSE(IsInBackoffMode());
}

TEST_F(RealTimeUrlLookupServiceTest, TestExponentialBackoffWithResetOnSuccess) {
  ///////////////////////////////
  // Initial backoff: 300 seconds
  ///////////////////////////////

  // Not in backoff at the beginning.
  ASSERT_FALSE(IsInBackoffMode());

  // Failure 1: No backoff.
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());

  // Failure 2: No backoff.
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());

  // Failure 3: Entered backoff.
  HandleLookupError();
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 1 second.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 299 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(298));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff should have been reset after 300 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_FALSE(IsInBackoffMode());

  /////////////////////////////////////
  // Exponential backoff 1: 600 seconds
  /////////////////////////////////////

  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());
  HandleLookupError();
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 1 second.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 599 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(598));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff should have been reset after 600 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_FALSE(IsInBackoffMode());

  // The next lookup is a success. This should reset the backoff duration to
  // |kMinBackOffResetDurationInSeconds|
  HandleLookupSuccess();

  // Failure 1: No backoff.
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());

  // Failure 2: No backoff.
  HandleLookupError();
  EXPECT_FALSE(IsInBackoffMode());

  // Failure 3: Entered backoff.
  HandleLookupError();
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 1 second.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff not reset after 299 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(298));
  EXPECT_TRUE(IsInBackoffMode());

  // Backoff should have been reset after 300 seconds.
  task_environment_->FastForwardBy(base::TimeDelta::FromSeconds(1));
  EXPECT_FALSE(IsInBackoffMode());
}

TEST_F(RealTimeUrlLookupServiceTest, TestGetSBThreatTypeForRTThreatType) {
  EXPECT_EQ(SB_THREAT_TYPE_URL_MALWARE,
            RealTimeUrlLookupServiceBase::GetSBThreatTypeForRTThreatType(
                RTLookupResponse::ThreatInfo::WEB_MALWARE));
  EXPECT_EQ(SB_THREAT_TYPE_URL_PHISHING,
            RealTimeUrlLookupServiceBase::GetSBThreatTypeForRTThreatType(
                RTLookupResponse::ThreatInfo::SOCIAL_ENGINEERING));
  EXPECT_EQ(SB_THREAT_TYPE_URL_UNWANTED,
            RealTimeUrlLookupServiceBase::GetSBThreatTypeForRTThreatType(
                RTLookupResponse::ThreatInfo::UNWANTED_SOFTWARE));
  EXPECT_EQ(SB_THREAT_TYPE_BILLING,
            RealTimeUrlLookupServiceBase::GetSBThreatTypeForRTThreatType(
                RTLookupResponse::ThreatInfo::UNCLEAR_BILLING));
}

TEST_F(RealTimeUrlLookupServiceTest, TestCanCheckUrl) {
  struct CanCheckUrlCases {
    const char* url;
    bool can_check;
  } can_check_url_cases[] = {{"ftp://example.test/path", false},
                             {"http://localhost/path", false},
                             {"http://127.0.0.1/path", false},
                             {"http://127.0.0.1:2222/path", false},
                             {"http://192.168.1.1/path", false},
                             {"http://172.16.2.2/path", false},
                             {"http://10.1.1.9/path", false},
                             {"http://10.1.1.09/path", true},
                             {"http://example.test/path", true},
                             {"http://nodothost/path", false},
                             {"http://x.x/shorthost", false}};
  for (auto& can_check_url_case : can_check_url_cases) {
    GURL url(can_check_url_case.url);
    bool expected_can_check = can_check_url_case.can_check;
    EXPECT_EQ(expected_can_check, CanCheckUrl(url));
  }
}

TEST_F(RealTimeUrlLookupServiceTest, TestCacheNotInCacheManager) {
  GURL url("https://a.example.test/path1/path2");
  ASSERT_EQ(nullptr, GetCachedRealTimeUrlVerdict(url));
}

TEST_F(RealTimeUrlLookupServiceTest, TestCacheInCacheManager) {
  GURL url("https://a.example.test/path1/path2");
  MayBeCacheRealTimeUrlVerdict(url, RTLookupResponse::ThreatInfo::DANGEROUS,
                               RTLookupResponse::ThreatInfo::SOCIAL_ENGINEERING,
                               60, "a.example.test/path1/path2",
                               RTLookupResponse::ThreatInfo::COVERING_MATCH);
  task_environment_->RunUntilIdle();

  std::unique_ptr<RTLookupResponse> cache_response =
      GetCachedRealTimeUrlVerdict(url);
  ASSERT_NE(nullptr, cache_response);
  EXPECT_EQ(RTLookupResponse::ThreatInfo::DANGEROUS,
            cache_response->threat_info(0).verdict_type());
  EXPECT_EQ(RTLookupResponse::ThreatInfo::SOCIAL_ENGINEERING,
            cache_response->threat_info(0).threat_type());
}

TEST_F(RealTimeUrlLookupServiceTest, TestStartLookup_ResponseIsAlreadyCached) {
  base::HistogramTester histograms;
  EnableRealTimeUrlLookup({kRealTimeUrlLookupEnabled},
                          {kRealTimeUrlLookupEnabledWithToken});
  GURL url(kTestUrl);
  MayBeCacheRealTimeUrlVerdict(url, RTLookupResponse::ThreatInfo::DANGEROUS,
                               RTLookupResponse::ThreatInfo::SOCIAL_ENGINEERING,
                               60, "example.test/",
                               RTLookupResponse::ThreatInfo::COVERING_MATCH);
  task_environment_->RunUntilIdle();

  base::MockCallback<RTLookupRequestCallback> request_callback;
  base::MockCallback<RTLookupResponseCallback> response_callback;
  rt_service()->StartLookup(url, request_callback.Get(),
                            response_callback.Get());

  // |request_callback| should not be called.
  EXPECT_CALL(request_callback, Run(_, _)).Times(0);
  EXPECT_CALL(response_callback, Run(/* is_rt_lookup_successful */ true,
                                     /* is_cached_response */ true, _));

  task_environment_->RunUntilIdle();

  // This metric is not recorded because the response is obtained from the
  // cache.
  histograms.ExpectUniqueSample("SafeBrowsing.RT.ThreatInfoSize",
                                /* sample */ 0,
                                /* expected_count */ 0);
}

TEST_F(RealTimeUrlLookupServiceTest,
       TestStartLookup_AttachTokenWhenWithTokenIsEnabled) {
  base::HistogramTester histograms;
  EnableRealTimeUrlLookup(
      {kRealTimeUrlLookupEnabled, kRealTimeUrlLookupEnabledWithToken}, {});
  EnableTokenFetchesInClient();
  GURL url(kTestUrl);
  SetUpRTLookupResponse(RTLookupResponse::ThreatInfo::DANGEROUS,
                        RTLookupResponse::ThreatInfo::SOCIAL_ENGINEERING, 60,
                        "example.test/",
                        RTLookupResponse::ThreatInfo::COVERING_MATCH);

  base::MockCallback<RTLookupResponseCallback> response_callback;
  rt_service()->StartLookup(
      url,
      base::BindOnce(
          [](std::unique_ptr<RTLookupRequest> request, std::string token) {
            EXPECT_FALSE(request->has_dm_token());
            // Check token is attached.
            EXPECT_EQ("access_token_string", token);
          }),
      response_callback.Get());

  EXPECT_CALL(response_callback, Run(/* is_rt_lookup_successful */ true,
                                     /* is_cached_response */ false, _));

  FulfillAccessTokenRequest("access_token_string");
  task_environment_->RunUntilIdle();

  // Check the response is cached.
  std::unique_ptr<RTLookupResponse> cache_response =
      GetCachedRealTimeUrlVerdict(url);
  EXPECT_NE(nullptr, cache_response);

  histograms.ExpectUniqueSample("SafeBrowsing.RT.ThreatInfoSize",
                                /* sample */ 1,
                                /* expected_count */ 1);
}

TEST_F(RealTimeUrlLookupServiceTest,
       TestStartLookup_NoTokenWhenNotConfiguredInClient) {
  EnableRealTimeUrlLookup(
      {kRealTimeUrlLookupEnabled, kRealTimeUrlLookupEnabledWithToken}, {});
  GURL url(kTestUrl);
  SetUpRTLookupResponse(RTLookupResponse::ThreatInfo::DANGEROUS,
                        RTLookupResponse::ThreatInfo::SOCIAL_ENGINEERING, 60,
                        "example.test/",
                        RTLookupResponse::ThreatInfo::COVERING_MATCH);

  base::MockCallback<RTLookupResponseCallback> response_callback;
  rt_service()->StartLookup(
      url,
      base::BindOnce(
          [](std::unique_ptr<RTLookupRequest> request, std::string token) {
            // Check the token field is empty as the passed-in client callback
            // indicates that token fetches are not configured in the client.
            EXPECT_EQ("", token);
          }),
      response_callback.Get());

  EXPECT_CALL(response_callback, Run(/* is_rt_lookup_successful */ true,
                                     /* is_cached_response */ false, _));

  task_environment_->RunUntilIdle();

  // Check the response is cached.
  std::unique_ptr<RTLookupResponse> cache_response =
      GetCachedRealTimeUrlVerdict(url);
  EXPECT_NE(nullptr, cache_response);
}

TEST_F(RealTimeUrlLookupServiceTest,
       TestStartLookup_NoTokenWhenWithTokenIsDisabled) {
  EnableRealTimeUrlLookup({kRealTimeUrlLookupEnabled},
                          {kRealTimeUrlLookupEnabledWithToken});
  EnableTokenFetchesInClient();
  GURL url(kTestUrl);
  SetUpRTLookupResponse(RTLookupResponse::ThreatInfo::DANGEROUS,
                        RTLookupResponse::ThreatInfo::SOCIAL_ENGINEERING, 60,
                        "example.test/",
                        RTLookupResponse::ThreatInfo::COVERING_MATCH);

  base::MockCallback<RTLookupResponseCallback> response_callback;
  rt_service()->StartLookup(
      url,
      base::BindOnce(
          [](std::unique_ptr<RTLookupRequest> request, std::string token) {
            // Check the token field is empty.
            EXPECT_EQ("", token);
          }),
      response_callback.Get());

  EXPECT_CALL(response_callback, Run(/* is_rt_lookup_successful */ true,
                                     /* is_cached_response */ false, _));

  task_environment_->RunUntilIdle();

  // Check the response is cached.
  std::unique_ptr<RTLookupResponse> cache_response =
      GetCachedRealTimeUrlVerdict(url);
  EXPECT_NE(nullptr, cache_response);
}

TEST_F(RealTimeUrlLookupServiceTest, TestReferrerChain_ReferrerChainAttached) {
  EnableRealTimeUrlLookup({kRealTimeUrlLookupReferrerChain}, {});
  GURL url(kTestUrl);
  SetUpRTLookupResponse(RTLookupResponse::ThreatInfo::DANGEROUS,
                        RTLookupResponse::ThreatInfo::SOCIAL_ENGINEERING, 60,
                        "example.test/",
                        RTLookupResponse::ThreatInfo::COVERING_MATCH);
  ReferrerChain returned_referrer_chain;
  returned_referrer_chain.Add()->Swap(
      CreateReferrerChainEntry(kTestUrl, /*main_frame_url=*/"",
                               /*referrer_url=*/"",
                               /*referrer_main_frame_url=*/"")
          .get());
  returned_referrer_chain.Add()->Swap(
      CreateReferrerChainEntry(kTestReferrerUrl, /*main_frame_url=*/"",
                               /*referrer_url=*/"",
                               /*referrer_main_frame_url=*/"")
          .get());
  EXPECT_CALL(*referrer_chain_provider_,
              IdentifyReferrerChainByPendingEventURL(
                  url, /*user_gesture_count_limit=*/2, _))
      .WillOnce(DoAll(SetArgPointee<2>(returned_referrer_chain),
                      Return(ReferrerChainProvider::SUCCESS)));

  base::MockCallback<RTLookupResponseCallback> response_callback;
  EXPECT_CALL(response_callback, Run(/* is_rt_lookup_successful */ true,
                                     /* is_cached_response */ false, _));
  rt_service()->StartLookup(
      url,
      base::BindOnce(
          [](std::unique_ptr<RTLookupRequest> request, std::string token) {
            EXPECT_EQ(2, request->version());
            // Check referrer chain is attached.
            EXPECT_EQ(2, request->referrer_chain().size());
            EXPECT_EQ(kTestUrl, request->referrer_chain().Get(0).url());
            EXPECT_EQ(kTestReferrerUrl, request->referrer_chain().Get(1).url());
          }),
      response_callback.Get());

  task_environment_->RunUntilIdle();
}

TEST_F(RealTimeUrlLookupServiceTest,
       TestReferrerChain_ReferrerChainNotAttachedWhenFeatureFlagDisabled) {
  EnableRealTimeUrlLookup({}, {kRealTimeUrlLookupReferrerChain});
  GURL url(kTestUrl);
  SetUpRTLookupResponse(RTLookupResponse::ThreatInfo::DANGEROUS,
                        RTLookupResponse::ThreatInfo::SOCIAL_ENGINEERING, 60,
                        "example.test/",
                        RTLookupResponse::ThreatInfo::COVERING_MATCH);
  ReferrerChain returned_referrer_chain;
  returned_referrer_chain.Add()->Swap(
      CreateReferrerChainEntry(kTestUrl, /*main_frame_url=*/"",
                               /*referrer_url=*/"",
                               /*referrer_main_frame_url=*/"")
          .get());
  EXPECT_CALL(*referrer_chain_provider_,
              IdentifyReferrerChainByPendingEventURL(_, _, _))
      .Times(0);

  base::MockCallback<RTLookupResponseCallback> response_callback;
  rt_service()->StartLookup(
      url,
      base::BindOnce(
          [](std::unique_ptr<RTLookupRequest> request, std::string token) {
            EXPECT_EQ(2, request->version());
            // Check referrer chain is attached.
            EXPECT_EQ(0, request->referrer_chain().size());
          }),
      response_callback.Get());

  EXPECT_CALL(response_callback, Run(/* is_rt_lookup_successful */ true,
                                     /* is_cached_response */ false, _));

  task_environment_->RunUntilIdle();
}

TEST_F(RealTimeUrlLookupServiceTest,
       TestReferrerChain_SanitizedIfSubresourceNotAllowed) {
  EnableRealTimeUrlLookup({kRealTimeUrlLookupReferrerChain}, {});
  GURL url(kTestUrl);
  SetUpRTLookupResponse(RTLookupResponse::ThreatInfo::DANGEROUS,
                        RTLookupResponse::ThreatInfo::SOCIAL_ENGINEERING, 60,
                        "example.test/",
                        RTLookupResponse::ThreatInfo::COVERING_MATCH);
  ReferrerChain returned_referrer_chain;
  returned_referrer_chain.Add()->Swap(
      CreateReferrerChainEntry(kTestSubframeUrl, kTestUrl,
                               kTestSubframeReferrerUrl, kTestReferrerUrl)
          .get());
  returned_referrer_chain.Add()->Swap(
      CreateReferrerChainEntry(kTestReferrerUrl, /*main_frame_url=*/"",
                               /*referrer_url=*/"",
                               /*referrer_main_frame_url=*/"")
          .get());
  EXPECT_CALL(*referrer_chain_provider_,
              IdentifyReferrerChainByPendingEventURL(
                  url, /*user_gesture_count_limit=*/2, _))
      .WillOnce(DoAll(SetArgPointee<2>(returned_referrer_chain),
                      Return(ReferrerChainProvider::SUCCESS)));

  base::MockCallback<RTLookupResponseCallback> response_callback;
  rt_service()->StartLookup(
      url,
      base::BindOnce(
          [](std::unique_ptr<RTLookupRequest> request, std::string token) {
            EXPECT_EQ(2, request->version());
            EXPECT_EQ(2, request->referrer_chain().size());
            // The first entry is sanitized because it is triggered in a
            // subframe.
            EXPECT_EQ(kTestUrl, request->referrer_chain().Get(0).url());
            EXPECT_FALSE(request->referrer_chain().Get(0).has_main_frame_url());
            EXPECT_TRUE(
                request->referrer_chain().Get(0).is_subframe_url_removed());
            EXPECT_EQ(kTestReferrerUrl,
                      request->referrer_chain().Get(0).referrer_url());
            EXPECT_FALSE(
                request->referrer_chain().Get(0).has_referrer_main_frame_url());
            EXPECT_TRUE(request->referrer_chain()
                            .Get(0)
                            .is_subframe_referrer_url_removed());
            // The second entry is not sanitized because it is triggered in a
            // mainframe.
            EXPECT_EQ(kTestReferrerUrl, request->referrer_chain().Get(1).url());
            EXPECT_FALSE(
                request->referrer_chain().Get(1).is_subframe_url_removed());
            EXPECT_FALSE(request->referrer_chain()
                             .Get(1)
                             .is_subframe_referrer_url_removed());
          }),
      response_callback.Get());

  EXPECT_CALL(response_callback, Run(/* is_rt_lookup_successful */ true,
                                     /* is_cached_response */ false, _));

  task_environment_->RunUntilIdle();
}

TEST_F(RealTimeUrlLookupServiceTest,
       TestReferrerChain_NotSanitizedIfSubresourceAllowed) {
  EnableRealTimeUrlLookup({kRealTimeUrlLookupReferrerChain}, {});
  // Subresource is allowed when enhanced protection is enabled.
  SetSafeBrowsingState(&test_pref_service_, ENHANCED_PROTECTION);
  GURL url(kTestUrl);
  SetUpRTLookupResponse(RTLookupResponse::ThreatInfo::DANGEROUS,
                        RTLookupResponse::ThreatInfo::SOCIAL_ENGINEERING, 60,
                        "example.test/",
                        RTLookupResponse::ThreatInfo::COVERING_MATCH);
  ReferrerChain returned_referrer_chain;
  returned_referrer_chain.Add()->Swap(
      CreateReferrerChainEntry(kTestSubframeUrl, kTestUrl,
                               kTestSubframeReferrerUrl, kTestReferrerUrl)
          .get());
  returned_referrer_chain.Add()->Swap(
      CreateReferrerChainEntry(kTestSubframeReferrerUrl, kTestReferrerUrl,
                               /*referrer_url=*/"",
                               /*referrer_main_frame_url=*/"")
          .get());
  EXPECT_CALL(*referrer_chain_provider_,
              IdentifyReferrerChainByPendingEventURL(
                  url, /*user_gesture_count_limit=*/2, _))
      .WillOnce(DoAll(SetArgPointee<2>(returned_referrer_chain),
                      Return(ReferrerChainProvider::SUCCESS)));

  base::MockCallback<RTLookupResponseCallback> response_callback;
  rt_service()->StartLookup(
      url,
      base::BindOnce([](std::unique_ptr<RTLookupRequest> request,
                        std::string token) {
        EXPECT_EQ(2, request->version());
        EXPECT_EQ(2, request->referrer_chain().size());
        // Check referrer chain is not sanitized.
        EXPECT_EQ(kTestSubframeUrl, request->referrer_chain().Get(0).url());
        EXPECT_EQ(kTestUrl, request->referrer_chain().Get(0).main_frame_url());
        EXPECT_FALSE(
            request->referrer_chain().Get(0).is_subframe_url_removed());
        EXPECT_EQ(kTestSubframeReferrerUrl,
                  request->referrer_chain().Get(0).referrer_url());
        EXPECT_EQ(kTestReferrerUrl,
                  request->referrer_chain().Get(0).referrer_main_frame_url());
        EXPECT_FALSE(request->referrer_chain()
                         .Get(0)
                         .is_subframe_referrer_url_removed());
        EXPECT_EQ(kTestSubframeReferrerUrl,
                  request->referrer_chain().Get(1).url());
        EXPECT_FALSE(
            request->referrer_chain().Get(1).is_subframe_url_removed());
      }),
      response_callback.Get());

  EXPECT_CALL(response_callback, Run(/* is_rt_lookup_successful */ true,
                                     /* is_cached_response */ false, _));

  task_environment_->RunUntilIdle();
}

TEST_F(RealTimeUrlLookupServiceTest,
       TestReferrerChain_UserGestureLimitIsConfigurable) {
  EnableRealTimeUrlLookupWithParameters(
      {{kRealTimeUrlLookupReferrerChain,
        {{"SafeBrowsingRealTimeUrlLookupReferrerLengthParam",
          base::NumberToString(1)}}}},
      {});
  GURL url(kTestUrl);
  SetUpRTLookupResponse(RTLookupResponse::ThreatInfo::DANGEROUS,
                        RTLookupResponse::ThreatInfo::SOCIAL_ENGINEERING, 60,
                        "example.test/",
                        RTLookupResponse::ThreatInfo::COVERING_MATCH);
  ReferrerChain returned_referrer_chain;
  returned_referrer_chain.Add()->Swap(
      CreateReferrerChainEntry(kTestUrl, /*main_frame_url=*/"",
                               /*referrer_url=*/"",
                               /*referrer_main_frame_url=*/"")
          .get());
  // The user gesture count limit should be set to 1.
  EXPECT_CALL(*referrer_chain_provider_,
              IdentifyReferrerChainByPendingEventURL(
                  url, /*user_gesture_count_limit=*/1, _))
      .WillOnce(DoAll(SetArgPointee<2>(returned_referrer_chain),
                      Return(ReferrerChainProvider::SUCCESS)));

  base::MockCallback<RTLookupResponseCallback> response_callback;
  rt_service()->StartLookup(
      url,
      base::BindOnce(
          [](std::unique_ptr<RTLookupRequest> request, std::string token) {
            EXPECT_EQ(2, request->version());
            EXPECT_EQ(1, request->referrer_chain().size());
            EXPECT_EQ(kTestUrl, request->referrer_chain().Get(0).url());
          }),
      response_callback.Get());

  EXPECT_CALL(response_callback, Run(/* is_rt_lookup_successful */ true,
                                     /* is_cached_response */ false, _));

  task_environment_->RunUntilIdle();
}

TEST_F(RealTimeUrlLookupServiceTest, TestShutdown_CallbackNotPostedOnShutdown) {
  EnableRealTimeUrlLookup({kRealTimeUrlLookupEnabled},
                          {kRealTimeUrlLookupEnabledWithToken});
  GURL url(kTestUrl);

  base::MockCallback<RTLookupRequestCallback> request_callback;
  base::MockCallback<RTLookupResponseCallback> response_callback;
  rt_service()->StartLookup(url, request_callback.Get(),
                            response_callback.Get());

  EXPECT_CALL(request_callback, Run(_, _)).Times(1);
  EXPECT_CALL(response_callback, Run(_, _, _)).Times(0);
  rt_service()->Shutdown();

  task_environment_->RunUntilIdle();
}

}  // namespace safe_browsing
