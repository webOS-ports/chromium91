// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/optional.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_restrictions.h"
#include "components/network_session_configurator/common/network_switches.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_features.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/content_browser_test.h"
#include "content/public/test/content_browser_test_utils.h"
#include "content/shell/browser/shell.h"
#include "content/test/content_browser_test_utils_internal.h"
#include "net/dns/mock_host_resolver.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace content {
namespace {

const char kUrnUuidURL[] = "urn:uuid:429fcc4e-0696-4bad-b099-ee9175f023ae";
const char kUrnUuidURL2[] = "urn:uuid:e219d992-b7f7-4da7-9722-481bc40cfda1";

class TestBrowserClient : public ContentBrowserClient {
 public:
  TestBrowserClient() = default;
  ~TestBrowserClient() override = default;
  bool HandleExternalProtocol(
      const GURL& url,
      base::OnceCallback<WebContents*()> web_contents_getter,
      int child_id,
      int frame_tree_node_id,
      NavigationUIData* navigation_data,
      bool is_main_frame,
      ui::PageTransition page_transition,
      bool has_user_gesture,
      const base::Optional<url::Origin>& initiating_origin,
      mojo::PendingRemote<network::mojom::URLLoaderFactory>* out_factory)
      override {
    EXPECT_FALSE(observed_url_.has_value());
    observed_url_ = url;
    return true;
  }

  GURL observed_url() const { return observed_url_ ? *observed_url_ : GURL(); }

 private:
  base::Optional<GURL> observed_url_;
};

class FinishNavigationObserver : public WebContentsObserver {
 public:
  FinishNavigationObserver() = default;
  ~FinishNavigationObserver() override = default;
  explicit FinishNavigationObserver(WebContents* contents,
                                    const GURL& expected_url,
                                    base::OnceClosure done_closure)
      : WebContentsObserver(contents),
        expected_url_(expected_url),
        done_closure_(std::move(done_closure)) {}

  void DidFinishNavigation(NavigationHandle* navigation_handle) override {
    if (navigation_handle->GetURL() == expected_url_) {
      error_code_ = navigation_handle->GetNetErrorCode();
      std::move(done_closure_).Run();
    }
  }

  const base::Optional<net::Error>& error_code() const { return error_code_; }

 private:
  GURL expected_url_;
  base::OnceClosure done_closure_;
  base::Optional<net::Error> error_code_;
};

int64_t GetTestDataFileSize(const base::FilePath::CharType* file_path) {
  int64_t file_size;
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::FilePath test_data_dir;
  CHECK(base::PathService::Get(base::DIR_SOURCE_ROOT, &test_data_dir));
  CHECK(base::GetFileSize(test_data_dir.Append(base::FilePath(file_path)),
                          &file_size));
  return file_size;
}

FrameTreeNode* GetFirstChild(WebContents* web_contents) {
  return static_cast<WebContentsImpl*>(web_contents)
      ->GetFrameTree()
      ->root()
      ->child_at(0);
}

}  // namespace

class LinkWebBundleBrowserTest : public ContentBrowserTest {
 protected:
  LinkWebBundleBrowserTest() {
    feature_list_.InitAndEnableFeature(features::kSubresourceWebBundles);
  }
  ~LinkWebBundleBrowserTest() override = default;

  void SetUpCommandLine(base::CommandLine* command_line) override {
    ContentBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitch(switches::kIgnoreCertificateErrors);
  }

  void SetUpOnMainThread() override {
    ContentBrowserTest::SetUpOnMainThread();
    original_client_ = SetBrowserClientForTesting(&browser_client_);
    host_resolver()->AddRule("*", "127.0.0.1");
    https_server_.ServeFilesFromSourceDirectory(GetTestDataFilePath());
    ASSERT_TRUE(https_server_.Start());
  }

  void TearDownOnMainThread() override {
    ContentBrowserTest::TearDownOnMainThread();
    SetBrowserClientForTesting(original_client_);
  }

  void CreateIframeAndWaitForOnload(const std::string& url) {
    DOMMessageQueue dom_message_queue(shell()->web_contents());
    std::string message;
    ExecuteScriptAsync(
        shell(),
        "let iframe = document.createElement('iframe');"
        "iframe.src = '" +
            url +
            "';"
            "iframe.onload = function() {"
            "   window.domAutomationController.send('iframe.onload');"
            "};"
            "document.body.appendChild(iframe);");
    EXPECT_TRUE(dom_message_queue.WaitForMessage(&message));
    EXPECT_EQ("\"iframe.onload\"", message);
  }

  GURL GetObservedUnknownSchemeUrl() { return browser_client_.observed_url(); }

  net::EmbeddedTestServer* https_server() { return &https_server_; }

 private:
  ContentBrowserClient* original_client_ = nullptr;
  TestBrowserClient browser_client_;
  base::test::ScopedFeatureList feature_list_;
  net::EmbeddedTestServer https_server_{
      net::EmbeddedTestServer::Type::TYPE_HTTPS};
};

IN_PROC_BROWSER_TEST_F(LinkWebBundleBrowserTest, SubframeLoad) {
  base::HistogramTester histogram_tester;
  GURL url(https_server()->GetURL("/web_bundle/link_web_bundle.html"));
  EXPECT_TRUE(NavigateToURL(shell(), url));

  // Create an iframe with a urn:uuid resource in a bundle.
  base::RunLoop run_loop;
  FinishNavigationObserver finish_navigation_observer(
      shell()->web_contents(), GURL(kUrnUuidURL), run_loop.QuitClosure());
  ExecuteScriptAsync(
      shell(),
      "let iframe = document.createElement('iframe');"
      "iframe.src = 'urn:uuid:429fcc4e-0696-4bad-b099-ee9175f023ae';"
      "document.body.appendChild(iframe);");
  run_loop.Run();
  EXPECT_EQ(net::OK, *finish_navigation_observer.error_code());

  // Check the metrics recorded in the network process.
  FetchHistogramsFromChildProcesses();
  int64_t web_bundle_size = GetTestDataFileSize(
      FILE_PATH_LITERAL("content/test/data/web_bundle/urn-uuid.wbn"));
  histogram_tester.ExpectUniqueSample("SubresourceWebBundles.ReceivedSize",
                                      web_bundle_size, 1);
  histogram_tester.ExpectUniqueSample("SubresourceWebBundles.ContentLength",
                                      web_bundle_size, 1);
}

IN_PROC_BROWSER_TEST_F(LinkWebBundleBrowserTest, SubframeLoadError) {
  GURL url(https_server()->GetURL("/web_bundle/invalid_web_bundle.html"));
  EXPECT_TRUE(NavigateToURL(shell(), url));

  // Attempt to create an iframe with a resource in a broken WebBundle.
  base::RunLoop run_loop;
  FinishNavigationObserver finish_navigation_observer(
      shell()->web_contents(), GURL(kUrnUuidURL), run_loop.QuitClosure());
  ExecuteScriptAsync(
      shell(),
      "let iframe = document.createElement('iframe');"
      "iframe.src = 'urn:uuid:429fcc4e-0696-4bad-b099-ee9175f023ae';"
      "document.body.appendChild(iframe);");
  run_loop.Run();
  EXPECT_EQ(net::ERR_INVALID_WEB_BUNDLE,
            *finish_navigation_observer.error_code());
}

IN_PROC_BROWSER_TEST_F(LinkWebBundleBrowserTest, FollowLink) {
  GURL url(https_server()->GetURL("/web_bundle/link_web_bundle.html"));
  EXPECT_TRUE(NavigateToURL(shell(), url));

  // Clicking a link to a urn:uuid resource in a bundle should not be loaded
  // from the bundle.
  base::RunLoop run_loop;
  FinishNavigationObserver finish_navigation_observer(
      shell()->web_contents(), GURL(kUrnUuidURL), run_loop.QuitClosure());
  EXPECT_TRUE(ExecuteScript(shell()->web_contents(),
                            "document.getElementById('link').click();"));
  run_loop.Run();
  EXPECT_EQ(net::ERR_ABORTED, *finish_navigation_observer.error_code());
  EXPECT_EQ(GURL(kUrnUuidURL), GetObservedUnknownSchemeUrl());
}

IN_PROC_BROWSER_TEST_F(LinkWebBundleBrowserTest, IframeChangeSource) {
  GURL main_url(https_server()->GetURL("/simple_page.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // Create an iframe whose document has <link rel="webbundle">.
  CreateIframeAndWaitForOnload("/web_bundle/link_web_bundle.html");

  // Attempt to navigate the iframe to a bundled resource.
  base::RunLoop run_loop;
  FinishNavigationObserver finish_navigation_observer(
      shell()->web_contents(), GURL(kUrnUuidURL), run_loop.QuitClosure());
  ExecuteScriptAsync(
      shell(), "iframe.src = 'urn:uuid:429fcc4e-0696-4bad-b099-ee9175f023ae';");
  run_loop.Run();
  EXPECT_EQ(net::ERR_ABORTED, *finish_navigation_observer.error_code());
  EXPECT_EQ(GURL(kUrnUuidURL), GetObservedUnknownSchemeUrl());
}

IN_PROC_BROWSER_TEST_F(LinkWebBundleBrowserTest, IframeFollowLink) {
  GURL main_url(https_server()->GetURL("/simple_page.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // Create an iframe whose document has <link rel="webbundle">.
  CreateIframeAndWaitForOnload("/web_bundle/link_web_bundle.html");

  // Click a link inside the iframe. The resource should not be loaded from
  // the bundle.
  base::RunLoop run_loop;
  FinishNavigationObserver finish_navigation_observer(
      shell()->web_contents(), GURL(kUrnUuidURL), run_loop.QuitClosure());
  ExecuteScriptAsync(shell(),
                     "iframe.contentDocument.getElementById('link').click();");
  run_loop.Run();
  EXPECT_EQ(net::ERR_ABORTED, *finish_navigation_observer.error_code());
  EXPECT_EQ(GURL(kUrnUuidURL), GetObservedUnknownSchemeUrl());
}

IN_PROC_BROWSER_TEST_F(LinkWebBundleBrowserTest, NavigationFromSiblingFrame) {
  GURL main_url(https_server()->GetURL("/web_bundle/link_web_bundle.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // Create an iframe and wait for the initial load.
  DOMMessageQueue dom_message_queue(shell()->web_contents());
  std::string message;
  ExecuteScriptAsync(shell(),
                     "let iframe1 = document.createElement('iframe');"
                     "iframe1.name = 'iframe1';"
                     "iframe1.src = '/simple_page.html';"
                     "iframe1.onload = function() {"
                     "   window.domAutomationController.send('iframe1.onload');"
                     "};"
                     "document.body.appendChild(iframe1);");
  EXPECT_TRUE(dom_message_queue.WaitForMessage(&message));
  EXPECT_EQ("\"iframe1.onload\"", message);

  // Create another iframe and wait for the initial load.
  ExecuteScriptAsync(shell(),
                     "let iframe2 = document.createElement('iframe');"
                     "iframe2.name = 'iframe2';"
                     "iframe2.src = '/simple_page.html';"
                     "iframe2.onload = function() {"
                     "   window.domAutomationController.send('iframe2.onload');"
                     "};"
                     "document.body.appendChild(iframe2);");
  EXPECT_TRUE(dom_message_queue.WaitForMessage(&message));
  EXPECT_EQ("\"iframe2.onload\"", message);

  // Navigate iframe2 to a urn:uuid URL by clicking a link in iframe1, which
  // should not be loaded from the WebBundle associated with the parent
  // document.
  base::RunLoop run_loop;
  FinishNavigationObserver finish_navigation_observer(
      shell()->web_contents(), GURL(kUrnUuidURL), run_loop.QuitClosure());
  ExecuteScriptAsync(shell(),
                     "let a = iframe1.contentDocument.createElement('a');"
                     "a.href = 'urn:uuid:429fcc4e-0696-4bad-b099-ee9175f023ae';"
                     "a.target = 'iframe2';"
                     "iframe1.contentDocument.body.appendChild(a);"
                     "a.click();");
  run_loop.Run();
  EXPECT_EQ(net::ERR_ABORTED, *finish_navigation_observer.error_code());
  EXPECT_EQ(GURL(kUrnUuidURL), GetObservedUnknownSchemeUrl());
}

IN_PROC_BROWSER_TEST_F(LinkWebBundleBrowserTest,
                       GrandChildShouldNotBeLoadedFromBundle) {
  GURL main_url(https_server()->GetURL("/web_bundle/link_web_bundle.html"));
  EXPECT_TRUE(NavigateToURL(shell(), main_url));

  // Create an iframe with a urn:uuid resource, which has a nested iframe with
  // another urn:uuid resource in the bundle. The resource for the nested should
  // iframe not be loaded from the bundle.
  base::RunLoop run_loop;
  FinishNavigationObserver finish_navigation_observer(
      shell()->web_contents(), GURL(kUrnUuidURL), run_loop.QuitClosure());
  ExecuteScriptAsync(
      shell(),
      "let iframe = document.createElement('iframe');"
      "iframe.src = 'urn:uuid:1084e1fc-2122-4155-a4dd-28efb2e8ccb1';"
      "document.body.appendChild(iframe);");
  run_loop.Run();
  EXPECT_EQ(net::ERR_ABORTED, *finish_navigation_observer.error_code());
  EXPECT_EQ(GURL(kUrnUuidURL), GetObservedUnknownSchemeUrl());
}

IN_PROC_BROWSER_TEST_F(LinkWebBundleBrowserTest, NetworkIsolationKey) {
  GURL bundle_url(
      https_server()->GetURL("bundle.test", "/web_bundle/urn-uuid.wbn"));
  GURL page_url(https_server()->GetURL(
      "page.test", "/web_bundle/frame_parent.html?wbn=" + bundle_url.spec()));
  EXPECT_TRUE(NavigateToURL(shell(), page_url));
  std::u16string expected_title(u"OK");
  TitleWatcher title_watcher(shell()->web_contents(), expected_title);
  EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());

  RenderFrameHost* main_frame = shell()->web_contents()->GetMainFrame();
  RenderFrameHost* urn_frame = ChildFrameAt(main_frame, 0);
  EXPECT_EQ("https://page.test https://bundle.test",
            urn_frame->GetNetworkIsolationKey().ToString());
}

IN_PROC_BROWSER_TEST_F(LinkWebBundleBrowserTest, ReloadSubframe) {
  GURL url(https_server()->GetURL("/web_bundle/link_web_bundle.html"));
  EXPECT_TRUE(NavigateToURL(shell(), url));

  // Create an iframe with a urn:uuid resource in a bundle.
  {
    base::RunLoop run_loop;
    FinishNavigationObserver finish_navigation_observer(
        shell()->web_contents(), GURL(kUrnUuidURL), run_loop.QuitClosure());
    ExecuteScriptAsync(
        shell(),
        "let iframe = document.createElement('iframe');"
        "iframe.src = 'urn:uuid:429fcc4e-0696-4bad-b099-ee9175f023ae';"
        "document.body.appendChild(iframe);");
    run_loop.Run();
    EXPECT_EQ(net::OK, *finish_navigation_observer.error_code());
  }
  FrameTreeNode* iframe_node = GetFirstChild(shell()->web_contents());
  EXPECT_EQ(iframe_node->current_url(), GURL(kUrnUuidURL));

  // Reload the iframe.
  {
    base::RunLoop run_loop;
    FinishNavigationObserver finish_navigation_observer(
        shell()->web_contents(), GURL(kUrnUuidURL), run_loop.QuitClosure());
    iframe_node->current_frame_host()->Reload();
    run_loop.Run();
    EXPECT_EQ(net::OK, *finish_navigation_observer.error_code());
  }
}

IN_PROC_BROWSER_TEST_F(LinkWebBundleBrowserTest, SubframeHistoryNavigation) {
  GURL url(https_server()->GetURL("/web_bundle/link_web_bundle.html"));
  EXPECT_TRUE(NavigateToURL(shell(), url));

  // Create an iframe with a urn:uuid resource in a bundle.
  {
    base::RunLoop run_loop;
    FinishNavigationObserver finish_navigation_observer(
        shell()->web_contents(), GURL(kUrnUuidURL), run_loop.QuitClosure());
    ExecuteScriptAsync(
        shell(),
        "let iframe = document.createElement('iframe');"
        "iframe.src = 'urn:uuid:429fcc4e-0696-4bad-b099-ee9175f023ae';"
        "document.body.appendChild(iframe);");
    run_loop.Run();
    EXPECT_EQ(net::OK, *finish_navigation_observer.error_code());
  }
  FrameTreeNode* iframe_node = GetFirstChild(shell()->web_contents());
  EXPECT_EQ(iframe_node->current_url(), GURL(kUrnUuidURL));

  // Navigate the iframe to a page outside the bundle.
  {
    GURL another_page_url(https_server()->GetURL("/simple_page.html"));
    base::RunLoop run_loop;
    FinishNavigationObserver finish_navigation_observer(
        shell()->web_contents(), another_page_url, run_loop.QuitClosure());
    EXPECT_TRUE(ExecJs(iframe_node,
                       base::StringPrintf("location.href = '%s';",
                                          another_page_url.spec().c_str())));
    run_loop.Run();
    EXPECT_EQ(net::OK, *finish_navigation_observer.error_code());
    EXPECT_EQ(iframe_node->current_url(), another_page_url);
  }

  // Back navigate the iframe to the urn:uuid resource in the bundle.
  {
    base::RunLoop run_loop;
    FinishNavigationObserver finish_navigation_observer(
        shell()->web_contents(), GURL(kUrnUuidURL), run_loop.QuitClosure());
    EXPECT_TRUE(ExecJs(iframe_node, "history.back()"));
    run_loop.Run();
    EXPECT_EQ(net::OK, *finish_navigation_observer.error_code());
    EXPECT_EQ(iframe_node->current_url(), GURL(kUrnUuidURL));
  }

  // Navigate the iframe to another urn:uuid resource in the bundle, by changing
  // iframe.src attribute.
  {
    base::RunLoop run_loop;
    FinishNavigationObserver finish_navigation_observer(
        shell()->web_contents(), GURL(kUrnUuidURL2), run_loop.QuitClosure());
    ExecuteScriptAsync(shell(),
                       base::StringPrintf("iframe.src = '%s';", kUrnUuidURL2));
    run_loop.Run();
    EXPECT_EQ(net::OK, *finish_navigation_observer.error_code());
    EXPECT_EQ(iframe_node->current_url(), GURL(kUrnUuidURL2));
  }

  // Back navigate the iframe to the first urn:uuid resource.
  {
    base::RunLoop run_loop;
    FinishNavigationObserver finish_navigation_observer(
        shell()->web_contents(), GURL(kUrnUuidURL), run_loop.QuitClosure());
    EXPECT_TRUE(ExecJs(iframe_node, "history.back()"));
    run_loop.Run();
    EXPECT_EQ(net::OK, *finish_navigation_observer.error_code());
    EXPECT_EQ(iframe_node->current_url(), GURL(kUrnUuidURL));
  }

  // Forward navigate the iframe to the second urn:uuid resource.
  {
    base::RunLoop run_loop;
    FinishNavigationObserver finish_navigation_observer(
        shell()->web_contents(), GURL(kUrnUuidURL2), run_loop.QuitClosure());
    EXPECT_TRUE(ExecJs(iframe_node, "history.forward()"));
    run_loop.Run();
    EXPECT_EQ(net::OK, *finish_navigation_observer.error_code());
    EXPECT_EQ(iframe_node->current_url(), GURL(kUrnUuidURL2));
  }

  GURL url_with_hash(std::string(kUrnUuidURL2) + "#hash");
  // Same document navigation.
  {
    base::RunLoop run_loop;
    FinishNavigationObserver finish_navigation_observer(
        shell()->web_contents(), url_with_hash, run_loop.QuitClosure());
    EXPECT_TRUE(ExecJs(iframe_node, "location.href = '#hash';"));
    run_loop.Run();
    EXPECT_EQ(net::OK, *finish_navigation_observer.error_code());
    EXPECT_EQ(iframe_node->current_url(), url_with_hash);
  }

  // Back from same document navigation.
  {
    base::RunLoop run_loop;
    FinishNavigationObserver finish_navigation_observer(
        shell()->web_contents(), GURL(kUrnUuidURL2), run_loop.QuitClosure());
    EXPECT_TRUE(ExecJs(iframe_node, "history.back()"));
    run_loop.Run();
    EXPECT_EQ(net::OK, *finish_navigation_observer.error_code());
    EXPECT_EQ(iframe_node->current_url(), GURL(kUrnUuidURL2));
  }

  // Forward navigate to #hash.
  {
    base::RunLoop run_loop;
    FinishNavigationObserver finish_navigation_observer(
        shell()->web_contents(), url_with_hash, run_loop.QuitClosure());
    EXPECT_TRUE(ExecJs(iframe_node, "history.forward()"));
    run_loop.Run();
    EXPECT_EQ(net::OK, *finish_navigation_observer.error_code());
    EXPECT_EQ(iframe_node->current_url(), url_with_hash);
  }
}

}  // namespace content
