// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/aura/env.h"

#include "base/command_line.h"
#include "base/lazy_instance.h"
#include "base/memory/ptr_util.h"
#include "base/observer_list_types.h"
#include "build/build_config.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/aura/env_input_state_controller.h"
#include "ui/aura/env_observer.h"
#include "ui/aura/input_state_lookup.h"
#include "ui/aura/window.h"
#include "ui/aura/window_event_dispatcher_observer.h"
#include "ui/aura/window_occlusion_tracker.h"
#include "ui/base/ui_base_features.h"
#include "ui/events/event_observer.h"
#include "ui/events/event_target_iterator.h"
#include "ui/events/gestures/gesture_recognizer_impl.h"
#include "ui/events/platform/platform_event_source.h"

#if defined(OS_WIN)
#include "ui/base/cursor/win/win_cursor_factory.h"
#endif

#if defined(USE_OZONE)
#include "ui/ozone/public/ozone_platform.h"
#endif

#if defined(OS_WEBOS)
#include "ui/aura/window_tree_host.h"
#endif

#if defined(USE_X11)
#include "ui/base/x/x11_cursor_factory.h"
#endif

#if defined(OS_WIN) || defined(USE_X11)
#include "ui/gfx/switches.h"
#endif

namespace aura {

namespace {

// Instance created by all static functions, except
// CreateLocalInstanceForInProcess(). See GetInstance() for details.
Env* g_primary_instance = nullptr;

}  // namespace

// EventObserverAdapter is an aura::Env pre-target handler that forwards
// read-only events to its observer when they match the requested types.
class EventObserverAdapter : public ui::EventHandler,
                             public base::CheckedObserver {
 public:
  EventObserverAdapter(ui::EventObserver* observer,
                       ui::EventTarget* target,
                       const std::set<ui::EventType>& types)
      : observer_(observer), target_(target), types_(types) {
    target_->AddPreTargetHandler(this);
  }

  ~EventObserverAdapter() override { target_->RemovePreTargetHandler(this); }

  ui::EventObserver* observer() { return observer_; }
  ui::EventTarget* target() { return target_; }
  const std::set<ui::EventType>& types() const { return types_; }

  // ui::EventHandler:
  void OnEvent(ui::Event* event) override {
    if (types_.count(event->type()) > 0) {
      std::unique_ptr<ui::Event> cloned_event = ui::Event::Clone(*event);
      ui::Event::DispatcherApi(cloned_event.get()).set_target(event->target());
      // The root location of located events should be in screen coordinates.
      if (cloned_event->IsLocatedEvent() && cloned_event->target()) {
        ui::LocatedEvent* located_event = cloned_event->AsLocatedEvent();
        auto root = located_event->target()->GetScreenLocationF(*located_event);
        located_event->set_root_location_f(root);
      }
      observer_->OnEvent(*cloned_event);
    }
  }

 private:
  ui::EventObserver* observer_;
  ui::EventTarget* target_;
  const std::set<ui::EventType> types_;

  DISALLOW_COPY_AND_ASSIGN(EventObserverAdapter);
};

////////////////////////////////////////////////////////////////////////////////
// Env, public:

Env::~Env() {
  for (EnvObserver& observer : observers_)
    observer.OnWillDestroyEnv();

  if (this == g_primary_instance)
    g_primary_instance = nullptr;
}

// static
std::unique_ptr<Env> Env::CreateInstance() {
  DCHECK(!g_primary_instance);
  // No make_unique as constructor is private.
  std::unique_ptr<Env> env(new Env());
  g_primary_instance = env.get();
  env->Init();
  return env;
}

// static
Env* Env::GetInstance() {
  Env* env = g_primary_instance;
  DCHECK(env) << "Env::CreateInstance must be called before getting the "
                 "instance of Env.";
  return env;
}

// static
bool Env::HasInstance() {
  return !!g_primary_instance;
}

#if defined(OS_WEBOS)
// static
Window* Env::GetRootWindow() {
  return GetInstance()->RootWindow();
}
#endif

void Env::AddObserver(EnvObserver* observer) {
  observers_.AddObserver(observer);
}

void Env::RemoveObserver(EnvObserver* observer) {
  observers_.RemoveObserver(observer);
}

void Env::AddWindowEventDispatcherObserver(
    WindowEventDispatcherObserver* observer) {
  window_event_dispatcher_observers_.AddObserver(observer);
}

void Env::RemoveWindowEventDispatcherObserver(
    WindowEventDispatcherObserver* observer) {
  window_event_dispatcher_observers_.RemoveObserver(observer);
}

bool Env::IsMouseButtonDown() const {
  return input_state_lookup_.get() ? input_state_lookup_->IsMouseButtonDown() :
      mouse_button_flags_ != 0;
}

void Env::SetLastMouseLocation(const gfx::Point& last_mouse_location) {
  last_mouse_location_ = last_mouse_location;
}

void Env::SetGestureRecognizer(
    std::unique_ptr<ui::GestureRecognizer> gesture_recognizer) {
  gesture_recognizer_ = std::move(gesture_recognizer);
}

WindowOcclusionTracker* Env::GetWindowOcclusionTracker() {
  if (!window_occlusion_tracker_) {
    // Use base::WrapUnique + new because of the constructor is private.
    window_occlusion_tracker_ = base::WrapUnique(new WindowOcclusionTracker());
  }

  return window_occlusion_tracker_.get();
}

void Env::PauseWindowOcclusionTracking() {
  GetWindowOcclusionTracker()->Pause();
}

void Env::UnpauseWindowOcclusionTracking() {
  GetWindowOcclusionTracker()->Unpause();
}

void Env::AddEventObserver(ui::EventObserver* observer,
                           ui::EventTarget* target,
                           const std::set<ui::EventType>& types) {
  DCHECK(!types.empty()) << "Observers must observe at least one event type";
  auto adapter(std::make_unique<EventObserverAdapter>(observer, target, types));
  event_observer_adapter_list_.AddObserver(adapter.get());
  event_observer_adapters_.insert(std::move(adapter));
}

void Env::RemoveEventObserver(ui::EventObserver* observer) {
  for (auto& adapter : event_observer_adapters_) {
    if (adapter->observer() == observer) {
      event_observer_adapter_list_.RemoveObserver(adapter.get());
      event_observer_adapters_.erase(adapter);
      return;
    }
  }
}

void Env::NotifyEventObservers(const ui::Event& event) {
  for (auto& adapter : event_observer_adapter_list_) {
    if (adapter.types().count(event.type()) > 0 &&
        (adapter.target() == event.target() || adapter.target() == this)) {
      adapter.observer()->OnEvent(event);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// Env, private:

// static
bool Env::initial_throttle_input_on_resize_ = true;

Env::Env()
    : env_controller_(std::make_unique<EnvInputStateController>(this)),
      gesture_recognizer_(std::make_unique<ui::GestureRecognizerImpl>()),
      input_state_lookup_(InputStateLookup::Create()) {
#if defined(OS_WIN) || defined(USE_X11)
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(switches::kHeadless)) {
#if defined(USE_X11)
    // In Ozone/X11, the cursor factory is initialized by the platform
    // initialization code.
    if (!features::IsUsingOzonePlatform())
      cursor_factory_ = std::make_unique<ui::X11CursorFactory>();
#else
    cursor_factory_ = std::make_unique<ui::WinCursorFactory>();
#endif
  }
#endif
}

void Env::Init() {
#if defined(USE_OZONE)
  // The ozone platform can provide its own event source. So initialize the
  // platform before creating the default event source. If running inside mus
  // let the mus process initialize ozone instead.
  if (features::IsUsingOzonePlatform()) {
    ui::OzonePlatform::InitParams params;
    base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
    // TODO(kylechar): Pass in single process information to
    // Env::CreateInstance() instead of checking flags here.
    params.single_process = command_line->HasSwitch("single-process") ||
                            command_line->HasSwitch("in-process-gpu");
    ui::OzonePlatform::InitializeForUI(params);
  }
#endif
  if (!ui::PlatformEventSource::GetInstance())
    event_source_ = ui::PlatformEventSource::CreateDefault();
}

void Env::NotifyWindowInitialized(Window* window) {
  for (EnvObserver& observer : observers_)
    observer.OnWindowInitialized(window);
}

void Env::NotifyHostInitialized(WindowTreeHost* host) {
#if defined(OS_WEBOS)
  if (host)
    root_window_ = host->window();
#endif

  window_tree_hosts_.push_back(host);
  for (EnvObserver& observer : observers_)
    observer.OnHostInitialized(host);
}

void Env::NotifyHostDestroyed(WindowTreeHost* host) {
  base::Erase(window_tree_hosts_, host);
  for (EnvObserver& observer : observers_)
    observer.OnHostDestroyed(host);
}

////////////////////////////////////////////////////////////////////////////////
// Env, ui::EventTarget implementation:

bool Env::CanAcceptEvent(const ui::Event& event) {
  return true;
}

ui::EventTarget* Env::GetParentTarget() {
  return nullptr;
}

std::unique_ptr<ui::EventTargetIterator> Env::GetChildIterator() const {
  return nullptr;
}

ui::EventTargeter* Env::GetEventTargeter() {
  NOTREACHED();
  return nullptr;
}

}  // namespace aura
