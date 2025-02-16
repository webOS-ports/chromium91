// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PDF_PAINT_MANAGER_H_
#define PDF_PAINT_MANAGER_H_

#include <stdint.h>

#include <memory>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "pdf/paint_aggregator.h"
#include "pdf/ppapi_migration/callback.h"
#include "ui/gfx/geometry/size.h"

namespace base {
class Location;
}  // namespace base

namespace gfx {
class Point;
class Rect;
class Vector2d;
}  // namespace gfx

namespace chrome_pdf {

class Graphics;

// Custom PaintManager for the PDF plugin.  This is branched from the Pepper
// version.  The difference is that this supports progressive rendering of dirty
// rects, where multiple calls to the rendering engine are needed.  It also
// supports having higher-priority rects flushing right away, i.e. the
// scrollbars.
//
// The client's OnPaint
class PaintManager {
 public:
  class Client {
   public:
    // Creates a new, unbound `Graphics` for the paint manager, with the given
    // |size| and always-opaque rendering.
    virtual std::unique_ptr<Graphics> CreatePaintGraphics(
        const gfx::Size& size) = 0;

    // Binds a `Graphics` created by `CreatePaintGraphics()`, returning `true`
    // if binding was successful.
    virtual bool BindPaintGraphics(Graphics& graphics) = 0;

    // Paints the given invalid area of the plugin to the given graphics
    // device. Returns true if anything was painted.
    //
    // You are given the list of rects to paint in `paint_rects`.  You can
    // combine painting into less rectangles if it's more efficient.  When a
    // rect is painted, information about that paint should be inserted into
    // `ready`.  Otherwise if a paint needs more work, add the rect to
    // `pending`.  If `pending` is not empty, your OnPaint function will get
    // called again.  Once OnPaint is called and it returns no pending rects,
    // all the previously ready rects will be flushed on screen.  The exception
    // is for ready rects that have `flush_now` set to true.  These will be
    // flushed right away.
    //
    // Do not call Flush() on the graphics device, this will be done
    // automatically if you return true from this function since the
    // PaintManager needs to handle the callback.
    //
    // Calling Invalidate/Scroll is not allowed while inside an OnPaint
    virtual void OnPaint(const std::vector<gfx::Rect>& paint_rects,
                         std::vector<PaintReadyRect>& ready,
                         std::vector<gfx::Rect>& pending) = 0;

    // Schedules work to be executed on a main thread after a specific delay.
    // The `result` parameter will be passed as the argument to the `callback`.
    // `result` is needed sometimes to emulate calls of some callbacks, but it's
    // not always needed. `delay` should be no longer than `INT32_MAX`
    // milliseconds for the Pepper plugin implementation to prevent integer
    // overflow.
    virtual void ScheduleTaskOnMainThread(const base::Location& from_here,
                                          ResultCallback callback,
                                          int32_t result,
                                          base::TimeDelta delay) = 0;

   protected:
    // You shouldn't be doing deleting through this interface.
    ~Client() = default;
  };

  // The Client is a non-owning pointer and must remain valid (normally the
  // object implementing the Client interface will own the paint manager).
  //
  // You will need to call SetSize() before this class will do anything.
  // Normally you do this from UpdateGeometryOnViewChanged() of your plugin
  // instance.
  explicit PaintManager(Client* client);
  PaintManager(const PaintManager&) = delete;
  PaintManager& operator=(const PaintManager&) = delete;
  ~PaintManager();

  // Returns the size of the graphics context to allocate for a given plugin
  // size. We may allocated a slightly larger buffer than required so that we
  // don't have to resize the context when scrollbars appear/dissapear due to
  // zooming (which can result in flickering).
  static gfx::Size GetNewContextSize(const gfx::Size& current_context_size,
                                     const gfx::Size& plugin_size);

  // Sets the size of the plugin. If the size is the same as the previous call,
  // this will be a NOP. If the size has changed, a new device will be
  // allocated to the given size and a paint to that device will be scheduled.
  //
  // This is intended to be called from ViewChanged with the size of the
  // plugin. Since it tracks the old size and only allocates when the size
  // changes, you can always call this function without worrying about whether
  // the size changed or ViewChanged is called for another reason (like the
  // position changed).
  void SetSize(const gfx::Size& new_size, float new_device_scale);

  // Invalidate the entire plugin.
  void Invalidate();

  // Invalidate the given rect.
  void InvalidateRect(const gfx::Rect& rect);

  // The given rect should be scrolled by the given amounts.
  void ScrollRect(const gfx::Rect& clip_rect, const gfx::Vector2d& amount);

  // Returns the size of the graphics context for the next paint operation.
  // This is the pending size if a resize is pending (the plugin has called
  // SetSize but we haven't actually painted it yet), or the current size of
  // no resize is pending.
  gfx::Size GetEffectiveSize() const;
  float GetEffectiveDeviceScale() const;

  // Set the transform for the graphics layer.
  // If |schedule_flush| is true, it ensures a flush will be scheduled for
  // this change. If |schedule_flush| is false, then the change will not take
  // effect until another change causes a flush.
  void SetTransform(float scale,
                    const gfx::Point& origin,
                    const gfx::Vector2d& translate,
                    bool schedule_flush);
  // Resets any transform for the graphics layer.
  // This does not schedule a flush.
  void ClearTransform();

 private:
  // Makes sure there is a callback that will trigger a paint at a later time.
  // This will be either a Flush callback telling us we're allowed to generate
  // more data, or, if there's no flush callback pending, a manual call back
  // to the message loop via ExecuteOnMainThread.
  void EnsureCallbackPending();

  // Does the client paint and executes a Flush if necessary.
  void DoPaint();

  // Executes a Flush.
  void Flush();

  // Callback for asynchronous completion of Flush.
  void OnFlushComplete(int32_t);

  // Callback for manual scheduling of paints when there is no flush callback
  // pending.
  void OnManualCallbackComplete(int32_t);

  // Non-owning pointer. See the constructor.
  Client* const client_;

  // This graphics device will be null if no graphics has been set yet.
  std::unique_ptr<Graphics> graphics_;

  PaintAggregator aggregator_;

  // See comment for EnsureCallbackPending for more on how these work.
  bool manual_callback_pending_ = false;
  bool flush_pending_ = false;
  bool flush_requested_ = false;

  // When we get a resize, we don't bind right away (see SetSize). The
  // has_pending_resize_ tells us that we need to do a resize for the next
  // paint operation. When true, the new size is in pending_size_.
  bool has_pending_resize_ = false;
  bool graphics_need_to_be_bound_ = false;
  gfx::Size pending_size_;
  gfx::Size plugin_size_;
  float pending_device_scale_ = 1.0f;
  float device_scale_ = 1.0f;

  // True iff we're in the middle of a paint.
  bool in_paint_ = false;

  // True if we haven't painted the plugin viewport yet.
  bool first_paint_ = true;

  // True when the view size just changed and we're waiting for a paint.
  bool view_size_changed_waiting_for_paint_ = false;

  base::WeakPtrFactory<PaintManager> weak_factory_{this};
};

}  // namespace chrome_pdf

#endif  // PDF_PAINT_MANAGER_H_
