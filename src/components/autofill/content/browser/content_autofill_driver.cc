// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/autofill/content/browser/content_autofill_driver.h"

#include <tuple>
#include <utility>
#include <vector>

#include "base/feature_list.h"
#include "base/metrics/histogram_macros.h"
#include "build/build_config.h"
#include "components/autofill/content/browser/content_autofill_driver_factory.h"
#include "components/autofill/core/browser/autofill_client.h"
#include "components/autofill/core/browser/autofill_handler_proxy.h"
#include "components/autofill/core/browser/autofill_manager.h"
#include "components/autofill/core/browser/form_structure.h"
#include "components/autofill/core/browser/payments/payments_service_url.h"
#include "components/autofill/core/common/autofill_features.h"
#include "components/autofill/core/common/autofill_payments_features.h"
#include "components/profile_metrics/browser_profile_type.h"
#include "components/version_info/channel.h"
#include "content/public/browser/back_forward_cache.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/origin_util.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "ui/gfx/geometry/size_f.h"
#include "url/origin.h"

namespace {

bool ShouldEnableHeavyFormDataScraping(const version_info::Channel channel) {
  switch (channel) {
    case version_info::Channel::CANARY:
    case version_info::Channel::DEV:
      return true;
    case version_info::Channel::STABLE:
    case version_info::Channel::BETA:
    case version_info::Channel::UNKNOWN:
      return false;
  }
  NOTREACHED();
  return false;
}

}  // namespace

namespace autofill {

ContentAutofillDriver::ContentAutofillDriver(
    content::RenderFrameHost* render_frame_host,
    AutofillClient* client,
    const std::string& app_locale,
    AutofillHandler::AutofillDownloadManagerState enable_download_manager,
    AutofillProvider* provider)
    : render_frame_host_(render_frame_host),
      autofill_manager_(nullptr),
      key_press_handler_manager_(this),
      log_manager_(client->GetLogManager()) {
  // AutofillManager isn't used if provider is valid, Autofill provider is
  // currently used by Android WebView only.
  if (provider) {
    SetAutofillProvider(provider, client, enable_download_manager);
  } else {
    SetAutofillManager(std::make_unique<AutofillManager>(
        this, client, app_locale, enable_download_manager));
  }
  if (client && ShouldEnableHeavyFormDataScraping(client->GetChannel())) {
    GetAutofillAgent()->EnableHeavyFormDataScraping();
  }
}

ContentAutofillDriver::~ContentAutofillDriver() = default;

// static
ContentAutofillDriver* ContentAutofillDriver::GetForRenderFrameHost(
    content::RenderFrameHost* render_frame_host) {
  ContentAutofillDriverFactory* factory =
      ContentAutofillDriverFactory::FromWebContents(
          content::WebContents::FromRenderFrameHost(render_frame_host));
  return factory ? factory->DriverForFrame(render_frame_host) : nullptr;
}

void ContentAutofillDriver::BindPendingReceiver(
    mojo::PendingAssociatedReceiver<mojom::AutofillDriver> pending_receiver) {
  receiver_.Bind(std::move(pending_receiver));
}

bool ContentAutofillDriver::IsIncognito() const {
  // TODO(https://crbug.com/1125474): Enable Autofill for Ephemeral Guest
  // profiles.
  // TODO(https://crbug.com/1125474): Consider renaming this function to
  // |IsOffTheRecord| after deprecation of off-the-record or ephemeral Guest
  // profiles.
  if (autofill_manager_ &&
      autofill_manager_->client()->GetProfileType() ==
          profile_metrics::BrowserProfileType::kEphemeralGuest) {
    return true;
  }

  return render_frame_host_->GetSiteInstance()
      ->GetBrowserContext()
      ->IsOffTheRecord();
}

bool ContentAutofillDriver::IsInMainFrame() const {
  return render_frame_host_->GetParent() == nullptr;
}

bool ContentAutofillDriver::CanShowAutofillUi() const {
  // Don't show AutofillUi for non-current RenderFrameHost. Here it is safe to
  // ignore the calls from inactive RFH as the renderer is not expecting a reply
  // and it doesn't lead to browser-renderer consistency issues.
  return render_frame_host_->IsCurrent();
}

ui::AXTreeID ContentAutofillDriver::GetAxTreeId() const {
  return render_frame_host_->GetAXTreeID();
}

scoped_refptr<network::SharedURLLoaderFactory>
ContentAutofillDriver::GetURLLoaderFactory() {
  return content::BrowserContext::GetDefaultStoragePartition(
             render_frame_host_->GetSiteInstance()->GetBrowserContext())
      ->GetURLLoaderFactoryForBrowserProcess();
}

bool ContentAutofillDriver::RendererIsAvailable() {
  return render_frame_host_->GetRenderViewHost() != nullptr;
}

InternalAuthenticator*
ContentAutofillDriver::GetOrCreateCreditCardInternalAuthenticator() {
  if (!authenticator_impl_ && autofill_manager_ &&
      autofill_manager_->client()) {
    authenticator_impl_ =
        autofill_manager_->client()->CreateCreditCardInternalAuthenticator(
            render_frame_host_);
  }
  return authenticator_impl_.get();
}

void ContentAutofillDriver::SendFormDataToRenderer(
    int query_id,
    RendererFormDataAction action,
    const FormData& data) {
  if (!RendererIsAvailable())
    return;

  switch (action) {
    case FORM_DATA_ACTION_FILL:
      GetAutofillAgent()->FillForm(query_id, data);
      break;
    case FORM_DATA_ACTION_PREVIEW:
      GetAutofillAgent()->PreviewForm(query_id, data);
      break;
  }
}

void ContentAutofillDriver::PropagateAutofillPredictions(
    const std::vector<FormStructure*>& forms) {
  AutofillHandler* handler =
      autofill_manager_ ? autofill_manager_ : autofill_handler_.get();
  DCHECK(handler);
  handler->PropagateAutofillPredictions(render_frame_host_, forms);
}

void ContentAutofillDriver::HandleParsedForms(
    const std::vector<const FormData*>& forms) {
  // No op.
}

void ContentAutofillDriver::SendAutofillTypePredictionsToRenderer(
    const std::vector<FormStructure*>& forms) {
  if (!RendererIsAvailable())
    return;

  std::vector<FormDataPredictions> type_predictions =
      FormStructure::GetFieldTypePredictions(forms);
  GetAutofillAgent()->FieldTypePredictionsAvailable(type_predictions);
}

void ContentAutofillDriver::RendererShouldAcceptDataListSuggestion(
    const FieldGlobalId& field,
    const std::u16string& value) {
  if (!RendererIsAvailable())
    return;
  GetAutofillAgent()->AcceptDataListSuggestion(field.renderer_id, value);
}

void ContentAutofillDriver::RendererShouldClearFilledSection() {
  if (!RendererIsAvailable())
    return;
  GetAutofillAgent()->ClearSection();
}

void ContentAutofillDriver::RendererShouldClearPreviewedForm() {
  if (!RendererIsAvailable())
    return;
  GetAutofillAgent()->ClearPreviewedForm();
}

void ContentAutofillDriver::RendererShouldFillFieldWithValue(
    const FieldGlobalId& field,
    const std::u16string& value) {
  if (!RendererIsAvailable())
    return;
  GetAutofillAgent()->FillFieldWithValue(field.renderer_id, value);
}

void ContentAutofillDriver::RendererShouldPreviewFieldWithValue(
    const FieldGlobalId& field,
    const std::u16string& value) {
  if (!RendererIsAvailable())
    return;
  GetAutofillAgent()->PreviewFieldWithValue(field.renderer_id, value);
}

void ContentAutofillDriver::RendererShouldSetSuggestionAvailability(
    const FieldGlobalId& field,
    const mojom::AutofillState state) {
  if (!RendererIsAvailable())
    return;
  GetAutofillAgent()->SetSuggestionAvailability(field.renderer_id, state);
}

void ContentAutofillDriver::PopupHidden() {
  // If the unmask prompt is showing, keep showing the preview. The preview
  // will be cleared when the prompt closes.
  if (autofill_manager_ && autofill_manager_->ShouldClearPreviewedForm())
    RendererShouldClearPreviewedForm();
}

gfx::RectF ContentAutofillDriver::TransformBoundingBoxToViewportCoordinates(
    const gfx::RectF& bounding_box) {
  content::RenderWidgetHostView* view = render_frame_host_->GetView();
  if (!view)
    return bounding_box;

  gfx::PointF orig_point(bounding_box.x(), bounding_box.y());
  gfx::PointF transformed_point =
      view->TransformPointToRootCoordSpaceF(orig_point);
  return gfx::RectF(transformed_point.x(), transformed_point.y(),
                    bounding_box.width(), bounding_box.height());
}

net::IsolationInfo ContentAutofillDriver::IsolationInfo() {
  return render_frame_host_->GetPendingIsolationInfoForSubresources();
}

void ContentAutofillDriver::FormsSeen(const std::vector<FormData>& forms) {
  autofill_handler_->OnFormsSeen(forms);
}

void ContentAutofillDriver::SetFormToBeProbablySubmitted(
    const base::Optional<FormData>& form) {
  potentially_submitted_form_ = form;
}

void ContentAutofillDriver::ProbablyFormSubmitted() {
  if (potentially_submitted_form_.has_value()) {
    FormSubmitted(potentially_submitted_form_.value(), false,
                  mojom::SubmissionSource::PROBABLY_FORM_SUBMITTED);
  }
}

void ContentAutofillDriver::FormSubmitted(const FormData& form,
                                          bool known_success,
                                          mojom::SubmissionSource source) {
  // Omit duplicate form submissions. It may be reasonable to take |source|
  // into account here as well.
  // TODO(crbug/1117451): Clean up experiment code.
  if (base::FeatureList::IsEnabled(
          features::kAutofillProbableFormSubmissionInBrowser) &&
      !base::FeatureList::IsEnabled(
          features::kAutofillAllowDuplicateFormSubmissions) &&
      !submitted_forms_.insert(form.unique_renderer_id).second) {
    return;
  }

  autofill_handler_->OnFormSubmitted(form, known_success, source);
}

void ContentAutofillDriver::TextFieldDidChange(const FormData& form,
                                               const FormFieldData& field,
                                               const gfx::RectF& bounding_box,
                                               base::TimeTicks timestamp) {
  autofill_handler_->OnTextFieldDidChange(form, field, bounding_box, timestamp);
}

void ContentAutofillDriver::TextFieldDidScroll(const FormData& form,
                                               const FormFieldData& field,
                                               const gfx::RectF& bounding_box) {
  autofill_handler_->OnTextFieldDidScroll(form, field, bounding_box);
}

void ContentAutofillDriver::SelectControlDidChange(
    const FormData& form,
    const FormFieldData& field,
    const gfx::RectF& bounding_box) {
  autofill_handler_->OnSelectControlDidChange(form, field, bounding_box);
}

void ContentAutofillDriver::QueryFormFieldAutofill(
    int32_t id,
    const FormData& form,
    const FormFieldData& field,
    const gfx::RectF& bounding_box,
    bool autoselect_first_suggestion) {
  autofill_handler_->OnQueryFormFieldAutofill(id, form, field, bounding_box,
                                              autoselect_first_suggestion);
}

void ContentAutofillDriver::HidePopup() {
  autofill_handler_->OnHidePopup();
}

void ContentAutofillDriver::FocusNoLongerOnForm(bool had_interacted_form) {
  autofill_handler_->OnFocusNoLongerOnForm(had_interacted_form);
}

void ContentAutofillDriver::FocusOnFormField(const FormData& form,
                                             const FormFieldData& field,
                                             const gfx::RectF& bounding_box) {
  autofill_handler_->OnFocusOnFormField(form, field, bounding_box);
}

void ContentAutofillDriver::DidFillAutofillFormData(const FormData& form,
                                                    base::TimeTicks timestamp) {
  autofill_handler_->OnDidFillAutofillFormData(form, timestamp);
}

void ContentAutofillDriver::DidPreviewAutofillFormData() {
  autofill_handler_->OnDidPreviewAutofillFormData();
}

void ContentAutofillDriver::DidEndTextFieldEditing() {
  autofill_handler_->OnDidEndTextFieldEditing();
}

void ContentAutofillDriver::SelectFieldOptionsDidChange(const FormData& form) {
  autofill_handler_->SelectFieldOptionsDidChange(form);
}

void ContentAutofillDriver::DidNavigateFrame(
    content::NavigationHandle* navigation_handle) {
  if (navigation_handle->IsSameDocument()) {
    // On page refresh, reset the rate limiter for fetching authentication
    // details for credit card unmasking.
    if (autofill_manager_) {
      autofill_manager_->credit_card_access_manager()
          ->SignalCanFetchUnmaskDetails();
    }
    return;
  }

  ShowOfferNotificationIfApplicable(navigation_handle);

  // When IsServedFromBackForwardCache, the form data is not parsed
  // again. So, we should keep and use the autofill handler's
  // form_structures from BFCache for form submit.
  if (navigation_handle->IsServedFromBackForwardCache())
    return;

  submitted_forms_.clear();
  autofill_handler_->Reset();
}

void ContentAutofillDriver::SetAutofillManager(
    std::unique_ptr<AutofillManager> manager) {
  autofill_handler_ = std::move(manager);
  autofill_manager_ = static_cast<AutofillManager*>(autofill_handler_.get());
}

ContentAutofillDriver::ContentAutofillDriver()
    : render_frame_host_(nullptr),
      autofill_manager_(nullptr),
      key_press_handler_manager_(this),
      log_manager_(nullptr) {}

const mojo::AssociatedRemote<mojom::AutofillAgent>&
ContentAutofillDriver::GetAutofillAgent() {
  // Here is a lazy binding, and will not reconnect after connection error.
  if (!autofill_agent_) {
    render_frame_host_->GetRemoteAssociatedInterfaces()->GetInterface(
        &autofill_agent_);
  }

  return autofill_agent_;
}

void ContentAutofillDriver::RegisterKeyPressHandler(
    const content::RenderWidgetHost::KeyPressEventCallback& handler) {
  key_press_handler_manager_.RegisterKeyPressHandler(handler);
}

void ContentAutofillDriver::RemoveKeyPressHandler() {
  key_press_handler_manager_.RemoveKeyPressHandler();
}

void ContentAutofillDriver::AddHandler(
    const content::RenderWidgetHost::KeyPressEventCallback& handler) {
  content::RenderWidgetHostView* view = render_frame_host_->GetView();
  if (!view)
    return;
  view->GetRenderWidgetHost()->AddKeyPressEventCallback(handler);
}

void ContentAutofillDriver::RemoveHandler(
    const content::RenderWidgetHost::KeyPressEventCallback& handler) {
  content::RenderWidgetHostView* view = render_frame_host_->GetView();
  if (!view)
    return;
  view->GetRenderWidgetHost()->RemoveKeyPressEventCallback(handler);
}

void ContentAutofillDriver::SetAutofillProvider(
    AutofillProvider* provider,
    AutofillClient* client,
    AutofillHandler::AutofillDownloadManagerState enable_download_manager) {
  autofill_handler_ = std::make_unique<AutofillHandlerProxy>(
      this, client, provider, enable_download_manager);
  GetAutofillAgent()->SetUserGestureRequired(false);
  GetAutofillAgent()->SetSecureContextRequired(true);
  GetAutofillAgent()->SetFocusRequiresScroll(false);
  GetAutofillAgent()->SetQueryPasswordSuggestion(true);
}

bool ContentAutofillDriver::DocumentUsedWebOTP() const {
  return render_frame_host_->DocumentUsedWebOTP();
}

void ContentAutofillDriver::MaybeReportAutofillWebOTPMetrics() {
  // In tests, the autofill_manager_ may be unset or destroyed before |this|.
  if (!autofill_manager_)
    return;
  // It's possible that a frame without any form uses WebOTP. e.g. a server may
  // send the verification code to a phone number that was collected beforehand
  // and uses the WebOTP API for authentication purpose without user manually
  // entering the code.
  if (!autofill_manager_->has_parsed_forms() && !DocumentUsedWebOTP())
    return;

  ReportAutofillWebOTPMetrics(DocumentUsedWebOTP());
}

void ContentAutofillDriver::ReportAutofillWebOTPMetrics(
    bool document_used_webotp) {
  if (autofill_manager_->has_observed_phone_number_field())
    phone_collection_metric_state_ |= phone_collection_metric::kPhoneCollected;
  if (autofill_manager_->has_observed_one_time_code_field())
    phone_collection_metric_state_ |= phone_collection_metric::kOTCUsed;
  if (document_used_webotp)
    phone_collection_metric_state_ |= phone_collection_metric::kWebOTPUsed;

  ukm::UkmRecorder* recorder = autofill_manager_->client()->GetUkmRecorder();
  ukm::SourceId source_id = autofill_manager_->client()->GetUkmSourceId();
  AutofillMetrics::LogWebOTPPhoneCollectionMetricStateUkm(
      recorder, source_id, phone_collection_metric_state_);

  UMA_HISTOGRAM_ENUMERATION(
      "Autofill.WebOTP.PhonePlusWebOTPPlusOTC",
      static_cast<PhoneCollectionMetricState>(phone_collection_metric_state_));
}

void ContentAutofillDriver::SetAutofillProviderForTesting(
    AutofillProvider* provider,
    AutofillClient* client) {
  SetAutofillProvider(provider, client,
                      AutofillHandler::AutofillDownloadManagerState::
                          DISABLE_AUTOFILL_DOWNLOAD_MANAGER);
  // AutofillManager isn't used if provider is valid.
  autofill_manager_ = nullptr;
}

void ContentAutofillDriver::ShowOfferNotificationIfApplicable(
    content::NavigationHandle* navigation_handle) {
  if (!navigation_handle->IsInMainFrame())
    return;

  // TODO(crbug.com/1093057): Android webview does not have |autofill_manager_|,
  // so flow is not enabled in Android Webview.
  if (!base::FeatureList::IsEnabled(
          features::kAutofillEnableOfferNotification) ||
      !autofill_manager_) {
    return;
  }

  AutofillOfferManager* offer_manager = autofill_manager_->offer_manager();
  // This happens in the Incognito mode.
  if (!offer_manager)
    return;

  GURL url = autofill_manager_->client()->GetLastCommittedURL();
  if (!offer_manager->IsUrlEligible(url))
    return;

  // Try to show offer notification when the last committed URL has the domain
  // that an offer is applicable for.
  std::tuple<std::vector<GURL>, GURL, CreditCard*> result =
      offer_manager->GetEligibleDomainsAndCardForOfferForUrl(url);
  std::vector<GURL>& domains = std::get<0>(result);
  GURL offer_details_url = std::get<1>(result);
  CreditCard* card = std::get<2>(result);
  // TODO(crbug.com/1093057): Update return condition once we introduce the
  // promo offers.
  if (domains.empty() || !card)
    return;

  autofill_manager_->client()->ShowOfferNotificationIfApplicable(
      domains, offer_details_url, card);
}

}  // namespace autofill
