// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_BROWSER_RENDER_FRAME_HOST_H_
#define CONTENT_PUBLIC_BROWSER_RENDER_FRAME_HOST_H_

#include <string>
#include <vector>

#include "base/callback_forward.h"
#include "base/containers/flat_set.h"
#include "build/build_config.h"
#include "cc/input/browser_controls_state.h"
#include "content/common/content_export.h"
#include "content/public/common/isolated_world_ids.h"
#include "ipc/ipc_listener.h"
#include "ipc/ipc_sender.h"
#include "services/metrics/public/cpp/ukm_source_id.h"
#include "services/network/public/mojom/web_sandbox_flags.mojom-forward.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "third_party/blink/public/mojom/ad_tagging/ad_frame.mojom-forward.h"
#include "third_party/blink/public/mojom/devtools/console_message.mojom-forward.h"
#include "third_party/blink/public/mojom/devtools/inspector_issue.mojom-forward.h"
#include "third_party/blink/public/mojom/frame/frame_owner_element_type.mojom-forward.h"
#include "third_party/blink/public/mojom/frame/sudden_termination_disabler_type.mojom-forward.h"
#include "third_party/blink/public/mojom/frame/user_activation_notification_type.mojom-forward.h"
#include "third_party/blink/public/mojom/page/page_visibility_state.mojom-forward.h"
#include "third_party/blink/public/mojom/permissions_policy/permissions_policy_feature.mojom-forward.h"
#include "ui/gfx/native_widget_types.h"

class GURL;

namespace base {
#if defined(OS_ANDROID)
namespace android {
template <typename T>
class JavaRef;
}  // namespace android
#endif

template <typename T>
class Optional;
class TimeDelta;
class UnguessableToken;
class Value;
}  // namespace base

namespace blink {
class AssociatedInterfaceProvider;

namespace mojom {
enum class AuthenticatorStatus;
enum class PermissionsPolicyFeature;
class MediaPlayerAction;
}  // namespace mojom
}  // namespace blink

namespace gfx {
class Point;
class Size;
}  // namespace gfx

namespace mojo {
template <typename T>
class PendingReceiver;
}  // namespace mojo

namespace net {
class IsolationInfo;
class NetworkIsolationKey;
}

namespace network {
namespace mojom {
class URLLoaderFactory;
}
}  // namespace network

namespace perfetto {
class TracedValue;
}

namespace service_manager {
class InterfaceProvider;
}

namespace ui {
struct AXActionData;
struct AXTreeUpdate;
class AXMode;
class AXTreeID;
}  // namespace ui

namespace url {
class Origin;
}

namespace content {

class BrowserContext;
struct GlobalFrameRoutingId;
class RenderProcessHost;
class RenderViewHost;
class RenderWidgetHost;
class RenderWidgetHostView;
class SiteInstance;
class StoragePartition;
class WebUI;

// The interface provides a communication conduit with a frame in the renderer.
// The preferred way to keep a reference to a RenderFrameHost is storing a
// GlobalFrameRoutingId and using RenderFrameHost::FromID() when you need to
// access it.
class CONTENT_EXPORT RenderFrameHost : public IPC::Listener,
                                       public IPC::Sender {
 public:
  // Constant used to denote that a lookup of a FrameTreeNode ID has failed.
  enum { kNoFrameTreeNodeId = -1 };

  // Returns the RenderFrameHost given its ID and the ID of its render process.
  // Returns nullptr if the IDs do not correspond to a live RenderFrameHost.
  static RenderFrameHost* FromID(const GlobalFrameRoutingId& id);
  static RenderFrameHost* FromID(int render_process_id, int render_frame_id);

  // Returns the RenderFrameHost given its frame token and its process
  // ID. Returns nullptr if the frame token does not correspond to a live
  // RenderFrameHost.
  static RenderFrameHost* FromFrameToken(
      int initiator_process_id,
      const blink::LocalFrameToken& frame_token);

  // Globally allows for injecting JavaScript into the main world. This feature
  // is present only to support Android WebView, WebLayer, Fuchsia web.Contexts,
  // and CastOS content shell. It must not be used in other configurations.
  static void AllowInjectingJavaScript();

  // Returns a RenderFrameHost given its accessibility tree ID.
  static RenderFrameHost* FromAXTreeID(const ui::AXTreeID& ax_tree_id);

  // Returns the FrameTreeNode ID corresponding to the specified |process_id|
  // and |routing_id|. This routing ID pair may represent a placeholder for
  // frame that is currently rendered in a different process than |process_id|.
  static int GetFrameTreeNodeIdForRoutingId(int process_id, int routing_id);

  // Returns the RenderFrameHost corresponding to the
  // |placeholder_frame_token| in the given |render_process_id|. The returned
  // RenderFrameHost will always be in a different process.  It may be null if
  // the placeholder is not found in the given process, which may happen if the
  // frame was recently deleted or swapped to |render_process_id| itself.
  static RenderFrameHost* FromPlaceholderToken(
      int render_process_id,
      const blink::RemoteFrameToken& placeholder_frame_token);

#if defined(OS_ANDROID)
  // Returns the RenderFrameHost object associated with a Java native pointer.
  static RenderFrameHost* FromJavaRenderFrameHost(
      const base::android::JavaRef<jobject>& jrender_frame_host_android);
#endif

  ~RenderFrameHost() override {}

  // Returns the route id for this frame.
  virtual int GetRoutingID() = 0;

  // Returns the frame token for this frame.
  virtual const blink::LocalFrameToken& GetFrameToken() = 0;

  // Returns the accessibility tree ID for this RenderFrameHost.
  virtual ui::AXTreeID GetAXTreeID() = 0;

  using AXTreeSnapshotCallback =
      base::OnceCallback<void(const ui::AXTreeUpdate&)>;
  // Request a one-time snapshot of the accessibility tree without changing
  // the accessibility mode.
  virtual void RequestAXTreeSnapshot(AXTreeSnapshotCallback callback,
                                     const ui::AXMode& ax_mode,
                                     bool exclude_offscreen,
                                     size_t max_nodes,
                                     const base::TimeDelta& timeout) = 0;

  // Returns the SiteInstance grouping all RenderFrameHosts that have script
  // access to this RenderFrameHost, and must therefore live in the same
  // process.
  // Associated SiteInstance never changes.
  virtual SiteInstance* GetSiteInstance() = 0;

  // Returns the process for this frame.
  // Associated RenderProcessHost never changes.
  virtual RenderProcessHost* GetProcess() = 0;

  // Returns the GlobalFrameRoutingId for this frame. Embedders should store
  // this instead of a raw RenderFrameHost pointer.
  virtual GlobalFrameRoutingId GetGlobalFrameRoutingId() = 0;

  // Returns a StoragePartition associated with this RenderFrameHost.
  // Associated StoragePartition never changes.
  virtual StoragePartition* GetStoragePartition() = 0;

  // Returns the user browser context associated with this RenderFrameHost.
  // Associated BrowserContext never changes.
  virtual BrowserContext* GetBrowserContext() = 0;

  // Returns the RenderWidgetHostView for this frame or the nearest ancestor
  // frame, which can be used to control input, focus, rendering and visibility
  // for this frame.
  // This returns null when there is no connection to a renderer process, which
  // can be checked with IsRenderFrameLive().
  // NOTE: Due to historical relationships between RenderViewHost and
  // RenderWidgetHost, the main frame RenderWidgetHostView may initially exist
  // before IsRenderFrameCreated() is true, but they would afterward change
  // values together. It is better to not rely on this behaviour as it is
  // intended to change. See https://crbug.com/419087.
  virtual RenderWidgetHostView* GetView() = 0;

  // Returns the RenderWidgetHost attached to this frame or the nearest ancestor
  // frame, which could potentially be the root. This allows access to the
  // RenderWidgetHost without having to go through GetView() which can be null,
  // so should be preferred to GetView()->GetRenderWidgetHost().
  //
  // This method is not valid to be called when the RenderFrameHost is detached
  // from the frame tree, though this would only happen during destruction of
  // the RenderFrameHost.
  virtual RenderWidgetHost* GetRenderWidgetHost() = 0;

  // Returns the parent of this RenderFrameHost, or nullptr if this
  // RenderFrameHost is the main one and there is no parent.
  // The result may be in a different process than the
  // current RenderFrameHost.
  virtual RenderFrameHost* GetParent() = 0;

  // Returns the eldest parent of this RenderFrameHost.
  // Always non-null, but might be equal to |this|.
  // The result may be in a different process that the current RenderFrameHost.
  //
  // NOTE: The result might be different from
  // WebContents::FromRenderFrameHost(this)->GetMainFrame().
  // This function (RenderFrameHost::GetMainFrame) is the preferred API in
  // almost all of the cases. See RenderFrameHost::IsCurrent for the details.
  virtual RenderFrameHost* GetMainFrame() = 0;

  // Returns a vector of all RenderFrameHosts in the subtree rooted at |this|.
  // The results may be in different processes.
  // TODO(https://crbug.com/1013740): Consider exposing a way for the browser
  // process to run a function across a subtree in all renderers rather than
  // exposing the RenderFrameHosts of the frames here.
  virtual std::vector<RenderFrameHost*> GetFramesInSubtree() = 0;

  // Returns whether or not this RenderFrameHost is a descendant of |ancestor|.
  // This is equivalent to check that |ancestor| is reached by iterating on
  // GetParent().
  // This is a strict relationship, a RenderFrameHost is never an ancestor of
  // itself.
  virtual bool IsDescendantOf(RenderFrameHost* ancestor) = 0;

  // Returns the FrameTreeNode ID for this frame. This ID is browser-global and
  // uniquely identifies a frame that hosts content. The identifier is fixed at
  // the creation of the frame and stays constant for the lifetime of the frame.
  // When the frame is removed, the ID is not used again.
  //
  // A RenderFrameHost is tied to a process. Due to cross-process navigations,
  // the RenderFrameHost may have a shorter lifetime than a frame. Consequently,
  // the same FrameTreeNode ID may refer to a different RenderFrameHost after a
  // navigation.
  virtual int GetFrameTreeNodeId() = 0;

  // Used for devtools instrumentation and trace-ability. The token is
  // propagated to Blink's LocalFrame and both Blink and content/
  // can tag calls and requests with this token in order to attribute them
  // to the context frame. The token is only defined by the browser process and
  // is never sent back from the renderer in the control calls. It should be
  // never used to look up the FrameTreeNode instance.
  virtual base::UnguessableToken GetDevToolsFrameToken() = 0;

  // This token is present on all frames. For frames with parents, it allows
  // identification of embedding relationships between parent and child. For
  // main frames, it also allows generalization of the embedding relationship
  // when the WebContents itself is embedded in another context such as the rest
  // of the browser UI. This will be nullopt prior to the RenderFrameHost
  // committing a navigation. After the first navigation commits this
  // will return the token for the last committed document.
  //
  // TODO(crbug/1098283): Remove the nullopt scenario by creating the token in
  // CreateChildFrame() or similar.
  virtual base::Optional<base::UnguessableToken> GetEmbeddingToken() = 0;

  // Returns the assigned name of the frame, the name of the iframe tag
  // declaring it. For example, <iframe name="framename">[...]</iframe>. It is
  // quite possible for a frame to have no name, in which case GetFrameName will
  // return an empty string.
  virtual const std::string& GetFrameName() = 0;

  // Returns true if the frame is display: none.
  virtual bool IsFrameDisplayNone() = 0;

  // Returns the size of the frame in the viewport. The frame may not be aware
  // of its size.
  virtual const base::Optional<gfx::Size>& GetFrameSize() = 0;

  // Returns the distance from this frame to the root frame.
  virtual size_t GetFrameDepth() = 0;

  // Returns true if the frame is out of process.
  virtual bool IsCrossProcessSubframe() = 0;

  // Indicates whether this frame is in a cross-origin isolated agent cluster.
  // See [1] and [2] for a description of what this means for web content.
  // Specifically, an agent cluster may be cross-origin isolated if:
  // - its top-level document has "Cross-Origin-Opener-Policy: same-origin" and
  //   "Cross-Origin-Embedder-Policy: require-corp" HTTP headers; or,
  // - its top-level worker script has a
  //   "Cross-Origin-Embedder-Policy: require-corp" HTTP header.
  //
  // In practice this means that the frame is guaranteed to be hosted in a
  // process that is isolated to the frame's origin. The process may also host
  // cross-origin frames and workers only if they have opted in to being
  // embedded with CORS or CORP headers.
  //
  // Certain advanced web platform APIs are gated behind this property. It will
  // correspond to the value returned by accessing
  // "WindowOrWorkerGlobalScope.crossOriginIsolated" in Javascript.
  //
  // NOTE: some of the information needed to fully determine a frame's
  // cross-isolation status is currently not available in the browser process.
  // Access to web platform API's must be checked in the renderer, with the
  // CrossOriginIsolationStatus on the browser side only used as a backup to
  // catch misbehaving renderers.
  //
  // [1]
  // https://developer.mozilla.org/en-US/docs/Web/API/WindowOrWorkerGlobalScope/crossOriginIsolated
  // [2] https://w3c.github.io/webappsec-permissions-policy/
  enum class CrossOriginIsolationStatus {
    // The frame is in a cross-origin isolated process and agent cluster.
    // It is allowed to call web platform API's gated behind the
    // crossOriginIsolated property.
    kIsolated,

    // The frame is not in a cross-origin isolated agent cluster. It may be
    // hosted in a cross-origin isolated process but it is not allowed to call
    // web platform API's gated behind the crossOriginIsolated property.
    kNotIsolated,

    // The frame is in a cross-origin isolated process, but it's not possible
    // to determine whether it's in a cross-origin isolated agent cluster. The
    // browser process should not prevent it from calling web platform API's
    // gated behind the crossOriginIsolated property because it may be allowed.
    // TODO(clamy): Remove this status once the document policy is available on
    // the browser side.
    kMaybeIsolated,
  };

  // Returns whether the frame is in a cross-origin isolated agent cluster.
  //
  // Note that this is a property of the document so can change as the frame
  // navigates.
  //
  // TODO(https://936696): Once RenderDocument ships this should be exposed as
  // an invariant of the document host.
  virtual CrossOriginIsolationStatus GetCrossOriginIsolationStatus() = 0;

  // Returns the last committed URL of this RenderFrameHost. This will be empty
  // until the first commit in this RenderFrameHost.
  //
  // Note that this does not reflect navigations in other RenderFrameHosts,
  // frames, or pages within the same WebContents, so it may differ from
  // NavigationController::GetLastCommittedEntry().
  virtual const GURL& GetLastCommittedURL() = 0;

  // Returns the last committed origin of this RenderFrameHost.
  virtual const url::Origin& GetLastCommittedOrigin() = 0;

  // Returns the network isolation key used for subresources from the currently
  // committed navigation. It's set on commit and does not change until the next
  // navigation is committed.
  //
  // TODO(mmenke): Remove this in favor of GetIsolationInfoForSubresoruces().
  virtual const net::NetworkIsolationKey& GetNetworkIsolationKey() = 0;

  // Returns the IsolationInfo used for subresources from the currently
  // committed navigation. It's set on commit and does not change until the next
  // navigation is committed.
  virtual const net::IsolationInfo& GetIsolationInfoForSubresources() = 0;

  // Returns the IsolationInfo used for subresources for the pending commit, if
  // there is one. Otherwise, returns the IsolationInfo used for subresources of
  // the last committed page load.
  //
  // TODO(https://936696): Remove this once RenderDocument ships, at which point
  // it will no longer be needed.
  virtual net::IsolationInfo GetPendingIsolationInfoForSubresources() = 0;

  // Returns the associated widget's native view.
  virtual gfx::NativeView GetNativeView() = 0;

  // Adds |message| to the DevTools console.
  virtual void AddMessageToConsole(blink::mojom::ConsoleMessageLevel level,
                                   const std::string& message) = 0;

  // Functions to run JavaScript in this frame's context. Pass in a callback to
  // receive a result when it is available. If there is no need to receive the
  // result, pass in a default-constructed callback. If provided, the callback
  // will be invoked on the UI thread.
  using JavaScriptResultCallback = base::OnceCallback<void(base::Value)>;

  // This API allows to execute JavaScript methods in this frame, without
  // having to serialize the arguments into a single string, and is a lot
  // cheaper than ExecuteJavaScript below since it avoids the need to compile
  // and evaluate new scripts all the time.
  //
  // Calling
  //
  //   ExecuteJavaScriptMethod("obj", "foo", [1, true], callback)
  //
  // is semantically equivalent to
  //
  //   ExecuteJavaScript("obj.foo(1, true)", callback)
  virtual void ExecuteJavaScriptMethod(const std::u16string& object_name,
                                       const std::u16string& method_name,
                                       base::Value&& arguments,
                                       JavaScriptResultCallback callback) = 0;

  // This is the default API to run JavaScript in this frame. This API can only
  // be called on chrome:// or devtools:// URLs.
  virtual void ExecuteJavaScript(const std::u16string& javascript,
                                 JavaScriptResultCallback callback) = 0;

  // This runs the JavaScript in an isolated world of the top of this frame's
  // context.
  virtual void ExecuteJavaScriptInIsolatedWorld(
      const std::u16string& javascript,
      JavaScriptResultCallback callback,
      int32_t world_id) = 0;

  // This runs the JavaScript, but without restrictions. THIS IS ONLY FOR TESTS.
  virtual void ExecuteJavaScriptForTests(
      const std::u16string& javascript,
      JavaScriptResultCallback callback,
      int32_t world_id = ISOLATED_WORLD_ID_GLOBAL) = 0;

  // This runs the JavaScript, but without restrictions. THIS IS ONLY FOR TESTS.
  // Unlike the method above, this one triggers a fake user activation
  // notification to test functionalities that are gated by user
  // activation.
  virtual void ExecuteJavaScriptWithUserGestureForTests(
      const std::u16string& javascript,
      int32_t world_id = ISOLATED_WORLD_ID_GLOBAL) = 0;

  // Send a message to the RenderFrame to trigger an action on an
  // accessibility object.
  virtual void AccessibilityPerformAction(const ui::AXActionData& data) = 0;

  // This is called when the user has committed to the given find in page
  // request (e.g. by pressing enter or by clicking on the next / previous
  // result buttons). It triggers sending a native accessibility event on
  // the result object on the page, navigating assistive technology to that
  // result.
  virtual void ActivateFindInPageResultForAccessibility(int request_id) = 0;

  // See RenderWidgetHost::InsertVisualStateCallback().
  using VisualStateCallback = base::OnceCallback<void(bool)>;
  virtual void InsertVisualStateCallback(VisualStateCallback callback) = 0;

  // Copies the image at the location in viewport coordinates (not frame
  // coordinates) to the clipboard. If there is no image at that location, does
  // nothing.
  virtual void CopyImageAt(int x, int y) = 0;

  // Requests to save the image at the location in viewport coordinates (not
  // frame coordinates). If there is a data-URL-based image at the location, the
  // renderer will post back the appropriate download message to trigger the
  // save UI.  Nothing gets done if there is no image at that location (or if
  // the image has a non-data URL).
  virtual void SaveImageAt(int x, int y) = 0;

  // RenderViewHost for this frame.
  virtual RenderViewHost* GetRenderViewHost() = 0;

  // Returns the InterfaceProvider that this process can use to bind
  // interfaces exposed to it by the application running in this frame.
  virtual service_manager::InterfaceProvider* GetRemoteInterfaces() = 0;

  // Returns the AssociatedInterfaceProvider that this process can use to access
  // remote frame-specific Channel-associated interfaces for this frame.
  virtual blink::AssociatedInterfaceProvider*
  GetRemoteAssociatedInterfaces() = 0;

  // Returns the visibility state of the frame. The different visibility states
  // of a frame are defined in Blink.
  virtual blink::mojom::PageVisibilityState GetVisibilityState() = 0;

  // Returns true if WebContentsObserver::RenderFrameCreated notification has
  // been dispatched for this frame, and so a RenderFrameDeleted notification
  // will later be dispatched for this frame.
  virtual bool IsRenderFrameCreated() = 0;

  // Returns whether the RenderFrame in the renderer process has been created
  // and still has a connection.  This is valid for all frames.
  virtual bool IsRenderFrameLive() = 0;

  // Defines different states the RenderFrameHost can be in during its lifetime,
  // i.e., from the point of creation to deletion. Please see comments in
  // RenderFrameHostImpl::LifecycleStateImpl for more details.
  //
  // Compared to the internal LifecycleStateImpl, this public LifecycleState has
  // two main differences. First, it collapses kRunningUnloadHandlers and
  // kReadyToBeDeleted into a single kPendingDeletion state, since embedders
  // need not care about the difference between having started and having
  // finished running unload handlers. Second, it intentionally does not expose
  // speculative RenderFrameHosts (corresponding to the kSpeculative internal
  // state): this is a content-internal implementation detail that is planned to
  // be eventually removed, and //content embedders shouldn't rely on their
  // existence.
  enum class LifecycleState {
    // RenderFrameHost is waiting for an acknowledgment from the renderer to
    // to commit a cross-RenderFrameHost navigation and swap in this
    // RenderFrameHost. Documents are in this state from
    // WebContentsObserver::ReadyToCommitNavigation to
    // WebContentsObserver::DidFinishNavigation.
    kPendingCommit,

    // RenderFrameHost committed in a primary page.
    // Documents in this state are visible to the user. kActive is the most
    // common case and the documents that have reached DidFinishNavigation will
    // be in this state (except for prerendered documents). A RenderFrameHost
    // can also be created in this state for an initial empty document when
    // creating new root frames or new child frames on a primary page.
    //
    // With MPArch (crbug.com/1164280), a WebContents may have multiple
    // coexisting pages (trees of documents), including a primary page
    // (currently shown to the user), prerendered pages, and/or pages in
    // BackForwardCache, where the two latter kinds of pages may become primary.
    kActive,

    // Prerender2:
    // RenderFrameHost committed in a prerendered page.
    // A RenderFrameHost can reach this state after a navigation in a
    // prerendered page, or be created in this state for an initial empty
    // document when creating new root frames or new child frames on a
    // prerendered page.
    //
    // Documents in this state are invisible to the user and aren't allowed to
    // show any UI changes, but the page is allowed to load and run in the
    // background. Documents in kPrerendering state can be evicted
    // (canceling prerendering) at any time (e.g. by calling
    // IsInactiveAndDisallowActivation).
    kPrerendering,

    // RenderFrameHost is stored in BackForwardCache.
    // A document may be stored in BackForwardCache after the user has navigated
    // away so that the RenderFrameHost can be re-used after history navigation.
    kInBackForwardCache,

    // RenderFrameHost is waiting to be unloaded and deleted, and is no longer
    // visible to the user.
    // After a cross-document navigation, the old documents are going to run
    // unload handlers in the background and will be deleted thereafter e.g.
    // after a DidFinishNavigation in the same frame for a different
    // RenderFrameHost, up until RenderFrameDeleted.
    kPendingDeletion,
  };

  // Returns the LifecycleState associated with this RenderFrameHost.
  // Features that display UI to the user (or cross document/tab boundary in
  // general, e.g. when using WebContents::FromRenderFrameHost) should first
  // check whether the RenderFrameHost is in the appropriate lifecycle state.
  //
  // TODO(https://crbug.com/1183639): Currently, //content embedders that
  // observe WebContentsObserver::RenderFrameCreated() may also learn about
  // speculative RenderFrameHosts, which is the state before a RenderFrameHost
  // becomes kPendingCommit and is picked as the final RenderFrameHost for a
  // navigation.  The speculative state is a content-internal implementation
  // detail that may go away and should not be relied on, and hence
  // GetLifecycleState() will crash if it is called on a RenderFrameHost in such
  // a state.  Eventually, we should make sure that embedders only learn about
  // new RenderFrameHosts when they reach the kPendingCommit state.
  virtual LifecycleState GetLifecycleState() = 0;

  // Returns true if this RenderFrameHost is currently in the frame tree for its
  // page. Specifically, this is when the RenderFrameHost and all of its
  // ancestors are the current RenderFrameHost in their respective
  // FrameTreeNodes.
  //
  // For instance, during a navigation, if a new RenderFrameHost replaces this
  // RenderFrameHost, IsCurrent() becomes false for this frame and its
  // children even if the children haven't been replaced.
  //
  // After a RenderFrameHost has been replaced in its frame, it will either:
  //  1) Enter the BackForwardCache.
  //  2) Start running unload handlers and will be deleted after this ("pending
  //  deletion").
  // In both cases, IsCurrent() becomes false for this frame and all its
  // children.
  //
  // This method should be called before trying to display some UI to the user
  // on behalf of the given RenderFrameHost (or when crossing document / tab
  // boundary in general, e.g. when using WebContents::FromRenderFrameHost) to
  // check if the given RenderFrameHost is currently being displayed in a given
  // tab.
  //
  // TODO(https://crbug.com/1184622): Rename IsCurrent to a more suitable name
  // and update this comment accordingly considering the new feature Prerender2,
  // where it is possible to have current RenderFrameHosts in multiple frame
  // trees.
  virtual bool IsCurrent() = 0;

  // Returns true iff the RenderFrameHost is inactive, i.e., when the
  // RenderFrameHost is either in BackForwardCache, Prerendering, or pending
  // deletion. This function should be used when we are unsure if inactive
  // RenderFrameHosts can properly handle events and events processing shouldn't
  // or can't be deferred until the RenderFrameHost becomes active again.
  // Callers that only want to check whether a RenderFrameHost is active or not
  // should use IsCurrent() instead.

  // Additionally, this method has a side effect for back-forward cache and
  // prerendering, where the document is prevented from ever becoming active
  // after calling this method. This allows to safely ignore the event as the
  // RenderFrameHost will never be shown to the user again.

  // For BackForwardCache: it evicts the document from the cache and triggers
  // deletion.
  // For Prerendering: it cancels prerendering and triggers deletion.

  // This should not be called for speculative and pending commit
  // RenderFrameHosts as disallowing activation is not supported. In that case
  // |IsInactiveAndDisallowActivation()| returns false along with terminating
  // the renderer process.

  // Note that if |IsInactiveAndDisallowActivation()| returns true, then
  // IsCurrent() returns false.
  //
  // TODO(https://crbug.com/1175866): Rename this method with a more suitable
  // name considering all document states.
  virtual bool IsInactiveAndDisallowActivation() = 0;

  // Get the number of proxies to this frame, in all processes. Exposed for
  // use by resource metrics.
  virtual size_t GetProxyCount() = 0;

  // Returns true if the frame has a selection.
  virtual bool HasSelection() = 0;

  // Text surrounding selection.
  virtual void RequestTextSurroundingSelection(
      base::OnceCallback<void(const std::u16string&, uint32_t, uint32_t)>
          callback,
      int max_length) = 0;

  // Generates an intervention report in this frame.
  virtual void SendInterventionReport(const std::string& id,
                                      const std::string& message) = 0;

  // Returns the WebUI object associated wit this RenderFrameHost or nullptr
  // otherwise.
  virtual WebUI* GetWebUI() = 0;

  // Tell the render frame to enable a set of javascript bindings. The argument
  // should be a combination of values from BindingsPolicy.
  virtual void AllowBindings(int binding_flags) = 0;

  // Returns a bitwise OR of bindings types that have been enabled for this
  // RenderFrame. See BindingsPolicy for details.
  virtual int GetEnabledBindings() = 0;

  // Sets a property with the given name and value on the WebUI object
  // associated with this RenderFrameHost, if one exists.
  virtual void SetWebUIProperty(const std::string& name,
                                const std::string& value) = 0;

#if defined(OS_ANDROID)
  // Returns the Java object of this instance.
  virtual base::android::ScopedJavaLocalRef<jobject>
  GetJavaRenderFrameHost() = 0;

  // Returns an InterfaceProvider for Java-implemented interfaces that are
  // scoped to this RenderFrameHost. This provides access to interfaces
  // implemented in Java in the browser process to C++ code in the browser
  // process.
  virtual service_manager::InterfaceProvider* GetJavaInterfaces() = 0;
#endif  // OS_ANDROID

  // Stops and disables the hang monitor for beforeunload. This avoids flakiness
  // in tests that need to observe beforeunload dialogs, which could fail if the
  // timeout skips the dialog.
  virtual void DisableBeforeUnloadHangMonitorForTesting() = 0;
  virtual bool IsBeforeUnloadHangMonitorDisabledForTesting() = 0;

  // Check whether the specific Blink feature is currently preventing fast
  // shutdown of the frame.
  virtual bool GetSuddenTerminationDisablerState(
      blink::mojom::SuddenTerminationDisablerType disabler_type) = 0;

  // Returns true if the queried PermissionsPolicyFeature is allowed by
  // permissions policy.
  virtual bool IsFeatureEnabled(
      blink::mojom::PermissionsPolicyFeature feature) = 0;

  // Opens view-source tab for the document last committed in this
  // RenderFrameHost.
  virtual void ViewSource() = 0;

#if defined(USE_NEVA_APPRUNTIME)
  virtual void DropAllPeerConnections(base::OnceClosure cb) = 0;
#endif  // defined(USE_NEVA_APPRUNTIME)

#if defined(USE_NEVA_MEDIA)
  virtual void SetSuppressed(bool is_suppressed) = 0;
  virtual gfx::AcceleratedWidget GetAcceleratedWidget() = 0;
#endif

  // Run the given action on the media player location at the given point.
  virtual void ExecuteMediaPlayerActionAtLocation(
      const gfx::Point& location,
      const blink::mojom::MediaPlayerAction& action) = 0;

  // Creates a Network Service-backed factory from appropriate |NetworkContext|.
  // If this returns true, any redirect safety checks should be bypassed in
  // downstream loaders.
  virtual bool CreateNetworkServiceDefaultFactory(
      mojo::PendingReceiver<network::mojom::URLLoaderFactory>&&
          default_factory_receiver) = 0;

  // Requests that future URLLoaderFactoryBundle(s) sent to the renderer should
  // use a separate URLLoaderFactory for requests initiated by isolated worlds
  // listed in |isolated_world_origins|.  The URLLoaderFactory(s) for each
  // origin will be created via
  // ContentBrowserClient::CreateURLLoaderFactoryForNetworkRequests method.
  virtual void MarkIsolatedWorldsAsRequiringSeparateURLLoaderFactory(
      base::flat_set<url::Origin> isolated_world_origins,
      bool push_to_renderer_now) = 0;

  // Returns true if the given sandbox flag |flags| is in effect on this frame.
  // The effective flags include those which have been set by a
  // Content-Security-Policy header, in addition to those which are set by the
  // embedding frame.
  virtual bool IsSandboxed(network::mojom::WebSandboxFlags flags) = 0;

  // Calls |FlushForTesting()| on Network Service and FrameNavigationControl
  // related interfaces to make sure all in-flight mojo messages have been
  // received by the other end. For test use only.
  virtual void FlushNetworkAndNavigationInterfacesForTesting() = 0;

  using PrepareForInnerWebContentsAttachCallback =
      base::OnceCallback<void(RenderFrameHost*)>;
  // This API is used to provide the caller with a RenderFrameHost which is safe
  // for usage in WebContents::AttachToOuterWebContentsFrame API. The final
  // frame returned with |callback| will share the same FrameTreeNodeId with
  // this RenderFrameHost but might not necessarily be the same RenderFrameHost.
  // IMPORTANT: This method can only be called on a child frame. It does not
  // make sense to attach an inner WebContents to the outer WebContents main
  // frame.
  // Essentially, this method will:
  //  1- Cancel any ongoing navigation and navigation requests for this frame.
  //  2- Dispatch beforeunload event on this frame and all of the frame's
  //     subframes, and wait for all beforeunload events to complete.
  //  3- Will create and return a new RenderFrameHost (destroying this one) if
  //     this RenderFrameHost is a cross-process subframe.
  // After steps 1-3 are completed, the callback is invoked asynchronously with
  // the RenderFrameHost which can be safely used for attaching. This
  // RenderFrameHost could be different than |this| which is the case if this
  // RenderFrameHost is for a cross-process frame. The callback could also be
  // invoked with nullptr. This happens if:
  //  1- This frame has beforeunload handlers under it and the user decides to
  //     remain on the page in response to beforeunload prompt.
  //  2- Preparations happened successfully but the frame was somehow removed (
  //     e.g. parent frame detached).
  virtual void PrepareForInnerWebContentsAttach(
      PrepareForInnerWebContentsAttachCallback callback) = 0;

  // Returns the type of frame owner element for the FrameTreeNode associated
  // with this RenderFrameHost (e.g., <iframe>, <object>, etc). Note that it
  // returns blink::mojom::FrameOwnerElementType::kNone if the RenderFrameHost
  // is a main frame.
  virtual blink::mojom::FrameOwnerElementType GetFrameOwnerElementType() = 0;

  // Returns the transient bit of the User Activation v2 state of the
  // FrameTreeNode associated with this RenderFrameHost.
  virtual bool HasTransientUserActivation() = 0;

  // Notifies the renderer of a user activation event for the associated frame.
  // The |notification_type| parameter is used for histograms only.
  virtual void NotifyUserActivation(
      blink::mojom::UserActivationNotificationType notification_type) = 0;

  // Notifies the renderer whether hiding/showing the browser controls is
  // enabled, what the current state should be, and whether or not to animate to
  // the proper state.
  virtual void UpdateBrowserControlsState(cc::BrowserControlsState constraints,
                                          cc::BrowserControlsState current,
                                          bool animate) = 0;

  // Reloads the frame. It initiates a reload but doesn't wait for it to finish.
  // In some rare cases, there is no history related to the frame, nothing
  // happens and this returns false.
  virtual bool Reload() = 0;

  // Returns true if this frame has fired DOMContentLoaded.
  virtual bool IsDOMContentLoaded() = 0;

  // Update the ad frame state. The parameter |ad_frame_type| cannot be kNonAd,
  // and once this has been called, it cannot be called again with a different
  // |ad_frame_type|, since once a frame is determined to be an ad, it will stay
  // tagged as an ad of the same type for its entire lifetime.
  //
  // Note: The ad frame type is currently maintained and updated *outside*
  // content. This is used to ensure the render frame proxies are in sync (since
  // they aren't exposed in the public API). Eventually, we might be able to
  // simplify this somewhat (maybe //content would be responsible for
  // maintaining the state, with some content client method used to update it).
  virtual void UpdateAdFrameType(blink::mojom::AdFrameType ad_frame_type) = 0;

  // Perform security checks on Web Authentication requests. These can be
  // called by other |Authenticator| mojo interface implementations in the
  // browser process so that they don't have to duplicate security policies.
  // For requests originating from the render process, |effective_origin| will
  // be the same as the last committed origin. However, for request originating
  // from the browser process, this may be different.
  virtual blink::mojom::AuthenticatorStatus
  PerformGetAssertionWebAuthSecurityChecks(
      const std::string& relying_party_id,
      const url::Origin& effective_origin) = 0;
  virtual blink::mojom::AuthenticatorStatus
  PerformMakeCredentialWebAuthSecurityChecks(
      const std::string& relying_party_id,
      const url::Origin& effective_origin) = 0;

  // Tells the host that this is part of setting up a WebXR DOM Overlay. This
  // starts a short timer that permits entering fullscreen mode, similar to a
  // recent orientation change.
  virtual void SetIsXrOverlaySetup() = 0;

  // Returns true if this RenderFrameHost is currently stored in the
  // back-forward cache.
  //
  // TODO(hajimehoshi): Introduce an enum value for lifecycle states and replace
  // IsInBackForwardCache with the enum values and a new function like
  // DidChangeLifecycleState.
  virtual bool IsInBackForwardCache() = 0;

  // Returns the UKM source id for the page load (last committed cross-document
  // non-bfcache navigation in the main frame).
  // This id typically has an associated PageLoad UKM event.
  // Note: this can be called on any frame, but this id for all subframes is the
  // same as the id for the main frame.
  virtual ukm::SourceId GetPageUkmSourceId() = 0;

  // Report an inspector issue to devtools. Note that the issue is stored on the
  // browser-side, and may contain information that we don't want to share
  // with the renderer.
  // TODO(crbug.com/1091720): This reporting should be done directly in the
  // chrome layer in the future.
  virtual void ReportInspectorIssue(blink::mojom::InspectorIssueInfoPtr) = 0;

  // Returns whether a document uses WebOTP. Returns true if a WebOTPService is
  // created on the document.
  virtual bool DocumentUsedWebOTP() = 0;

  // Write a description of this RenderFrameHost into the provided |context|.
  virtual void WriteIntoTracedValue(perfetto::TracedValue&& context) = 0;

  // Start/stop event log output from WebRTC on this RFH for the peer connection
  // identified locally within the RFH using the ID `lid`.
  virtual void EnableWebRtcEventLogOutput(int lid, int output_period_ms) = 0;
  virtual void DisableWebRtcEventLogOutput(int lid) = 0;

 private:
  // This interface should only be implemented inside content.
  friend class RenderFrameHostImpl;
  RenderFrameHost() {}
};

}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_RENDER_FRAME_HOST_H_
