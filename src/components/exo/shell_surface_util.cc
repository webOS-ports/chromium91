// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/exo/shell_surface_util.h"

#include <memory>

#include "base/strings/string_number_conversions.h"
#include "base/trace_event/trace_event.h"
#include "build/chromeos_buildflags.h"
#include "components/exo/permission.h"
#include "components/exo/shell_surface_base.h"
#include "components/exo/surface.h"
#include "components/exo/wm_helper.h"
#include "ui/aura/client/capture_client.h"
#include "ui/aura/window.h"
#include "ui/aura/window_delegate.h"
#include "ui/aura/window_targeter.h"
#include "ui/events/base_event_utils.h"
#include "ui/events/event.h"
#include "ui/views/widget/widget.h"
#include "ui/wm/core/window_util.h"

#if BUILDFLAG(IS_CHROMEOS_ASH)
#include "chromeos/ui/base/window_properties.h"
#endif  // BUILDFLAG(IS_CHROMEOS_ASH)

namespace exo {

namespace {

DEFINE_UI_CLASS_PROPERTY_KEY(Surface*, kRootSurfaceKey, nullptr)

// Application Id set by the client. For example:
// "org.chromium.arc.<task-id>" for ARC++ shell surfaces.
// "org.chromium.lacros.<window-id>" for Lacros browser shell surfaces.
DEFINE_OWNED_UI_CLASS_PROPERTY_KEY(std::string, kApplicationIdKey, nullptr)

// Startup Id set by the client.
DEFINE_OWNED_UI_CLASS_PROPERTY_KEY(std::string, kStartupIdKey, nullptr)

// Accessibility Id set by the client.
DEFINE_UI_CLASS_PROPERTY_KEY(int32_t, kClientAccessibilityIdKey, -1)

// Returns true if the component for a located event should be taken care of
// by the window system.
bool ShouldHTComponentBlocked(int component) {
  switch (component) {
    case HTCAPTION:
    case HTCLOSE:
    case HTMAXBUTTON:
    case HTMINBUTTON:
    case HTMENU:
    case HTSYSMENU:
      return true;
    default:
      return false;
  }
}

// Find the lowest targeter in the parent chain.
aura::WindowTargeter* FindTargeter(ui::EventTarget* target) {
  do {
    ui::EventTargeter* targeter = target->GetEventTargeter();
    if (targeter)
      return static_cast<aura::WindowTargeter*>(targeter);
    target = target->GetParentTarget();
  } while (target);

  return nullptr;
}

}  // namespace

void SetShellApplicationId(ui::PropertyHandler* property_handler,
                           const base::Optional<std::string>& id) {
  TRACE_EVENT1("exo", "SetApplicationId", "application_id", id ? *id : "null");

  if (id)
    property_handler->SetProperty(kApplicationIdKey, *id);
  else
    property_handler->ClearProperty(kApplicationIdKey);
}

const std::string* GetShellApplicationId(const aura::Window* property_handler) {
  return property_handler->GetProperty(kApplicationIdKey);
}

void SetShellStartupId(ui::PropertyHandler* property_handler,
                       const base::Optional<std::string>& id) {
  TRACE_EVENT1("exo", "SetStartupId", "startup_id", id ? *id : "null");

  if (id)
    property_handler->SetProperty(kStartupIdKey, *id);
  else
    property_handler->ClearProperty(kStartupIdKey);
}

const std::string* GetShellStartupId(aura::Window* window) {
  return window->GetProperty(kStartupIdKey);
}

void SetShellUseImmersiveForFullscreen(aura::Window* window, bool value) {
#if BUILDFLAG(IS_CHROMEOS_ASH)
  window->SetProperty(chromeos::kImmersiveImpliedByFullscreen, value);

  // Ensure the shelf is fully hidden in plain fullscreen, but shown
  // (auto-hides based on mouse movement) when in immersive fullscreen.
  window->SetProperty(chromeos::kHideShelfWhenFullscreenKey, !value);
#endif  // BUILDFLAG(IS_CHROMEOS_ASH)
}

void SetShellClientAccessibilityId(aura::Window* window,
                                   const base::Optional<int32_t>& id) {
  TRACE_EVENT1("exo", "SetClientAccessibilityId", "id",
               id ? base::NumberToString(*id) : "null");

  if (id)
    window->SetProperty(kClientAccessibilityIdKey, *id);
  else
    window->ClearProperty(kClientAccessibilityIdKey);
}

const base::Optional<int32_t> GetShellClientAccessibilityId(
    aura::Window* window) {
  auto id = window->GetProperty(kClientAccessibilityIdKey);
  if (id < 0)
    return base::nullopt;
  else
    return id;
}

void SetShellRootSurface(ui::PropertyHandler* property_handler,
                         Surface* surface) {
  property_handler->SetProperty(kRootSurfaceKey, surface);
}

Surface* GetShellRootSurface(const aura::Window* window) {
  return window->GetProperty(kRootSurfaceKey);
}

ShellSurfaceBase* GetShellSurfaceBaseForWindow(aura::Window* window) {
  // Only windows with a surface can have a shell surface.
  if (!GetShellRootSurface(window))
    return nullptr;
  views::Widget* widget = views::Widget::GetWidgetForNativeWindow(window);
  if (!widget)
    return nullptr;
  return static_cast<ShellSurfaceBase*>(widget->widget_delegate());
}

Surface* GetTargetSurfaceForLocatedEvent(
    const ui::LocatedEvent* original_event) {
  aura::Window* window =
      WMHelper::GetInstance()->GetCaptureClient()->GetCaptureWindow();
  if (!window) {
    return Surface::AsSurface(
        static_cast<aura::Window*>(original_event->target()));
  }

  Surface* root_surface = GetShellRootSurface(window);
  // Skip if the event is captured by non exo windows.
  if (!root_surface) {
    auto* widget = views::Widget::GetTopLevelWidgetForNativeView(window);
    if (!widget)
      return nullptr;
    root_surface = GetShellRootSurface(widget->GetNativeWindow());
    if (!root_surface)
      return nullptr;

    ShellSurfaceBase* shell_surface_base =
        GetShellSurfaceBaseForWindow(widget->GetNativeWindow());
    // Check if it's overlay window.
    if (!shell_surface_base->host_window()->Contains(window) &&
        shell_surface_base->GetWidget()->GetNativeWindow() != window) {
      return nullptr;
    }
  }

  // Create a clone of the event as targeter may update it during the
  // search.
  auto cloned = ui::Event::Clone(*original_event);
  ui::LocatedEvent* event = cloned->AsLocatedEvent();

  while (true) {
    gfx::PointF location_in_target_f = event->location_f();
    gfx::Point location_in_target = event->location();
    ui::EventTarget* event_target = window;
    aura::WindowTargeter* targeter = FindTargeter(event_target);
    DCHECK(targeter);

    aura::Window* focused =
        static_cast<aura::Window*>(targeter->FindTargetForEvent(window, event));

    if (focused) {
      Surface* surface = Surface::AsSurface(focused);
      if (focused != window)
        return surface;
      else if (surface && surface->HitTest(location_in_target)) {
        // If the targeting fallback to the root (first) window, test the
        // hit region again.
        return surface;
      }
    }

    // If the event falls into the place where the window system should care
    // about (i.e. window caption), do not check the transient parent but just
    // return nullptr. See b/149517682.
    if (window->delegate() &&
        ShouldHTComponentBlocked(
            window->delegate()->GetNonClientComponent(location_in_target))) {
      return nullptr;
    }

    aura::Window* parent_window = wm::GetTransientParent(window);

    if (!parent_window)
      return root_surface;

    event->set_location_f(location_in_target_f);
    event_target->ConvertEventToTarget(parent_window, event);
    window = parent_window;
  }
}

void GrantPermissionToActivate(aura::Window* window, base::TimeDelta timeout) {
  // Activation is the only permission, so just set the property. The window
  // owns the Permission object.
  window->SetProperty(
      kPermissionKey,
      new Permission(Permission::Capability::kActivate, timeout));
}

void RevokePermissionToActivate(aura::Window* window) {
  // Activation is the only permission, so just clear the property.
  window->ClearProperty(kPermissionKey);
}

bool HasPermissionToActivate(aura::Window* window) {
  Permission* permission = window->GetProperty(kPermissionKey);
  return permission && permission->Check(Permission::Capability::kActivate);
}

bool ConsumedByIme(aura::Window* window, const ui::KeyEvent& event) {
  // When IME is blocked, Exo can handle any key events.
  if (WMHelper::GetInstance()->IsImeBlocked(window))
    return false;

  // Check if IME consumed the event, to avoid it to be doubly processed.
  // First let us see whether IME is active and is in text input mode.
  views::Widget* widget = views::Widget::GetTopLevelWidgetForNativeView(window);
  ui::InputMethod* ime = widget ? widget->GetInputMethod() : nullptr;
  if (!ime || ime->GetTextInputType() == ui::TEXT_INPUT_TYPE_NONE ||
      ime->GetTextInputType() == ui::TEXT_INPUT_TYPE_NULL) {
    return false;
  }

  // Case 1:
  // When IME ate a key event but did not emit character insertion event yet
  // (e.g., when it is still showing a candidate list UI to the user,) the
  // consumed key event is re-sent after masked |key_code| by VKEY_PROCESSKEY.
  if (event.key_code() == ui::VKEY_PROCESSKEY)
    return true;

  // Except for PROCESSKEY, never discard "key-up" events. A keydown not paired
  // by a keyup can trigger a never-ending key repeat in the client, which can
  // never be desirable.
  if (event.type() == ui::ET_KEY_RELEASED)
    return false;

  // Case 2:
  // When IME ate a key event and generated a single character input, it leaves
  // the key event as-is, and in addition calls the active ui::TextInputClient's
  // InsertChar() method. (In our case, arc::ArcImeService::InsertChar()).
  //
  // In Chrome OS (and Web) convention, the two calls won't cause duplicates,
  // because key-down events do not mean any character inputs there.
  // (InsertChar issues a DOM "keypress" event, which is distinct from keydown.)
  // Unfortunately, this is not necessary the case for our clients that may
  // treat keydown as a trigger of text inputs. We need suppression for keydown.
  //
  // Same condition as components/arc/ime/arc_ime_service.cc#InsertChar.
  const char16_t ch = event.GetCharacter();
  const bool is_control_char =
      (0x00 <= ch && ch <= 0x1f) || (0x7f <= ch && ch <= 0x9f);
  if (!is_control_char && !ui::IsSystemKeyModifier(event.flags()))
    return true;

  // Case 3:
  // Workaround for apps that doesn't handle hardware keyboard events well.
  // Keys typically on software keyboard and lack of them are fatal, namely,
  // unmodified enter and backspace keys, are sent through IME.
  constexpr int kModifierMask = ui::EF_SHIFT_DOWN | ui::EF_CONTROL_DOWN |
                                ui::EF_ALT_DOWN | ui::EF_COMMAND_DOWN |
                                ui::EF_ALTGR_DOWN | ui::EF_MOD3_DOWN;
  // Same condition as components/arc/ime/arc_ime_service.cc#InsertChar.
  if ((event.flags() & kModifierMask) == 0) {
    if (event.key_code() == ui::VKEY_RETURN ||
        event.key_code() == ui::VKEY_BACK) {
      return true;
    }
  }

  return false;
}

}  // namespace exo
