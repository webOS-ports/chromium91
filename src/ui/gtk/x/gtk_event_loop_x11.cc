// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/gtk/x/gtk_event_loop_x11.h"

#include <gtk/gtk.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include "ui/base/x/x11_util.h"
#include "ui/gfx/x/event.h"

extern "C" {
#if BUILDFLAG(GTK_VERSION) >= 4
unsigned long gdk_x11_surface_get_xid(GdkSurface* surface);
#else
unsigned long gdk_x11_window_get_xid(GdkWindow* window);
#endif
}

namespace ui {

namespace {

x11::KeyButMask BuildXkbStateFromGdkEvent(unsigned int state,
                                          unsigned char group) {
  return static_cast<x11::KeyButMask>(state | ((group & 0x3) << 13));
}

x11::KeyEvent ConvertGdkEventToKeyEvent(GdkEvent* gdk_event) {
#if BUILDFLAG(GTK_VERSION) >= 4
  GdkKeymapKey* keys = nullptr;
  guint* keyvals = nullptr;
  gint n_entries = 0;
  gdk_display_map_keycode(gdk_display_get_default(),
                          gdk_key_event_get_keycode(gdk_event), &keys, &keyvals,
                          &n_entries);
  guint keyval = gdk_key_event_get_keyval(gdk_event);
  GdkKeymapKey keymap_key{0, 0, 0};
  if (keys) {
    for (gint i = 0; i < n_entries; i++) {
      if (keyvals[i] == keyval) {
        keymap_key = keys[i];
        break;
      }
    }
    g_free(keys);
    g_free(keyvals);
  }

  return {
      .opcode = gdk_event_get_event_type(gdk_event) == GDK_KEY_PRESS
                    ? x11::KeyEvent::Press
                    : x11::KeyEvent::Release,
      .detail = static_cast<x11::KeyCode>(keymap_key.keycode),
      .time = static_cast<x11::Time>(gdk_event_get_time(gdk_event)),
      .root = ui::GetX11RootWindow(),
      .event = static_cast<x11::Window>(
          gdk_x11_surface_get_xid(gdk_event_get_surface(gdk_event))),
      .state = BuildXkbStateFromGdkEvent(
          gdk_event_get_modifier_state(gdk_event), keymap_key.group),
      .same_screen = true,
  };
#else
  return {
      .opcode = gdk_event->key.type == GDK_KEY_PRESS ? x11::KeyEvent::Press
                                                     : x11::KeyEvent::Release,
      .send_event = gdk_event->key.send_event,
      .detail = static_cast<x11::KeyCode>(gdk_event->key.hardware_keycode),
      .time = static_cast<x11::Time>(gdk_event->key.time),
      .root = ui::GetX11RootWindow(),
      .event = static_cast<x11::Window>(
          gdk_x11_window_get_xid(gdk_event->key.window)),
      .state =
          BuildXkbStateFromGdkEvent(gdk_event->key.state, gdk_event->key.group),
      .same_screen = true,
  };
#endif
}

void ProcessGdkEvent(GdkEvent* gdk_event) {
  // This function translates GdkEventKeys into XKeyEvents and puts them to
  // the X event queue.
  //
  // base::MessagePumpX11 is using the X11 event queue and all key events should
  // be processed there.  However, there are cases(*1) that GdkEventKeys are
  // created instead of XKeyEvents.  In these cases, we have to translate
  // GdkEventKeys to XKeyEvents and puts them to the X event queue so our main
  // event loop can handle those key events.
  //
  // (*1) At least ibus-gtk in async mode creates a copy of user's key event and
  // pushes it back to the GDK event queue.  In this case, there is no
  // corresponding key event in the X event queue.  So we have to handle this
  // case.  ibus-gtk is used through gtk-immodule to support IMEs.

#if BUILDFLAG(GTK_VERSION) >= 4
  auto event_type = gdk_event_get_event_type(gdk_event);
#else
  auto event_type = gdk_event->type;
#endif
  switch (event_type) {
    case GDK_KEY_PRESS:
    case GDK_KEY_RELEASE:
      break;
    default:
      return;
  }

  // We want to process the gtk event; mapped to an X11 event immediately
  // otherwise if we put it back on the queue we may get items out of order.
  x11::Connection::Get()->DispatchEvent(
      x11::Event{ConvertGdkEventToKeyEvent(gdk_event)});
}

}  // namespace

GtkEventLoopX11::GtkEventLoopX11(GtkWidget* widget) {
#if BUILDFLAG(GTK_VERSION) >= 4
  surface_ = gtk_native_get_surface(gtk_widget_get_native(widget));
  signal_id_ =
      g_signal_connect(surface_, "event", G_CALLBACK(OnEventThunk), this);
#else
  gdk_event_handler_set(DispatchGdkEvent, nullptr, nullptr);
#endif
}

GtkEventLoopX11::~GtkEventLoopX11() {
#if BUILDFLAG(GTK_VERSION) >= 4
  g_signal_handler_disconnect(surface_, signal_id_);
#else
  gdk_event_handler_set(reinterpret_cast<GdkEventFunc>(gtk_main_do_event),
                        nullptr, nullptr);
#endif
}

#if BUILDFLAG(GTK_VERSION) >= 4
gboolean GtkEventLoopX11::OnEvent(GdkEvent* gdk_event) {
  ProcessGdkEvent(gdk_event);
  return false;
}
#else
// static
void GtkEventLoopX11::DispatchGdkEvent(GdkEvent* gdk_event, gpointer) {
  ProcessGdkEvent(gdk_event);
  gtk_main_do_event(gdk_event);
}
#endif

}  // namespace ui
