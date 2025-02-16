// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PAGE_INFO_PAGE_INFO_DELEGATE_H_
#define COMPONENTS_PAGE_INFO_PAGE_INFO_DELEGATE_H_

#include <string>

#include "build/build_config.h"
#include "components/content_settings/browser/page_specific_content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/permissions/permission_result.h"
#include "components/permissions/permission_uma_util.h"
#include "components/safe_browsing/buildflags.h"
#include "components/safe_browsing/core/password_protection/metrics_util.h"
#include "components/security_state/core/security_state.h"

namespace permissions {
class ChooserContextBase;
class PermissionDecisionAutoBlocker;
}  // namespace permissions

namespace safe_browsing {
class PasswordProtectionService;
}  // namespace safe_browsing

namespace ui {
class Event;
}  // namespace ui

class HostContentSettingsMap;
class StatefulSSLHostStateDelegate;

// PageInfoDelegate allows an embedder to customize PageInfo logic.
class PageInfoDelegate {
 public:
  virtual ~PageInfoDelegate() = default;

  // Return the |ChooserContextBase| corresponding to the  content settings
  // type, |type|. Returns a nullptr for content settings for which there's no
  // ChooserContextBase.
  virtual permissions::ChooserContextBase* GetChooserContext(
      ContentSettingsType type) = 0;

#if BUILDFLAG(FULL_SAFE_BROWSING)
  // Helper methods requiring access to PasswordProtectionService.
  virtual safe_browsing::PasswordProtectionService*
  GetPasswordProtectionService() const = 0;
  virtual void OnUserActionOnPasswordUi(
      safe_browsing::WarningAction action) = 0;
  virtual std::u16string GetWarningDetailText() = 0;
#endif
  // Get permission status for the permission associated with ContentSetting of
  // type |type|.
  virtual permissions::PermissionResult GetPermissionStatus(
      ContentSettingsType type,
      const GURL& site_url) = 0;
#if !defined(OS_ANDROID)
  // Creates an InfoBarService and an InfoBarDelegate using it, if possible.
  // Returns true if an InfoBarDelegate was created, false otherwise.
  virtual bool CreateInfoBarDelegate() = 0;

  virtual void ShowSiteSettings(const GURL& site_url) = 0;
  virtual void OpenCookiesDialog() = 0;
  virtual void OpenCertificateDialog(net::X509Certificate* certificate) = 0;
  virtual void OpenConnectionHelpCenterPage(const ui::Event& event) = 0;
  virtual void OpenSafetyTipHelpCenterPage() = 0;
#endif
  virtual permissions::PermissionDecisionAutoBlocker*
  GetPermissionDecisionAutoblocker() = 0;

  // Service for managing SSL error page bypasses. Used to revoke bypass
  // decisions by users.
  virtual StatefulSSLHostStateDelegate* GetStatefulSSLHostStateDelegate() = 0;

  // The |HostContentSettingsMap| is the service that provides and manages
  // content settings (aka. site permissions).
  virtual HostContentSettingsMap* GetContentSettings() = 0;

  // The subresource filter service determines whether ads should be blocked on
  // the site and relevant permission prompts should be shown respectively.
  virtual bool IsSubresourceFilterActivated(const GURL& site_url) = 0;

  virtual std::unique_ptr<
      content_settings::PageSpecificContentSettings::Delegate>
  GetPageSpecificContentSettingsDelegate() = 0;
  virtual bool IsContentDisplayedInVrHeadset() = 0;
  virtual security_state::SecurityLevel GetSecurityLevel() = 0;
  virtual security_state::VisibleSecurityState GetVisibleSecurityState() = 0;
#if defined(OS_ANDROID)
  // Gets the name of the embedder.
  virtual const std::u16string GetClientApplicationName() = 0;
#endif
};

#endif  // COMPONENTS_PAGE_INFO_PAGE_INFO_DELEGATE_H_
