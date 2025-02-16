// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/components/connectivity_diagnostics/connectivity_diagnostics_ui.h"

#include <utility>

#include "chromeos/components/connectivity_diagnostics/url_constants.h"
#include "chromeos/components/network_ui/network_diagnostics_resource_provider.h"
#include "chromeos/components/network_ui/network_health_resource_provider.h"
#include "chromeos/grit/connectivity_diagnostics_resources.h"
#include "chromeos/grit/connectivity_diagnostics_resources_map.h"
#include "chromeos/services/network_health/public/mojom/network_diagnostics.mojom.h"
#include "chromeos/services/network_health/public/mojom/network_health.mojom.h"
#include "chromeos/strings/grit/chromeos_strings.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui_data_source.h"
#include "ui/resources/grit/webui_generated_resources.h"

namespace chromeos {

namespace {

// TODO(crbug/1051793): Replace with webui::SetUpWebUIDataSource() once it no
// longer requires a dependency on //chrome/browser.
void SetUpWebUIDataSource(content::WebUIDataSource* source,
                          base::span<const webui::ResourcePath> resources,
                          int default_resource) {
  source->AddResourcePaths(resources);
  source->SetDefaultResource(default_resource);
  source->AddResourcePath("test_loader.html", IDR_WEBUI_HTML_TEST_LOADER_HTML);
  source->AddResourcePath("test_loader.js", IDR_WEBUI_JS_TEST_LOADER_JS);
  source->AddResourcePath("test_loader_util.js",
                          IDR_WEBUI_JS_TEST_LOADER_UTIL_JS);
}

}  // namespace

ConnectivityDiagnosticsUI::ConnectivityDiagnosticsUI(
    content::WebUI* web_ui,
    BindNetworkDiagnosticsServiceCallback bind_network_diagnostics_callback,
    BindNetworkHealthServiceCallback bind_network_health_callback,
    SendFeedbackReportCallback send_feedback_report_callback)
    : ui::MojoWebUIController(web_ui, /*enable_chrome_send=*/true),
      bind_network_diagnostics_service_callback_(
          std::move(bind_network_diagnostics_callback)),
      bind_network_health_service_callback_(
          std::move(bind_network_health_callback)),
      send_feedback_report_callback_(std::move(send_feedback_report_callback)) {
  DCHECK(bind_network_diagnostics_service_callback_);
  DCHECK(bind_network_health_service_callback_);
  DCHECK(send_feedback_report_callback_);
  web_ui->RegisterMessageCallback(
      "sendFeedbackReport",
      base::BindRepeating(&ConnectivityDiagnosticsUI::SendFeedbackReportRequest,
                          weak_factory_.GetWeakPtr()));
  content::WebUIDataSource* source =
      content::WebUIDataSource::Create(kChromeUIConnectivityDiagnosticsHost);
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ScriptSrc,
      "script-src chrome://resources chrome://test 'self';");

  source->DisableTrustedTypesCSP();
  source->UseStringsJs();
  source->EnableReplaceI18nInJS();

  const auto resources = base::make_span(kConnectivityDiagnosticsResources,
                                         kConnectivityDiagnosticsResourcesSize);
  SetUpWebUIDataSource(source, resources,
                       IDR_CONNECTIVITY_DIAGNOSTICS_INDEX_HTML);
  source->AddLocalizedString("appTitle", IDS_CONNECTIVITY_DIAGNOSTICS_TITLE);
  source->AddLocalizedString("networkDevicesLabel",
                             IDS_CONNECTIVITY_DIAGNOSTICS_NETWORK_DEVICES);
  source->AddLocalizedString("diagnosticRoutinesLabel",
                             IDS_CONNECTIVITY_DIAGNOSTICS_DIAGNOSTIC_ROUTINES);
  source->AddLocalizedString("rerunRoutinesBtn",
                             IDS_CONNECTIVITY_DIAGNOSTICS_RERUN_ROUTINES);
  source->AddLocalizedString("closeBtn", IDS_CONNECTIVITY_DIAGNOSTICS_CLOSE);
  source->AddLocalizedString("sendFeedbackBtn",
                             IDS_CONNECTIVITY_DIAGNOSTICS_SEND_FEEDBACK);
  network_diagnostics::AddResources(source);
  network_health::AddResources(source);

  content::WebUIDataSource::Add(web_ui->GetWebContents()->GetBrowserContext(),
                                source);
}

ConnectivityDiagnosticsUI::~ConnectivityDiagnosticsUI() = default;

void ConnectivityDiagnosticsUI::BindInterface(
    mojo::PendingReceiver<
        network_diagnostics::mojom::NetworkDiagnosticsRoutines> receiver) {
  bind_network_diagnostics_service_callback_.Run(std::move(receiver));
}

void ConnectivityDiagnosticsUI::BindInterface(
    mojo::PendingReceiver<network_health::mojom::NetworkHealthService>
        receiver) {
  bind_network_health_service_callback_.Run(std::move(receiver));
}

void ConnectivityDiagnosticsUI::SendFeedbackReportRequest(
    const base::ListValue* value) {
  std::string extra_diagnostics = "";
  auto values = value->GetList();
  if (values.size() && values[0].is_string())
    extra_diagnostics = values[0].GetString();
  send_feedback_report_callback_.Run(extra_diagnostics);
}

WEB_UI_CONTROLLER_TYPE_IMPL(ConnectivityDiagnosticsUI)

}  // namespace chromeos
