// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/version_ui/version_ui_constants.h"

#include "build/chromeos_buildflags.h"

namespace version_ui {

// Resource paths.
const char kAboutVersionCSS[] = "about_version.css";
#if defined(OS_IOS) || defined(OS_ANDROID)
const char kAboutVersionMobileCSS[] = "about_version_mobile.css";
#endif
const char kVersionJS[] = "about_version.js";

// Message handlers.
const char kRequestVersionInfo[] = "requestVersionInfo";
const char kRequestVariationInfo[] = "requestVariationInfo";
const char kRequestPathInfo[] = "requestPathInfo";

// Named keys used in message handler responses.
const char kKeyVariationsList[] = "variationsList";
const char kKeyVariationsCmd[] = "variationsCmd";
const char kKeyExecPath[] = "execPath";
const char kKeyProfilePath[] = "profilePath";

// Strings.
const char kApplicationLabel[] = "application_label";
#if BUILDFLAG(IS_CHROMEOS_ASH)
const char kARC[] = "arc_label";
#endif
const char kCL[] = "cl";
const char kCommandLine[] = "command_line";
const char kCommandLineName[] = "command_line_name";
const char kCompany[] = "company";
#if defined(OS_IOS)
const char kCompiler[] = "compiler";
#endif
#if defined(OS_WIN)
const char kUpdateCohortName[] = "update_cohort_name";
#endif
const char kCopyright[] = "copyright";
#if BUILDFLAG(IS_CHROMEOS_ASH)
const char kCustomizationId[] = "customization_id";
#endif
#if !defined(OS_IOS)
const char kExecutablePath[] = "executable_path";
const char kExecutablePathName[] = "executable_path_name";
#endif
#if BUILDFLAG(IS_CHROMEOS_ASH)
const char kFirmwareVersion[] = "firmware_version";
#endif
#if !defined(OS_IOS)
const char kJSEngine[] = "js_engine";
const char kJSVersion[] = "js_version";
#endif
const char kLogoAltText[] = "logo_alt_text";
const char kOfficial[] = "official";
#if !BUILDFLAG(IS_CHROMEOS_ASH)
const char kOSName[] = "os_name";
const char kOSType[] = "os_type";
#endif
#if defined(OS_ANDROID)
const char kOSVersion[] = "os_version";
const char kGmsName[] = "gms_name";
const char kGmsVersion[] = "gms_version";
#endif
#if BUILDFLAG(IS_CHROMEOS_ASH)
const char kPlatform[] = "platform";
#endif
#if !defined(OS_IOS)
const char kProfilePath[] = "profile_path";
const char kProfilePathName[] = "profile_path_name";
#endif
const char kRevision[] = "revision";
const char kSanitizer[] = "sanitizer";
const char kTitle[] = "title";
const char kUserAgent[] = "useragent";
const char kUserAgentName[] = "user_agent_name";
const char kVariationsCmdName[] = "variations_cmd_name";
const char kVariationsName[] = "variations_name";
const char kVersion[] = "version";
const char kVersionModifier[] = "version_modifier";
const char kVersionProcessorVariation[] = "version_processor_variation";

}  // namespace version_ui
