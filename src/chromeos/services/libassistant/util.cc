// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/services/libassistant/util.h"

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/strings/stringprintf.h"
#include "base/system/sys_info.h"
#include "base/values.h"
#include "build/util/webkit_version.h"
#include "chromeos/assistant/internal/internal_constants.h"
#include "chromeos/assistant/internal/util_headers.h"
#include "chromeos/dbus/util/version_loader.h"
#include "chromeos/services/assistant/public/cpp/features.h"

using chromeos::assistant::shared::ClientInteraction;
using chromeos::assistant::shared::ClientOpResult;
using chromeos::assistant::shared::GetDeviceSettingsResult;
using chromeos::assistant::shared::Interaction;
using chromeos::assistant::shared::Protobuf;
using chromeos::assistant::shared::ProviderVerificationResult;
using chromeos::assistant::shared::ResponseCode;
using chromeos::assistant::shared::SettingInfo;
using chromeos::assistant::shared::VerifyProviderClientOpResult;

namespace chromeos {
namespace libassistant {

namespace {

using AppStatus = ::chromeos::assistant::AppStatus;

void CreateUserAgent(std::string* user_agent) {
  DCHECK(user_agent->empty());
  base::StringAppendF(user_agent,
                      "Mozilla/5.0 (X11; CrOS %s %s; %s) "
                      "AppleWebKit/%d.%d (KHTML, like Gecko)",
                      base::SysInfo::OperatingSystemArchitecture().c_str(),
                      base::SysInfo::OperatingSystemVersion().c_str(),
                      base::SysInfo::GetLsbReleaseBoard().c_str(),
                      WEBKIT_VERSION_MAJOR, WEBKIT_VERSION_MINOR);

  std::string arc_version = chromeos::version_loader::GetARCVersion();
  if (!arc_version.empty())
    base::StringAppendF(user_agent, " ARC/%s", arc_version.c_str());
}

// Get the root path for assistant files.
base::FilePath GetRootPath() {
  base::FilePath home_dir;
  CHECK(base::PathService::Get(base::DIR_HOME, &home_dir));
  // Ensures DIR_HOME is overridden after primary user sign-in.
  CHECK_NE(base::GetHomeDir(), home_dir);
  return home_dir;
}

ProviderVerificationResult::VerificationStatus GetProviderVerificationStatus(
    AppStatus status) {
  switch (status) {
    case AppStatus::kUnknown:
      return ProviderVerificationResult::UNKNOWN;
    case AppStatus::kAvailable:
      return ProviderVerificationResult::AVAILABLE;
    case AppStatus::kUnavailable:
      return ProviderVerificationResult::UNAVAILABLE;
    case AppStatus::kVersionMismatch:
      return ProviderVerificationResult::VERSION_MISMATCH;
    case AppStatus::kDisabled:
      return ProviderVerificationResult::DISABLED;
  }
}

SettingInfo ToSettingInfo(bool is_supported) {
  SettingInfo result;
  result.set_available(is_supported);
  result.set_setting_status(is_supported
                                ? SettingInfo::AVAILABLE_AND_MODIFY_SUPPORTED
                                : SettingInfo::UNAVAILABLE);
  return result;
}

// Helper class used for constructing V1 interaction proto messages.
class V1InteractionBuilder {
 public:
  V1InteractionBuilder() = default;
  V1InteractionBuilder(V1InteractionBuilder&) = delete;
  V1InteractionBuilder& operator=(V1InteractionBuilder&) = delete;
  ~V1InteractionBuilder() = default;

  V1InteractionBuilder& SetInResponseTo(int interaction_id) {
    interaction_.set_in_response_to(interaction_id);
    return *this;
  }

  V1InteractionBuilder& AddResult(
      const std::string& key,
      const google::protobuf::MessageLite& result_proto) {
    auto* result = client_op_result()->mutable_results()->add_result();
    result->set_key(key);
    result->mutable_value()->set_protobuf_type(result_proto.GetTypeName());
    result->mutable_value()->set_protobuf_data(
        result_proto.SerializeAsString());
    return *this;
  }

  V1InteractionBuilder& SetStatusCode(ResponseCode::Status status_code) {
    ResponseCode* response_code = client_op_result()->mutable_response_code();
    response_code->set_status_code(status_code);
    return *this;
  }

  // Set the status code to |OK| (if true) or |NOT_FOUND| (if false).
  V1InteractionBuilder& SetStatusCodeFromEntityFound(bool found) {
    SetStatusCode(found ? ResponseCode::OK : ResponseCode::NOT_FOUND);
    return *this;
  }

  V1InteractionBuilder& SetClientInputName(const std::string& name) {
    auto* client_input = client_interaction()->mutable_client_input();
    client_input->set_client_input_name(name);
    return *this;
  }

  V1InteractionBuilder& AddClientInputParams(
      const std::string& key,
      const google::protobuf::MessageLite& params_proto) {
    auto* client_input = client_interaction()->mutable_client_input();
    Protobuf& value = (*client_input->mutable_params())[key];
    value.set_protobuf_type(params_proto.GetTypeName());
    value.set_protobuf_data(params_proto.SerializeAsString());
    return *this;
  }

  std::string SerializeAsString() { return interaction_.SerializeAsString(); }

 private:
  ClientInteraction* client_interaction() {
    return interaction_.mutable_from_client();
  }

  ClientOpResult* client_op_result() {
    return client_interaction()->mutable_client_op_result();
  }

  Interaction interaction_;
};

bool ShouldPutLogsInHomeDirectory() {
  // Redirects libassistant logging to /var/log/chrome/. This is mainly used to
  // help collect logs when running tests.
  constexpr char kRedirectLibassistantLogging[] =
      "redirect-libassistant-logging";

  const bool redirect_logging =
      base::CommandLine::ForCurrentProcess()->HasSwitch(
          kRedirectLibassistantLogging);
  return !redirect_logging;
}

bool ShouldLogToFile() {
  // Redirects libassistant logging to stdout. This is mainly used to help test
  // locally.
  constexpr char kDisableLibAssistantLogfile[] = "disable-libassistant-logfile";

  const bool disable_logfile =
      base::CommandLine::ForCurrentProcess()->HasSwitch(
          kDisableLibAssistantLogfile);
  return !disable_logfile;
}

}  // namespace

base::FilePath GetBaseAssistantDir() {
  return GetRootPath().Append(FILE_PATH_LITERAL("google-assistant-library"));
}

std::string CreateLibAssistantConfig(
    base::Optional<std::string> s3_server_uri_override,
    base::Optional<std::string> device_id_override) {
  using Value = base::Value;
  using Type = base::Value::Type;

  Value config(Type::DICTIONARY);

  Value device(Type::DICTIONARY);
  device.SetKey("board_name", Value(base::SysInfo::GetLsbReleaseBoard()));
  device.SetKey("board_revision", Value("1"));
  device.SetKey("embedder_build_info",
                Value(chromeos::version_loader::GetVersion(
                    chromeos::version_loader::VERSION_FULL)));
  device.SetKey("model_id", Value(assistant::kModelId));
  device.SetKey("model_revision", Value(1));
  config.SetKey("device", std::move(device));

  Value discovery(Type::DICTIONARY);
  discovery.SetKey("enable_mdns", Value(false));
  config.SetKey("discovery", std::move(discovery));

  Value internal(Type::DICTIONARY);
  internal.SetKey("surface_type", Value("OPA_CROS"));

  std::string user_agent;
  CreateUserAgent(&user_agent);
  internal.SetKey("user_agent", Value(user_agent));

  // Prevent LibAssistant from automatically playing ready message TTS during
  // the startup sequence when the version of LibAssistant has been upgraded.
  internal.SetKey("override_ready_message", Value(true));

  // Set DeviceProperties.visibility to Visibility::PRIVATE.
  // See //libassistant/shared/proto/device_properties.proto.
  internal.SetKey("visibility", Value("PRIVATE"));

  if (ShouldLogToFile()) {
    Value logging(Type::DICTIONARY);
    std::string log_dir("/var/log/chrome/");
    if (ShouldPutLogsInHomeDirectory()) {
      base::FilePath log_path =
          GetBaseAssistantDir().Append(FILE_PATH_LITERAL("log"));
      CHECK(base::CreateDirectory(log_path));
      log_dir = log_path.value();
    }
    logging.SetKey("directory", Value(log_dir));
    // Maximum disk space consumed by all log files. There are 5 rotating log
    // files on disk.
    logging.SetKey("max_size_kb", Value(3 * 1024));
    // Empty "output_type" disables logging to stderr.
    logging.SetKey("output_type", Value(Type::LIST));
    config.SetKey("logging", std::move(logging));
  } else {
    // Print logs to console if running in desktop mode.
    internal.SetKey("disable_log_files", Value(true));
  }

  // Enable logging.
  internal.SetBoolKey("enable_logging", true);

  // This only enables logging to local disk combined with the flag above. When
  // user choose to file a Feedback report, user can examine the log and choose
  // to upload the log with the report or not.
  internal.SetBoolKey("logging_opt_in", true);

  // Allows libassistant to automatically toggle signed-out mode depending on
  // whether it has auth_tokens.
  internal.SetBoolKey("enable_signed_out_mode", true);

  config.SetKey("internal", std::move(internal));

  Value audio_input(Type::DICTIONARY);
  // Skip sending speaker ID selection info to disable user verification.
  audio_input.SetKey("should_send_speaker_id_selection_info", Value(false));

  Value sources(Type::LIST);
  Value dict(Type::DICTIONARY);
  dict.SetKey("enable_eraser",
              Value(assistant::features::IsAudioEraserEnabled()));
  dict.SetKey("enable_eraser_toggling",
              Value(assistant::features::IsAudioEraserEnabled()));
  sources.Append(std::move(dict));
  audio_input.SetKey("sources", std::move(sources));

  config.SetKey("audio_input", std::move(audio_input));

  if (assistant::features::IsLibAssistantBetaBackendEnabled())
    config.SetStringPath("internal.backend_type", "BETA_DOGFOOD");

  // Use http unless we're using the fake s3 server, which requires grpc.
  if (s3_server_uri_override)
    config.SetStringPath("internal.transport_type", "GRPC");
  else
    config.SetStringPath("internal.transport_type", "HTTP");

  if (device_id_override)
    config.SetStringPath("internal.cast_device_id", device_id_override.value());

  config.SetBoolPath("internal.enable_on_device_assistant_tts_as_text", true);

  // Finally add in the server uri override.
  if (s3_server_uri_override) {
    config.SetStringPath("testing.s3_grpc_server_uri",
                         s3_server_uri_override.value());
  }

  std::string json;
  base::JSONWriter::Write(config, &json);
  return json;
}

std::string CreateVerifyProviderResponseInteraction(
    const int interaction_id,
    const std::vector<libassistant::mojom::AndroidAppInfoPtr>& apps_info) {
  // Construct verify provider result proto.
  VerifyProviderClientOpResult result_proto;
  bool any_provider_available = false;
  for (const auto& android_app_info : apps_info) {
    auto* provider_status = result_proto.add_provider_status();
    provider_status->set_status(
        GetProviderVerificationStatus(android_app_info->status));
    auto* app_info =
        provider_status->mutable_provider_info()->mutable_android_app_info();
    app_info->set_package_name(android_app_info->package_name);
    app_info->set_app_version(android_app_info->version);
    app_info->set_localized_app_name(android_app_info->localized_app_name);
    app_info->set_android_intent(android_app_info->intent);

    if (android_app_info->status == AppStatus::kAvailable)
      any_provider_available = true;
  }

  // Construct response interaction.
  return V1InteractionBuilder()
      .SetInResponseTo(interaction_id)
      .SetStatusCodeFromEntityFound(any_provider_available)
      .AddResult(assistant::kResultKeyVerifyProvider, result_proto)
      .SerializeAsString();
}

std::string CreateGetDeviceSettingInteraction(
    int interaction_id,
    const std::vector<libassistant::mojom::DeviceSettingPtr>& device_settings) {
  GetDeviceSettingsResult result_proto;
  for (const auto& setting : device_settings) {
    (*result_proto.mutable_settings_info())[setting->setting_id] =
        ToSettingInfo(setting->is_supported);
  }

  // Construct response interaction.
  return V1InteractionBuilder()
      .SetInResponseTo(interaction_id)
      .SetStatusCode(ResponseCode::OK)
      .AddResult(/*key=*/assistant::kResultKeyGetDeviceSettings, result_proto)
      .SerializeAsString();
}

}  // namespace libassistant
}  // namespace chromeos
