// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/webauth/authenticator_common.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "base/base64url.h"
#include "base/bind.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/metrics/histogram_macros.h"
#include "base/notreached.h"
#include "base/rand_util.h"
#include "base/strings/string_piece.h"
#include "base/strings/utf_string_conversion_utils.h"
#include "base/timer/timer.h"
#include "build/build_config.h"
#include "build/chromeos_buildflags.h"
#include "content/browser/bad_message.h"
#include "content/browser/renderer_host/back_forward_cache_disable.h"
#include "content/browser/webauth/authenticator_environment_impl.h"
#include "content/browser/webauth/virtual_authenticator_request_delegate.h"
#include "content/browser/webauth/virtual_fido_discovery_factory.h"
#include "content/browser/webauth/webauth_request_security_checker.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/device_service.h"
#include "content/public/browser/is_uvpaa.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_features.h"
#include "crypto/sha2.h"
#include "device/base/features.h"
#include "device/fido/attestation_statement.h"
#include "device/fido/ctap_make_credential_request.h"
#include "device/fido/features.h"
#include "device/fido/fido_authenticator.h"
#include "device/fido/fido_constants.h"
#include "device/fido/fido_parsing_utils.h"
#include "device/fido/fido_transport_protocol.h"
#include "device/fido/filter.h"
#include "device/fido/get_assertion_request_handler.h"
#include "device/fido/make_credential_request_handler.h"
#include "device/fido/public_key.h"
#include "device/fido/public_key_credential_descriptor.h"
#include "device/fido/public_key_credential_params.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/cert/asn1_util.h"
#include "net/der/input.h"
#include "net/der/parse_values.h"
#include "net/der/parser.h"
#include "services/network/public/cpp/is_potentially_trustworthy.h"
#include "url/url_constants.h"
#include "url/url_util.h"

#if defined(OS_MAC)
#include "device/fido/mac/authenticator.h"
#include "device/fido/mac/credential_metadata.h"
#endif

#if BUILDFLAG(IS_CHROMEOS_ASH)
#include "device/fido/cros/authenticator.h"
#endif

namespace content {

// RequestExtension is a type of extension in a WebAuthn request that might
// yield an extension output in the response.
enum class RequestExtension {
  kAppID,
  kHMACSecret,
  kPRF,
  kCredProps,
  kLargeBlobEnable,
  kLargeBlobRead,
  kLargeBlobWrite,
};

namespace client_data {
const char kCreateType[] = "webauthn.create";
const char kGetType[] = "webauthn.get";
const char kU2fSignType[] = "navigator.id.getAssertion";
const char kU2fRegisterType[] = "navigator.id.finishEnrollment";
}  // namespace client_data

namespace {

WebAuthenticationDelegate* GetWebAuthenticationDelegate() {
  return GetContentClient()->browser()->GetWebAuthenticationDelegate();
}

std::string Base64UrlEncode(const base::span<const uint8_t> input) {
  std::string ret;
  base::Base64UrlEncode(
      base::StringPiece(reinterpret_cast<const char*>(input.data()),
                        input.size()),
      base::Base64UrlEncodePolicy::OMIT_PADDING, &ret);
  return ret;
}

// Validates whether the given origin is authorized to use the provided App
// ID value, mostly according to the rules in
// https://fidoalliance.org/specs/fido-u2f-v1.2-ps-20170411/fido-appid-and-facets-v1.2-ps-20170411.html#determining-if-a-caller-s-facetid-is-authorized-for-an-appid.
//
// Returns the App ID to use for the request, or base::nullopt if the origin
// is not authorized to use the provided value.
base::Optional<std::string> ProcessAppIdExtension(std::string appid,
                                                  const url::Origin& origin) {
  // The CryptoToken U2F extension checks the appid before calling the WebAuthn
  // API so there is no need to validate it here.
  if (WebAuthRequestSecurityChecker::OriginIsCryptoTokenExtension(origin)) {
    if (!GURL(appid).is_valid()) {
      DCHECK(false) << "cryptotoken request did not set a valid App ID";
      return base::nullopt;
    }
    return appid;
  }

  // Step 1: "If the AppID is not an HTTPS URL, and matches the FacetID of the
  // caller, no additional processing is necessary and the operation may
  // proceed."

  // Webauthn is only supported on secure origins and |ValidateEffectiveDomain|
  // has already checked this property of |origin| before this call. Thus this
  // step is moot.
  // TODO(https://crbug.com/1158302): Use IsOriginPotentiallyTrustworthy?
  DCHECK(network::IsUrlPotentiallyTrustworthy(origin.GetURL()));

  // Step 2: "If the AppID is null or empty, the client must set the AppID to be
  // the FacetID of the caller, and the operation may proceed without additional
  // processing."
  if (appid.empty()) {
    // While the U2F spec says to default the App ID to the Facet ID, which is
    // the origin plus a trailing forward slash [1], cryptotoken and Firefox
    // just use the site's Origin without trailing slash. We follow their
    // implementations rather than the spec.
    //
    // [1]https://fidoalliance.org/specs/fido-v2.0-id-20180227/fido-appid-and-facets-v2.0-id-20180227.html#determining-the-facetid-of-a-calling-application
    appid = origin.Serialize();
  }

  // Step 3: "If the caller's FacetID is an https:// Origin sharing the same
  // host as the AppID, (e.g. if an application hosted at
  // https://fido.example.com/myApp set an AppID of
  // https://fido.example.com/myAppId), no additional processing is necessary
  // and the operation may proceed."
  GURL appid_url = GURL(appid);
  if (!appid_url.is_valid() || appid_url.scheme() != url::kHttpsScheme ||
      appid_url.scheme_piece() != origin.scheme()) {
    return base::nullopt;
  }

  // This check is repeated inside |SameDomainOrHost|, just after this. However
  // it's cheap and mirrors the structure of the spec.
  if (appid_url.host_piece() == origin.host()) {
    return appid;
  }

  // At this point we diverge from the specification in order to avoid the
  // complexity of making a network request which isn't believed to be
  // necessary in practice. See also
  // https://bugzilla.mozilla.org/show_bug.cgi?id=1244959#c8
  if (net::registry_controlled_domains::SameDomainOrHost(
          appid_url, origin,
          net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES)) {
    return appid;
  }

  // As a compatibility hack, sites within google.com are allowed to assert two
  // special-case AppIDs. Firefox also does this:
  // https://groups.google.com/forum/#!msg/mozilla.dev.platform/Uiu3fwnA2xw/201ynAiPAQAJ
  const GURL kGstatic1 =
      GURL("https://www.gstatic.com/securitykey/origins.json");
  const GURL kGstatic2 =
      GURL("https://www.gstatic.com/securitykey/a/google.com/origins.json");
  DCHECK(kGstatic1.is_valid() && kGstatic2.is_valid());

  if (origin.DomainIs("google.com") && !appid_url.has_ref() &&
      (appid_url.EqualsIgnoringRef(kGstatic1) ||
       appid_url.EqualsIgnoringRef(kGstatic2))) {
    return appid;
  }

  return base::nullopt;
}

// The application parameter is the SHA-256 hash of the UTF-8 encoding of
// the application identity (i.e. relying_party_id) of the application
// requesting the registration.
std::array<uint8_t, crypto::kSHA256Length> CreateApplicationParameter(
    const std::string& relying_party_id) {
  std::array<uint8_t, crypto::kSHA256Length> application_parameter;
  crypto::SHA256HashString(relying_party_id, application_parameter.data(),
                           application_parameter.size());
  return application_parameter;
}

device::CtapGetAssertionRequest CreateCtapGetAssertionRequest(
    const std::string& client_data_json,
    const blink::mojom::PublicKeyCredentialRequestOptionsPtr& options,
    base::Optional<std::string> app_id,
    bool is_off_the_record) {
  device::CtapGetAssertionRequest request_parameter(options->relying_party_id,
                                                    client_data_json);

  request_parameter.allow_list = options->allow_credentials;

  request_parameter.user_verification = options->user_verification;

  if (app_id) {
    request_parameter.alternative_application_parameter =
        CreateApplicationParameter(*app_id);
    request_parameter.app_id = std::move(*app_id);
  }

  if (!options->cable_authentication_data.empty()) {
    request_parameter.cable_extension = options->cable_authentication_data;
  }
  if (options->large_blob_read) {
    request_parameter.large_blob_read = true;
    request_parameter.large_blob_key = true;
  }
  if (options->large_blob_write) {
    request_parameter.large_blob_key = true;
  }
  request_parameter.is_off_the_record_context = is_off_the_record;
  return request_parameter;
}

// Parses the FIDO transport types extension from the DER-encoded, X.509
// certificate in |der_cert| and appends any unique transport types found to
// |out_transports|.
void AppendUniqueTransportsFromCertificate(
    base::span<const uint8_t> der_cert,
    std::vector<device::FidoTransportProtocol>* out_transports) {
  // See
  // https://fidoalliance.org/specs/fido-u2f-v1.2-ps-20170411/fido-u2f-authenticator-transports-extension-v1.2-ps-20170411.html#fido-u2f-certificate-transports-extension
  static constexpr uint8_t kTransportTypesOID[] = {
      0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0xe5, 0x1c, 0x02, 0x01, 0x01};
  bool present, critical;
  base::StringPiece contents;
  if (!net::asn1::ExtractExtensionFromDERCert(
          base::StringPiece(reinterpret_cast<const char*>(der_cert.data()),
                            der_cert.size()),
          base::StringPiece(reinterpret_cast<const char*>(kTransportTypesOID),
                            sizeof(kTransportTypesOID)),
          &present, &critical, &contents) ||
      !present) {
    return;
  }

  const net::der::Input contents_der(contents);
  net::der::Parser contents_parser(contents_der);
  net::der::BitString transport_bits;
  if (!contents_parser.ReadBitString(&transport_bits)) {
    return;
  }

  // The certificate extension contains a BIT STRING where different bits
  // indicate support for different transports. The following array maps
  // between these bit indexes and the FidoTransportProtocol enum.
  static constexpr struct {
    uint8_t bit_index;
    device::FidoTransportProtocol transport;
  } kTransportMapping[] = {
      // Bit 0 is "Bluetooth Classic", not BLE. Since webauthn doesn't define a
      // transport type for this we ignore it.
      {1, device::FidoTransportProtocol::kBluetoothLowEnergy},
      {2, device::FidoTransportProtocol::kUsbHumanInterfaceDevice},
      {3, device::FidoTransportProtocol::kNearFieldCommunication},
      {4, device::FidoTransportProtocol::kInternal},
  };

  for (const auto& mapping : kTransportMapping) {
    if (transport_bits.AssertsBit(mapping.bit_index) &&
        !base::Contains(*out_transports, mapping.transport)) {
      out_transports->push_back(mapping.transport);
    }
  }
}

enum class AttestationErasureOption {
  kIncludeAttestation,
  kEraseAttestationButIncludeAaguid,
  kEraseAttestationAndAaguid,
};

base::TimeDelta AdjustTimeout(base::Optional<base::TimeDelta> timeout,
                              RenderFrameHost* render_frame_host) {
  // Time to wait for an authenticator to successfully complete an operation.
  static constexpr base::TimeDelta kAdjustedTimeoutLower =
      base::TimeDelta::FromSeconds(10);
  static constexpr base::TimeDelta kAdjustedTimeoutUpper =
      base::TimeDelta::FromMinutes(10);

  if (!timeout) {
    return kAdjustedTimeoutUpper;
  }
  const bool testing_api_enabled =
      AuthenticatorEnvironmentImpl::GetInstance()
          ->IsVirtualAuthenticatorEnabledFor(
              static_cast<RenderFrameHostImpl*>(render_frame_host)
                  ->frame_tree_node());
  if (testing_api_enabled) {
    return *timeout;
  }
  return std::max(kAdjustedTimeoutLower,
                  std::min(kAdjustedTimeoutUpper, *timeout));
}

blink::mojom::MakeCredentialAuthenticatorResponsePtr
CreateMakeCredentialResponse(
    const std::string& client_data_json,
    device::AuthenticatorMakeCredentialResponse response_data,
    AttestationErasureOption attestation_erasure,
    const base::flat_set<RequestExtension>& requested_extensions) {
  auto response = blink::mojom::MakeCredentialAuthenticatorResponse::New();
  auto common_info = blink::mojom::CommonCredentialInfo::New();
  common_info->client_data_json.assign(client_data_json.begin(),
                                       client_data_json.end());
  common_info->authenticator_data = response_data.attestation_object()
                                        .authenticator_data()
                                        .SerializeToByteArray();
  common_info->raw_id = response_data.attestation_object().GetCredentialId();
  common_info->id = Base64UrlEncode(common_info->raw_id);
  response->info = std::move(common_info);

  // The transport list must not contain duplicates but the order doesn't matter
  // because Blink will sort the resulting strings before returning them.
  std::vector<device::FidoTransportProtocol> transports;
  if (response_data.transport_used()) {
    transports.push_back(*response_data.transport_used());
  }
  // If the attestation certificate specifies that the token supports any other
  // transports, include them in the list.
  base::Optional<base::span<const uint8_t>> leaf_cert =
      response_data.attestation_object()
          .attestation_statement()
          .GetLeafCertificate();
  if (leaf_cert) {
    AppendUniqueTransportsFromCertificate(*leaf_cert, &transports);
  }

  response->transports = std::move(transports);

  bool did_create_hmac_secret = false;
  const base::Optional<cbor::Value>& maybe_extensions =
      response_data.attestation_object().authenticator_data().extensions();
  if (maybe_extensions) {
    DCHECK(maybe_extensions->is_map());
    const cbor::Value::MapValue& extensions = maybe_extensions->GetMap();
    const auto hmac_secret_it =
        extensions.find(cbor::Value(device::kExtensionHmacSecret));
    if (hmac_secret_it != extensions.end() &&
        hmac_secret_it->second.is_bool() && hmac_secret_it->second.GetBool()) {
      did_create_hmac_secret = true;
    }
  }

  for (const RequestExtension ext : requested_extensions) {
    switch (ext) {
      case RequestExtension::kPRF:
        response->echo_prf = true;
        response->prf = did_create_hmac_secret;
        break;
      case RequestExtension::kHMACSecret:
        response->echo_hmac_create_secret = true;
        response->hmac_create_secret = did_create_hmac_secret;
        break;
      case RequestExtension::kCredProps:
        response->echo_cred_props = true;
        if (response_data.is_resident_key) {
          response->has_cred_props_rk = true;
          response->cred_props_rk = *response_data.is_resident_key;
        }
        break;
      case RequestExtension::kLargeBlobEnable:
        response->echo_large_blob = true;
        response->supports_large_blob =
            response_data.large_blob_key().has_value();
        break;
      case RequestExtension::kAppID:
      case RequestExtension::kLargeBlobRead:
      case RequestExtension::kLargeBlobWrite:
        NOTREACHED();
        break;
    }
  }

  switch (attestation_erasure) {
    case AttestationErasureOption::kIncludeAttestation:
      break;
    case AttestationErasureOption::kEraseAttestationButIncludeAaguid:
      response_data.EraseAttestationStatement(
          device::AttestationObject::AAGUID::kInclude);
      break;
    case AttestationErasureOption::kEraseAttestationAndAaguid:
      response_data.EraseAttestationStatement(
          device::AttestationObject::AAGUID::kErase);
      break;
  }
  response->attestation_object =
      response_data.GetCBOREncodedAttestationObject();

  const device::PublicKey* public_key = response_data.attestation_object()
                                            .authenticator_data()
                                            .attested_data()
                                            ->public_key();
  response->public_key_algo = public_key->algorithm;
  const base::Optional<std::vector<uint8_t>>& public_key_der =
      public_key->der_bytes;
  if (public_key_der) {
    response->public_key_der.emplace(public_key_der.value());
  }

  return response;
}

blink::mojom::GetAssertionAuthenticatorResponsePtr CreateGetAssertionResponse(
    const std::string& client_data_json,
    device::AuthenticatorGetAssertionResponse response_data,
    const base::Optional<std::string>& app_id,
    const base::flat_set<RequestExtension>& requested_extensions) {
  auto response = blink::mojom::GetAssertionAuthenticatorResponse::New();
  auto common_info = blink::mojom::CommonCredentialInfo::New();
  common_info->client_data_json.assign(client_data_json.begin(),
                                       client_data_json.end());
  common_info->raw_id = response_data.credential->id();
  common_info->id = Base64UrlEncode(common_info->raw_id);
  response->info = std::move(common_info);
  response->info->authenticator_data =
      response_data.authenticator_data.SerializeToByteArray();
  response->signature = response_data.signature;
  response_data.user_entity
      ? response->user_handle.emplace(response_data.user_entity->id)
      : response->user_handle.emplace();

  for (RequestExtension ext : requested_extensions) {
    switch (ext) {
      case RequestExtension::kAppID:
        DCHECK(app_id);
        response->echo_appid_extension = true;
        if (response_data.authenticator_data.application_parameter() ==
            CreateApplicationParameter(*app_id)) {
          response->appid_extension = true;
        }
        break;
      case RequestExtension::kPRF: {
        response->echo_prf = true;
        base::Optional<base::span<const uint8_t>> hmac_secret =
            response_data.hmac_secret;
        if (hmac_secret) {
          auto prf_values = blink::mojom::PRFValues::New();
          DCHECK(hmac_secret->size() == 32 || hmac_secret->size() == 64);
          prf_values->first = device::fido_parsing_utils::Materialize(
              hmac_secret->subspan(0, 32));
          if (hmac_secret->size() == 64) {
            prf_values->second = device::fido_parsing_utils::Materialize(
                hmac_secret->subspan(32, 32));
          }
          response->prf_results = std::move(prf_values);
        } else {
          response->prf_not_evaluated = response_data.hmac_secret_not_evaluated;
        }
        break;
      }
      case RequestExtension::kLargeBlobRead:
        response->echo_large_blob = true;
        response->large_blob = response_data.large_blob;
        break;
      case RequestExtension::kLargeBlobWrite:
        response->echo_large_blob = true;
        response->echo_large_blob_written = true;
        response->large_blob_written = response_data.large_blob_written;
        break;
      case RequestExtension::kHMACSecret:
      case RequestExtension::kCredProps:
      case RequestExtension::kLargeBlobEnable:
        NOTREACHED();
        break;
    }
  }

  return response;
}

void IsUserVerifyingPlatformAuthenticatorAvailableImpl(
    RenderFrameHost* render_frame_host,
    blink::mojom::Authenticator::
        IsUserVerifyingPlatformAuthenticatorAvailableCallback callback) {
  // Check for a delegate override. Chrome overrides IsUVPAA() in Guest mode.
  base::Optional<bool> is_uvpaa_override =
      GetWebAuthenticationDelegate()
          ->IsUserVerifyingPlatformAuthenticatorAvailableOverride(
              render_frame_host);
  if (is_uvpaa_override) {
    std::move(callback).Run(*is_uvpaa_override);
    return;
  }

#if defined(OS_MAC)
  const base::Optional<device::fido::mac::AuthenticatorConfig> config =
      GetWebAuthenticationDelegate()->GetTouchIdAuthenticatorConfig(
          render_frame_host->GetBrowserContext());
  if (!config) {
    std::move(callback).Run(false);
    return;
  }
  IsUVPlatformAuthenticatorAvailable(*config, std::move(callback));
#elif defined(OS_WIN)
  // TODO(crbug.com/908622): Enable platform authenticators in Incognito on
  // Windows once the API allows triggering an adequate warning dialog.
  if (render_frame_host->GetBrowserContext()->IsOffTheRecord()) {
    std::move(callback).Run(false);
    return;
  }

  IsUVPlatformAuthenticatorAvailable(
      AuthenticatorEnvironmentImpl::GetInstance()->win_webauthn_api(),
      std::move(callback));
#elif BUILDFLAG(IS_CHROMEOS_ASH)
  IsUVPlatformAuthenticatorAvailable(std::move(callback));
#else
  std::move(callback).Run(false);
#endif
}

// GetAvailableTransports returns the set of transports that should be passed to
// a FidoRequestHandler for the current request. This determines for which
// transports the request handler will attempt to obtain FidoDiscovery
// instances.
base::flat_set<device::FidoTransportProtocol> GetAvailableTransports(
    RenderFrameHost* render_frame_host,
    device::FidoDiscoveryFactory* discovery_factory,
    const url::Origin& caller_origin) {
  // U2F requests proxied from the cryptotoken extension are limited to USB
  // devices.
  if (WebAuthRequestSecurityChecker::OriginIsCryptoTokenExtension(
          caller_origin)) {
    return base::flat_set<device::FidoTransportProtocol>(
        {device::FidoTransportProtocol::kUsbHumanInterfaceDevice});
  }

  base::flat_set<device::FidoTransportProtocol> transports;
  transports.insert(device::FidoTransportProtocol::kUsbHumanInterfaceDevice);

  // Only instantiate platform discovery if the embedder hasn't chosen to
  // override IsUserVerifyingPlatformAuthenticatorAvailable() to be false.
  // Chrome disables platform authenticators in Guest modes this way.
  base::Optional<bool> embedder_isuvpaa_override =
      GetWebAuthenticationDelegate()
          ->IsUserVerifyingPlatformAuthenticatorAvailableOverride(
              render_frame_host);
  if (!embedder_isuvpaa_override || *embedder_isuvpaa_override) {
    transports.insert(device::FidoTransportProtocol::kInternal);
  }

  if (discovery_factory->IsTestOverride()) {
    // The desktop implementation does not support BLE or NFC, but we emulate
    // them if the testing API is enabled.
    transports.insert(device::FidoTransportProtocol::kBluetoothLowEnergy);
    transports.insert(device::FidoTransportProtocol::kNearFieldCommunication);

    // Ensure virtual platform authenticators can be instantiated even if they
    // are not-user-verifying, i.e. IsUVPAA() returns false.
    transports.insert(device::FidoTransportProtocol::kInternal);
  }

  if (base::FeatureList::IsEnabled(features::kWebAuthCable) ||
      base::FeatureList::IsEnabled(device::kWebAuthPhoneSupport)) {
    transports.insert(
        device::FidoTransportProtocol::kCloudAssistedBluetoothLowEnergy);
  }

#if !defined(OS_WIN)
  // kAndroidAccessory doesn't work on Windows because of USB stack issues.
  // Note: even if this value were inserted it wouldn't take effect on Windows
  // versions with a native API because FidoRequestHandlerBase filters out
  // non-kCloudAssistedBluetoothLowEnergy transports in that case.

  if (base::FeatureList::IsEnabled(device::kWebAuthPhoneSupport) ||
      base::FeatureList::IsEnabled(device::kWebAuthCableServerLink)) {
    // In order for AOA to be active the |AuthenticatorRequestClientDelegate|
    // must configure a |UsbDeviceManager|, which it'll only do if
    // |kWebAuthPhoneSupport| is enabled, or if a V2 caBLE extension is seen.
    transports.insert(device::FidoTransportProtocol::kAndroidAccessory);
  }
#endif

  return transports;
}

// Returns a new FidoDiscoveryFactory for the current request. This may be
// a factory for virtual authenticators if the testing API is enabled for the
// given frame.
std::unique_ptr<device::FidoDiscoveryFactory> MakeDiscoveryFactory(
    RenderFrameHost* render_frame_host,
    bool is_u2f_api_request) {
  VirtualAuthenticatorManagerImpl* virtual_authenticator_manager =
      AuthenticatorEnvironmentImpl::GetInstance()
          ->MaybeGetVirtualAuthenticatorManager(
              static_cast<RenderFrameHostImpl*>(render_frame_host)
                  ->frame_tree_node());
  if (virtual_authenticator_manager) {
    return virtual_authenticator_manager->MakeDiscoveryFactory();
  }

  auto discovery_factory = std::make_unique<device::FidoDiscoveryFactory>();

#if defined(OS_MAC)
  discovery_factory->set_mac_touch_id_info(
      GetWebAuthenticationDelegate()->GetTouchIdAuthenticatorConfig(
          render_frame_host->GetBrowserContext()));
#endif  // defined(OS_MAC)

#if defined(OS_WIN)
  if (base::FeatureList::IsEnabled(device::kWebAuthUseNativeWinApi)) {
    discovery_factory->set_win_webauthn_api(
        AuthenticatorEnvironmentImpl::GetInstance()->win_webauthn_api());
  }
#endif  // defined(OS_WIN)

#if BUILDFLAG(IS_CHROMEOS_ASH)
  // Ignore the ChromeOS u2fd virtual U2F HID device for WebAuthn requests so
  // that it doesn't collide with the ChromeOS platform authenticator, also
  // implemented in u2fd.
  if (base::FeatureList::IsEnabled(device::kWebAuthCrosPlatformAuthenticator) &&
      !is_u2f_api_request) {
    constexpr device::VidPid kChromeOsU2fdVidPid{0x18d1, 0x502c};
    discovery_factory->set_hid_ignore_list({kChromeOsU2fdVidPid});
    discovery_factory->set_generate_request_id_callback(
        GetWebAuthenticationDelegate()->GetGenerateRequestIdCallback(
            render_frame_host));
  }
#endif  // BUILDFLAG(IS_CHROMEOS_ASH)

  return discovery_factory;
}

// ToJSONString encodes |in| as a JSON string, using the specific escaping rules
// required by https://github.com/w3c/webauthn/pull/1375.
std::string ToJSONString(base::StringPiece in) {
  std::string ret;
  ret.reserve(in.size() + 2);
  ret.push_back('"');

  const char* const in_bytes = in.data();
  // ICU uses |int32_t| for lengths.
  const int32_t length = base::checked_cast<int32_t>(in.size());
  int32_t offset = 0;

  while (offset < length) {
    const int32_t prior_offset = offset;
    // Input strings must be valid UTF-8.
    uint32_t codepoint;
    CHECK(base::ReadUnicodeCharacter(in_bytes, length, &offset, &codepoint));
    // offset is updated by |ReadUnicodeCharacter| to index the last byte of the
    // codepoint. Increment it to index the first byte of the next codepoint for
    // the subsequent iteration.
    offset++;

    if (codepoint == 0x20 || codepoint == 0x21 ||
        (codepoint >= 0x23 && codepoint <= 0x5b) || codepoint >= 0x5d) {
      ret.append(&in_bytes[prior_offset], &in_bytes[offset]);
    } else if (codepoint == 0x22) {
      ret.append("\\\"");
    } else if (codepoint == 0x5c) {
      ret.append("\\\\");
    } else {
      static const char hextable[17] = "0123456789abcdef";
      ret.append("\\u00");
      ret.push_back(hextable[codepoint >> 4]);
      ret.push_back(hextable[codepoint & 15]);
    }
  }

  ret.push_back('"');
  return ret;
}

}  // namespace

std::string SerializeWebAuthnCollectedClientDataToJson(
    const std::string& type,
    const std::string& origin,
    base::span<const uint8_t> challenge,
    bool is_cross_origin,
    bool use_legacy_u2f_type_key /* = false */) {
  std::string ret;
  ret.reserve(128);

  if (use_legacy_u2f_type_key) {
    ret.append(R"({"typ":)");
  } else {
    ret.append(R"({"type":)");
  }
  ret.append(ToJSONString(type));

  ret.append(R"(,"challenge":)");
  ret.append(ToJSONString(Base64UrlEncode(challenge)));

  ret.append(R"(,"origin":)");
  ret.append(ToJSONString(origin));

  if (is_cross_origin) {
    ret.append(R"(,"crossOrigin":true)");
  } else {
    ret.append(R"(,"crossOrigin":false)");
  }

  if (base::RandDouble() < 0.2) {
    // An extra key is sometimes added to ensure that RPs do not make
    // unreasonably specific assumptions about the clientData JSON. This is
    // done in the fashion of
    // https://tools.ietf.org/html/draft-ietf-tls-grease
    ret.append(R"(,"other_keys_can_be_added_here":")");
    ret.append(
        "do not compare clientDataJSON against a template. See "
        "https://goo.gl/yabPex\"");
  }

  ret.append("}");
  return ret;
}

AuthenticatorCommon::AuthenticatorCommon(RenderFrameHost* render_frame_host)
    : render_frame_host_id_(render_frame_host->GetGlobalFrameRoutingId()),
      security_checker_(static_cast<RenderFrameHostImpl*>(render_frame_host)
                            ->GetWebAuthRequestSecurityChecker()) {
  // Disable the back-forward cache for any document that makes WebAuthn
  // requests. Pages using privacy-sensitive APIs are generally exempt from
  // back-forward cache for now as a precaution.
  BackForwardCache::DisableForRenderFrameHost(
      render_frame_host,
      BackForwardCacheDisable::DisabledReason(
          BackForwardCacheDisable::DisabledReasonId::kWebAuthenticationAPI));
}

AuthenticatorCommon::~AuthenticatorCommon() {
  // Resolve pending callbacks before disconnecting the receiver.
  if (make_credential_response_callback_) {
    CompleteMakeCredentialRequest(
        blink::mojom::AuthenticatorStatus::UNKNOWN_ERROR);
  }
  if (get_assertion_response_callback_) {
    CompleteGetAssertionRequest(
        blink::mojom::AuthenticatorStatus::UNKNOWN_ERROR);
  }
}

std::unique_ptr<AuthenticatorRequestClientDelegate>
AuthenticatorCommon::MaybeCreateRequestDelegate() {
  RenderFrameHostImpl* const render_frame_host_impl =
      static_cast<RenderFrameHostImpl*>(GetRenderFrameHost());
  if (AuthenticatorEnvironmentImpl::GetInstance()
          ->IsVirtualAuthenticatorEnabledFor(
              render_frame_host_impl->frame_tree_node())) {
    return std::make_unique<VirtualAuthenticatorRequestDelegate>();
  }
  return GetContentClient()->browser()->GetWebAuthenticationRequestDelegate(
      render_frame_host_impl);
}

void AuthenticatorCommon::StartMakeCredentialRequest(
    bool allow_skipping_pin_touch) {
  InitDiscoveryFactory();

  request_delegate_->ConfigureCable(
      caller_origin_, base::span<const device::CableDiscoveryData>(),
      discovery_factory());

  make_credential_options_->allow_skipping_pin_touch = allow_skipping_pin_touch;

  request_ = std::make_unique<device::MakeCredentialRequestHandler>(
      discovery_factory(),
      GetAvailableTransports(GetRenderFrameHost(), discovery_factory(),
                             caller_origin_),
      *ctap_make_credential_request_, *make_credential_options_,
      base::BindOnce(&AuthenticatorCommon::OnRegisterResponse,
                     weak_factory_.GetWeakPtr()));

  request_delegate_->RegisterActionCallbacks(
      base::BindOnce(&AuthenticatorCommon::OnCancelFromUI,
                     weak_factory_.GetWeakPtr()) /* cancel_callback */,
      base::BindRepeating(
          &AuthenticatorCommon::StartMakeCredentialRequest,
          weak_factory_.GetWeakPtr(),
          /*allow_skipping_pin_touch=*/false) /* start_over_callback */,
      base::BindRepeating(
          &device::FidoRequestHandlerBase::StartAuthenticatorRequest,
          request_->GetWeakPtr()) /* request_callback */,
      base::BindRepeating(
          &device::FidoRequestHandlerBase::PowerOnBluetoothAdapter,
          request_->GetWeakPtr()) /* bluetooth_adapter_power_on_callback */);
  request_->set_observer(request_delegate_.get());
}

void AuthenticatorCommon::StartGetAssertionRequest(
    bool allow_skipping_pin_touch) {
  InitDiscoveryFactory();

  base::span<const device::CableDiscoveryData> cable_pairings;
  if (ctap_get_assertion_request_->cable_extension && IsFocused()) {
    cable_pairings = *ctap_get_assertion_request_->cable_extension;
  }
  request_delegate_->ConfigureCable(caller_origin_, cable_pairings,
                                    discovery_factory());
#if BUILDFLAG(IS_CHROMEOS_ASH)
  discovery_factory()->set_get_assertion_request_for_legacy_credential_check(
      *ctap_get_assertion_request_);
#endif

  request_ = std::make_unique<device::GetAssertionRequestHandler>(
      discovery_factory(),
      GetAvailableTransports(GetRenderFrameHost(), discovery_factory(),
                             caller_origin_),
      *ctap_get_assertion_request_, *ctap_get_assertion_options_,
      allow_skipping_pin_touch,
      base::BindOnce(&AuthenticatorCommon::OnSignResponse,
                     weak_factory_.GetWeakPtr()));

  request_delegate_->RegisterActionCallbacks(
      base::BindOnce(&AuthenticatorCommon::OnCancelFromUI,
                     weak_factory_.GetWeakPtr()) /* cancel_callback */,
      base::BindRepeating(
          &AuthenticatorCommon::StartGetAssertionRequest,
          weak_factory_.GetWeakPtr(),
          /*allow_skipping_pin_touch=*/false) /* start_over_callback */,
      base::BindRepeating(
          &device::FidoRequestHandlerBase::StartAuthenticatorRequest,
          request_->GetWeakPtr()) /* request_callback */,
      base::BindRepeating(
          &device::FidoRequestHandlerBase::PowerOnBluetoothAdapter,
          request_->GetWeakPtr()) /* bluetooth_adapter_power_on_callback */);

  request_->set_observer(request_delegate_.get());
}

bool AuthenticatorCommon::IsFocused() const {
  return GetRenderFrameHost()->IsCurrent() && request_delegate_->IsFocused();
}

void AuthenticatorCommon::OnLargeBlobCompressed(
    data_decoder::DataDecoder::ResultOrError<mojo_base::BigBuffer> result) {
  ctap_get_assertion_request_->large_blob_write =
      device::fido_parsing_utils::MaterializeOrNull(result.value);
  StartGetAssertionRequest(/*allow_skipping_pin_touch=*/true);
}

void AuthenticatorCommon::OnLargeBlobUncompressed(
    device::AuthenticatorGetAssertionResponse response,
    data_decoder::DataDecoder::ResultOrError<mojo_base::BigBuffer> result) {
  response.large_blob =
      device::fido_parsing_utils::MaterializeOrNull(result.value);
  CompleteGetAssertionRequest(
      blink::mojom::AuthenticatorStatus::SUCCESS,
      CreateGetAssertionResponse(client_data_json_, std::move(response),
                                 app_id_, requested_extensions_));
}

// mojom::Authenticator
void AuthenticatorCommon::MakeCredential(
    url::Origin caller_origin,
    blink::mojom::PublicKeyCredentialCreationOptionsPtr options,
    blink::mojom::Authenticator::MakeCredentialCallback callback) {
  if (request_) {
    if (WebAuthRequestSecurityChecker::OriginIsCryptoTokenExtension(
            caller_origin)) {
      // Requests originating from cryptotoken will generally outlive any
      // navigation events on the tab of the request's sender. Evict pending
      // requests if cryptotoken sends a new one such that requests from before
      // a navigation event do not prevent new requests. See
      // https://crbug.com/935480.
      CancelWithStatus(blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
    } else {
      std::move(callback).Run(
          blink::mojom::AuthenticatorStatus::PENDING_REQUEST, nullptr);
      return;
    }
  }
  DCHECK(!request_);
  DCHECK(make_credential_response_callback_.is_null());
  make_credential_response_callback_ = std::move(callback);

  bool is_cross_origin;
  blink::mojom::AuthenticatorStatus status =
      security_checker_->ValidateAncestorOrigins(
          caller_origin,
          options->is_payment_credential_creation
              ? WebAuthRequestSecurityChecker::RequestType::
                    kMakePaymentCredential
              : WebAuthRequestSecurityChecker::RequestType::kMakeCredential,
          &is_cross_origin);
  if (status != blink::mojom::AuthenticatorStatus::SUCCESS) {
    CompleteMakeCredentialRequest(status);
    return;
  }

  request_delegate_ = MaybeCreateRequestDelegate();
  if (!request_delegate_) {
    CompleteMakeCredentialRequest(
        blink::mojom::AuthenticatorStatus::PENDING_REQUEST);
    return;
  }

  base::Optional<std::string> rp_id =
      request_delegate_->MaybeGetRelyingPartyIdOverride(
          options->relying_party.id, caller_origin);

  if (!rp_id) {
    // If the delegate didn't override RP ID selection then apply standard
    // rules.
    rp_id = std::move(options->relying_party.id);
    status = security_checker_->ValidateDomainAndRelyingPartyID(caller_origin,
                                                                *rp_id);
    if (status != blink::mojom::AuthenticatorStatus::SUCCESS) {
      CompleteMakeCredentialRequest(status);
      return;
    }
  }

  caller_origin_ = caller_origin;
  relying_party_id_ = *rp_id;
  options->relying_party.id = std::move(*rp_id);
  request_delegate_->SetRelyingPartyId(relying_party_id_);

  device::fido_filter::MaybeInitialize();
  switch (device::fido_filter::Evaluate(
      device::fido_filter::Operation::MAKE_CREDENTIAL, relying_party_id_,
      /*device=*/base::nullopt,
      /*id=*/base::nullopt)) {
    case device::fido_filter::Action::ALLOW:
      break;
    case device::fido_filter::Action::NO_ATTESTATION:
      // This will be handled by the request handler.
      break;
    case device::fido_filter::Action::BLOCK:
      CompleteMakeCredentialRequest(
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
  }

  base::Optional<std::string> appid_exclude;
  if (options->appid_exclude) {
    appid_exclude =
        ProcessAppIdExtension(*options->appid_exclude, caller_origin);
    if (!appid_exclude) {
      CompleteMakeCredentialRequest(
          blink::mojom::AuthenticatorStatus::INVALID_DOMAIN);
      return;
    }
  }

  if (options->user.icon_url) {
    status = security_checker_->ValidateAPrioriAuthenticatedUrl(
        *options->user.icon_url);
  }
  if (status == blink::mojom::AuthenticatorStatus::SUCCESS &&
      options->relying_party.icon_url) {
    status = security_checker_->ValidateAPrioriAuthenticatedUrl(
        *options->relying_party.icon_url);
  }
  if (status != blink::mojom::AuthenticatorStatus::SUCCESS) {
    bad_message::ReceivedBadMessage(GetRenderFrameHost()->GetProcess(),
                                    bad_message::AUTH_INVALID_ICON_URL);
    CompleteMakeCredentialRequest(status);
    return;
  }

  if (!IsFocused()) {
    CompleteMakeCredentialRequest(
        blink::mojom::AuthenticatorStatus::NOT_FOCUSED);
    return;
  }

  const device::AuthenticatorSelectionCriteria
      authenticator_selection_criteria =
          options->authenticator_selection
              ? *options->authenticator_selection
              : device::AuthenticatorSelectionCriteria();
  make_credential_options_ = device::MakeCredentialRequestHandler::Options(
      authenticator_selection_criteria);

  const bool might_create_resident_key =
      make_credential_options_->resident_key !=
      device::ResidentKeyRequirement::kDiscouraged;
  if (might_create_resident_key && !request_delegate_->SupportsResidentKeys()) {
    if (make_credential_options_->resident_key ==
        device::ResidentKeyRequirement::kRequired) {
      CompleteMakeCredentialRequest(
          blink::mojom::AuthenticatorStatus::RESIDENT_CREDENTIALS_UNSUPPORTED);
      return;
    }
    // Downgrade 'preferred' to 'discouraged'.
    make_credential_options_->resident_key =
        device::ResidentKeyRequirement::kDiscouraged;
  }

  // Reject any non-sensical credProtect extension values.
  if (  // Can't require the default policy (or no policy).
      (options->enforce_protection_policy &&
       (options->protection_policy ==
            blink::mojom::ProtectionPolicy::UNSPECIFIED ||
        options->protection_policy == blink::mojom::ProtectionPolicy::NONE)) ||
      // For non-resident keys, NONE doesn't make sense. (UV_OR_CRED_ID_REQUIRED
      // does because, with CTAP 2.0, just because a resident key isn't
      // _required_ doesn't mean that one won't be created and an RP might want
      // credProtect to take effect if that happens.)
      (!might_create_resident_key &&
       options->protection_policy == blink::mojom::ProtectionPolicy::NONE) ||
      // UV_REQUIRED only makes sense if UV is required overall.
      (options->protection_policy ==
           blink::mojom::ProtectionPolicy::UV_REQUIRED &&
       authenticator_selection_criteria.user_verification_requirement() !=
           device::UserVerificationRequirement::kRequired)) {
    CompleteMakeCredentialRequest(
        blink::mojom::AuthenticatorStatus::PROTECTION_POLICY_INCONSISTENT);
    return;
  }

  base::Optional<device::CredProtectRequest> cred_protect_request;
  switch (options->protection_policy) {
    case blink::mojom::ProtectionPolicy::UNSPECIFIED:
      if (might_create_resident_key) {
        // If not specified, kUVOrCredIDRequired is made the default unless
        // the authenticator defaults to something better.
        cred_protect_request =
            device::CredProtectRequest::kUVOrCredIDRequiredOrBetter;
      }
      break;
    case blink::mojom::ProtectionPolicy::NONE:
      cred_protect_request = device::CredProtectRequest::kUVOptional;
      break;
    case blink::mojom::ProtectionPolicy::UV_OR_CRED_ID_REQUIRED:
      cred_protect_request = device::CredProtectRequest::kUVOrCredIDRequired;
      break;
    case blink::mojom::ProtectionPolicy::UV_REQUIRED:
      cred_protect_request = device::CredProtectRequest::kUVRequired;
      break;
  }

  if (cred_protect_request) {
    make_credential_options_->cred_protect_request = {
        {*cred_protect_request, options->enforce_protection_policy}};
  }

  timer_->Start(
      FROM_HERE, AdjustTimeout(options->timeout, GetRenderFrameHost()),
      base::BindOnce(&AuthenticatorCommon::OnTimeout, base::Unretained(this)));

  const bool origin_is_crypto_token_extension =
      WebAuthRequestSecurityChecker::OriginIsCryptoTokenExtension(
          caller_origin_);

  // Cryptotoken provides the sender origin for register requests in the
  // |relying_party| |name| attribute. (The |id| attribute contains the AppID.)
  client_data_json_ =
      origin_is_crypto_token_extension
          ? SerializeWebAuthnCollectedClientDataToJson(
                client_data::kU2fRegisterType, *options->relying_party.name,
                options->challenge, /*is_cross_origin=*/false,
                /*use_legacy_u2f_type_key=*/true)
          : SerializeWebAuthnCollectedClientDataToJson(
                client_data::kCreateType, caller_origin_.Serialize(),
                options->challenge, is_cross_origin);

  // Cryptotoken requests, making payment credentials, and Touch-to-Autofill
  // should be proxied without UI.
  if (origin_is_crypto_token_extension ||
      options->is_payment_credential_creation || disable_ui_) {
    request_delegate_->DisableUI();
  }

  ctap_make_credential_request_ = device::CtapMakeCredentialRequest(
      client_data_json_, options->relying_party, options->user,
      device::PublicKeyCredentialParams(options->public_key_parameters));
  ctap_make_credential_request_->exclude_list = options->exclude_credentials;
  if (options->prf_enable) {
    requested_extensions_.insert(RequestExtension::kPRF);
    ctap_make_credential_request_->hmac_secret = true;
  }
  if (options->hmac_create_secret) {
    requested_extensions_.insert(RequestExtension::kHMACSecret);
    ctap_make_credential_request_->hmac_secret = true;
  }
  if (options->cred_props) {
    requested_extensions_.insert(RequestExtension::kCredProps);
  }
  if (options->large_blob_enable != device::LargeBlobSupport::kNotRequested) {
    requested_extensions_.insert(RequestExtension::kLargeBlobEnable);
  }
  make_credential_options_->large_blob_support = options->large_blob_enable;
  ctap_make_credential_request_->app_id = std::move(appid_exclude);
  ctap_make_credential_request_->is_off_the_record_context =
      GetBrowserContext()->IsOffTheRecord();
  // On dual protocol CTAP2/U2F devices, force credential creation over U2F.
  ctap_make_credential_request_->is_u2f_only = origin_is_crypto_token_extension;

  // Compute the effective attestation conveyance preference.
  device::AttestationConveyancePreference attestation = options->attestation;
  // Enterprise attestation should not have been approved by this point.
  DCHECK(attestation !=
         device::AttestationConveyancePreference::kEnterpriseApprovedByBrowser);
  if (attestation == device::AttestationConveyancePreference::
                         kEnterpriseIfRPListedOnAuthenticator &&
      request_delegate_->ShouldPermitIndividualAttestation(relying_party_id_)) {
    attestation =
        device::AttestationConveyancePreference::kEnterpriseApprovedByBrowser;
  }
  ctap_make_credential_request_->attestation_preference = attestation;

  StartMakeCredentialRequest(/*allow_skipping_pin_touch=*/true);
}

// mojom:Authenticator
void AuthenticatorCommon::GetAssertion(
    url::Origin caller_origin,
    blink::mojom::PublicKeyCredentialRequestOptionsPtr options,
    blink::mojom::Authenticator::GetAssertionCallback callback) {
  if (request_) {
    if (WebAuthRequestSecurityChecker::OriginIsCryptoTokenExtension(
            caller_origin)) {
      // Requests originating from cryptotoken will generally outlive any
      // navigation events on the tab of the request's sender. Evict pending
      // requests if cryptotoken sends a new one such that requests from before
      // a navigation event do not prevent new requests. See
      // https://crbug.com/935480.
      CancelWithStatus(blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
    } else {
      std::move(callback).Run(
          blink::mojom::AuthenticatorStatus::PENDING_REQUEST, nullptr);
      return;
    }
  }
  DCHECK(!request_);
  DCHECK(get_assertion_response_callback_.is_null());
  get_assertion_response_callback_ = std::move(callback);

  bool is_cross_origin;
  blink::mojom::AuthenticatorStatus status =
      security_checker_->ValidateAncestorOrigins(
          caller_origin,
          WebAuthRequestSecurityChecker::RequestType::kGetAssertion,
          &is_cross_origin);
  if (status != blink::mojom::AuthenticatorStatus::SUCCESS) {
    CompleteGetAssertionRequest(status);
    return;
  }

  request_delegate_ = MaybeCreateRequestDelegate();
  if (!request_delegate_) {
    CompleteGetAssertionRequest(
        blink::mojom::AuthenticatorStatus::PENDING_REQUEST);
    return;
  }

  base::Optional<std::string> rp_id =
      request_delegate_->MaybeGetRelyingPartyIdOverride(
          options->relying_party_id, caller_origin);

  if (!rp_id) {
    // If the delegate didn't override RP ID selection then apply standard
    // rules.
    status = security_checker_->ValidateDomainAndRelyingPartyID(
        caller_origin, options->relying_party_id);
    if (status != blink::mojom::AuthenticatorStatus::SUCCESS) {
      CompleteGetAssertionRequest(status);
      return;
    }

    rp_id = std::move(options->relying_party_id);
  }

  caller_origin_ = caller_origin;
  relying_party_id_ = *rp_id;
  options->relying_party_id = std::move(*rp_id);
  request_delegate_->SetRelyingPartyId(relying_party_id_);

  const bool origin_is_crypto_token_extension =
      WebAuthRequestSecurityChecker::OriginIsCryptoTokenExtension(
          caller_origin_);

  // Cryptotoken provides the sender origin for U2F sign requests in the
  // |relying_party_id| attribute.
  client_data_json_ =
      origin_is_crypto_token_extension
          ? SerializeWebAuthnCollectedClientDataToJson(
                client_data::kU2fSignType, options->relying_party_id,
                options->challenge, /*is_cross_origin=*/false,
                /*use_legacy_u2f_type_key=*/true)
          : SerializeWebAuthnCollectedClientDataToJson(
                client_data::kGetType, caller_origin_.Serialize(),
                options->challenge, is_cross_origin);

  device::fido_filter::MaybeInitialize();
  if (device::fido_filter::Evaluate(
          device::fido_filter::Operation::GET_ASSERTION, relying_party_id_,
          /*device=*/base::nullopt,
          /*id=*/base::nullopt) == device::fido_filter::Action::BLOCK) {
    CompleteGetAssertionRequest(
        blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
    return;
  }

  // Cryptotoken requests should be proxied without UI.
  if (origin_is_crypto_token_extension || disable_ui_) {
    DCHECK(!options->is_conditional);
    request_delegate_->DisableUI();
  }

  request_delegate_->SetConditionalRequest(options->is_conditional);

  if (options->allow_credentials.empty()) {
    if (!request_delegate_->SupportsResidentKeys()) {
      CompleteGetAssertionRequest(
          blink::mojom::AuthenticatorStatus::RESIDENT_CREDENTIALS_UNSUPPORTED);
      return;
    }
    empty_allow_list_ = true;
  }

  if (options->appid) {
    requested_extensions_.insert(RequestExtension::kAppID);
    app_id_ = ProcessAppIdExtension(*options->appid, caller_origin_);
    if (!app_id_) {
      CompleteGetAssertionRequest(
          blink::mojom::AuthenticatorStatus::INVALID_DOMAIN);
      return;
    }
  }

  if (options->large_blob_read && options->large_blob_write) {
    CompleteGetAssertionRequest(
        blink::mojom::AuthenticatorStatus::CANNOT_READ_AND_WRITE_LARGE_BLOB);
    return;
  }

  if (options->large_blob_read) {
    requested_extensions_.insert(RequestExtension::kLargeBlobRead);
  } else if (options->large_blob_write) {
    if (options->allow_credentials.size() != 1) {
      CompleteGetAssertionRequest(blink::mojom::AuthenticatorStatus::
                                      INVALID_ALLOW_CREDENTIALS_FOR_LARGE_BLOB);
      return;
    }
    requested_extensions_.insert(RequestExtension::kLargeBlobWrite);
  }

  timer_->Start(
      FROM_HERE, AdjustTimeout(options->timeout, GetRenderFrameHost()),
      base::BindOnce(&AuthenticatorCommon::OnTimeout, base::Unretained(this)));

  ctap_get_assertion_request_ =
      CreateCtapGetAssertionRequest(client_data_json_, options, app_id_,
                                    GetBrowserContext()->IsOffTheRecord());
  ctap_get_assertion_options_.emplace();

  bool is_first = true;
  base::Optional<std::vector<uint8_t>> last_id;
  if (options->prf) {
    requested_extensions_.insert(RequestExtension::kPRF);
    for (const auto& prf_input_from_renderer : options->prf_inputs) {
      device::CtapGetAssertionOptions::PRFInput prf_input;

      // This statement enforces invariants that should be established by the
      // renderer.
      if (
          // Only the first element in the vector may be the default.
          (!is_first && !prf_input_from_renderer->id) ||
          // The PRF inputs must be sorted by credential ID to show that there
          // are no duplicates.
          (last_id.has_value() && prf_input_from_renderer->id.has_value() &&
           *last_id >= *prf_input_from_renderer->id) ||
          // The lengths are specified in authenticator.mojom, so hopefully Mojo
          // enforces them too.
          prf_input_from_renderer->first.size() != prf_input.salt1.size() ||
          (prf_input_from_renderer->second &&
           prf_input_from_renderer->second->size() != prf_input.salt1.size())) {
        NOTREACHED();

        CompleteGetAssertionRequest(
            blink::mojom::AuthenticatorStatus::UNKNOWN_ERROR);
        return;
      }
      is_first = false;
      last_id = prf_input_from_renderer->id;

      if (prf_input_from_renderer->id) {
        prf_input.credential_id = std::move(*prf_input_from_renderer->id);
      }

      memcpy(prf_input.salt1.data(), prf_input_from_renderer->first.data(),
             prf_input.salt1.size());
      if (prf_input_from_renderer->second) {
        prf_input.salt2.emplace();
        memcpy(prf_input.salt2->data(), prf_input_from_renderer->second->data(),
               prf_input.salt2->size());
      }

      ctap_get_assertion_options_->prf_inputs.emplace_back(
          std::move(prf_input));
    }
  }

  ctap_get_assertion_request_->is_u2f_only = origin_is_crypto_token_extension;

  if (options->large_blob_write) {
    data_decoder_.GzipCompress(
        *options->large_blob_write,
        base::BindOnce(&AuthenticatorCommon::OnLargeBlobCompressed,
                       weak_factory_.GetWeakPtr()));
    return;
  }

  StartGetAssertionRequest(/*allow_skipping_pin_touch=*/true);
}

void AuthenticatorCommon::IsUserVerifyingPlatformAuthenticatorAvailable(
    blink::mojom::Authenticator::
        IsUserVerifyingPlatformAuthenticatorAvailableCallback callback) {
  auto post_done_callback = base::BindOnce(
      [](blink::mojom::Authenticator::
             IsUserVerifyingPlatformAuthenticatorAvailableCallback callback,
         bool is_available) {
        base::SequencedTaskRunnerHandle::Get()->PostTask(
            FROM_HERE, base::BindOnce(std::move(callback), is_available));
      },
      std::move(callback));

  IsUserVerifyingPlatformAuthenticatorAvailableImpl(
      GetRenderFrameHost(), std::move(post_done_callback));
}

void AuthenticatorCommon::Cancel() {
  CancelWithStatus(blink::mojom::AuthenticatorStatus::ABORT_ERROR);
}

// Callback to handle the async registration response from a U2fDevice.
void AuthenticatorCommon::OnRegisterResponse(
    device::MakeCredentialStatus status_code,
    base::Optional<device::AuthenticatorMakeCredentialResponse> response_data,
    const device::FidoAuthenticator* authenticator) {
  if (!request_) {
    // Either the callback was called immediately and |request_| has not yet
    // been assigned (this is a bug), or a navigation caused the request to be
    // canceled while a callback was enqueued.
    return;
  }

  switch (status_code) {
    case device::MakeCredentialStatus::kUserConsentButCredentialExcluded:
      // Duplicate registration: the new credential would be created on an
      // authenticator that already contains one of the credentials in
      // |exclude_credentials|.
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kKeyAlreadyRegistered,
          blink::mojom::AuthenticatorStatus::CREDENTIAL_EXCLUDED);
      return;
    case device::MakeCredentialStatus::kAuthenticatorResponseInvalid:
      // The response from the authenticator was corrupted.
      CompleteMakeCredentialRequest(
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR, nullptr,
          Focus::kDoCheck);
      return;
    case device::MakeCredentialStatus::kUserConsentDenied:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kUserConsentDenied,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::MakeCredentialStatus::kSoftPINBlock:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kSoftPINBlock,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::MakeCredentialStatus::kHardPINBlock:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kHardPINBlock,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::MakeCredentialStatus::kAuthenticatorRemovedDuringPINEntry:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kAuthenticatorRemovedDuringPINEntry,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::MakeCredentialStatus::kAuthenticatorMissingResidentKeys:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kAuthenticatorMissingResidentKeys,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::MakeCredentialStatus::kAuthenticatorMissingUserVerification:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kAuthenticatorMissingUserVerification,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::MakeCredentialStatus::kAuthenticatorMissingLargeBlob:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kAuthenticatorMissingLargeBlob,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::MakeCredentialStatus::kNoCommonAlgorithms:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kNoCommonAlgorithms,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::MakeCredentialStatus::kStorageFull:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kStorageFull,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::MakeCredentialStatus::kWinInvalidStateError:
      CompleteMakeCredentialRequest(
          blink::mojom::AuthenticatorStatus::CREDENTIAL_EXCLUDED, nullptr,
          Focus::kDoCheck);
      return;
    case device::MakeCredentialStatus::kWinNotAllowedError:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kWinUserCancelled,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::MakeCredentialStatus::kSuccess:
      DCHECK(response_data.has_value());
      DCHECK(authenticator);

      const base::Optional<device::FidoTransportProtocol> transport_used =
          authenticator->AuthenticatorTransport();
      bool is_transport_used_internal = false;
      bool is_transport_used_cable = false;
      if (transport_used) {
        is_transport_used_internal =
            *transport_used == device::FidoTransportProtocol::kInternal;
        is_transport_used_cable =
            *transport_used ==
            device::FidoTransportProtocol::kCloudAssistedBluetoothLowEnergy;
      }

      const auto attestation =
          ctap_make_credential_request_->attestation_preference;
      base::Optional<AttestationErasureOption> attestation_erasure;
      const bool origin_is_crypto_token_extension =
          WebAuthRequestSecurityChecker::OriginIsCryptoTokenExtension(
              caller_origin_);

      // cryptotoken checks the attestation blocklist itself.
      if (!origin_is_crypto_token_extension &&
          response_data->attestation_should_be_filtered &&
          !request_delegate_->ShouldPermitIndividualAttestation(
              relying_party_id_)) {
        attestation_erasure =
            AttestationErasureOption::kEraseAttestationAndAaguid;
      } else if (origin_is_crypto_token_extension &&
                 attestation !=
                     device::AttestationConveyancePreference::kNone) {
        // Cryptotoken requests may bypass the attestation prompt because the
        // extension implements its own. Invoking the attestation prompt code
        // here would not work anyway, because the WebContents associated with
        // the extension is not associated with any tab and therefore cannot
        // draw modal dialogs for the UI.
        //
        // Note that for AttestationConveyancePreference::kNone, attestation
        // erasure is still performed as usual.
        attestation_erasure = AttestationErasureOption::kIncludeAttestation;
      } else if (attestation == device::AttestationConveyancePreference::
                                    kEnterpriseApprovedByBrowser) {
        // If enterprise attestation was approved by policy then it can be
        // returned immediately.
        attestation_erasure = AttestationErasureOption::kIncludeAttestation;
      } else if (attestation == device::AttestationConveyancePreference::
                                    kEnterpriseIfRPListedOnAuthenticator &&
                 !response_data->enterprise_attestation_returned) {
        // If enterprise attestation was requested, not approved by policy, and
        // not approved by the authenticator, then any attestation is stripped.
        attestation_erasure =
            AttestationErasureOption::kEraseAttestationAndAaguid;
      } else if (is_transport_used_cable) {
        // Attestation is not returned when caBLEv2 is used, but the AAGUID is
        // maintained.
        attestation_erasure =
            AttestationErasureOption::kEraseAttestationButIncludeAaguid;
      } else if (is_transport_used_internal) {
        // Direct attestation from platform authenticators is known to be
        // privacy preserving, so we always return it when requested. Also,
        // counter to what the WebAuthn spec says, we do not erase the AAGUID
        // even when attestation wasn't requested.
        attestation_erasure =
            attestation != device::AttestationConveyancePreference::kNone
                ? AttestationErasureOption::kIncludeAttestation
                : AttestationErasureOption::kEraseAttestationButIncludeAaguid;
      } else if (attestation !=
                 device::AttestationConveyancePreference::kNone) {
        awaiting_attestation_response_ = true;
        request_delegate_->ShouldReturnAttestation(
            relying_party_id_, authenticator,
            response_data->enterprise_attestation_returned,
            base::BindOnce(
                &AuthenticatorCommon::OnRegisterResponseAttestationDecided,
                weak_factory_.GetWeakPtr(), std::move(*response_data)));
      } else if (response_data->IsSelfAttestation()) {
        attestation_erasure = AttestationErasureOption::kIncludeAttestation;
      } else {
        attestation_erasure =
            AttestationErasureOption::kEraseAttestationAndAaguid;
      }

      if (attestation_erasure.has_value()) {
        CompleteMakeCredentialRequest(
            blink::mojom::AuthenticatorStatus::SUCCESS,
            CreateMakeCredentialResponse(
                client_data_json_, std::move(*response_data),
                *attestation_erasure, requested_extensions_),
            Focus::kDoCheck);
      }

      return;
  }
  NOTREACHED();
}

void AuthenticatorCommon::OnRegisterResponseAttestationDecided(
    device::AuthenticatorMakeCredentialResponse response_data,
    bool attestation_permitted) {
  awaiting_attestation_response_ = false;
  if (!request_) {
    // The request has already been cleaned up, probably because a navigation
    // occurred while the permissions prompt was pending.
    return;
  }

  AttestationErasureOption attestation_erasure;
  if (!attestation_permitted) {
    attestation_erasure = AttestationErasureOption::kEraseAttestationAndAaguid;
  } else {
    attestation_erasure = AttestationErasureOption::kIncludeAttestation;
  }

  // The check for IsAttestationCertificateInappropriatelyIdentifying is
  // performed after the permissions prompt, even though we know the answer
  // before, because this still effectively discloses the make & model of
  // the authenticator: If an RP sees a "none" attestation from Chrome after
  // requesting direct attestation then it knows that it was one of the
  // tokens with inappropriate certs.
  if (response_data.IsAttestationCertificateInappropriatelyIdentifying() &&
      !request_delegate_->ShouldPermitIndividualAttestation(
          relying_party_id_)) {
    // The attestation response is incorrectly individually identifiable, but
    // the consent is for make & model information about a token, not for
    // individually-identifiable information. Erase the attestation to stop it
    // begin a tracking signal.

    // The only way to get the underlying attestation will be to list the RP ID
    // in the enterprise policy, because that enables the individual attestation
    // bit in the register request and permits individual attestation generally.
    attestation_erasure = AttestationErasureOption::kEraseAttestationAndAaguid;
  }

  CompleteMakeCredentialRequest(
      blink::mojom::AuthenticatorStatus::SUCCESS,
      CreateMakeCredentialResponse(client_data_json_, std::move(response_data),
                                   attestation_erasure, requested_extensions_),
      Focus::kDoCheck);
}

void AuthenticatorCommon::OnSignResponse(
    device::GetAssertionStatus status_code,
    base::Optional<std::vector<device::AuthenticatorGetAssertionResponse>>
        response_data,
    const device::FidoAuthenticator* authenticator) {
  DCHECK(!response_data || !response_data->empty());  // empty vector is invalid

  if (!request_) {
    // Either the callback was called immediately and |request_| has not yet
    // been assigned (this is a bug), or a navigation caused the request to be
    // canceled while a callback was enqueued.
    return;
  }

  switch (status_code) {
    case device::GetAssertionStatus::kUserConsentButCredentialNotRecognized:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kKeyNotRegistered,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::GetAssertionStatus::kAuthenticatorResponseInvalid:
      // The response from the authenticator was corrupted.
      CompleteGetAssertionRequest(
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::GetAssertionStatus::kUserConsentDenied:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kUserConsentDenied,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::GetAssertionStatus::kSoftPINBlock:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kSoftPINBlock,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::GetAssertionStatus::kHardPINBlock:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kHardPINBlock,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::GetAssertionStatus::kAuthenticatorRemovedDuringPINEntry:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kAuthenticatorRemovedDuringPINEntry,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::GetAssertionStatus::kAuthenticatorMissingResidentKeys:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kAuthenticatorMissingResidentKeys,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::GetAssertionStatus::kAuthenticatorMissingUserVerification:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kAuthenticatorMissingUserVerification,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::GetAssertionStatus::kWinNotAllowedError:
      SignalFailureToRequestDelegate(
          authenticator,
          AuthenticatorRequestClientDelegate::InterestingFailureReason::
              kWinUserCancelled,
          blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
      return;
    case device::GetAssertionStatus::kSuccess:
      DCHECK(response_data.has_value());
      DCHECK(authenticator);

      // Show an account picker for requests with empty allow lists.
      // Authenticators may omit the identifying information in the user entity
      // if only one credential matches, or if they have account selection UI
      // built-in. In that case, consider that credential pre-selected.
      if (empty_allow_list_ &&
          (response_data->size() > 1 ||
           (response_data->at(0).user_entity &&
            (response_data->at(0).user_entity->name ||
             response_data->at(0).user_entity->display_name)))) {
        std::vector<device::PublicKeyCredentialUserEntity> users_list;
        users_list.reserve(response_data->size());
        for (const auto& response : *response_data) {
          if (response.user_entity) {
            users_list.push_back(*response.user_entity);
          }
        }
        request_delegate_->SelectAccount(
            std::move(*response_data),
            base::BindOnce(&AuthenticatorCommon::OnAccountSelected,
                           weak_factory_.GetWeakPtr()));
      } else {
        OnAccountSelected(std::move(response_data->at(0)));
      }
      return;
  }
  NOTREACHED();
}

void AuthenticatorCommon::OnAccountSelected(
    device::AuthenticatorGetAssertionResponse response) {
  if (response.large_blob) {
    std::vector<uint8_t> blob = std::move(*response.large_blob);
    data_decoder_.GzipUncompress(
        blob, base::BindOnce(&AuthenticatorCommon::OnLargeBlobUncompressed,
                             weak_factory_.GetWeakPtr(), std::move(response)));
    return;
  }
  CompleteGetAssertionRequest(
      blink::mojom::AuthenticatorStatus::SUCCESS,
      CreateGetAssertionResponse(client_data_json_, std::move(response),
                                 app_id_, requested_extensions_));
  return;
}

void AuthenticatorCommon::SignalFailureToRequestDelegate(
    const ::device::FidoAuthenticator* authenticator,
    AuthenticatorRequestClientDelegate::InterestingFailureReason reason,
    blink::mojom::AuthenticatorStatus status) {
  error_awaiting_user_acknowledgement_ = status;

  // The request has failed, but the UI may delay resolution of the request
  // callback and cleanup of the FidoRequestHandler and its associated
  // discoveries and authenticators. Tell them to stop processing the request in
  // the meantime.
  request_->StopDiscoveries();
  request_->CancelActiveAuthenticators();

  // If WebAuthnUi is enabled, this error blocks until after receiving user
  // acknowledgement. Otherwise, the error is returned right away.
  if (request_delegate_->DoesBlockRequestOnFailure(reason)) {
    return;
  }
  CancelWithStatus(error_awaiting_user_acknowledgement_);
}  // namespace content

// TODO(crbug.com/814418): Add web tests to verify timeouts are
// indistinguishable from NOT_ALLOWED_ERROR cases.
void AuthenticatorCommon::OnTimeout() {
  DCHECK(request_delegate_);
  if (awaiting_attestation_response_) {
    awaiting_attestation_response_ = false;
  }

  SignalFailureToRequestDelegate(
      /*authenticator=*/nullptr,
      AuthenticatorRequestClientDelegate::InterestingFailureReason::kTimeout,
      blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR);
}

void AuthenticatorCommon::CancelWithStatus(
    blink::mojom::AuthenticatorStatus status) {
  DCHECK(!make_credential_response_callback_ ||
         !get_assertion_response_callback_);
  if (make_credential_response_callback_) {
    CompleteMakeCredentialRequest(status);
  } else if (get_assertion_response_callback_) {
    CompleteGetAssertionRequest(status);
  }
}

void AuthenticatorCommon::OnCancelFromUI() {
  CancelWithStatus(error_awaiting_user_acknowledgement_);
}

void AuthenticatorCommon::CompleteMakeCredentialRequest(
    blink::mojom::AuthenticatorStatus status,
    blink::mojom::MakeCredentialAuthenticatorResponsePtr response,
    Focus check_focus) {
  DCHECK(make_credential_response_callback_);
  if (check_focus != Focus::kDontCheck && !(request_delegate_ && IsFocused())) {
    std::move(make_credential_response_callback_)
        .Run(blink::mojom::AuthenticatorStatus::NOT_FOCUSED, nullptr);
  } else {
    std::move(make_credential_response_callback_)
        .Run(status, std::move(response));
  }

  Cleanup();
}

void AuthenticatorCommon::CompleteGetAssertionRequest(
    blink::mojom::AuthenticatorStatus status,
    blink::mojom::GetAssertionAuthenticatorResponsePtr response) {
  DCHECK(get_assertion_response_callback_);
  std::move(get_assertion_response_callback_).Run(status, std::move(response));
  Cleanup();
}

void AuthenticatorCommon::Cleanup() {
  if (awaiting_attestation_response_) {
    awaiting_attestation_response_ = false;
  }

  timer_->Stop();
  request_.reset();
  discovery_factory_.reset();
  discovery_factory_testing_override_ = nullptr;
  ctap_make_credential_request_.reset();
  make_credential_options_.reset();
  ctap_get_assertion_request_.reset();
  ctap_get_assertion_options_.reset();
  request_delegate_.reset();
  make_credential_response_callback_.Reset();
  get_assertion_response_callback_.Reset();
  client_data_json_.clear();
  app_id_.reset();
  caller_origin_ = url::Origin();
  relying_party_id_.clear();
  empty_allow_list_ = false;
  error_awaiting_user_acknowledgement_ =
      blink::mojom::AuthenticatorStatus::NOT_ALLOWED_ERROR;
  requested_extensions_.clear();
}

void AuthenticatorCommon::DisableUI() {
  disable_ui_ = true;
}

RenderFrameHost* AuthenticatorCommon::GetRenderFrameHost() const {
  RenderFrameHost* ret = RenderFrameHost::FromID(render_frame_host_id_);
  DCHECK(ret);
  return ret;
}

BrowserContext* AuthenticatorCommon::GetBrowserContext() const {
  return GetRenderFrameHost()->GetBrowserContext();
}

device::FidoDiscoveryFactory* AuthenticatorCommon::discovery_factory() {
  DCHECK(discovery_factory_);
  return discovery_factory_testing_override_
             ? discovery_factory_testing_override_
             : discovery_factory_.get();
}

void AuthenticatorCommon::InitDiscoveryFactory() {
  const bool is_u2f_api_request =
      WebAuthRequestSecurityChecker::OriginIsCryptoTokenExtension(
          caller_origin_);
  discovery_factory_ =
      MakeDiscoveryFactory(GetRenderFrameHost(), is_u2f_api_request);
  // TODO(martinkr): |discovery_factory_testing_override_| is a long-lived
  // VirtualFidoDeviceDiscovery so that tests can maintain and alter virtual
  // authenticator state in between requests. We should extract a longer-lived
  // configuration object from VirtualFidoDeviceDiscovery, so we can simply
  // stick a short-lived instance into |discovery_factory_| and eliminate
  // |discovery_factory_testing_override_|.
  discovery_factory_testing_override_ =
      AuthenticatorEnvironmentImpl::GetInstance()
          ->MaybeGetDiscoveryFactoryTestOverride();
}

}  // namespace content
