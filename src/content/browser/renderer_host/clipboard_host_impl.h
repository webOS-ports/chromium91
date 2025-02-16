// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_RENDERER_HOST_CLIPBOARD_HOST_IMPL_H_
#define CONTENT_BROWSER_RENDERER_HOST_CLIPBOARD_HOST_IMPL_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/optional.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "content/browser/renderer_host/render_frame_host_impl.h"
#include "content/common/content_export.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "third_party/blink/public/mojom/clipboard/clipboard.mojom.h"
#include "ui/base/clipboard/clipboard.h"

class GURL;

namespace ui {
class ScopedClipboardWriter;
}  // namespace ui

namespace content {

class ClipboardHostImplTest;

class CONTENT_EXPORT ClipboardHostImpl : public blink::mojom::ClipboardHost {
 public:
  ~ClipboardHostImpl() override;

  static void Create(
      RenderFrameHost* render_frame_host,
      mojo::PendingReceiver<blink::mojom::ClipboardHost> receiver);

 protected:
  // These types and methods are protected for testing.

  using ClipboardPasteContentAllowed =
      RenderFrameHostImpl::ClipboardPasteContentAllowed;
  using IsClipboardPasteContentAllowedCallback =
      RenderFrameHostImpl::IsClipboardPasteContentAllowedCallback;

  // Keeps track of a request to see if some clipboard content, identified by
  // its sequence number, is allowed to be pasted into the render frame host
  // that owns this clipboard host.
  //
  // A request starts in the state incomplete until Complete() is called with
  // a value.  Callbacks can be added to the request before or after it has
  // completed.
  class CONTENT_EXPORT IsPasteContentAllowedRequest {
   public:
    IsPasteContentAllowedRequest();
    ~IsPasteContentAllowedRequest();

    // Adds |callback| to be notified when the request completes.  If the
    // request is already completed |callback| is invoked immediately.  Returns
    // true if a request should be started after adding this callback.
    bool AddCallback(IsClipboardPasteContentAllowedCallback callback);

    // Mark this request as completed with the specified result.
    // Invoke all callbacks now.
    void Complete(ClipboardPasteContentAllowed allowed);

    // Returns true if this request is obsolete.  An obsolete request
    // is one that is completed, all registered callbacks have been
    // called, and is considered old.
    //
    // |now| represents the current time.  It is an argument to ease testing.
    bool IsObsolete(base::Time now);

    // Returns the time at which this request was created.
    base::Time time() { return time_; }

   private:
    // Calls all the callbacks in |callbacks_| with the current value of
    // |allowed_|.  |allowed_| must not be empty.
    void InvokeCallbacks();

    base::Time time_{base::Time::Now()};
    base::Optional<ClipboardPasteContentAllowed> allowed_;
    std::vector<IsClipboardPasteContentAllowedCallback> callbacks_;
  };

  // A paste allowed request is obsolete if it is older than this time.
  static const base::TimeDelta kIsPasteContentAllowedRequestTooOld;

  explicit ClipboardHostImpl(RenderFrameHost* render_frame_host);

  // Performs a check to see if pasting `data` is allowed by data transfer
  // policies and invokes PasteIfPolicyAllowedCallback upon completion.
  // PerformPasteIfContentAllowed maybe be invoked immediately if the policy
  // controller doesn't exist.
  void PasteIfPolicyAllowed(ui::ClipboardBuffer clipboard_buffer,
                            const ui::ClipboardFormatType& data_type,
                            std::string data,
                            IsClipboardPasteContentAllowedCallback callback);

  // Performs a check to see if pasting |data| is allowed and invokes |callback|
  // upon completion. |callback| may be invoked immediately if the data has
  // already been checked. |data| and |seqno| should corresponds to the same
  // clipboard data.
  void PerformPasteIfContentAllowed(
      uint64_t seqno,
      const ui::ClipboardFormatType& data_type,
      std::string data,
      IsClipboardPasteContentAllowedCallback callback);

  // Remove obsolete entries from the outstanding requests map.
  // A request is obsolete if:
  //  - its sequence number is less than |seqno|
  //  - it has no callbacks
  //  - it is too old
  void CleanupObsoleteRequests();

  // Completion callback of PerformPasteIfContentAllowed(). Sets the allowed
  // status for the clipboard data corresponding to sequence number |seqno|.
  void FinishPasteIfContentAllowed(uint64_t seqno,
                                   ClipboardPasteContentAllowed allowed);

  const std::map<uint64_t, IsPasteContentAllowedRequest>&
  is_paste_allowed_requests_for_testing() {
    return is_allowed_requests_;
  }

 private:
  friend class ClipboardHostImplTest;
  friend class ClipboardHostImplScanTest;
  FRIEND_TEST_ALL_PREFIXES(ClipboardHostImplTest,
                           IsPasteContentAllowedRequest_AddCallback);
  FRIEND_TEST_ALL_PREFIXES(ClipboardHostImplTest,
                           IsPasteContentAllowedRequest_Complete);
  FRIEND_TEST_ALL_PREFIXES(ClipboardHostImplTest,
                           IsPasteContentAllowedRequest_IsObsolete);
  FRIEND_TEST_ALL_PREFIXES(ClipboardHostImplScanTest,
                           PerformPasteIfContentAllowed_EmptyData);
  FRIEND_TEST_ALL_PREFIXES(ClipboardHostImplScanTest,
                           PerformPasteIfContentAllowed);

  // mojom::ClipboardHost
  void GetSequenceNumber(ui::ClipboardBuffer clipboard_buffer,
                         GetSequenceNumberCallback callback) override;
  void IsFormatAvailable(blink::mojom::ClipboardFormat format,
                         ui::ClipboardBuffer clipboard_buffer,
                         IsFormatAvailableCallback callback) override;
  void ReadAvailableTypes(ui::ClipboardBuffer clipboard_buffer,
                          ReadAvailableTypesCallback callback) override;
  void ReadText(ui::ClipboardBuffer clipboard_buffer,
                ReadTextCallback callback) override;
  void ReadHtml(ui::ClipboardBuffer clipboard_buffer,
                ReadHtmlCallback callback) override;
  void ReadSvg(ui::ClipboardBuffer clipboard_buffer,
               ReadSvgCallback callback) override;
  void ReadRtf(ui::ClipboardBuffer clipboard_buffer,
               ReadRtfCallback callback) override;
  void ReadImage(ui::ClipboardBuffer clipboard_buffer,
                 ReadImageCallback callback) override;
  void ReadFiles(ui::ClipboardBuffer clipboard_buffer,
                 ReadFilesCallback callback) override;
  void ReadCustomData(ui::ClipboardBuffer clipboard_buffer,
                      const std::u16string& type,
                      ReadCustomDataCallback callback) override;
  void WriteText(const std::u16string& text) override;
  void WriteHtml(const std::u16string& markup, const GURL& url) override;
  void WriteSvg(const std::u16string& markup) override;
  void WriteSmartPasteMarker() override;
  void WriteCustomData(
      const base::flat_map<std::u16string, std::u16string>& data) override;
  void WriteBookmark(const std::string& url,
                     const std::u16string& title) override;
  void WriteImage(const SkBitmap& unsafe_bitmap) override;
  void CommitWrite() override;
#if defined(OS_MAC)
  void WriteStringToFindPboard(const std::u16string& text) override;
#endif

  // Called by PerformPasteIfContentAllowed() when an is allowed request is
  // needed. Virtual to be overridden in tests.
  virtual void StartIsPasteContentAllowedRequest(
      uint64_t seqno,
      const ui::ClipboardFormatType& data_type,
      std::string data);

  // Completion callback of PasteIfPolicyAllowed. If `is_allowed` is set to
  // true, PerformPasteIfContentAllowed will be invoked. Otherwise `callback`
  // will be invoked immediately to cancel the paste.
  void PasteIfPolicyAllowedCallback(
      ui::ClipboardBuffer clipboard_buffer,
      const ui::ClipboardFormatType& data_type,
      std::string data,
      IsClipboardPasteContentAllowedCallback callback,
      bool is_allowed);

  void OnReadImage(ui::ClipboardBuffer clipboard_buffer,
                   ReadImageCallback callback,
                   const SkBitmap& bitmap);

  std::unique_ptr<ui::DataTransferEndpoint> CreateDataEndpoint();

  ui::Clipboard* const clipboard_;  // Not owned
  GlobalFrameRoutingId render_frame_routing_id_;
  std::unique_ptr<ui::ScopedClipboardWriter> clipboard_writer_;

  // Outstanding is allowed requests per clipboard contents.  Maps a clipboard
  // sequence number to an outstanding request.
  std::map<uint64_t, IsPasteContentAllowedRequest> is_allowed_requests_;

  base::WeakPtrFactory<ClipboardHostImpl> weak_ptr_factory_{this};
};

}  // namespace content

#endif  // CONTENT_BROWSER_RENDERER_HOST_CLIPBOARD_HOST_IMPL_H_
