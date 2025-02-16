// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/media/session/media_session_service_impl.h"

#include <memory>

#include "base/command_line.h"
#include "base/test/scoped_feature_list.h"
#include "build/build_config.h"
#include "content/browser/media/session/media_session_impl.h"
#include "content/browser/media/session/media_session_player_observer.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/content_switches.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/content_browser_test.h"
#include "content/public/test/content_browser_test_utils.h"
#include "content/shell/browser/shell.h"
#include "media/base/media_content_type.h"
#include "media/base/media_switches.h"
#include "services/media_session/public/cpp/test/mock_media_session.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace content {

namespace {

class NavigationWatchingWebContentsObserver : public WebContentsObserver {
 public:
  explicit NavigationWatchingWebContentsObserver(
      WebContents* contents,
      base::OnceClosure closure_on_navigate)
      : WebContentsObserver(contents),
        closure_on_navigate_(std::move(closure_on_navigate)) {}

  void DidFinishNavigation(NavigationHandle* navigation_handle) override {
    std::move(closure_on_navigate_).Run();
  }

 private:
  base::OnceClosure closure_on_navigate_;
};

class MockMediaSessionPlayerObserver : public MediaSessionPlayerObserver {
 public:
  explicit MockMediaSessionPlayerObserver(RenderFrameHost* rfh)
      : render_frame_host_(rfh) {}

  ~MockMediaSessionPlayerObserver() override = default;

  void OnSuspend(int player_id) override {}
  void OnResume(int player_id) override {}
  void OnSeekForward(int player_id, base::TimeDelta seek_time) override {}
  void OnSeekBackward(int player_id, base::TimeDelta seek_time) override {}
  void OnSeekTo(int player_id, base::TimeDelta seek_time) override {}
  void OnSetVolumeMultiplier(int player_id, double volume_multiplier) override {
  }
  void OnEnterPictureInPicture(int player_id) override {}
  void OnExitPictureInPicture(int player_id) override {}
  void OnSetAudioSinkId(int player_id,
                        const std::string& raw_device_id) override {}

  base::Optional<media_session::MediaPosition> GetPosition(
      int player_id) const override {
    return base::nullopt;
  }

  bool IsPictureInPictureAvailable(int player_id) const override {
    return false;
  }

  bool HasAudio(int player_id) const override { return true; }
  bool HasVideo(int player_id) const override { return false; }

  std::string GetAudioOutputSinkId(int player_id) const override { return ""; }

  bool SupportsAudioOutputDeviceSwitching(int player_id) const override {
    return false;
  }

  RenderFrameHost* render_frame_host() const override {
    return render_frame_host_;
  }

 private:
  RenderFrameHost* render_frame_host_;
};

void NavigateToURLAndWaitForFinish(Shell* window, const GURL& url) {
  base::RunLoop run_loop;
  NavigationWatchingWebContentsObserver observer(window->web_contents(),
                                                 run_loop.QuitClosure());

  EXPECT_TRUE(NavigateToURL(window, url));
  run_loop.Run();
}

char kSetUpMediaSessionScript[] =
    "navigator.mediaSession.playbackState = \"playing\";\n"
    "navigator.mediaSession.metadata = new MediaMetadata({ title: \"foo\" });\n"
    "navigator.mediaSession.setActionHandler(\"seekforward\", _ => {});";

char kSetUpWebRTCMediaSessionScript[] =
    "navigator.mediaSession.playbackState = \"playing\";\n"
    "navigator.mediaSession.metadata = new MediaMetadata({ title: \"foo\" });\n"
    "navigator.mediaSession.setMicrophoneActive(true);\n"
    "navigator.mediaSession.setCameraActive(true);\n"
    "navigator.mediaSession.setActionHandler(\"togglemicrophone\", _ => {});\n"
    "navigator.mediaSession.setActionHandler(\"togglecamera\", _ => {});\n"
    "navigator.mediaSession.setActionHandler(\"hangup\", _ => {});";

const int kPlayerId = 0;

}  // anonymous namespace

class MediaSessionServiceImplBrowserTest : public ContentBrowserTest {
 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    ContentBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitchASCII(switches::kEnableBlinkFeatures,
                                    "MediaSession");
  }

  void EnsurePlayer() {
    if (player_)
      return;

    player_ = std::make_unique<MockMediaSessionPlayerObserver>(
        shell()->web_contents()->GetMainFrame());

    MediaSessionImpl::Get(shell()->web_contents())
        ->AddPlayer(player_.get(), kPlayerId,
                    media::MediaContentType::Persistent);
  }

  MediaSessionImpl* GetSession() {
    return MediaSessionImpl::Get(shell()->web_contents());
  }

  MediaSessionServiceImpl* GetService() {
    RenderFrameHost* main_frame = shell()->web_contents()->GetMainFrame();
    const auto main_frame_id = main_frame->GetGlobalFrameRoutingId();
    if (GetSession()->services_.count(main_frame_id))
      return GetSession()->services_[main_frame_id];

    return nullptr;
  }

  bool ExecuteScriptToSetUpMediaSessionSync() {
    bool result = ExecuteScript(shell(), kSetUpMediaSessionScript);
    media_session::test::MockMediaSessionMojoObserver observer(*GetSession());

    std::set<media_session::mojom::MediaSessionAction> expected_actions;
    expected_actions.insert(media_session::mojom::MediaSessionAction::kPlay);
    expected_actions.insert(media_session::mojom::MediaSessionAction::kPause);
    expected_actions.insert(media_session::mojom::MediaSessionAction::kStop);
    expected_actions.insert(media_session::mojom::MediaSessionAction::kSeekTo);
    expected_actions.insert(media_session::mojom::MediaSessionAction::kScrubTo);
    expected_actions.insert(
        media_session::mojom::MediaSessionAction::kSeekForward);

    observer.WaitForExpectedActions(expected_actions);
    return result;
  }

 private:
  std::unique_ptr<MockMediaSessionPlayerObserver> player_;
};

#if defined(LEAK_SANITIZER)
// TODO(crbug.com/850870) Plug the leaks.
#define MAYBE_CrashMessageOnUnload DISABLED_CrashMessageOnUnload
#else
#define MAYBE_CrashMessageOnUnload CrashMessageOnUnload
#endif
// Two windows from the same BrowserContext.
IN_PROC_BROWSER_TEST_F(MediaSessionServiceImplBrowserTest,
                       MAYBE_CrashMessageOnUnload) {
  EXPECT_TRUE(
      NavigateToURL(shell(), GetTestUrl("media/session", "embedder.html")));
  // Navigate to a chrome:// URL to avoid render process re-use.
  EXPECT_TRUE(NavigateToURL(shell(), GURL("chrome://gpu")));
  // Should not crash.
}

// Tests for checking if the media session service members are correctly reset
// when navigating. Due to the mojo services have different message queues, it's
// hard to wait for the messages to arrive. Temporarily, the tests are using
// observers to wait for the message to be processed on the MediaSessionObserver
// side.

#if defined(LEAK_SANITIZER)
// TODO(crbug.com/850870) Plug the leaks.
#define MAYBE_ResetServiceWhenNavigatingAway \
  DISABLED_ResetServiceWhenNavigatingAway
#elif defined(OS_CHROMEOS) || defined(OS_LINUX) || defined(OS_MAC) || \
    defined(OS_ANDROID)
// crbug.com/927234.
#define MAYBE_ResetServiceWhenNavigatingAway \
  DISABLED_ResetServiceWhenNavigatingAway
#else
#define MAYBE_ResetServiceWhenNavigatingAway ResetServiceWhenNavigatingAway
#endif
IN_PROC_BROWSER_TEST_F(MediaSessionServiceImplBrowserTest,
                       MAYBE_ResetServiceWhenNavigatingAway) {
  EXPECT_TRUE(NavigateToURL(shell(), GetTestUrl(".", "title1.html")));
  EnsurePlayer();

  EXPECT_TRUE(ExecuteScriptToSetUpMediaSessionSync());

  EXPECT_EQ(blink::mojom::MediaSessionPlaybackState::PLAYING,
            GetService()->playback_state());
  EXPECT_TRUE(GetService()->metadata());
  EXPECT_EQ(1u, GetService()->actions().size());

  // Start a non-same-page navigation and check the playback state, metadata,
  // actions are reset.
  NavigateToURLAndWaitForFinish(shell(), GetTestUrl(".", "title2.html"));

  EXPECT_EQ(blink::mojom::MediaSessionPlaybackState::NONE,
            GetService()->playback_state());
  EXPECT_FALSE(GetService()->metadata());
  EXPECT_EQ(0u, GetService()->actions().size());
}

#if defined(LEAK_SANITIZER)
// TODO(crbug.com/850870) Plug the leaks.
#define MAYBE_DontResetServiceForSameDocumentNavigation \
  DISABLED_DontResetServiceForSameDocumentNavigation
#else
// crbug.com/927234.
#define MAYBE_DontResetServiceForSameDocumentNavigation \
  DISABLED_DontResetServiceForSameDocumentNavigation
#endif
IN_PROC_BROWSER_TEST_F(MediaSessionServiceImplBrowserTest,
                       MAYBE_DontResetServiceForSameDocumentNavigation) {
  EXPECT_TRUE(NavigateToURL(shell(), GetTestUrl(".", "title1.html")));
  EnsurePlayer();

  EXPECT_TRUE(ExecuteScriptToSetUpMediaSessionSync());

  // Start a fragment navigation and check the playback state, metadata,
  // actions are not reset.
  GURL fragment_change_url = GetTestUrl(".", "title1.html");
  fragment_change_url = GURL(fragment_change_url.spec() + "#some-anchor");
  NavigateToURLAndWaitForFinish(shell(), fragment_change_url);

  EXPECT_EQ(blink::mojom::MediaSessionPlaybackState::PLAYING,
            GetService()->playback_state());
  EXPECT_TRUE(GetService()->metadata());
  EXPECT_EQ(1u, GetService()->actions().size());
}

// Browser tests with the MediaSessionWebRTC feature enabled.
// TODO(steimel): Merge with above tests when the feature is enabled by default.
class MediaSessionServiceImplWebRTCBrowserTest
    : public MediaSessionServiceImplBrowserTest {
 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    MediaSessionServiceImplBrowserTest::SetUpCommandLine(command_line);
    feature_list_.InitAndEnableFeature(media::kMediaSessionWebRTC);
  }

  bool ExecuteScriptToSetUpWebRTCMediaSessionSync() {
    bool result = ExecuteScript(shell(), kSetUpWebRTCMediaSessionScript);
    media_session::test::MockMediaSessionMojoObserver observer(*GetSession());

    std::set<media_session::mojom::MediaSessionAction> expected_actions;
    expected_actions.insert(media_session::mojom::MediaSessionAction::kPlay);
    expected_actions.insert(media_session::mojom::MediaSessionAction::kPause);
    expected_actions.insert(media_session::mojom::MediaSessionAction::kStop);
    expected_actions.insert(media_session::mojom::MediaSessionAction::kSeekTo);
    expected_actions.insert(media_session::mojom::MediaSessionAction::kScrubTo);
    expected_actions.insert(
        media_session::mojom::MediaSessionAction::kToggleMicrophone);
    expected_actions.insert(
        media_session::mojom::MediaSessionAction::kToggleCamera);
    expected_actions.insert(media_session::mojom::MediaSessionAction::kHangUp);

    observer.WaitForExpectedActions(expected_actions);
    return result;
  }

 private:
  base::test::ScopedFeatureList feature_list_;
};

IN_PROC_BROWSER_TEST_F(MediaSessionServiceImplWebRTCBrowserTest,
                       MicrophoneAndCameraStatesInitiallyUnknown) {
  EXPECT_TRUE(NavigateToURL(shell(), GetTestUrl(".", "title1.html")));
  EnsurePlayer();

  EXPECT_TRUE(ExecuteScriptToSetUpMediaSessionSync());

  media_session::test::MockMediaSessionMojoObserver observer(*GetSession());
  observer.WaitForMicrophoneState(
      media_session::mojom::MicrophoneState::kUnknown);
  observer.WaitForCameraState(media_session::mojom::CameraState::kUnknown);
}

IN_PROC_BROWSER_TEST_F(MediaSessionServiceImplWebRTCBrowserTest,
                       MicrophoneAndCameraStatesCanBeSet) {
  EXPECT_TRUE(NavigateToURL(shell(), GetTestUrl(".", "title1.html")));
  EnsurePlayer();

  EXPECT_TRUE(ExecuteScriptToSetUpWebRTCMediaSessionSync());

  media_session::test::MockMediaSessionMojoObserver observer(*GetSession());
  observer.WaitForMicrophoneState(
      media_session::mojom::MicrophoneState::kUnmuted);
  observer.WaitForCameraState(media_session::mojom::CameraState::kTurnedOn);
}

}  // namespace content
