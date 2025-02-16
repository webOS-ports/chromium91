// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "weblayer/renderer/content_renderer_client_impl.h"

#include "base/feature_list.h"
#include "build/build_config.h"
#include "components/autofill/content/renderer/autofill_agent.h"
#include "components/autofill/content/renderer/password_autofill_agent.h"
#include "components/content_capture/common/content_capture_features.h"
#include "components/content_capture/renderer/content_capture_sender.h"
#include "components/content_settings/renderer/content_settings_agent_impl.h"
#include "components/error_page/common/error.h"
#include "components/grit/components_scaled_resources.h"
#include "components/js_injection/renderer/js_communication.h"
#include "components/no_state_prefetch/common/prerender_url_loader_throttle.h"
#include "components/no_state_prefetch/renderer/no_state_prefetch_client.h"
#include "components/no_state_prefetch/renderer/no_state_prefetch_helper.h"
#include "components/no_state_prefetch/renderer/prerender_render_frame_observer.h"
#include "components/no_state_prefetch/renderer/prerender_utils.h"
#include "components/page_load_metrics/renderer/metrics_render_frame_observer.h"
#include "components/subresource_filter/content/renderer/ad_resource_tracker.h"
#include "components/subresource_filter/content/renderer/subresource_filter_agent.h"
#include "components/subresource_filter/content/renderer/unverified_ruleset_dealer.h"
#include "components/subresource_filter/core/common/common_features.h"
#include "components/webapps/renderer/web_page_metadata_agent.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_thread.h"
#include "content/public/renderer/render_view.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/web_runtime_features.h"
#include "ui/base/resource/resource_bundle.h"
#include "weblayer/common/features.h"
#include "weblayer/renderer/error_page_helper.h"
#include "weblayer/renderer/url_loader_throttle_provider.h"
#include "weblayer/renderer/weblayer_render_frame_observer.h"
#include "weblayer/renderer/weblayer_render_thread_observer.h"

#if defined(OS_ANDROID)
#include "components/android_system_error_page/error_page_populator.h"
#include "components/cdm/renderer/android_key_systems.h"
#include "components/embedder_support/android/common/url_constants.h"
#include "components/spellcheck/renderer/spellcheck.h"           // nogncheck
#include "components/spellcheck/renderer/spellcheck_provider.h"  // nogncheck
#include "content/public/renderer/render_thread.h"
#include "services/service_manager/public/cpp/local_interface_provider.h"
#include "third_party/blink/public/platform/web_runtime_features.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_security_policy.h"
#endif

namespace weblayer {

namespace {

#if defined(OS_ANDROID)
class SpellcheckInterfaceProvider
    : public service_manager::LocalInterfaceProvider {
 public:
  SpellcheckInterfaceProvider() = default;
  ~SpellcheckInterfaceProvider() override = default;

  // service_manager::LocalInterfaceProvider:
  void GetInterface(const std::string& interface_name,
                    mojo::ScopedMessagePipeHandle interface_pipe) override {
    // A dirty hack to make SpellCheckHost requests work on WebLayer.
    // TODO(crbug.com/806394): Use a WebView-specific service for SpellCheckHost
    // and SafeBrowsing, instead of |content_browser|.
    content::RenderThread::Get()->BindHostReceiver(mojo::GenericPendingReceiver(
        interface_name, std::move(interface_pipe)));
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(SpellcheckInterfaceProvider);
};
#endif  // defined(OS_ANDROID)

}  // namespace

ContentRendererClientImpl::ContentRendererClientImpl() = default;
ContentRendererClientImpl::~ContentRendererClientImpl() = default;

void ContentRendererClientImpl::RenderThreadStarted() {
#if defined(OS_ANDROID)
  if (!spellcheck_) {
    local_interface_provider_ = std::make_unique<SpellcheckInterfaceProvider>();
    spellcheck_ = std::make_unique<SpellCheck>(local_interface_provider_.get());
  }
  blink::WebSecurityPolicy::RegisterURLSchemeAsAllowedForReferrer(
      blink::WebString::FromUTF8(embedder_support::kAndroidAppScheme));
#endif

  content::RenderThread* thread = content::RenderThread::Get();
  weblayer_observer_ = std::make_unique<WebLayerRenderThreadObserver>();
  thread->AddObserver(weblayer_observer_.get());

  browser_interface_broker_ =
      blink::Platform::Current()->GetBrowserInterfaceBroker();

  subresource_filter_ruleset_dealer_ =
      std::make_unique<subresource_filter::UnverifiedRulesetDealer>();
  thread->AddObserver(subresource_filter_ruleset_dealer_.get());
}

void ContentRendererClientImpl::RenderFrameCreated(
    content::RenderFrame* render_frame) {
  auto* render_frame_observer = new WebLayerRenderFrameObserver(render_frame);
  new prerender::PrerenderRenderFrameObserver(render_frame);

  ErrorPageHelper::Create(render_frame);

  autofill::PasswordAutofillAgent* password_autofill_agent =
      new autofill::PasswordAutofillAgent(
          render_frame, render_frame_observer->associated_interfaces());
  new autofill::AutofillAgent(render_frame, password_autofill_agent, nullptr,
                              nullptr,
                              render_frame_observer->associated_interfaces());
  auto* agent = new content_settings::ContentSettingsAgentImpl(
      render_frame, false /* should_whitelist */,
      std::make_unique<content_settings::ContentSettingsAgentImpl::Delegate>());
  if (weblayer_observer_)
    agent->SetContentSettingRules(weblayer_observer_->content_setting_rules());

  auto* metrics_render_frame_observer =
      new page_load_metrics::MetricsRenderFrameObserver(render_frame);

  auto ad_resource_tracker =
      std::make_unique<subresource_filter::AdResourceTracker>();
  metrics_render_frame_observer->SetAdResourceTracker(
      ad_resource_tracker.get());
  auto* subresource_filter_agent =
      new subresource_filter::SubresourceFilterAgent(
          render_frame, subresource_filter_ruleset_dealer_.get(),
          std::move(ad_resource_tracker));
  subresource_filter_agent->Initialize();

#if defined(OS_ANDROID)
  // |SpellCheckProvider| manages its own lifetime (and destroys itself when the
  // RenderFrame is destroyed).
  new SpellCheckProvider(render_frame, spellcheck_.get(),
                         local_interface_provider_.get());
#endif
  new js_injection::JsCommunication(render_frame);

  if (render_frame->IsMainFrame())
    new webapps::WebPageMetadataAgent(render_frame);

  if (content_capture::features::IsContentCaptureEnabled()) {
    new content_capture::ContentCaptureSender(
        render_frame, render_frame_observer->associated_interfaces());
  }

  if (!render_frame->IsMainFrame()) {
    auto* main_frame_no_state_prefetch_helper =
        prerender::NoStatePrefetchHelper::Get(
            render_frame->GetRenderView()->GetMainRenderFrame());
    if (main_frame_no_state_prefetch_helper) {
      // Avoid any race conditions from having the browser tell subframes that
      // they're no-state prefetching.
      new prerender::NoStatePrefetchHelper(
          render_frame,
          main_frame_no_state_prefetch_helper->histogram_prefix());
    }
  }
}

void ContentRendererClientImpl::RenderViewCreated(
    content::RenderView* render_view) {
  new prerender::NoStatePrefetchClient(render_view->GetWebView());
}

SkBitmap* ContentRendererClientImpl::GetSadPluginBitmap() {
  return const_cast<SkBitmap*>(ui::ResourceBundle::GetSharedInstance()
                                   .GetImageNamed(IDR_SAD_PLUGIN)
                                   .ToSkBitmap());
}

SkBitmap* ContentRendererClientImpl::GetSadWebViewBitmap() {
  return const_cast<SkBitmap*>(ui::ResourceBundle::GetSharedInstance()
                                   .GetImageNamed(IDR_SAD_WEBVIEW)
                                   .ToSkBitmap());
}

void ContentRendererClientImpl::PrepareErrorPage(
    content::RenderFrame* render_frame,
    const blink::WebURLError& error,
    const std::string& http_method,
    std::string* error_html) {
  auto* error_page_helper = ErrorPageHelper::GetForFrame(render_frame);
  if (error_page_helper)
    error_page_helper->PrepareErrorPage();

#if defined(OS_ANDROID)
  // This does nothing if |error_html| is non-null (which happens if the
  // embedder injects an error page).
  android_system_error_page::PopulateErrorPageHtml(error, error_html);
#endif
}

std::unique_ptr<blink::URLLoaderThrottleProvider>
ContentRendererClientImpl::CreateURLLoaderThrottleProvider(
    blink::URLLoaderThrottleProviderType provider_type) {
  return std::make_unique<URLLoaderThrottleProvider>(
      browser_interface_broker_.get(), provider_type);
}

void ContentRendererClientImpl::AddSupportedKeySystems(
    std::vector<std::unique_ptr<::media::KeySystemProperties>>* key_systems) {
#if defined(OS_ANDROID)
  cdm::AddAndroidWidevine(key_systems);
  cdm::AddAndroidPlatformKeySystems(key_systems);
#endif
}

void ContentRendererClientImpl::
    SetRuntimeFeaturesDefaultsBeforeBlinkInitialization() {
  blink::WebRuntimeFeatures::EnablePerformanceManagerInstrumentation(true);
#if defined(OS_ANDROID)
  // Web Share is experimental by default, and explicitly enabled on Android
  // (for both Chrome and WebLayer).
  blink::WebRuntimeFeatures::EnableWebShare(true);
#endif

  if (base::FeatureList::IsEnabled(subresource_filter::kAdTagging)) {
    blink::WebRuntimeFeatures::EnableAdTagging(true);
  }
}

bool ContentRendererClientImpl::IsPrefetchOnly(
    content::RenderFrame* render_frame) {
  return prerender::NoStatePrefetchHelper::IsPrefetching(render_frame);
}

bool ContentRendererClientImpl::DeferMediaLoad(
    content::RenderFrame* render_frame,
    bool has_played_media_before,
    base::OnceClosure closure) {
  return prerender::DeferMediaLoad(render_frame, has_played_media_before,
                                   std::move(closure));
}

}  // namespace weblayer
