// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_CONSTANTS_ASH_SWITCHES_H_
#define ASH_CONSTANTS_ASH_SWITCHES_H_

#include "base/component_export.h"

namespace chromeos {
namespace switches {

// Prefer adding Features over switches. Features go in ash_features.h.
//
// Note: If you add a switch, consider if it needs to be copied to a subsequent
// command line if the process executes a new copy of itself.  (For example,
// see chromeos::LoginUtil::GetOffTheRecordCommandLine().)

// Please keep alphabetized.
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kAggressiveCacheDiscardThreshold[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kAllowFailedPolicyFetchForTest[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kAllowRAInDevMode[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kAppAutoLaunched[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kAppOemManifestFile[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kArcAvailability[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kArcAvailable[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kArcDataCleanupOnStart[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kArcDisableAppSync[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kArcDisableDownloadProvider[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kArcDisableGmsCoreCache[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kArcDisableLocaleSync[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kArcDisableMediaStoreMaintenance[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kArcDisableSystemDefaultApps[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kArcDisablePlayAutoInstall[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kArcEnableNativeBridge64BitSupportExperiment[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kArcForceShowOptInUi[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kArcGeneratePlayAutoInstall[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kArcInstallEventChromeLogForTests[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kArcPackagesCacheMode[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kArcPlayStoreAutoUpdate[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kArcScale[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kArcStartMode[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kArcTosHostForTests[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kArcVmUreadaheadMode[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kCellularFirst[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kChildWallpaperLarge[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kChildWallpaperSmall[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kCrosRegion[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kCrosRegionsMode[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kCrosRegionsModeHide[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kCrosRegionsModeOverride[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kCryptohomeUseAuthSession[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kDefaultWallpaperIsOem[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kDefaultWallpaperLarge[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kDefaultWallpaperSmall[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kDerelictDetectionTimeout[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kDerelictIdleTimeout[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kDisableArcDataWipe[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kDisableArcOptInVerification[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kDisableDemoMode[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kDisableDeviceDisabling[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kDisableEncryptionMigration[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kDisableFineGrainedTimeZoneDetection[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kDisableGaiaServices[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kDisableHIDDetectionOnOOBEForTesting[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kDisableLoginAnimations[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kDisableMachineCertRequest[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kDisableOOBEChromeVoxHintTimerForTesting[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kEnableOOBEChromeVoxHintForDevMode[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kDisablePerUserTimezone[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kDisableRollbackOption[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kDisableSigninFrameClientCerts[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kDisableVolumeAdjustSound[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kEnableArc[];
COMPONENT_EXPORT(ASH_CONSTANTS)
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kEnableArcVm[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kEnableArcVmRtVcpu[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kEnableCastReceiver[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kEnableConsumerKiosk[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kEnableTabletFormFactor[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kEnableEncryptionMigration[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kEnableExtensionAssetsSharing[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kEnableHoudini[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kEnableHoudini64[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kEnableNdkTranslation[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kEnableNdkTranslation64[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kEnableRequisitionEdits[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kEnableRequestTabletSite[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kEnableTouchCalibrationSetting[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kEnableTouchpadThreeFingerClick[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kEnterpriseDisableArc[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kEnterpriseEnableForcedReEnrollment[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kEnterpriseEnableInitialEnrollment[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kEnterpriseEnablePsm[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kEnterpriseEnableZeroTouchEnrollment[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kEnterpriseEnrollmentInitialModulus[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kEnterpriseEnrollmentModulusLimit[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kExternalMetricsCollectionInterval[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kExtraWebAppsDir[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kFirstExecAfterBoot[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kFakeDriveFsLauncherChrootPath[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kFakeDriveFsLauncherSocketPath[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kFakeArcRecommendedAppsForTesting[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kFingerprintSensorLocation[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kForceDevToolsAvailable[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kForceFirstRunUI[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kForceHappinessTrackingSystem[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kForceHWIDCheckFailureForTest[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kForceLoginManagerInTests[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kForceSystemCompositorMode[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kFormFactor[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kGuestSession[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kGuestWallpaperLarge[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kGuestWallpaperSmall[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kHasChromeOSKeyboard[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kHomedir[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kIgnoreArcVmDevConf[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kIgnoreUserProfileMappingForTests[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kInstallLogFastUploadForTests[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kKernelnextRestrictVMs[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kLacrosChromeAdditionalArgs[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kLacrosChromeAdditionalEnv[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kLacrosChromePath[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kLacrosMojoSocketForTesting[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kLoginManager[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kLoginProfile[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kLoginUser[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kMarketingOptInUrl[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kNaturalScrollDefault[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kNoteTakingAppIds[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kOobeEulaUrlForTests[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kOobeForceTabletFirstRun[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kOobeGuestSession[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kOobeSkipPostLogin[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kOobeSkipToLogin[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kOobeTimerInterval[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kOobeTimezoneOverrideForTests[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kPublicAccountsSamlAclUrl[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kDisableArcCpuRestriction[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kProfileRequiresPolicy[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kRegulatoryLabelDir[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kRlzPingDelay[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kSafeMode[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kSamlPasswordChangeUrl[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kOfflineSignInTimeLimitInSecondsOverrideForTesting[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kSamlLockScreenReauthenticationEnabledOverrideForTesting[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kShelfHoverPreviews[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kShelfHotseat[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kShowLoginDevOverlay[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kShowOobeDevOverlay[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kEnableOobeTestAPI[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kOobeScreenshotDirectory[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kSkipForceOnlineSignInForTesting[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kTelemetryExtensionDirectory[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kTestEncryptionMigrationUI[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kTestWallpaperServer[];
COMPONENT_EXPORT(ASH_CONSTANTS) extern const char kTetherStub[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kTetherHostScansIgnoreWiredConnections[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kWaitForInitialPolicyFetchForTest[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kUnfilteredBluetoothDevices[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kUpdateRequiredAueForTest[];
COMPONENT_EXPORT(ASH_CONSTANTS)
extern const char kUseFakeMLServiceForTest[];

////////////////////////////////////////////////////////////////////////////////

// Returns true if memory pressure handling is enabled.
COMPONENT_EXPORT(ASH_CONSTANTS) bool MemoryPressureHandlingEnabled();

// Returns true if flags are set indicating that stored user keys are being
// converted to GAIA IDs.
COMPONENT_EXPORT(ASH_CONSTANTS) bool IsGaiaIdMigrationStarted();

// Returns true if flag if AuthSession should be used to communicate with
// cryptohomed instead of explicitly authorizing each operation.
COMPONENT_EXPORT(ASH_CONSTANTS) bool IsAuthSessionCryptohomeEnabled();

// Returns true if this is a Cellular First device.
COMPONENT_EXPORT(ASH_CONSTANTS) bool IsCellularFirstDevice();

// Returns true if client certificate authentication for the sign-in frame on
// the Chrome OS sign-in screen is enabled.
COMPONENT_EXPORT(ASH_CONSTANTS) bool IsSigninFrameClientCertsEnabled();

// Returns true if we should show window previews when hovering over an app
// on the shelf.
COMPONENT_EXPORT(ASH_CONSTANTS) bool ShouldShowShelfHoverPreviews();

// Returns true if the Chromebook should ignore its wired connections when
// deciding whether to run scans for tethering hosts. Should be used only for
// testing.
COMPONENT_EXPORT(ASH_CONSTANTS)
bool ShouldTetherHostScansIgnoreWiredConnections();

// Returns true if we should skip all other OOBE pages after user login.
COMPONENT_EXPORT(ASH_CONSTANTS) bool ShouldSkipOobePostLogin();

// Returns true if the device is of tablet form factor.
COMPONENT_EXPORT(ASH_CONSTANTS) bool IsTabletFormFactor();

// Returns true if GAIA services has been disabled.
COMPONENT_EXPORT(ASH_CONSTANTS) bool IsGaiaServicesDisabled();

// Returns true if |kDisableArcCpuRestriction| is true.
COMPONENT_EXPORT(ASH_CONSTANTS) bool IsArcCpuRestrictionDisabled();

// Returns true if all Bluetooth devices in UI (System Tray/Settings Page.)
COMPONENT_EXPORT(ASH_CONSTANTS) bool IsUnfilteredBluetoothDevicesEnabled();

// Returns whether the first user run OOBE flow (sequence of screens shown to
// the user on their first login) should show tablet mode screens when the
// device is not in tablet mode.
COMPONENT_EXPORT(ASH_CONSTANTS) bool ShouldOobeUseTabletModeFirstRun();

// Returns true if device policy DeviceMinimumVersion should assume that
// Auto Update Expiration is reached. This should only be used for testing.
COMPONENT_EXPORT(ASH_CONSTANTS)
bool IsAueReachedForUpdateRequiredForTest();

// Returns true if the OOBE ChromeVox hint idle detection is disabled for
// testing.
COMPONENT_EXPORT(ASH_CONSTANTS)
bool IsOOBEChromeVoxHintTimerDisabledForTesting();

// Returns true if the OOBE ChromeVox hint is enabled for dev mode.
COMPONENT_EXPORT(ASH_CONSTANTS)
bool IsOOBEChromeVoxHintEnabledForDevMode();

// Returns true if the OEM Device Requisition can be configured.
COMPONENT_EXPORT(ASH_CONSTANTS)
bool IsDeviceRequisitionConfigurable();

}  // namespace switches
}  // namespace chromeos

// TODO(https://crbug.com/1164001): remove after //chrome/browser/chromeos
// source migration is finished.
namespace ash {
namespace switches {
using namespace ::chromeos::switches;
}
}  // namespace ash

#endif  // ASH_CONSTANTS_ASH_SWITCHES_H_
