// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/ozone/platform/wayland/ozone_platform_wayland.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/memory/ptr_util.h"
#include "base/message_loop/message_pump_type.h"
#include "base/no_destructor.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "ui/base/buildflags.h"
#include "ui/base/cursor/cursor_factory.h"
#include "ui/base/ime/linux/input_method_auralinux.h"
#include "ui/base/ui_base_features.h"
#include "ui/events/devices/device_data_manager.h"
#include "ui/events/ozone/layout/keyboard_layout_engine_manager.h"
#include "ui/gfx/linux/client_native_pixmap_dmabuf.h"
#include "ui/gfx/native_widget_types.h"
#include "ui/ozone/common/features.h"
#include "ui/ozone/platform/wayland/common/wayland_util.h"
#include "ui/ozone/platform/wayland/gpu/drm_render_node_path_finder.h"
#include "ui/ozone/platform/wayland/gpu/wayland_buffer_manager_gpu.h"
#include "ui/ozone/platform/wayland/gpu/wayland_overlay_manager.h"
#include "ui/ozone/platform/wayland/gpu/wayland_surface_factory.h"
#include "ui/ozone/platform/wayland/host/wayland_buffer_manager_connector.h"
#include "ui/ozone/platform/wayland/host/wayland_buffer_manager_host.h"
#include "ui/ozone/platform/wayland/host/wayland_connection.h"
#include "ui/ozone/platform/wayland/host/wayland_cursor_factory.h"
#include "ui/ozone/platform/wayland/host/wayland_input_method_context_factory.h"
#include "ui/ozone/platform/wayland/host/wayland_menu_utils.h"
#include "ui/ozone/platform/wayland/host/wayland_output_manager.h"
#include "ui/ozone/platform/wayland/host/wayland_window.h"
#include "ui/ozone/public/gpu_platform_support_host.h"
#include "ui/ozone/public/input_controller.h"
#include "ui/ozone/public/ozone_platform.h"
#include "ui/ozone/public/platform_menu_utils.h"
#include "ui/ozone/public/system_input_injector.h"
#include "ui/platform_window/platform_window_init_properties.h"

#if BUILDFLAG(USE_XKBCOMMON)
#include "ui/events/ozone/layout/xkb/xkb_evdev_codes.h"
#include "ui/events/ozone/layout/xkb/xkb_keyboard_layout_engine.h"
#else
#include "ui/events/ozone/layout/stub/stub_keyboard_layout_engine.h"
#endif

#include "ui/gfx/buffer_format_util.h"

#if defined(WAYLAND_GBM)
#include "ui/base/ui_base_features.h"
#include "ui/gfx/linux/gbm_wrapper.h"  // nogncheck
#include "ui/ozone/platform/wayland/gpu/drm_render_node_handle.h"
#endif

#if defined(OZONE_PLATFORM_WAYLAND_EXTERNAL)
#include "ui/ozone/public/gpu_platform_support.h"
#endif

#if BUILDFLAG(USE_GTK)
#include "ui/gtk/gtk_ui_delegate.h"  // nogncheck
#include "ui/ozone/platform/wayland/host/gtk_ui_delegate_wayland.h"  //nogncheck
#endif

#if defined(USE_NEVA_MEDIA)
#include "ui/ozone/common/neva/video_window_controller_impl.h"
#include "ui/ozone/common/neva/video_window_provider_mojo.h"
#endif  // defined(USE_NEVA_MEDIA)

namespace ui {

namespace {

class OzonePlatformWayland : public OzonePlatform {
 public:
  OzonePlatformWayland() { CHECK(features::IsUsingOzonePlatform()); }
  ~OzonePlatformWayland() override {}

  // OzonePlatform
  SurfaceFactoryOzone* GetSurfaceFactoryOzone() override {
    return surface_factory_.get();
  }

  OverlayManagerOzone* GetOverlayManager() override {
    return overlay_manager_.get();
  }

  CursorFactory* GetCursorFactory() override { return cursor_factory_.get(); }

  InputController* GetInputController() override {
    return input_controller_.get();
  }

#if defined(OZONE_PLATFORM_WAYLAND_EXTERNAL)
  GpuPlatformSupport* GetGpuPlatformSupport() override {
    return gpu_platform_support_.get();
  }
#endif

  GpuPlatformSupportHost* GetGpuPlatformSupportHost() override {
    return buffer_manager_connector_ ? buffer_manager_connector_.get()
                                     : gpu_platform_support_host_.get();
  }

  std::unique_ptr<SystemInputInjector> CreateSystemInputInjector() override {
    return nullptr;
  }

  std::unique_ptr<PlatformWindow> CreatePlatformWindow(
      PlatformWindowDelegate* delegate,
      PlatformWindowInitProperties properties) override {
    return WaylandWindow::Create(delegate, connection_.get(),
                                 std::move(properties));
  }

  std::unique_ptr<display::NativeDisplayDelegate> CreateNativeDisplayDelegate()
      override {
    return nullptr;
  }

  std::unique_ptr<PlatformScreen> CreateScreen() override {
    // The WaylandConnection and the WaylandOutputManager must be created
    // before PlatformScreen.
    DCHECK(connection_ && connection_->wayland_output_manager());
    return connection_->wayland_output_manager()->CreateWaylandScreen();
  }

  PlatformClipboard* GetPlatformClipboard() override {
    DCHECK(connection_);
    return connection_->clipboard();
  }

  std::unique_ptr<InputMethod> CreateInputMethod(
      internal::InputMethodDelegate* delegate,
      gfx::AcceleratedWidget widget) override {
    // Instantiate and set LinuxInputMethodContextFactory unless it is already
    // set (e.g: tests may have already set it).
    if (!LinuxInputMethodContextFactory::instance() &&
        !input_method_context_factory_) {
      input_method_context_factory_ =
          std::make_unique<WaylandInputMethodContextFactory>(connection_.get());
      LinuxInputMethodContextFactory::SetInstance(
          input_method_context_factory_.get());
    }

    return std::make_unique<InputMethodAuraLinux>(delegate);
  }

  PlatformMenuUtils* GetPlatformMenuUtils() override {
    return menu_utils_.get();
  }

  bool IsNativePixmapConfigSupported(gfx::BufferFormat format,
                                     gfx::BufferUsage usage) const override {
    // If there is no drm render node device available, native pixmaps are not
    // supported.
    if (path_finder_.GetDrmRenderNodePath().empty())
      return false;

    if (supported_buffer_formats_.find(format) ==
        supported_buffer_formats_.end()) {
      return false;
    }

    return gfx::ClientNativePixmapDmaBuf::IsConfigurationSupported(format,
                                                                   usage);
  }

  void InitializeUI(const InitParams& args) override {
    // Initialize DeviceDataManager early as devices are set during
    // WaylandConnection::Initialize().
    DeviceDataManager::CreateInstance();
#if BUILDFLAG(USE_XKBCOMMON)
    keyboard_layout_engine_ =
        std::make_unique<XkbKeyboardLayoutEngine>(xkb_evdev_code_converter_);
#else
    keyboard_layout_engine_ = std::make_unique<StubKeyboardLayoutEngine>();
#endif
    KeyboardLayoutEngineManager::SetKeyboardLayoutEngine(
        keyboard_layout_engine_.get());
    connection_ = std::make_unique<WaylandConnection>();
    if (!connection_->Initialize())
      LOG(FATAL) << "Failed to initialize Wayland platform";

    buffer_manager_connector_ = std::make_unique<WaylandBufferManagerConnector>(
        connection_->buffer_manager_host());
#if BUILDFLAG(IS_CHROMEOS_LACROS)
    cursor_factory_ = std::make_unique<BitmapCursorFactoryOzone>();
#else
    cursor_factory_ = std::make_unique<WaylandCursorFactory>(connection_.get());
#endif
    input_controller_ = CreateStubInputController();
    gpu_platform_support_host_.reset(CreateStubGpuPlatformSupportHost());

    supported_buffer_formats_ =
        connection_->buffer_manager_host()->GetSupportedBufferFormats();
#if BUILDFLAG(USE_GTK)
    DCHECK(!GtkUiDelegate::instance());
    gtk_ui_delegate_ =
        std::make_unique<GtkUiDelegateWayland>(connection_.get());
    GtkUiDelegate::SetInstance(gtk_ui_delegate_.get());
#endif

    menu_utils_ = std::make_unique<WaylandMenuUtils>(connection_.get());

    // TODO(crbug.com/1138740): report which Wayland compositor is used.
  }

  void InitializeGPU(const InitParams& args) override {
    buffer_manager_ = std::make_unique<WaylandBufferManagerGpu>();
#if defined(OZONE_PLATFORM_WAYLAND_EXTERNAL)
    gpu_platform_support_.reset(CreateStubGpuPlatformSupport());
#endif
    surface_factory_ = std::make_unique<WaylandSurfaceFactory>(
        connection_.get(), buffer_manager_.get());
    overlay_manager_ = std::make_unique<WaylandOverlayManager>();
#if defined(WAYLAND_GBM)
    const base::FilePath drm_node_path = path_finder_.GetDrmRenderNodePath();
    if (drm_node_path.empty()) {
      LOG(WARNING) << "Failed to find drm render node path.";
    } else {
      DrmRenderNodeHandle handle;
      if (!handle.Initialize(drm_node_path)) {
        LOG(WARNING) << "Failed to initialize drm render node handle.";
      } else {
        auto gbm = CreateGbmDevice(handle.PassFD().release());
        if (!gbm)
          LOG(WARNING) << "Failed to initialize gbm device.";
        buffer_manager_->set_gbm_device(std::move(gbm));
      }
    }
#endif

#if defined(USE_NEVA_MEDIA)
    video_window_controller_ = std::make_unique<VideoWindowControllerImpl>();
    video_window_controller_->Initialize(base::ThreadTaskRunnerHandle::Get());
#endif  // defined(USE_NEVA_MEDIA)
  }

  const PlatformProperties& GetPlatformProperties() override {
    static base::NoDestructor<OzonePlatform::PlatformProperties> properties;
    static bool initialised = false;
    if (!initialised) {
      // Supporting server-side decorations requires a support of
      // xdg-decorations. But this protocol has been accepted into the upstream
      // recently, and it will take time before it is taken by compositors. For
      // now, always use custom frames and disallow switching to server-side
      // frames.
      // https://github.com/wayland-project/wayland-protocols/commit/76d1ae8c65739eff3434ef219c58a913ad34e988
      properties->custom_frame_pref_default = true;

      properties->uses_external_vulkan_image_factory = true;

      // Wayland doesn't provide clients with global screen coordinates.
      // Instead, it forces clients to position windows relative to their top
      // level windows if the have child-parent relationship. In case of
      // toplevel windows, clients simply don't know their position on screens
      // and always assume they are located at some arbitrary position.
      properties->ignore_screen_bounds_for_menus = true;
      // Wayland uses sub-surfaces to show tooltips, and sub-surfaces must be
      // bound to their root surfaces always, but finding the correct root
      // surface at the moment of creating the tooltip is not always possible
      // due to how Wayland handles focus and activation.
      // Therefore, the platform should be given a hint at the moment when the
      // surface is initialised, where it is known for sure which root surface
      // shows the tooltip.
      properties->set_parent_for_non_top_level_windows = true;
      properties->app_modal_dialogs_use_event_blocker = true;

      // Primary planes can be transluscent due to underlay strategy. As a
      // result Wayland server draws contents occluded by an accelerated widget.
      // To prevent this, an opaque background image is stacked below the
      // accelerated widget to occlude contents below.
      properties->needs_background_image =
          ui::IsWaylandOverlayDelegationEnabled();

      initialised = true;
    }

    return *properties;
  }

  const InitializedHostProperties& GetInitializedHostProperties() override {
    static base::NoDestructor<OzonePlatform::InitializedHostProperties>
        properties;
    static bool initialized = false;
    if (!initialized) {
      properties->supports_overlays =
          ui::IsWaylandOverlayDelegationEnabled() && connection_->viewporter();
      initialized = true;
    }
    return *properties;
  }

  void AddInterfaces(mojo::BinderMap* binders) override {
    binders->Add<ozone::mojom::WaylandBufferManagerGpu>(
        base::BindRepeating(
            &OzonePlatformWayland::CreateWaylandBufferManagerGpuBinding,
            base::Unretained(this)),
        base::SequencedTaskRunnerHandle::Get());

#if defined(USE_NEVA_MEDIA)
    // This is called from GPU main thread and we want to get callback with GPU
    // main thread.
    binders->Add(base::BindRepeating(
                     &OzonePlatformWayland::CreateVideoWindowConnectorBinding,
                     base::Unretained(this)),
                 base::ThreadTaskRunnerHandle::Get());

    binders->Add(
        base::BindRepeating(
            &OzonePlatformWayland::CreateVideoWindowProviderClientBinding,
            base::Unretained(this)),
        base::ThreadTaskRunnerHandle::Get());
#endif  // defined(USE_NEVA_MEDIA)
  }

  void CreateWaylandBufferManagerGpuBinding(
      mojo::PendingReceiver<ozone::mojom::WaylandBufferManagerGpu> receiver) {
    buffer_manager_->AddBindingWaylandBufferManagerGpu(std::move(receiver));
  }

#if defined(USE_NEVA_MEDIA)
  void CreateVideoWindowConnectorBinding(
      mojo::PendingReceiver<mojom::VideoWindowConnector> receiver) {
    video_window_controller_->Bind(std::move(receiver));
  }

  void CreateVideoWindowProviderClientBinding(
      mojo::PendingReceiver<mojom::VideoWindowProviderClient> receiver) {
    video_window_provider_mojo_ = std::make_unique<VideoWindowProviderMojo>(
        video_window_controller_.get(), std::move(receiver));
    video_window_controller_->SetVideoWindowProvider(
        video_window_provider_mojo_.get());
  }

  VideoWindowGeometryManager* GetVideoWindowGeometryManager() override {
    return video_window_controller_.get();
  }
#endif  // defined(USE_NEVA_MEDIA)

  void PostMainMessageLoopStart(
      base::OnceCallback<void()> shutdown_cb) override {
    DCHECK(connection_);
    connection_->SetShutdownCb(std::move(shutdown_cb));
  }

 private:
#if BUILDFLAG(USE_XKBCOMMON)
  XkbEvdevCodes xkb_evdev_code_converter_;
#endif

  std::unique_ptr<KeyboardLayoutEngine> keyboard_layout_engine_;
  std::unique_ptr<WaylandConnection> connection_;
  std::unique_ptr<WaylandSurfaceFactory> surface_factory_;
  std::unique_ptr<CursorFactory> cursor_factory_;
  std::unique_ptr<InputController> input_controller_;
  std::unique_ptr<GpuPlatformSupportHost> gpu_platform_support_host_;
#if defined(OZONE_PLATFORM_WAYLAND_EXTERNAL)
  std::unique_ptr<GpuPlatformSupport> gpu_platform_support_;
#endif
  std::unique_ptr<WaylandInputMethodContextFactory>
      input_method_context_factory_;
  std::unique_ptr<WaylandBufferManagerConnector> buffer_manager_connector_;
  std::unique_ptr<WaylandMenuUtils> menu_utils_;

  // Objects, which solely live in the GPU process.
  std::unique_ptr<WaylandBufferManagerGpu> buffer_manager_;
  std::unique_ptr<WaylandOverlayManager> overlay_manager_;

#if defined(USE_NEVA_MEDIA)
  std::unique_ptr<VideoWindowControllerImpl> video_window_controller_;
  std::unique_ptr<VideoWindowProviderMojo> video_window_provider_mojo_;
#endif  // defined(USE_NEVA_MEDIA)

  // Provides supported buffer formats for native gpu memory buffers
  // framework.
  wl::BufferFormatsWithModifiersMap supported_buffer_formats_;

  // This is used both in the gpu and browser processes to find out if a drm
  // render node is available.
  DrmRenderNodePathFinder path_finder_;

#if BUILDFLAG(USE_GTK)
  std::unique_ptr<GtkUiDelegateWayland> gtk_ui_delegate_;
#endif

  DISALLOW_COPY_AND_ASSIGN(OzonePlatformWayland);
};

}  // namespace

OzonePlatform* CreateOzonePlatformWayland() {
  return new OzonePlatformWayland;
}

}  // namespace ui
