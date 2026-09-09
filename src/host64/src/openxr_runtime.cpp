#include "kotorvr/host/openxr_runtime.hpp"
#include "kotorvr/host/d3d12_bgra_upload.hpp"
#include "kotorvr/host/game_image_snapshot.hpp"
#include "kotorvr/host/gpu_stream_consumer.hpp"
#include "kotorvr/host/neural_stereo_pipeline.hpp"
#include "kotorvr/host/neural_reprojection.hpp"
#include "kotorvr/host/composition_depth.hpp"
#include "kotorvr/host/stereo_resolution.hpp"
#include "kotorvr/host/ui_pose.hpp"
#include "stereo_stream.hpp"
#include "movie_frame.hpp"
#include "vr_input.hpp"
#include "kotorvr/host/visible_smoke.hpp"

#ifndef KOTORVR_HAS_OPENXR
#define KOTORVR_HAS_OPENXR 0
#endif

#if KOTORVR_HAS_OPENXR
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <Windows.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace kotorvr::host {
namespace {

#if KOTORVR_HAS_OPENXR

inline constexpr float game_image_quad_width_m = 3.2F;
inline constexpr float game_image_quad_height_m = 1.8F;
inline constexpr float game_image_quad_distance_m = 1.0F;

XrPosef ToXrPose(const k2vr::math::Pose& pose) noexcept {
    return {{pose.orientation.x,pose.orientation.y,pose.orientation.z,pose.orientation.w},
        {pose.position.x,pose.position.y,pose.position.z}};
}

static_assert(static_cast<std::int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ==
              smoke_format_rgba8_srgb);
static_assert(static_cast<std::int64_t>(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ==
              smoke_format_bgra8_srgb);
static_assert(static_cast<std::int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM) ==
              smoke_format_rgba8_unorm);
static_assert(static_cast<std::int64_t>(DXGI_FORMAT_B8G8R8A8_UNORM) ==
              smoke_format_bgra8_unorm);

RuntimeSessionState portable_state(const XrSessionState state) {
    switch (state) {
    case XR_SESSION_STATE_IDLE: return RuntimeSessionState::idle;
    case XR_SESSION_STATE_READY: return RuntimeSessionState::ready;
    case XR_SESSION_STATE_SYNCHRONIZED: return RuntimeSessionState::synchronized;
    case XR_SESSION_STATE_VISIBLE: return RuntimeSessionState::visible;
    case XR_SESSION_STATE_FOCUSED: return RuntimeSessionState::focused;
    case XR_SESSION_STATE_STOPPING: return RuntimeSessionState::stopping;
    case XR_SESSION_STATE_LOSS_PENDING: return RuntimeSessionState::loss_pending;
    case XR_SESSION_STATE_EXITING: return RuntimeSessionState::exiting;
    case XR_SESSION_STATE_UNKNOWN:
    default: return RuntimeSessionState::unknown;
    }
}

bool extension_available(const std::vector<XrExtensionProperties>& extensions,
                         const std::string_view name) {
    return std::any_of(extensions.begin(), extensions.end(), [&](const auto& extension) {
        return name == extension.extensionName;
    });
}

std::string hex_u32(const std::uint32_t value) {
    std::array<char, 11> text{};
    std::snprintf(text.data(), text.size(), "0x%08X",
                  static_cast<unsigned>(value));
    return text.data();
}

bool game_window_focused() noexcept {
    DWORD foreground_pid{};
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, foreground_pid);
    if (!process) return false;
    wchar_t filename[MAX_PATH]{};
    DWORD size = MAX_PATH;
    bool focused = false;
    if (QueryFullProcessImageNameW(process, 0, filename, &size)) {
        const auto* name = std::wcsrchr(filename, L'\\');
        focused = name && _wcsicmp(name + 1, L"swkotor2.exe") == 0;
    }
    CloseHandle(process);
    return focused;
}

void copy_neural_visible(ID3D12GraphicsCommandList* list,ID3D12Resource* source,ID3D12Resource* destination,UINT width,UINT height) {
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (auto& b:barriers) b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition={source,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition={destination,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(2,barriers);
    D3D12_TEXTURE_COPY_LOCATION from{},to{}; from.pResource=source; to.pResource=destination;
    const D3D12_BOX box{0,0,0,width,height,1}; list->CopyTextureRegion(&to,0,0,0,&from,&box);
    for (auto& b:barriers) std::swap(b.Transition.StateBefore,b.Transition.StateAfter);
    list->ResourceBarrier(2,barriers);
}

// Depth images have their own acquire order. A zero-time wait may keep one
// acquired image pending across frames; never release it before a successful wait.
struct CompositorDepthResources {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::vector<XrSwapchainImageD3D12KHR> images;
    std::unique_ptr<CompositionDepthUnpack> unpack;
    Logger* logger{};
    std::uint32_t image_index{};
    bool enabled{},acquired{},waited{},gpu_pending{},timeout_logged{},metadata_logged{};
    std::uint64_t submissions{};

    void Disable(std::string_view reason,XrResult result=XR_SUCCESS) {
        enabled=false;
        if (logger) logger->write(LogLevel::warning,"native_compositor_depth_disabled",
            "Optional compositor depth disabled; color presentation continues",
            {{"reason",std::string(reason)},{"xr_result",std::to_string(result)}});
    }
    // Same retirement precondition as the color swapchain: host commands have
    // completed (or the D3D device has been removed). Call before session destroy.
    void Reset() noexcept {
        enabled=false;
        // Release COM references held by immutable descriptors BEFORE destroying XR.
        unpack.reset(); images.clear();
        if (swapchain!=XR_NULL_HANDLE) (void)xrDestroySwapchain(swapchain);
        swapchain=XR_NULL_HANDLE; acquired=waited=gpu_pending=false; image_index=0;
        timeout_logged=metadata_logged=false; submissions=0;
    }
    bool Initialize(XrSession session,ID3D12Device* device,
        const std::vector<std::int64_t>& formats,UINT width,UINT height,Logger* log) {
        logger=log;
        if (std::find(formats.begin(),formats.end(),std::int64_t(DXGI_FORMAT_D32_FLOAT))==formats.end()) {
            Disable("runtime_does_not_expose_D32_FLOAT"); return false;
        }
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags=XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        info.format=DXGI_FORMAT_D32_FLOAT; info.sampleCount=1;
        info.width=width*2U; info.height=height;
        info.faceCount=info.arraySize=info.mipCount=1;
        XrResult result=xrCreateSwapchain(session,&info,&swapchain);
        if (XR_FAILED(result)) { swapchain=XR_NULL_HANDLE; Disable("create_swapchain",result); return false; }
        const auto fail=[&](std::string_view reason,XrResult failure) {
            Disable(reason,failure); Reset(); return false;
        };
        std::uint32_t count{};
        result=xrEnumerateSwapchainImages(swapchain,0,&count,nullptr);
        if (XR_FAILED(result) || !count) return fail("enumerate_image_count",result);
        images.resize(count,XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        result=xrEnumerateSwapchainImages(swapchain,count,&count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if (XR_FAILED(result) || count!=images.size()) return fail("enumerate_images",result);
        for (const auto& image:images) {
            if (!image.texture) return fail("null_depth_image",XR_ERROR_RUNTIME_FAILURE);
            const auto d=image.texture->GetDesc();
            if (d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width!=width*2U || d.Height!=height ||
                d.DepthOrArraySize!=1 || d.MipLevels!=1 || d.SampleDesc.Count!=1 || d.SampleDesc.Quality!=0 ||
                (d.Format!=DXGI_FORMAT_D32_FLOAT && d.Format!=DXGI_FORMAT_R32_TYPELESS) ||
                !(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
                return fail("unsupported_depth_image_description",XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);
        }
        unpack=std::make_unique<CompositionDepthUnpack>();
        if (!unpack->Initialize(device,width,height,k2vr::ipc::StereoDepthOffset(height)))
            return fail("unpack_initialization",XR_ERROR_RUNTIME_FAILURE);
        enabled=true;
        logger->write(LogLevel::info,"native_compositor_depth_ready",
            "Optional D32 depth swapchain ready; both eyes require matching v5 metadata",
            {{"width",std::to_string(width*2U)},{"height",std::to_string(height)},
             {"images",std::to_string(images.size())}});
        return true;
    }
    bool AcquireReady() {
        if (!enabled || gpu_pending) return false;
        if (!acquired) {
            XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            const auto result=xrAcquireSwapchainImage(swapchain,&info,&image_index);
            if (XR_FAILED(result)) { Disable("acquire_image",result); return false; }
            acquired=true;
            if (image_index>=images.size()) { Disable("invalid_depth_image_index",XR_ERROR_RUNTIME_FAILURE); return false; }
        }
        if (!waited) {
            XrSwapchainImageWaitInfo info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            info.timeout=0; // Optional depth must not stall the color/Neural cadence.
            const auto result=xrWaitSwapchainImage(swapchain,&info);
            if (result==XR_TIMEOUT_EXPIRED) {
                if (!timeout_logged && logger) logger->write(LogLevel::info,"native_compositor_depth_wait_pending",
                    "Depth image not ready; retaining its acquire for a later wait and submitting color only");
                timeout_logged=true; return false;
            }
            if (result!=XR_SUCCESS && result!=XR_SESSION_LOSS_PENDING) {
                Disable("wait_image",result); return false;
            }
            waited=true;
        }
        return true;
    }
    bool ReleaseReady() {
        // A failed host signal/wait leaves submitted writes in flight. The
        // failure guard must retain this lease until safe teardown, too.
        if (!acquired || !waited || gpu_pending) return false;
        XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const auto result=xrReleaseSwapchainImage(swapchain,&info);
        // On an error ownership is uncertain: disable permanently, never retry
        // the release or acquire from this chain; destroy it during safe teardown.
        acquired=waited=false;
        if (XR_FAILED(result)) { Disable("release_image",result); return false; }
        return true;
    }
};
struct DepthImageReleaseGuard {
    CompositorDepthResources& depth;
    ~DepthImageReleaseGuard() { (void)depth.ReleaseReady(); }
};

#endif

} // namespace

struct OpenXrRuntime::Impl {
    Logger* logger{};
    SessionStateMachine* state_machine{};
    D3D12Context* graphics{};
    bool available{};
    std::string runtime_name{KOTORVR_HAS_OPENXR ? "uninitialized" : "compile-time stub"};
    std::uint64_t frame_sequence{};
#if KOTORVR_HAS_OPENXR
    // Own references independently of the caller's D3D12Context. An unresolved
    // retirement quarantines this entire Impl even if that context shuts down.
    Microsoft::WRL::ComPtr<ID3D12Device> gpu_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> gpu_queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> retirement_fence;
    HANDLE retirement_event{};
    std::uint64_t retirement_value{};
    bool compositor_depth_extension_enabled{};
    XrInstance instance{XR_NULL_HANDLE};
    XrSystemId system_id{XR_NULL_SYSTEM_ID};
    XrSession session{XR_NULL_HANDLE};
    XrSpace local_space{XR_NULL_HANDLE};
    XrSpace view_space{XR_NULL_HANDLE};
    XrEnvironmentBlendMode blend_mode{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
    Extent2D recommended_view_extent{};
    Extent2D maximum_swapchain_extent{};
    Extent2D maximum_view_extent{};
    k2vr::math::Pose upright_ui_head{};
    float ui_head_yaw{};
    bool have_ui_head{},previous_movie_theater{},ui_pose_warning_logged{};
    XrPosef theater_pose{{0,0,0,1},{0,0,-1.5F}};
    bool previous_theater{},force_theater{},theater_key_was_down{},recenter_was_down{};
    k2vr::ipc::MovieFrameChannel movie_channel;
    k2vr::ipc::MovieFrame movie_snapshot;
    GameImageFrame movie_image;
    bool movie_was_active{};
    std::chrono::steady_clock::time_point next_movie_open_attempt{};
    bool hud_visible{true},hud_key_was_down{};

    struct VisibleSmokeResources {
        XrSwapchain swapchain{XR_NULL_HANDLE};
        CompositorDepthResources compositor_depth;
        std::int64_t format{};
        VisibleSmokeLayout layout{};
        Extent2D stream_extent{};
        std::vector<XrSwapchainImageD3D12KHR> images;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> command_allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        HANDLE fence_event{};
        std::uint64_t fence_value{};
        UINT rtv_stride{};
        GameImageSnapshotReader game_image_reader;
        D3D12BgraUpload game_image_upload;
        GpuStreamConsumer gpu_stream;
        k2vr::ipc::StereoFrameMapping stereo_metadata;
        k2vr::ipc::StereoFrameMetadata cached_stereo_frame{};
        k2vr::ipc::StereoFrameMetadata cached_raw_stereo_frame{};
        std::unique_ptr<NeuralStereoPipeline> neural;
        std::unique_ptr<NeuralReprojection> reprojection;
        std::uint64_t reprojected_frames{},reprojected_fresh{},reprojected_last_raw{},reprojection_frequency{};
        double reprojection_gpu_ms_sum{};
        bool neural_enabled{true},neural_error_logged{},neural_f9_down{};
        std::uint64_t neural_last_pair{},neural_pairs{};
        bool presentation_trace_enabled{};
        std::uint32_t presentation_trace_frames{};
        bool native_stereo{};
        bool auxiliary_theater{};
        k2vr::ipc::SessionNonce game_image_nonce{};
        std::chrono::steady_clock::time_point next_game_image_open_attempt{};
        std::chrono::steady_clock::time_point next_gpu_stream_open_attempt{};
        std::chrono::steady_clock::time_point next_gpu_stream_stats_log{};
        GameImageSnapshotStatus last_game_image_status{
            GameImageSnapshotStatus::MappingUnavailable};
        GpuStreamConsumerStatus last_gpu_stream_status{
            GpuStreamConsumerStatus::ObjectUnavailable};
        std::uint32_t last_logged_game_image_sequence{};
        std::uint64_t gpu_stream_frames_copied{};
        std::uint64_t stats_previous_copied{};
        std::uint64_t stats_previous_neural_pairs{};
        std::chrono::steady_clock::time_point stats_previous_time{};
        std::uint64_t gpu_stream_frames_reused{};
        std::uint64_t gpu_stream_fallback_frames{};
        std::uint64_t gpu_stream_errors{};
        bool game_image_enabled{};
        bool game_image_upload_failure_logged{};
        bool gpu_stream_error_logged{};
        bool ready{};
    } visible_smoke, theater_smoke;

    bool RetireGpuWork() noexcept {
        const auto removed=[&]() noexcept {
            return gpu_device && FAILED(gpu_device->GetDeviceRemovedReason());
        };
        const auto unresolved=[&](const char* reason,HRESULT result=S_OK) noexcept {
            // Device removal cancels pending device work. Otherwise retain even
            // the retirement fence/event: Signal may have queued before failure.
            if (removed()) return true;
            try {
                if (logger) logger->write(LogLevel::error,"gpu_resources_quarantined",
                    "GPU retirement unconfirmed; retaining runtime resources until process exit",
                    {{"reason",reason},{"hresult",std::to_string(result)},
                     {"wait_limit_ms","5000"}});
            } catch (...) {} // Teardown must not throw before quarantine.
            return false;
        };
        if (removed()) return true;
        if (!gpu_device || !gpu_queue) {
            // Startup may fail before graphics/queue initialization. No session
            // or runtime commands can exist at that point (or after shutdown).
            if (!gpu_device && !gpu_queue && session==XR_NULL_HANDLE) return true;
            return unresolved("device_or_queue_unavailable");
        }
        HRESULT result=S_OK;
        if (!retirement_fence) {
            result=gpu_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&retirement_fence));
            if (FAILED(result)) return unresolved("retirement_fence_create",result);
        }
        if (!retirement_event) {
            retirement_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if (!retirement_event)
                return unresolved("retirement_event_create",HRESULT_FROM_WIN32(GetLastError()));
        }
        const auto value=++retirement_value;
        result=gpu_queue->Signal(retirement_fence.Get(),value);
        if (FAILED(result)) return unresolved("retirement_signal",result);
        result=retirement_fence->SetEventOnCompletion(value,retirement_event);
        if (FAILED(result)) return unresolved("retirement_event_registration",result);
        const DWORD wait=WaitForSingleObject(retirement_event,5000);
        const auto completed=retirement_fence->GetCompletedValue();
        if (removed()) return true;
        // UINT64_MAX is the device-removal sentinel, never proof of completion
        // by itself. Require the device query above to confirm that case.
        if (completed!=(std::numeric_limits<std::uint64_t>::max)() && completed>=value)
            return true;
        return unresolved(wait==WAIT_TIMEOUT ? "retirement_timeout":"retirement_wait_incomplete");
    }
#endif
};

OpenXrRuntime::OpenXrRuntime() : impl_(std::make_unique<Impl>()) {}
OpenXrRuntime::~OpenXrRuntime() { shutdown(); }

RuntimeStartResult OpenXrRuntime::start(D3D12Context& graphics,
                                        SessionStateMachine& state_machine,
                                        Logger& logger) {
    shutdown();
    impl_ = std::make_unique<Impl>();
    impl_->graphics = &graphics;
    impl_->state_machine = &state_machine;
    impl_->logger = &logger;

#if !KOTORVR_HAS_OPENXR
    logger.write(LogLevel::warning,
                 "openxr_sdk_missing",
                 "Host was compiled without an OpenXR SDK; runtime path is a safe stub",
                 {{"hint", "Install OpenXR headers/loader and reconfigure CMake"}});
    return {false, false, "OpenXR SDK was not present at configure time"};
#else
    std::uint32_t extension_count{};
    XrResult result = xrEnumerateInstanceExtensionProperties(
        nullptr, 0, &extension_count, nullptr);
    if (XR_FAILED(result)) {
        logger.write(LogLevel::error,
                     "openxr_extensions_failed",
                     "Unable to enumerate OpenXR instance extensions",
                     {{"xr_result", std::to_string(result)}});
        return {false, true, "xrEnumerateInstanceExtensionProperties failed"};
    }
    std::vector<XrExtensionProperties> extensions(
        extension_count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    result = xrEnumerateInstanceExtensionProperties(
        nullptr, extension_count, &extension_count, extensions.data());
    if (XR_FAILED(result) ||
        !extension_available(extensions, XR_KHR_D3D12_ENABLE_EXTENSION_NAME)) {
        logger.write(LogLevel::error,
                     "openxr_d3d12_unavailable",
                     "Active OpenXR runtime does not expose XR_KHR_D3D12_enable",
                     {{"xr_result", std::to_string(result)}});
        return {false, true, "XR_KHR_D3D12_enable is unavailable"};
    }

    wchar_t depth_option[2]{};
    const bool depth_requested=GetEnvironmentVariableW(L"KOTOR2VR_COMPOSITOR_DEPTH",depth_option,2)==1 && depth_option[0]==L'1';
    impl_->compositor_depth_extension_enabled=depth_requested &&
        extension_available(extensions,XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
    if (depth_requested && !impl_->compositor_depth_extension_enabled)
        logger.write(LogLevel::warning,"native_compositor_depth_unavailable",
            "Runtime does not expose XR_KHR_composition_layer_depth; color presentation retained");
    const char* enabled_extensions[]{XR_KHR_D3D12_ENABLE_EXTENSION_NAME,
        XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME};
    XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(instance_info.applicationInfo.applicationName,
                  XR_MAX_APPLICATION_NAME_SIZE,
                  "%s",
                  "KOTOR II VR Host");
    instance_info.applicationInfo.applicationVersion = 1;
    std::snprintf(instance_info.applicationInfo.engineName,
                  XR_MAX_ENGINE_NAME_SIZE,
                  "%s",
                  "KOTORVR native bridge");
    instance_info.applicationInfo.engineVersion = 1;
    instance_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    instance_info.enabledExtensionCount = impl_->compositor_depth_extension_enabled ? 2U:1U;
    instance_info.enabledExtensionNames = enabled_extensions;
    result = xrCreateInstance(&instance_info, &impl_->instance);
    if (XR_FAILED(result) && impl_->compositor_depth_extension_enabled) {
        logger.write(LogLevel::warning,"native_compositor_depth_instance_fallback",
            "Instance creation with optional depth failed; retrying the established D3D12-only extension set",
            {{"xr_result",std::to_string(result)}});
        impl_->compositor_depth_extension_enabled=false;
        instance_info.enabledExtensionCount=1; impl_->instance=XR_NULL_HANDLE;
        result=xrCreateInstance(&instance_info,&impl_->instance);
    }
    if (XR_FAILED(result)) {
        logger.write(LogLevel::error,
                     "openxr_instance_failed",
                     "xrCreateInstance failed",
                     {{"xr_result", std::to_string(result)}});
        return {false, true, "xrCreateInstance failed"};
    }

    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(impl_->instance, &properties))) {
        impl_->runtime_name = properties.runtimeName;
    }

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = xrGetSystem(impl_->instance, &system_info, &impl_->system_id);
    if (XR_FAILED(result)) {
        logger.write(LogLevel::error,
                     "openxr_system_failed",
                     "No OpenXR HMD system is currently available",
                     {{"runtime", impl_->runtime_name},
                      {"xr_result", std::to_string(result)}});
        return {false, true, "xrGetSystem failed"};
    }

    std::uint32_t view_count{};
    result = xrEnumerateViewConfigurationViews(
        impl_->instance,
        impl_->system_id,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        0,
        &view_count,
        nullptr);
    if (XR_FAILED(result) || view_count != eye_count) {
        logger.write(LogLevel::error,
                     "openxr_stereo_views_unavailable",
                     "Primary stereo must expose exactly two configuration views",
                     {{"xr_result", std::to_string(result)},
                      {"view_count", std::to_string(view_count)}});
        return {false, true, "Primary stereo view configuration is unavailable"};
    }
    std::array<XrViewConfigurationView, eye_count> view_configuration{
        XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},
        XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    result = xrEnumerateViewConfigurationViews(
        impl_->instance,
        impl_->system_id,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        static_cast<std::uint32_t>(view_configuration.size()),
        &view_count,
        view_configuration.data());
    const auto& left_configuration = view_configuration[eye_index(Eye::left)];
    const auto& right_configuration = view_configuration[eye_index(Eye::right)];
    if (XR_FAILED(result) || view_count != eye_count ||
        left_configuration.recommendedImageRectWidth == 0 ||
        left_configuration.recommendedImageRectHeight == 0 ||
        left_configuration.recommendedImageRectWidth !=
            right_configuration.recommendedImageRectWidth ||
        left_configuration.recommendedImageRectHeight !=
            right_configuration.recommendedImageRectHeight) {
        logger.write(LogLevel::error,
                     "openxr_stereo_view_layout_invalid",
                     "Stereo view recommendations are missing or asymmetric",
                     {{"xr_result", std::to_string(result)},
                      {"view_count", std::to_string(view_count)}});
        return {false, true, "Stereo view recommendations are incompatible"};
    }
    impl_->recommended_view_extent = {
        left_configuration.recommendedImageRectWidth,
        left_configuration.recommendedImageRectHeight};
    impl_->maximum_view_extent = {
        std::min(left_configuration.maxImageRectWidth,
                 right_configuration.maxImageRectWidth),
        std::min(left_configuration.maxImageRectHeight,
                 right_configuration.maxImageRectHeight)};
    // A packed stereo atlas is one swapchain containing multiple image rects.
    // Per-view limits apply to each eye, not to the complete atlas.
    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    result=xrGetSystemProperties(impl_->instance,impl_->system_id,&system_properties);
    if (XR_FAILED(result)) {
        logger.write(LogLevel::error,"openxr_system_properties_failed",
            "Cannot query swapchain image limits",{{"xr_result",std::to_string(result)}});
        return {false,true,"OpenXR swapchain limits unavailable"};
    }
    impl_->maximum_swapchain_extent={system_properties.graphicsProperties.maxSwapchainImageWidth,
        system_properties.graphicsProperties.maxSwapchainImageHeight};
    logger.write(LogLevel::info,"openxr_image_limits","Runtime image rectangle and swapchain limits",
        {{"view_width",std::to_string(impl_->maximum_view_extent.width)},
         {"view_height",std::to_string(impl_->maximum_view_extent.height)},
         {"swapchain_width",std::to_string(impl_->maximum_swapchain_extent.width)},
         {"swapchain_height",std::to_string(impl_->maximum_swapchain_extent.height)}});

    std::uint32_t blend_mode_count{};
    result = xrEnumerateEnvironmentBlendModes(
        impl_->instance,
        impl_->system_id,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        0,
        &blend_mode_count,
        nullptr);
    if (XR_FAILED(result) || blend_mode_count == 0) {
        return {false, true, "OpenXR environment blend modes are unavailable"};
    }
    std::vector<XrEnvironmentBlendMode> blend_modes(blend_mode_count);
    result = xrEnumerateEnvironmentBlendModes(
        impl_->instance,
        impl_->system_id,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        blend_mode_count,
        &blend_mode_count,
        blend_modes.data());
    if (XR_FAILED(result) ||
        std::find(blend_modes.begin(), blend_modes.end(),
                  XR_ENVIRONMENT_BLEND_MODE_OPAQUE) == blend_modes.end()) {
        logger.write(LogLevel::error,
                     "openxr_opaque_blend_unavailable",
                     "The target VR compositor must support opaque projection layers",
                     {{"xr_result", std::to_string(result)}});
        return {false, true, "Opaque OpenXR environment blend mode is unavailable"};
    }
    impl_->blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    PFN_xrVoidFunction requirements_function{};
    result = xrGetInstanceProcAddr(impl_->instance,
                                   "xrGetD3D12GraphicsRequirementsKHR",
                                   &requirements_function);
    if (XR_FAILED(result) || requirements_function == nullptr) {
        logger.write(LogLevel::error,
                     "openxr_requirements_function_missing",
                     "Unable to resolve xrGetD3D12GraphicsRequirementsKHR",
                     {{"xr_result", std::to_string(result)}});
        return {false, true, "D3D12 graphics-requirements function is unavailable"};
    }
    const auto get_requirements =
        reinterpret_cast<PFN_xrGetD3D12GraphicsRequirementsKHR>(requirements_function);
    XrGraphicsRequirementsD3D12KHR xr_requirements{
        XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    result = get_requirements(impl_->instance, impl_->system_id, &xr_requirements);
    if (XR_FAILED(result)) {
        logger.write(LogLevel::error,
                     "openxr_graphics_requirements_failed",
                     "OpenXR D3D12 requirements query failed",
                     {{"xr_result", std::to_string(result)}});
        return {false, true, "OpenXR D3D12 requirements query failed"};
    }

    const D3D12Requirements requirements{
        {xr_requirements.adapterLuid.LowPart, xr_requirements.adapterLuid.HighPart},
        static_cast<std::uint32_t>(xr_requirements.minFeatureLevel)};
    if (!graphics.initialize(requirements, logger)) {
        return {false, true, "D3D12 initialization failed for the runtime adapter"};
    }
    impl_->gpu_device=graphics.device();
    impl_->gpu_queue=graphics.queue();
    if (!impl_->gpu_device || !impl_->gpu_queue)
        return {false,true,"D3D12 device/queue unavailable after initialization"};

    XrGraphicsBindingD3D12KHR graphics_binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    graphics_binding.device = graphics.device();
    graphics_binding.queue = graphics.queue();
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = &graphics_binding;
    session_info.systemId = impl_->system_id;
    result = xrCreateSession(impl_->instance, &session_info, &impl_->session);
    if (XR_FAILED(result)) {
        logger.write(LogLevel::error,
                     "openxr_session_failed",
                     "xrCreateSession failed",
                     {{"xr_result", std::to_string(result)}});
        return {false, true, "xrCreateSession failed"};
    }

    XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation.w = 1.0F;
    result = xrCreateReferenceSpace(impl_->session, &space_info, &impl_->local_space);
    if (XR_FAILED(result)) {
        logger.write(LogLevel::error,
                     "openxr_space_failed",
                     "Unable to create the local reference space",
                     {{"xr_result", std::to_string(result)}});
        return {false, true, "xrCreateReferenceSpace failed"};
    }

    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    result = xrCreateReferenceSpace(impl_->session, &space_info, &impl_->view_space);
    if (XR_FAILED(result)) {
        logger.write(LogLevel::error,
                     "openxr_view_space_failed",
                     "Unable to create the view reference space",
                     {{"xr_result", std::to_string(result)}});
        return {false, true, "xrCreateReferenceSpace for VIEW failed"};
    }

    impl_->available = true;
    logger.write(LogLevel::info,
                 "openxr_ready",
                 "OpenXR instance, system, D3D12 binding, session, and reference spaces are ready",
                 {{"runtime", impl_->runtime_name},
                  {"reference_spaces", "LOCAL,VIEW"},
                  {"recommended_eye_width",
                   std::to_string(impl_->recommended_view_extent.width)},
                  {"recommended_eye_height",
                   std::to_string(impl_->recommended_view_extent.height)},
                  {"projection_swapchains", "not_created_yet"}});
    return {true, true, "OpenXR runtime initialized"};
#endif
}

RuntimePollResult OpenXrRuntime::poll_events() {
    RuntimePollResult poll_result{};
#if !KOTORVR_HAS_OPENXR
    poll_result.success = false;
    poll_result.exit_requested = true;
    return poll_result;
#else
    if (!impl_ || !impl_->available || impl_->instance == XR_NULL_HANDLE) {
        poll_result.success = false;
        poll_result.exit_requested = true;
        return poll_result;
    }

    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (true) {
        const XrResult result = xrPollEvent(impl_->instance, &event);
        if (result == XR_EVENT_UNAVAILABLE) {
            break;
        }
        if (XR_FAILED(result)) {
            impl_->logger->write(LogLevel::error,
                                 "openxr_poll_failed",
                                 "xrPollEvent failed",
                                 {{"xr_result", std::to_string(result)}});
            impl_->state_machine->mark_failed("xrPollEvent failed");
            poll_result.success = false;
            poll_result.exit_requested = true;
            break;
        }

        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& changed =
                *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (changed.session == impl_->session) {
                const SessionActions actions =
                    impl_->state_machine->apply(portable_state(changed.state));
                poll_result.history_reset_requested |= actions.reset_temporal_histories;
                poll_result.exit_requested |= actions.request_exit;

                if (actions.begin_session) {
                    XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
                    begin_info.primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    const XrResult begin_result = xrBeginSession(impl_->session, &begin_info);
                    if (XR_SUCCEEDED(begin_result)) {
                        impl_->state_machine->mark_session_begun();
                    } else {
                        impl_->state_machine->mark_failed("xrBeginSession failed");
                        poll_result.success = false;
                        poll_result.exit_requested = true;
                    }
                }
                if (actions.end_session) {
                    const XrResult end_result = xrEndSession(impl_->session);
                    if (XR_SUCCEEDED(end_result)) {
                        impl_->state_machine->mark_session_ended();
                    } else {
                        impl_->state_machine->mark_failed("xrEndSession failed");
                        poll_result.success = false;
                        poll_result.exit_requested = true;
                    }
                }
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            const auto& changed =
                *reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&event);
            if (changed.session == impl_->session &&
                changed.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                poll_result.tracking_reset_requested = true;
                poll_result.history_reset_requested = true;
                poll_result.tracking_change_time_ns =
                    static_cast<std::int64_t>(changed.changeTime);
                impl_->logger->write(
                    LogLevel::info,
                    "openxr_reference_space_change_pending",
                    "Local tracking origin will change; tracking and both temporal histories must reset",
                    {{"change_time_ns", std::to_string(changed.changeTime)},
                     {"pose_valid", changed.poseValid == XR_TRUE ? "true" : "false"}});
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            const SessionActions actions =
                impl_->state_machine->apply(RuntimeSessionState::loss_pending);
            poll_result.exit_requested = true;
            poll_result.history_reset_requested |= actions.reset_temporal_histories;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return poll_result;
#endif
}

bool OpenXrRuntime::wait_begin_frame(XrFrameToken& token, FrameTiming& timing) {
#if !KOTORVR_HAS_OPENXR
    (void)token;
    (void)timing;
    return false;
#else
    if (!impl_ || !impl_->available || !impl_->state_machine->should_drive_frames()) {
        return false;
    }
    const auto wait_start = std::chrono::steady_clock::now();
    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    XrResult result = xrWaitFrame(impl_->session, &wait_info, &frame_state);
    const auto wait_end = std::chrono::steady_clock::now();
    timing.wait_frame_ms =
        std::chrono::duration<double, std::milli>(wait_end - wait_start).count();
    if (XR_FAILED(result)) {
        impl_->state_machine->mark_failed("xrWaitFrame failed");
        return false;
    }

    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    result = xrBeginFrame(impl_->session, &begin_info);
    if (XR_FAILED(result)) {
        impl_->state_machine->mark_failed("xrBeginFrame failed");
        return false;
    }

    token.sequence = ++impl_->frame_sequence;
    token.predicted_display_time_ns =
        static_cast<std::int64_t>(frame_state.predictedDisplayTime);
    token.should_render = frame_state.shouldRender == XR_TRUE;
    token.begun = true;
    timing.frame_id = token.sequence;
    timing.predicted_display_time_ns = token.predicted_display_time_ns;
    return true;
#endif
}

bool OpenXrRuntime::locate_views(const XrFrameToken& token, LocatedViews& views) {
#if !KOTORVR_HAS_OPENXR
    (void)token;
    (void)views;
    return false;
#else
    if (!impl_ || !token.begun || impl_->local_space == XR_NULL_HANDLE) {
        return false;
    }
    XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate_info.displayTime = token.predicted_display_time_ns;
    locate_info.space = impl_->local_space;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    std::array<XrView, eye_count> xr_views{
        XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
    std::uint32_t view_count{};
    const XrResult result = xrLocateViews(impl_->session,
                                          &locate_info,
                                          &view_state,
                                          static_cast<std::uint32_t>(xr_views.size()),
                                          &view_count,
                                          xr_views.data());
    if (XR_FAILED(result) || view_count != eye_count) {
        impl_->logger->write(LogLevel::error,
                             "openxr_locate_views_failed",
                             "xrLocateViews did not return exactly two views",
                             {{"xr_result", std::to_string(result)},
                              {"view_count", std::to_string(view_count)}});
        return false;
    }

    views.predicted_display_time_ns = token.predicted_display_time_ns;
    views.orientation_valid =
        (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    views.position_valid =
        (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    for (std::size_t index = 0; index < eye_count; ++index) {
        const auto& source = xr_views[index];
        auto& destination = views.views[index];
        destination.pose.position = {
            source.pose.position.x, source.pose.position.y, source.pose.position.z};
        destination.pose.orientation = {source.pose.orientation.x,
                                        source.pose.orientation.y,
                                        source.pose.orientation.z,
                                        source.pose.orientation.w};
        destination.fov = {source.fov.angleLeft,
                           source.fov.angleRight,
                           source.fov.angleUp,
                           source.fov.angleDown};
    }
    if (views.orientation_valid) {
        // Locate the head's VIEW space itself: an individual eye may have a
        // calibrated cant and is not the reference orientation for level UI.
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        const auto head_result=xrLocateSpace(impl_->view_space,impl_->local_space,
            token.predicted_display_time_ns,&head);
        constexpr XrSpaceLocationFlags required=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
            XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if (XR_SUCCEEDED(head_result) && (head.locationFlags&required)==required) {
            const auto& p=head.pose;
            const auto upright=MakeUprightUiHead({{p.position.x,p.position.y,p.position.z},
                {p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w}},impl_->ui_head_yaw);
            if (upright.valid) {
                if (!impl_->have_ui_head)
                    impl_->logger->write(LogLevel::info,"ui_horizon_alignment_ready",
                        "UI follows gaze yaw and pitch while remaining level");
                impl_->upright_ui_head=upright.pose; impl_->ui_head_yaw=upright.yaw;
                impl_->have_ui_head=true; impl_->ui_pose_warning_logged=false;
            }
        } else if (!impl_->ui_pose_warning_logged) {
            impl_->logger->write(LogLevel::warning,"ui_head_pose_unavailable",
                "A valid head pose is unavailable; retaining the last level UI pose",
                {{"xr_result",std::to_string(head_result)},
                 {"location_flags",std::to_string(head.locationFlags)}});
            impl_->ui_pose_warning_logged=true;
        }
    }
    return views.orientation_valid;
#endif
}

bool OpenXrRuntime::end_frame(XrFrameToken& token, FrameTiming& timing) {
#if !KOTORVR_HAS_OPENXR
    (void)token;
    (void)timing;
    return false;
#else
    if (!impl_ || !token.begun) {
        return false;
    }
    const auto submit_start = std::chrono::steady_clock::now();
    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = token.predicted_display_time_ns;
    end_info.environmentBlendMode = impl_->blend_mode;
    end_info.layerCount = 0;
    end_info.layers = nullptr;
    const XrResult result = xrEndFrame(impl_->session, &end_info);
    const auto submit_end = std::chrono::steady_clock::now();
    timing.submit_ms =
        std::chrono::duration<double, std::milli>(submit_end - submit_start).count();
    token.begun = false;
    if (XR_FAILED(result)) {
        impl_->state_machine->mark_failed("xrEndFrame failed");
        return false;
    }
    return true;
#endif
}

bool OpenXrRuntime::initialize_visible_smoke(
    const std::optional<k2vr::ipc::SessionNonce> game_image_nonce,
    const bool native_stereo, const bool auxiliary_theater) {
#if !KOTORVR_HAS_OPENXR
    return false;
#else
    if (!impl_ || !impl_->available || impl_->session == XR_NULL_HANDLE ||
        impl_->graphics == nullptr || !impl_->graphics->ready()) {
        return false;
    }

    // Reinitialization releases the same resources as shutdown. Establish the
    // retirement precondition before entering any reset/error-cleanup path.
    if (!impl_->RetireGpuWork()) {
        (void)impl_.release();
        return false;
    }

    auto& smoke = auxiliary_theater ? impl_->theater_smoke : impl_->visible_smoke;
    const auto reset_smoke = [&]() noexcept {
        smoke.ready = false;
        smoke.compositor_depth.Reset();
        smoke.game_image_reader.Close();
        smoke.game_image_upload.Shutdown();
        smoke.gpu_stream.Close();
        smoke.stereo_metadata.Close();
        smoke.cached_stereo_frame = {};
        smoke.cached_raw_stereo_frame = {};
        smoke.neural.reset();
        smoke.reprojection.reset();
        smoke.reprojected_frames=smoke.reprojected_fresh=smoke.reprojected_last_raw=0;
        smoke.reprojection_gpu_ms_sum=0;
        if (smoke.fence_event != nullptr) {
            CloseHandle(smoke.fence_event);
            smoke.fence_event = nullptr;
        }
        smoke.fence.Reset();
        smoke.command_list.Reset();
        smoke.command_allocator.Reset();
        smoke.rtv_heap.Reset();
        smoke.images.clear();
        if (smoke.swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(smoke.swapchain);
            smoke.swapchain = XR_NULL_HANDLE;
        }
        smoke.format = 0;
        smoke.layout = {};
        smoke.fence_value = 0;
        smoke.rtv_stride = 0;
        smoke.game_image_nonce = {};
        smoke.next_game_image_open_attempt = {};
        smoke.next_gpu_stream_open_attempt = {};
        smoke.next_gpu_stream_stats_log = {};
        smoke.last_game_image_status =
            GameImageSnapshotStatus::MappingUnavailable;
        smoke.last_gpu_stream_status =
            GpuStreamConsumerStatus::ObjectUnavailable;
        smoke.last_logged_game_image_sequence = 0;
        smoke.gpu_stream_frames_copied = 0;
        smoke.stats_previous_copied = 0;
        smoke.stats_previous_time = {};
        smoke.gpu_stream_frames_reused = 0;
        smoke.gpu_stream_fallback_frames = 0;
        smoke.gpu_stream_errors = 0;
        smoke.game_image_enabled = false;
        smoke.game_image_upload_failure_logged = false;
        smoke.gpu_stream_error_logged = false;
    };
    reset_smoke();

    std::uint32_t format_count{};
    XrResult result =
        xrEnumerateSwapchainFormats(impl_->session, 0, &format_count, nullptr);
    if (XR_FAILED(result) || format_count == 0) {
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_formats_failed",
                             "No OpenXR swapchain formats are available",
                             {{"xr_result", std::to_string(result)}});
        return false;
    }
    std::vector<std::int64_t> formats(format_count);
    result = xrEnumerateSwapchainFormats(
        impl_->session, format_count, &format_count, formats.data());
    const auto selected_format = select_visible_smoke_format(formats);
    if (XR_FAILED(result) || !selected_format) {
        impl_->logger->write(
            LogLevel::error,
            "visible_smoke_format_unsupported",
            "Runtime exposes no supported 8-bit color swapchain format",
            {{"xr_result", std::to_string(result)},
             {"format_count", std::to_string(format_count)}});
        return false;
    }

    smoke.layout = select_visible_smoke_layout(impl_->maximum_swapchain_extent);
    smoke.auxiliary_theater=auxiliary_theater;
    if (auxiliary_theater) smoke.layout.pixel_extent={1920,1080};
    smoke.native_stereo = native_stereo && game_image_nonce.has_value();
    if (smoke.native_stereo) {
        smoke.layout.pixel_extent = {
            NativeStereoEyeWidth(impl_->recommended_view_extent.width)*2U,
            k2vr::ipc::StereoDepthOffset(NativeStereoEyeHeight(impl_->recommended_view_extent.height))};
        // Raw depth is transport-only. Quest's XR image-size limit need not
        // accommodate rows that are never submitted to its compositor.
        smoke.stream_extent={smoke.layout.pixel_extent.width,
            k2vr::ipc::StereoAtlasHeight(NativeStereoEyeHeight(impl_->recommended_view_extent.height))};
        if (NativeStereoEyeWidth(impl_->recommended_view_extent.width) > impl_->maximum_view_extent.width ||
            NativeStereoEyeHeight(impl_->recommended_view_extent.height) > impl_->maximum_view_extent.height ||
            smoke.layout.pixel_extent.width > impl_->maximum_swapchain_extent.width ||
            smoke.layout.pixel_extent.height > impl_->maximum_swapchain_extent.height ||
            smoke.layout.pixel_extent.width > 8192 || smoke.layout.pixel_extent.height > 8192 ||
            smoke.stream_extent.height > 8192) {
            impl_->logger->write(LogLevel::error,"stereo_extent_unsupported",
                "Runtime cannot provide a stereo atlas at the selected eye resolution",
                {{"mode",NativeStereoEyeResolutionSetting().fixed ? "fixed":"percent"},
                 {"eye_width",std::to_string(NativeStereoEyeWidth(impl_->recommended_view_extent.width))},
                 {"eye_height",std::to_string(NativeStereoEyeHeight(impl_->recommended_view_extent.height))},
                 {"atlas_width",std::to_string(smoke.layout.pixel_extent.width)},
                 {"atlas_height",std::to_string(smoke.layout.pixel_extent.height)}});
            reset_smoke(); return false;
        }
    }
    if (!smoke.layout.valid()) {
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_layout_invalid",
                             "Runtime reported an invalid maximum swapchain extent");
        reset_smoke();
        return false;
    }
    smoke.format = *selected_format;

    XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    if (game_image_nonce.has_value()) {
        create_info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    }
    create_info.format = smoke.format;
    create_info.sampleCount = 1;
    create_info.width = smoke.layout.pixel_extent.width;
    create_info.height = smoke.layout.pixel_extent.height;
    create_info.faceCount = 1;
    create_info.arraySize = 1;
    create_info.mipCount = 1;
    result = xrCreateSwapchain(impl_->session, &create_info, &smoke.swapchain);
    if (XR_FAILED(result)) {
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_swapchain_failed",
                             "xrCreateSwapchain failed for the diagnostic quad",
                             {{"xr_result", std::to_string(result)},
                              {"format", std::to_string(smoke.format)}});
        reset_smoke();
        return false;
    }

    std::uint32_t image_count{};
    result = xrEnumerateSwapchainImages(
        smoke.swapchain, 0, &image_count, nullptr);
    if (XR_FAILED(result) || image_count == 0) {
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_images_failed",
                             "Diagnostic swapchain has no D3D12 images",
                             {{"xr_result", std::to_string(result)}});
        reset_smoke();
        return false;
    }
    smoke.images.resize(image_count);
    for (auto& image : smoke.images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
        image.next = nullptr;
        image.texture = nullptr;
    }
    result = xrEnumerateSwapchainImages(
        smoke.swapchain,
        image_count,
        &image_count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(smoke.images.data()));
    if (XR_FAILED(result)) {
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_images_failed",
                             "Unable to enumerate D3D12 diagnostic swapchain images",
                             {{"xr_result", std::to_string(result)}});
        reset_smoke();
        return false;
    }

    ID3D12Device* const device = impl_->graphics->device();
    D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_description.NumDescriptors = image_count;
    HRESULT graphics_result = device->CreateDescriptorHeap(
        &heap_description, IID_PPV_ARGS(&smoke.rtv_heap));
    if (FAILED(graphics_result)) {
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_rtv_heap_failed",
                             "Unable to create RTV descriptors for the diagnostic quad",
                             {{"hresult", std::to_string(graphics_result)}});
        reset_smoke();
        return false;
    }
    smoke.rtv_stride = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        smoke.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC rtv_description{};
    rtv_description.Format = static_cast<DXGI_FORMAT>(smoke.format);
    rtv_description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    for (const auto& image : smoke.images) {
        if (image.texture == nullptr) {
            impl_->logger->write(LogLevel::error,
                                 "visible_smoke_image_null",
                                 "Runtime returned a null D3D12 swapchain image");
            reset_smoke();
            return false;
        }
        const D3D12_RESOURCE_DESC image_description = image.texture->GetDesc();
        impl_->logger->write(
            LogLevel::info,
            "visible_smoke_image_description",
            "OpenXR returned a D3D12 diagnostic swapchain image",
            {{"dimension", std::to_string(image_description.Dimension)},
             {"width", std::to_string(image_description.Width)},
             {"height", std::to_string(image_description.Height)},
             {"array_size", std::to_string(image_description.DepthOrArraySize)},
             {"mip_levels", std::to_string(image_description.MipLevels)},
             {"format", std::to_string(image_description.Format)},
             {"sample_count", std::to_string(image_description.SampleDesc.Count)},
             {"sample_quality", std::to_string(image_description.SampleDesc.Quality)}});
        device->CreateRenderTargetView(image.texture, &rtv_description, rtv);
        rtv.ptr += smoke.rtv_stride;
    }

    graphics_result = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&smoke.command_allocator));
    if (FAILED(graphics_result)) {
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_allocator_failed",
                             "Unable to create the diagnostic command allocator",
                             {{"hresult", std::to_string(graphics_result)}});
        reset_smoke();
        return false;
    }
    graphics_result = device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        smoke.command_allocator.Get(),
        nullptr,
        IID_PPV_ARGS(&smoke.command_list));
    if (FAILED(graphics_result) || FAILED(smoke.command_list->Close())) {
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_command_list_failed",
                             "Unable to create the diagnostic command list",
                             {{"hresult", std::to_string(graphics_result)}});
        reset_smoke();
        return false;
    }
    graphics_result =
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&smoke.fence));
    if (FAILED(graphics_result)) {
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_fence_failed",
                             "Unable to create the diagnostic GPU fence",
                             {{"hresult", std::to_string(graphics_result)}});
        reset_smoke();
        return false;
    }
    smoke.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (smoke.fence_event == nullptr) {
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_fence_event_failed",
                             "Unable to create the diagnostic GPU fence event",
                             {{"win32_error", std::to_string(GetLastError())}});
        reset_smoke();
        return false;
    }

    if (game_image_nonce.has_value()) {
        if (!k2vr::ipc::IsValid(*game_image_nonce) ||
            !smoke.game_image_upload.Initialize(
                device, smoke.layout.pixel_extent, smoke.format)) {
            impl_->logger->write(
                LogLevel::error,
                "game_image_upload_initialize_failed",
                "Unable to create the D3D12 upload path for the game image");
            reset_smoke();
            return false;
        }
        smoke.game_image_enabled = true;
        smoke.game_image_nonce = *game_image_nonce;
        smoke.next_game_image_open_attempt = std::chrono::steady_clock::now();
        smoke.next_gpu_stream_open_attempt = std::chrono::steady_clock::now();
        smoke.next_gpu_stream_stats_log =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        impl_->logger->write(
            LogLevel::info,
            "game_image_waiting",
            "Visible smoke is waiting for the matching x86 BGRA8 snapshot",
            {{"mapping",
              "Local\\Kotor2VR-game-image-v1-<SESSION_HIGH><SESSION_LOW>"}});
        impl_->logger->write(
            LogLevel::info,
            "gpu_stream_waiting",
            "Visible smoke is waiting for the nonce-bound shared D3D12 stream",
            {{"color", "Local\\Kotor2VR-gpu-stream-v1-<NONCE>-color"},
             {"ready", "Local\\Kotor2VR-gpu-stream-v1-<NONCE>-ready"},
             {"consumed",
              "Local\\Kotor2VR-gpu-stream-v1-<NONCE>-consumed"}});
    }

    if (smoke.native_stereo && impl_->compositor_depth_extension_enabled) {
        // Failure is optional: keep the already-created color/HUD swapchain.
        (void)smoke.compositor_depth.Initialize(impl_->session,device,formats,
            NativeStereoEyeWidth(impl_->recommended_view_extent.width),
            NativeStereoEyeHeight(impl_->recommended_view_extent.height),impl_->logger);
    }
    smoke.ready = true;
    const bool view_locked_game_image = smoke.game_image_enabled;
    const float quad_width_m = view_locked_game_image
                                   ? game_image_quad_width_m
                                   : smoke.layout.width_m;
    const float quad_height_m = view_locked_game_image
                                    ? game_image_quad_height_m
                                    : smoke.layout.height_m;
    const float quad_distance_m = view_locked_game_image
                                      ? game_image_quad_distance_m
                                      : smoke.layout.distance_m;
    impl_->logger->write(
        LogLevel::info,
        "visible_smoke_ready",
        "Visible OpenXR diagnostic quad is ready",
         {{"format", std::to_string(smoke.format)},
          {"width_px", std::to_string(smoke.layout.pixel_extent.width)},
          {"height_px", std::to_string(smoke.layout.pixel_extent.height)},
         {"space", view_locked_game_image ? "VIEW" : "LOCAL"},
         {"width_m", std::to_string(quad_width_m)},
         {"height_m", std::to_string(quad_height_m)},
         {"distance_m", std::to_string(quad_distance_m)},
         {"image_count", std::to_string(image_count)},
         {"game_image", smoke.game_image_enabled ? "enabled" : "disabled"}});
    if (smoke.native_stereo && !initialize_visible_smoke(game_image_nonce,false,true)) return false;
    return true;
#endif
}

bool OpenXrRuntime::end_frame_visible_smoke(XrFrameToken& token,
                                             FrameTiming& timing) {
#if !KOTORVR_HAS_OPENXR
    (void)token;
    (void)timing;
    return false;
#else
    if (!impl_ || !token.begun || !impl_->visible_smoke.ready) {
        return false;
    }

    if (impl_->visible_smoke.native_stereo) {
        const bool hud_down = k2vr::input::HudToggleDown();
        if (hud_down && !impl_->hud_key_was_down && game_window_focused()) {
            impl_->hud_visible = !impl_->hud_visible;
            impl_->logger->write(LogLevel::info, "native_hud_toggle",
                "VR HUD toggle (F8 / View+DpadDown)",
                {{"visible", impl_->hud_visible ? "true" : "false"}});
        }
        // Consume unfocused press edges as well; returning focus while held
        // must not toggle the HUD. This is independent of fresh/neural frames.
        impl_->hud_key_was_down = hud_down;
    }

    bool movie_active=false;
    if(impl_->visible_smoke.native_stereo && impl_->theater_smoke.ready){
        const auto now=std::chrono::steady_clock::now();
        if(now>=impl_->next_movie_open_attempt){
            (void)impl_->movie_channel.Open(impl_->visible_smoke.game_image_nonce,false);
            impl_->next_movie_open_attempt=now+std::chrono::milliseconds(100);
        }
        const auto read=impl_->movie_channel.ReadLatest(impl_->movie_snapshot);
        if(read==k2vr::ipc::MovieRead::Fresh){
            auto& image=impl_->movie_image;const auto& snapshot=impl_->movie_snapshot;
            image.width=snapshot.width;image.height=snapshot.height;
            image.frame_id=snapshot.sequence;image.sequence=static_cast<std::uint32_t>(snapshot.sequence) | 1U;
            image.pixels.swap(impl_->movie_snapshot.pixels);
        }
        movie_active=(read==k2vr::ipc::MovieRead::Fresh || read==k2vr::ipc::MovieRead::Unchanged ||
            (read==k2vr::ipc::MovieRead::Unavailable && impl_->movie_was_active &&
             GetTickCount64()>=impl_->movie_snapshot.tick_ms &&
             GetTickCount64()-impl_->movie_snapshot.tick_ms<=k2vr::ipc::kMovieFrameTimeoutMs)) && impl_->movie_image.valid();
        if(movie_active!=impl_->movie_was_active){
            impl_->logger->write(LogLevel::info,"bink_movie_presentation",movie_active ?
                "Showing decoded movie frames in the headset theater":"Movie ended; returning to normal VR presentation",
                {{"width",std::to_string(impl_->movie_image.width)},{"height",std::to_string(impl_->movie_image.height)}});
            impl_->movie_was_active=movie_active;
        }
    }
    bool theater=false;
    if (impl_->visible_smoke.native_stereo && impl_->theater_smoke.ready) {
        const bool down=k2vr::input::TheaterDown();
        if (down && !impl_->theater_key_was_down) impl_->force_theater=!impl_->force_theater;
        impl_->theater_key_was_down=down;
        auto& metadata=impl_->visible_smoke.stereo_metadata;
        const bool world=metadata.Open(impl_->visible_smoke.game_image_nonce,false) && metadata.WorldRecentlyRendered();
        theater=movie_active || impl_->force_theater || !world;
        const bool recenter=k2vr::input::RecenterDown();
        if (theater && impl_->have_ui_head && (!impl_->previous_theater ||
            (movie_active && !impl_->previous_movie_theater) ||
            (recenter && !impl_->recenter_was_down))) {
            impl_->theater_pose=ToXrPose(PlaceUiPanel(impl_->upright_ui_head,1.5F));
        }
        impl_->previous_movie_theater=movie_active;
        impl_->recenter_was_down=recenter;
        impl_->previous_theater=theater;
    }
    auto& smoke = theater ? impl_->theater_smoke : impl_->visible_smoke;
    GpuStreamFrameToken gpu_stream_frame{};
    k2vr::ipc::StereoFrameMetadata candidate_stereo_frame{};
    bool have_gpu_stream_frame = false;
    if (smoke.game_image_enabled && !movie_active) {
        const auto now = std::chrono::steady_clock::now();
        if (!smoke.gpu_stream.is_open() &&
            now >= smoke.next_gpu_stream_open_attempt) {
            const GpuStreamConsumerStatus opened =
                smoke.gpu_stream.TryOpen(impl_->graphics->device(),
                    smoke.native_stereo ? k2vr::ipc::StereoStreamNonce(smoke.game_image_nonce) : smoke.game_image_nonce,
                    smoke.native_stereo ? smoke.stream_extent : smoke.auxiliary_theater ? smoke.layout.pixel_extent :
                        Extent2D{k2vr::ipc::kGpuStreamWidth,k2vr::ipc::kGpuStreamHeight});
            smoke.next_gpu_stream_open_attempt =
                now + std::chrono::milliseconds(100);
            if (opened == GpuStreamConsumerStatus::Ok) {
                impl_->logger->write(
                    LogLevel::info,
                    "gpu_stream_opened",
                    "Host opened the shared D3D12 color and fence objects",
                    {{"width", std::to_string(smoke.layout.pixel_extent.width)},
                     {"height", std::to_string(smoke.layout.pixel_extent.height)},
                     {"format", std::to_string(
                                    k2vr::ipc::kGpuStreamDxgiFormatRgba8Unorm)}});
            } else if (opened !=
                           GpuStreamConsumerStatus::ObjectUnavailable &&
                       opened != smoke.last_gpu_stream_status) {
                ++smoke.gpu_stream_errors;
                impl_->logger->write(
                    LogLevel::warning,
                    "gpu_stream_open_failed",
                    "The optional shared D3D12 stream was rejected; fallbacks remain active",
                    {{"status", std::string(ToString(opened))},
                     {"native_error", std::to_string(
                                          smoke.gpu_stream.last_native_error())}});
            }
            smoke.last_gpu_stream_status = opened;
        }
        if (smoke.gpu_stream.is_open()) {
            const GpuStreamPollResult polled = smoke.gpu_stream.PollLatest();
            if (polled.has_frame()) {
                gpu_stream_frame = polled.frame;
                have_gpu_stream_frame = true;
                if (smoke.native_stereo) {
                    have_gpu_stream_frame = smoke.stereo_metadata.Open(smoke.game_image_nonce,false) &&
                        smoke.stereo_metadata.Read(polled.frame.ready_value,candidate_stereo_frame) &&
                        candidate_stereo_frame.request.render_width*2U == smoke.layout.pixel_extent.width &&
                        k2vr::ipc::StereoAtlasHeight(candidate_stereo_frame.request.render_height) == smoke.stream_extent.height;
                }
            } else if (polled.status ==
                       GpuStreamConsumerStatus::FenceQueryFailed) {
                ++smoke.gpu_stream_errors;
                if (!smoke.gpu_stream_error_logged) {
                    impl_->logger->write(
                        LogLevel::warning,
                        "gpu_stream_fence_query_failed",
                        "The optional ready fence became invalid; reverting to fallbacks");
                    smoke.gpu_stream_error_logged = true;
                }
                smoke.gpu_stream.Close();
                smoke.next_gpu_stream_open_attempt =
                    now + std::chrono::milliseconds(100);
            }
        }
    }
    bool use_game_image = movie_active;
    if (smoke.game_image_enabled && !movie_active) {
        const auto now = std::chrono::steady_clock::now();
        if (!smoke.game_image_reader.is_open() &&
            now >= smoke.next_game_image_open_attempt) {
            const GameImageSnapshotStatus opened =
                smoke.game_image_reader.TryOpen(smoke.game_image_nonce);
            smoke.next_game_image_open_attempt =
                now + std::chrono::milliseconds(100);
            if (opened == GameImageSnapshotStatus::Ok) {
                impl_->logger->write(
                    LogLevel::info,
                    "game_image_mapping_opened",
                    "Host opened the x86 game-image snapshot mapping");
            } else if (opened != GameImageSnapshotStatus::MappingUnavailable &&
                       opened != smoke.last_game_image_status) {
                impl_->logger->write(
                    LogLevel::warning,
                    "game_image_mapping_open_failed",
                    "Host could not open the game-image snapshot mapping",
                    {{"status", std::string(ToString(opened))}});
            }
            smoke.last_game_image_status = opened;
        }
        if (smoke.game_image_reader.is_open()) {
            const GameImageSnapshotStatus read =
                smoke.game_image_reader.ReadLatest();
            if (read == GameImageSnapshotStatus::Ok) {
                const auto& latest = smoke.game_image_reader.latest();
                if (latest.sequence != smoke.last_logged_game_image_sequence) {
                    impl_->logger->write(
                        LogLevel::info,
                        "game_image_frame_ready",
                        "A stable x86 game frame will replace the test pattern",
                        {{"frame_id", std::to_string(latest.frame_id)},
                         {"sequence", std::to_string(latest.sequence)},
                         {"width", std::to_string(latest.width)},
                         {"height", std::to_string(latest.height)}});
                    if (latest.gpu_diagnostic.present()) {
                        const auto& diagnostic = latest.gpu_diagnostic;
                        impl_->logger->write(
                            diagnostic.producer_status == 0U
                                ? LogLevel::info
                                : LogLevel::warning,
                            "gpu_stream_producer_report",
                            "x86 reported the result of the GPU stream start attempt",
                            {{"producer_status",
                              std::string(GpuProducerStatusName(
                                  diagnostic.producer_status))},
                             {"producer_status_code",
                              std::to_string(diagnostic.producer_status)},
                             {"interop_status",
                              std::string(GpuInteropStatusName(
                                  diagnostic.interop_status))},
                             {"interop_status_code",
                              std::to_string(diagnostic.interop_status)},
                             {"hresult",
                              hex_u32(diagnostic.hresult_bits)},
                             {"win32_error",
                              std::to_string(diagnostic.win32_error)}});
                    }
                    smoke.last_logged_game_image_sequence = latest.sequence;
                }
            } else if (read != GameImageSnapshotStatus::Unchanged &&
                       read != GameImageSnapshotStatus::NoFrame &&
                       read != GameImageSnapshotStatus::SnapshotContended &&
                       read != smoke.last_game_image_status) {
                impl_->logger->write(
                    LogLevel::warning,
                    "game_image_snapshot_rejected",
                    "The x86 game-image snapshot failed validation; retaining the fallback",
                    {{"status", std::string(ToString(read))}});
            }
            smoke.last_game_image_status = read;
            use_game_image = smoke.game_image_reader.has_frame();
        }
    }
    const auto submit_start = std::chrono::steady_clock::now();
    DepthImageReleaseGuard depth_release_guard{smoke.compositor_depth};
    const auto end_empty_after_failure = [&]() {
        // Release only unused or retired depth images. Submitted writes after
        // a signal/wait failure and timeout-only acquires remain owned.
        (void)smoke.compositor_depth.ReleaseReady();
        XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
        end_info.displayTime = token.predicted_display_time_ns;
        end_info.environmentBlendMode = impl_->blend_mode;
        const XrResult end_result = xrEndFrame(impl_->session, &end_info);
        token.begun = false;
        timing.submit_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - submit_start)
                               .count();
        if (XR_FAILED(end_result)) {
            impl_->logger->write(LogLevel::error,
                                 "visible_smoke_end_frame_failed",
                                 "xrEndFrame failed while balancing a failed frame",
                                 {{"xr_result", std::to_string(end_result)}});
        }
        return false;
    };

    if (!token.should_render) {
        XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
        end_info.displayTime = token.predicted_display_time_ns;
        end_info.environmentBlendMode = impl_->blend_mode;
        const XrResult result = xrEndFrame(impl_->session, &end_info);
        token.begun = false;
        timing.submit_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - submit_start)
                               .count();
        if (XR_FAILED(result)) {
            impl_->state_machine->mark_failed("xrEndFrame failed");
            return false;
        }
        return true;
    }

    std::uint32_t image_index{};
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result =
        xrAcquireSwapchainImage(smoke.swapchain, &acquire_info, &image_index);
    if (XR_FAILED(result) || image_index >= smoke.images.size()) {
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_acquire_failed",
                             "Unable to acquire a diagnostic swapchain image",
                             {{"xr_result", std::to_string(result)},
                              {"image_index", std::to_string(image_index)}});
        impl_->state_machine->mark_failed("xrAcquireSwapchainImage failed");
        return end_empty_after_failure();
    }

    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(smoke.swapchain, &wait_info);
    if (XR_FAILED(result)) {
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_wait_image_failed",
                             "Unable to wait for the diagnostic swapchain image",
                             {{"xr_result", std::to_string(result)}});
        impl_->state_machine->mark_failed("xrWaitSwapchainImage failed");
        return end_empty_after_failure();
    }

    HRESULT graphics_result = smoke.command_allocator->Reset();
    if (SUCCEEDED(graphics_result)) {
        graphics_result =
            smoke.command_list->Reset(smoke.command_allocator.Get(), nullptr);
    }
    if (FAILED(graphics_result)) {
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        (void)xrReleaseSwapchainImage(smoke.swapchain, &release_info);
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_command_reset_failed",
                             "Unable to reset the diagnostic command list",
                             {{"hresult", std::to_string(graphics_result)}});
        impl_->state_machine->mark_failed("D3D12 command reset failed");
        return end_empty_after_failure();
    }

    ID3D12Resource* const texture = smoke.images[image_index].texture;
    bool gpu_stream_copy_recorded = false;
    bool fresh_gpu_stream_copy_recorded = false;
    if (have_gpu_stream_frame && !movie_active) {
        GpuStreamConsumerStatus stream_status =
            IsGpuStreamDestinationCompatible(smoke.layout.pixel_extent,
                smoke.format,(smoke.native_stereo || smoke.auxiliary_theater) ? smoke.layout.pixel_extent :
                    Extent2D{k2vr::ipc::kGpuStreamWidth,k2vr::ipc::kGpuStreamHeight})
                ? smoke.gpu_stream.QueueWait(impl_->graphics->queue(),
                                             gpu_stream_frame)
                : GpuStreamConsumerStatus::IncompatibleDestination;
        if (stream_status == GpuStreamConsumerStatus::Ok) {
            stream_status = smoke.gpu_stream.RecordCopy(
                smoke.command_list.Get(), texture, gpu_stream_frame, !smoke.native_stereo);
            gpu_stream_copy_recorded =
                stream_status == GpuStreamConsumerStatus::Ok;
            fresh_gpu_stream_copy_recorded = gpu_stream_copy_recorded;
        }
        if (!gpu_stream_copy_recorded) {
            ++smoke.gpu_stream_errors;
            if (!smoke.gpu_stream_error_logged) {
                impl_->logger->write(
                    LogLevel::warning,
                    "gpu_stream_copy_skipped",
                    "The optional GPU stream frame could not be copied; using the existing fallback",
                    {{"status", std::string(ToString(stream_status))}});
                smoke.gpu_stream_error_logged = true;
            }
        }
    } else if (!movie_active && smoke.gpu_stream.has_cached_frame()) {
        const GpuStreamConsumerStatus stream_status =
            smoke.native_stereo ? GpuStreamConsumerStatus::Ok :
                smoke.gpu_stream.RecordCachedCopy(smoke.command_list.Get(), texture);
        gpu_stream_copy_recorded =
            stream_status == GpuStreamConsumerStatus::Ok;
        if (!gpu_stream_copy_recorded) {
            ++smoke.gpu_stream_errors;
            if (!smoke.gpu_stream_error_logged) {
                impl_->logger->write(
                    LogLevel::warning,
                    "gpu_stream_cached_copy_skipped",
                    "The last live game frame could not be reused",
                    {{"status", std::string(ToString(stream_status))}});
                smoke.gpu_stream_error_logged = true;
            }
        }
    }

    bool neural_displayed=false,reprojection_recorded=false,depth_recorded=false;
    k2vr::ipc::StereoFrameMetadata neural_frame{},displayed_frame{};
    if (smoke.native_stereo && gpu_stream_copy_recorded) {
        const auto& raw=fresh_gpu_stream_copy_recorded ? candidate_stereo_frame:smoke.cached_raw_stereo_frame;
        if (!smoke.neural && raw.guide_mask==3) {
            wchar_t root[32768]{};
            const DWORD length=GetEnvironmentVariableW(L"KOTOR2VR_NEURAL_WORKERS",root,32768);
            if (length>0 && length<32768) {
                UINT work_percent=100;
                wchar_t scale[16]{}; const auto scale_length=GetEnvironmentVariableW(L"KOTOR2VR_NEURAL_WORK_PERCENT",scale,16);
                if (scale_length>0 && scale_length<16) {
                    wchar_t* end{}; const auto parsed=std::wcstoul(scale,&end,10);
                    if (end!=scale && *end==0 && parsed>=50 && parsed<=100) work_percent=static_cast<UINT>(parsed);
                }
                smoke.neural=std::make_unique<NeuralStereoPipeline>();
                wchar_t presentation_trace[2]{};
                smoke.presentation_trace_enabled=GetEnvironmentVariableW(
                    L"KOTOR2VR_PRESENTATION_TRACE",presentation_trace,2)==1 && presentation_trace[0]==L'1';
                impl_->logger->write(LogLevel::info,"native_neural_display_target","XR neural copy destination",
                    {{"resource_format",std::to_string(texture->GetDesc().Format)},
                     {"width",std::to_string(texture->GetDesc().Width)},
                     {"height",std::to_string(texture->GetDesc().Height)}});
                wchar_t enabled[8]{};
                if (GetEnvironmentVariableW(L"KOTOR2VR_NEURAL_ENABLED",enabled,8)==1 && enabled[0]==L'0')
                    smoke.neural_enabled=false;
                wchar_t reproject[2]{};
                if (GetEnvironmentVariableW(L"KOTOR2VR_NEURAL_REPROJECT",reproject,2)==1 && reproject[0]==L'1') {
                    auto candidate=std::make_unique<NeuralReprojection>();
                    if (candidate->Initialize(impl_->graphics->device(),raw.request.render_width,raw.request.render_height)) {
                        smoke.reprojection=std::move(candidate);
                        (void)impl_->graphics->queue()->GetTimestampFrequency(&smoke.reprojection_frequency);
                    } else impl_->logger->write(LogLevel::warning,"native_reprojection_unavailable","Optional depth reprojection initialization failed; standard neural display retained");
                }
                (void)smoke.neural->Start(impl_->graphics->device(),raw.request.render_width,raw.request.render_height,root,work_percent,false,static_cast<bool>(smoke.reprojection) || smoke.compositor_depth.enabled);
                impl_->logger->write(LogLevel::info,"native_neural_start","Starting isolated per-eye neural workers; F9 / View+B toggles VR neural processing",{{"work_percent",std::to_string(work_percent)},
                    {"enabled",smoke.neural_enabled ? "true":"false"},
                    {"depth_reprojection",smoke.reprojection ? "true":"false"}});
            }
        }
        const bool f9=k2vr::input::NeuralToggleDown();
        if (smoke.neural && f9 && !smoke.neural_f9_down && game_window_focused()) {
            smoke.neural_enabled=!smoke.neural_enabled;
            impl_->logger->write(LogLevel::info,"native_neural_toggle","VR neural toggle",{{"enabled",smoke.neural_enabled ? "true":"false"}});
        }
        smoke.neural_f9_down=f9;
        if (smoke.neural && smoke.neural_enabled) {
            neural_displayed=smoke.neural->RecordDisplay(smoke.command_list.Get(),texture,neural_frame,!smoke.reprojection,token.predicted_display_time_ns);
            if (neural_displayed && smoke.reprojection) {
                auto* source=smoke.neural->displayed_texture();
                reprojection_recorded=smoke.reprojection->Record(smoke.command_list.Get(),source,neural_frame,
                    smoke.gpu_stream.cached_texture(),raw,texture);
                if (!reprojection_recorded) copy_neural_visible(smoke.command_list.Get(),source,texture,
                    raw.request.render_width*2,k2vr::ipc::StereoDepthOffset(raw.request.render_height));
            }
            (void)smoke.neural->RecordInput(smoke.command_list.Get(),smoke.gpu_stream.cached_texture(),raw,token.predicted_display_time_ns);
            if (smoke.neural->failed() && !smoke.neural_error_logged) {
                impl_->logger->write(LogLevel::error,"native_neural_failed",smoke.neural->error());
                smoke.neural_error_logged=true;
            }
        }
    }
    if (smoke.native_stereo && gpu_stream_copy_recorded && !neural_displayed) {
        // Raw fallback is needed only when neural output is disabled, warming
        // up or unavailable. Never present an unwritten XR image.
        gpu_stream_copy_recorded = smoke.gpu_stream.RecordCachedCopy(
            smoke.command_list.Get(), texture) == GpuStreamConsumerStatus::Ok;
    }
    if (smoke.native_stereo && gpu_stream_copy_recorded) {
        const auto& raw=fresh_gpu_stream_copy_recorded ? candidate_stereo_frame:smoke.cached_raw_stereo_frame;
        // This is also the metadata used for the color projection below.
        displayed_frame=neural_displayed && !reprojection_recorded ? neural_frame:raw;
        auto& depth=smoke.compositor_depth;
        if (depth.enabled) {
            const bool valid=displayed_frame.depth_mask==3 && displayed_frame.eyes_complete==3 &&
                displayed_frame.ready_value!=0 && displayed_frame.camera_frame_id!=0 &&
                k2vr::ipc::ValidStereoRequest(displayed_frame.request) &&
                k2vr::ipc::ValidStereoHud(displayed_frame) && k2vr::ipc::ValidStereoGuides(displayed_frame);
            if (!valid) {
                if (!depth.metadata_logged) impl_->logger->write(LogLevel::warning,"native_compositor_depth_metadata_missing",
                    "Color frame lacks complete valid v5 depth metadata; submitting both eyes without compositor depth");
                depth.metadata_logged=true;
            } else if (depth.AcquireReady()) {
                // An unchanged Neural pair uses its retained ORIGINAL depth.
                // Successful reprojection writes the CURRENT raw geometry;
                // failed reprojection falls back to the old Neural pair/depth.
                auto* source=neural_displayed && !reprojection_recorded ?
                    smoke.neural->displayed_texture():smoke.gpu_stream.cached_texture();
                depth_recorded=depth.unpack->Record(smoke.command_list.Get(),source,depth.images[depth.image_index].texture);
                if (!depth_recorded) depth.Disable("unpack_record_or_source_layout");
            }
        }
    }
    if (!gpu_stream_copy_recorded) {
        D3D12_RESOURCE_STATES output_state =
            use_game_image ? D3D12_RESOURCE_STATE_COPY_DEST
                           : D3D12_RESOURCE_STATE_RENDER_TARGET;
        D3D12_RESOURCE_BARRIER to_output{};
        to_output.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_output.Transition.pResource = texture;
        to_output.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_output.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        to_output.Transition.StateAfter = output_state;
        smoke.command_list->ResourceBarrier(1, &to_output);

        bool game_image_uploaded = false;
        if (use_game_image) {
            game_image_uploaded = smoke.game_image_upload.Record(
                smoke.command_list.Get(), texture,
                movie_active ? impl_->movie_image:smoke.game_image_reader.latest(),movie_active);
            if (!game_image_uploaded) {
                D3D12_RESOURCE_BARRIER copy_to_render_target = to_output;
                copy_to_render_target.Transition.StateBefore =
                    D3D12_RESOURCE_STATE_COPY_DEST;
                copy_to_render_target.Transition.StateAfter =
                    D3D12_RESOURCE_STATE_RENDER_TARGET;
                smoke.command_list->ResourceBarrier(1, &copy_to_render_target);
                output_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                if (!smoke.game_image_upload_failure_logged) {
                    impl_->logger->write(
                        LogLevel::warning,
                        "game_image_upload_failed",
                        "The stable game snapshot could not be uploaded; retaining the test pattern");
                    smoke.game_image_upload_failure_logged = true;
                }
            }
        }

        if (!game_image_uploaded) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            smoke.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(image_index) * smoke.rtv_stride;
        smoke.command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        constexpr float background[4]{0.015F, 0.01F, 0.06F, 1.0F};
        smoke.command_list->ClearRenderTargetView(rtv, background, 0, nullptr);

        const LONG width = static_cast<LONG>(std::min<std::uint32_t>(
            smoke.layout.pixel_extent.width,
            static_cast<std::uint32_t>(std::numeric_limits<LONG>::max())));
        const LONG height = static_cast<LONG>(std::min<std::uint32_t>(
            smoke.layout.pixel_extent.height,
            static_cast<std::uint32_t>(std::numeric_limits<LONG>::max())));
        if (width >= 32 && height >= 32) {
        const LONG margin = std::max<LONG>(4, std::min(width, height) / 40);
        const LONG gutter = std::max<LONG>(3, std::min(width, height) / 80);
        const LONG middle_x = width / 2;
        const LONG middle_y = height / 2;
        const std::array<D3D12_RECT, 4> color_rectangles{
            D3D12_RECT{margin, margin, middle_x - gutter, middle_y - gutter},
            D3D12_RECT{middle_x + gutter, margin, width - margin, middle_y - gutter},
            D3D12_RECT{margin, middle_y + gutter, middle_x - gutter, height - margin},
            D3D12_RECT{middle_x + gutter,
                       middle_y + gutter,
                       width - margin,
                       height - margin}};
        constexpr std::array<std::array<float, 4>, 4> colors{
            std::array<float, 4>{0.95F, 0.03F, 0.04F, 1.0F},
            std::array<float, 4>{0.03F, 0.85F, 0.12F, 1.0F},
            std::array<float, 4>{0.03F, 0.18F, 1.0F, 1.0F},
            std::array<float, 4>{1.0F, 0.72F, 0.02F, 1.0F}};
        for (std::size_t index = 0; index < color_rectangles.size(); ++index) {
            const auto& rectangle = color_rectangles[index];
            if (rectangle.left < rectangle.right && rectangle.top < rectangle.bottom) {
                smoke.command_list->ClearRenderTargetView(
                    rtv, colors[index].data(), 1, &rectangle);
            }
        }

        constexpr float white[4]{1.0F, 1.0F, 1.0F, 1.0F};
        const LONG cross_half_width = std::max<LONG>(2, width / 256);
        const LONG cross_half_height = std::max<LONG>(2, height / 144);
        const std::array<D3D12_RECT, 2> cross{
            D3D12_RECT{middle_x - cross_half_width,
                       margin,
                       middle_x + cross_half_width,
                       height - margin},
            D3D12_RECT{margin,
                       middle_y - cross_half_height,
                       width - margin,
                       middle_y + cross_half_height}};
            for (const auto& rectangle : cross) {
                smoke.command_list->ClearRenderTargetView(
                    rtv, white, 1, &rectangle);
            }
        }
        }

        D3D12_RESOURCE_BARRIER to_common = to_output;
        to_common.Transition.StateBefore = output_state;
        to_common.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        smoke.command_list->ResourceBarrier(1, &to_common);
    }
    graphics_result = smoke.command_list->Close();
    if (FAILED(graphics_result)) {
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        (void)xrReleaseSwapchainImage(smoke.swapchain, &release_info);
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_command_close_failed",
                             "Unable to close the diagnostic command list",
                             {{"hresult", std::to_string(graphics_result)}});
        impl_->state_machine->mark_failed("D3D12 command close failed");
        return end_empty_after_failure();
    }

    ID3D12CommandList* command_lists[]{smoke.command_list.Get()};
    if (depth_recorded) smoke.compositor_depth.gpu_pending=true;
    impl_->graphics->queue()->ExecuteCommandLists(1, command_lists);
    if (smoke.neural) smoke.neural->SubmitRecordedInput(impl_->graphics->queue());
    // Metadata follows the queued cache copy, even if producer acknowledgement
    // later fails. A stale cache label must never describe fresh color/depth.
    if (fresh_gpu_stream_copy_recorded && smoke.native_stereo)
        smoke.cached_raw_stereo_frame=candidate_stereo_frame;
    if (fresh_gpu_stream_copy_recorded) {
        const GpuStreamConsumerStatus consumed =
            smoke.gpu_stream.SignalConsumed(impl_->graphics->queue(),
                                            gpu_stream_frame);
        if (consumed == GpuStreamConsumerStatus::Ok) {
            ++smoke.gpu_stream_frames_copied;
            if (smoke.native_stereo && smoke.gpu_stream_frames_copied==1) {
                impl_->logger->write(LogLevel::info,"native_stereo_first_copy",
                    "Copied both native eyes with their render-time pose",
                    {{"camera_frame",std::to_string(candidate_stereo_frame.camera_frame_id)},
                     {"xr_frame",std::to_string(candidate_stereo_frame.request.frame_id)},
                     {"eye_width",std::to_string(candidate_stereo_frame.request.render_width)},
                     {"eye_height",std::to_string(candidate_stereo_frame.request.render_height)}});
            }
        } else {
            ++smoke.gpu_stream_errors;
            if (!smoke.gpu_stream_error_logged) {
                impl_->logger->write(
                    LogLevel::warning,
                    "gpu_stream_consumed_signal_failed",
                    "The optional consumed fence could not be signalled; fallbacks remain active",
                    {{"status", std::string(ToString(consumed))}});
                smoke.gpu_stream_error_logged = true;
            }
        }
    } else if (gpu_stream_copy_recorded) {
        ++smoke.gpu_stream_frames_reused;
    } else if (smoke.game_image_enabled) {
        ++smoke.gpu_stream_fallback_frames;
    }
    if (smoke.native_stereo) {
        // Reprojection targets the fresh raw camera; never label warped pixels
        // with the old neural pose (OpenXR would apply head rotation twice).
        smoke.cached_stereo_frame=gpu_stream_copy_recorded ? displayed_frame:k2vr::ipc::StereoFrameMetadata{};
        if (neural_displayed && neural_frame.ready_value!=smoke.neural_last_pair) {
            smoke.neural_last_pair=neural_frame.ready_value; ++smoke.neural_pairs;
            if (smoke.neural_pairs==1 || smoke.neural_pairs%60==0)
                impl_->logger->write(LogLevel::info,"native_neural_pair_presented","Both processed eyes and matching HUD copied to XR",
                    {{"pairs",std::to_string(smoke.neural_pairs)},{"ready",std::to_string(neural_frame.ready_value)},
                     {"camera_frame",std::to_string(neural_frame.camera_frame_id)},
                     {"age_ms",std::to_string(static_cast<double>(token.predicted_display_time_ns-neural_frame.request.predicted_display_time_ns)/1000000.0)},
                     {"guide_ms",std::to_string(smoke.neural->timing().guide_ms)},
                     {"worker_ms",std::to_string(smoke.neural->timing().worker_ms)},
                     {"compose_ms",std::to_string(smoke.neural->timing().compose_ms)},
                     {"idle_ms",std::to_string(smoke.neural->timing().idle_ms)},
                     {"input_queue_ms",std::to_string(smoke.neural->timing().input_queue_ms)}});
        }
    }
    const std::uint64_t fence_value = ++smoke.fence_value;
    graphics_result = impl_->graphics->queue()->Signal(smoke.fence.Get(), fence_value);
    if (FAILED(graphics_result)) {
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_signal_failed",
                             "Unable to signal the diagnostic GPU fence",
                             {{"hresult", std::to_string(graphics_result)}});
        impl_->state_machine->mark_failed("D3D12 queue signal failed");
        return end_empty_after_failure();
    }
    if (smoke.fence->GetCompletedValue() < fence_value) {
        graphics_result =
            smoke.fence->SetEventOnCompletion(fence_value, smoke.fence_event);
        if (FAILED(graphics_result) ||
            WaitForSingleObject(smoke.fence_event, 5000) != WAIT_OBJECT_0) {
            impl_->logger->write(LogLevel::error,
                                 "visible_smoke_gpu_wait_failed",
                                 "Diagnostic GPU work did not complete",
                                 {{"hresult", std::to_string(graphics_result)}});
            impl_->state_machine->mark_failed("D3D12 diagnostic GPU wait failed");
            return end_empty_after_failure();
        }
    }

    const auto completed_value=smoke.fence->GetCompletedValue();
    if (completed_value==(std::numeric_limits<std::uint64_t>::max)() ||
        completed_value<fence_value) {
        impl_->state_machine->mark_failed("D3D12 fence retirement unconfirmed");
        return end_empty_after_failure();
    }
    smoke.compositor_depth.gpu_pending=false;
    const bool depth_released=smoke.compositor_depth.ReleaseReady();
    const bool submit_compositor_depth=depth_recorded && depth_released;
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (reprojection_recorded) {
        ++smoke.reprojected_frames;
        if (smoke.cached_raw_stereo_frame.ready_value!=smoke.reprojected_last_raw) {
            ++smoke.reprojected_fresh; smoke.reprojected_last_raw=smoke.cached_raw_stereo_frame.ready_value;
        }
        const auto gpu_ms=smoke.reprojection->CompletedGpuMilliseconds(smoke.reprojection_frequency);
        smoke.reprojection_gpu_ms_sum+=gpu_ms;
        if (smoke.reprojected_frames==1 || smoke.reprojected_frames%360==0)
            impl_->logger->write(LogLevel::info,"native_neural_reprojection","Depth-warped neural pair to current raw camera and HUD",
                {{"frames",std::to_string(smoke.reprojected_frames)},{"fresh_geometry",std::to_string(smoke.reprojected_fresh)},
                 {"raw_ready",std::to_string(smoke.cached_raw_stereo_frame.ready_value)},
                 {"neural_ready",std::to_string(neural_frame.ready_value)},
                 {"gpu_ms",std::to_string(gpu_ms)},
                 {"gpu_ms_mean",std::to_string(smoke.reprojection_gpu_ms_sum/static_cast<double>(smoke.reprojected_frames))},
                 {"geometry_age_ms",std::to_string(static_cast<double>(token.predicted_display_time_ns-smoke.cached_raw_stereo_frame.request.predicted_display_time_ns)/1000000.0)}});
    }
    result = xrReleaseSwapchainImage(smoke.swapchain, &release_info);
    if (XR_FAILED(result)) {
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_release_failed",
                             "Unable to release the diagnostic swapchain image",
                             {{"xr_result", std::to_string(result)}});
        impl_->state_machine->mark_failed("xrReleaseSwapchainImage failed");
        return end_empty_after_failure();
    }

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const bool view_locked_game_image = smoke.game_image_enabled;
    quad.space = view_locked_game_image ? impl_->view_space : impl_->local_space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = smoke.swapchain;
    quad.subImage.imageRect.extent = {
        static_cast<std::int32_t>(smoke.layout.pixel_extent.width),
        static_cast<std::int32_t>(smoke.layout.pixel_extent.height)};
    quad.pose.orientation.w = 1.0F;
    quad.pose.position.z = -(view_locked_game_image
                                 ? game_image_quad_distance_m
                                 : smoke.layout.distance_m);
    quad.size = view_locked_game_image
                    ? XrExtent2Df{game_image_quad_width_m,
                                  game_image_quad_height_m}
                    : XrExtent2Df{smoke.layout.width_m, smoke.layout.height_m};
    if (smoke.auxiliary_theater) {
        quad.space=impl_->local_space;
        // Hold the level screen in the room. Entering the menu or pressing
        // recenter places it along the current gaze; later head motion does
        // not move it. The captured anchor already excludes head roll.
        quad.pose=impl_->theater_pose;
        quad.size={2.4F,1.35F};
    }
    const XrCompositionLayerBaseHeader* layers[]{
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad),nullptr};
    XrCompositionLayerQuad hud{XR_TYPE_COMPOSITION_LAYER_QUAD};
    std::uint32_t layer_count=1;
    std::array<XrCompositionLayerProjectionView,2> projection_views{};
    std::array<XrCompositionLayerDepthInfoKHR,2> depth_views{};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    if (smoke.native_stereo && gpu_stream_copy_recorded &&
        smoke.cached_stereo_frame.ready_value != 0) {
        const auto& rendered = smoke.cached_stereo_frame.request;
        for (std::size_t eye=0;eye<2;++eye) {
            auto& v=projection_views[eye];
            const auto& source=rendered.views[eye];
            v.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            v.pose.orientation={source.pose.orientation_x,source.pose.orientation_y,
                                 source.pose.orientation_z,source.pose.orientation_w};
            v.pose.position={source.pose.position_x,source.pose.position_y,source.pose.position_z};
            v.fov={source.fov.angle_left,source.fov.angle_right,source.fov.angle_up,source.fov.angle_down};
            v.subImage.swapchain=smoke.swapchain;
            v.subImage.imageRect.offset={static_cast<std::int32_t>(eye*rendered.render_width),0};
            v.subImage.imageRect.extent={static_cast<std::int32_t>(rendered.render_width),
                                         static_cast<std::int32_t>(rendered.render_height)};
            if (submit_compositor_depth) {
                auto& d=depth_views[eye];
                const auto& range=displayed_frame.depth[eye];
                d.type=XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR;
                d.subImage.swapchain=smoke.compositor_depth.swapchain;
                d.subImage.imageArrayIndex=0;
                d.subImage.imageRect=v.subImage.imageRect;
                d.minDepth=range.min_depth; d.maxDepth=range.max_depth;
                d.nearZ=range.near_z; d.farZ=range.far_z; // Already metres; do not divide again.
                v.next=&d;
            }
        }
        projection.space=impl_->local_space;
        projection.viewCount=2;
        projection.views=projection_views.data();
        layers[0]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        const auto& frame=smoke.cached_stereo_frame;
        // Suppress only the submitted XR HUD layer, including cached neural HUDs.
        // Keep the atlas, monitor UI and theater image intact.
        if (impl_->hud_visible && impl_->have_ui_head && frame.hud_width && frame.hud_height && frame.hud_width<=smoke.layout.pixel_extent.width &&
            frame.hud_height<=k2vr::ipc::kStereoHudMaximumHeight &&
            std::isfinite(frame.hud_source_aspect) && frame.hud_source_aspect>0.2F && frame.hud_source_aspect<8.0F) {
            hud.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            hud.space=impl_->local_space; hud.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
            hud.subImage.swapchain=smoke.swapchain;
            hud.subImage.imageRect.offset={0,static_cast<std::int32_t>(rendered.render_height)};
            hud.subImage.imageRect.extent={static_cast<std::int32_t>(frame.hud_width),static_cast<std::int32_t>(frame.hud_height)};
            hud.pose=ToXrPose(PlaceUiPanel(impl_->upright_ui_head,1.5F));
            constexpr float hud_center_scale=0.88F;
            hud.size={2.4F*hud_center_scale,2.4F*hud_center_scale/frame.hud_source_aspect};
            if (rendered.presentation_state==k2vr::ipc::PresentationState::DialogueStereo) {
                // Bring the original bottom/corner-anchored answers into the
                // central field without cropping text or changing hit targets.
                hud.size={1.85F*hud_center_scale,1.85F*hud_center_scale/frame.hud_source_aspect};
                hud.pose=ToXrPose(PlaceUiPanel(impl_->upright_ui_head,1.5F,0.14F*hud_center_scale));
            }
            layers[layer_count++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hud);
        }
    }
    XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
    end_info.displayTime = token.predicted_display_time_ns;
    end_info.environmentBlendMode = impl_->blend_mode;
    end_info.layerCount = layer_count;
    end_info.layers = layers;
    result = xrEndFrame(impl_->session, &end_info);
    timing.submit_ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - submit_start)
                           .count();
    token.begun = false;
    if (XR_FAILED(result)) {
        impl_->logger->write(LogLevel::error,
                             "visible_smoke_end_frame_failed",
                             "xrEndFrame rejected the diagnostic quad",
                             {{"xr_result", std::to_string(result)}});
        impl_->state_machine->mark_failed("xrEndFrame failed");
        return false;
    }
    if (submit_compositor_depth && (++smoke.compositor_depth.submissions==1 || smoke.compositor_depth.submissions%360==0))
        impl_->logger->write(LogLevel::info,"native_compositor_depth_first_submission",
            "Submitting both eyes with matching metric depth; HUD remains a separate quad",
            {{"submissions",std::to_string(smoke.compositor_depth.submissions)},
             {"ready",std::to_string(displayed_frame.ready_value)},
             {"source",reprojection_recorded ? "reprojected_raw_depth":(neural_displayed ? "retained_neural_depth":"raw_depth")},
             {"near_m",std::to_string(displayed_frame.depth[0].near_z)},
             {"far_m",std::to_string(displayed_frame.depth[0].far_z)}});
    const auto stats_now = std::chrono::steady_clock::now();
    // Bounded diagnostic after warm-up, recording successful XR submissions,
    // not claiming that each submission contains a newly rendered neural pair.
    if (smoke.presentation_trace_enabled && smoke.presentation_trace_frames<360 &&
        smoke.neural_pairs>=120 && smoke.native_stereo && gpu_stream_copy_recorded &&
        smoke.cached_stereo_frame.ready_value!=0) {
        const auto& presented=smoke.cached_stereo_frame;
        impl_->logger->write(LogLevel::info,"native_presentation_trace","Successful XR stereo submission",
            {{"sample",std::to_string(++smoke.presentation_trace_frames)},
             {"display_ns",std::to_string(token.predicted_display_time_ns)},
             {"source_ns",std::to_string(presented.request.predicted_display_time_ns)},
             {"pose_ready",std::to_string(presented.ready_value)},
             {"neural_ready",std::to_string(neural_displayed ? neural_frame.ready_value:0)},
             {"source",reprojection_recorded ? "reprojected":(neural_displayed ? "neural":"raw")},
             {"presentation",std::to_string(static_cast<unsigned>(presented.request.presentation_state))},
             {"history_reset",std::to_string(presented.request.history_reset_reasons)},
             {"layers",std::to_string(layer_count)}});
    }
    if (smoke.game_image_enabled &&
        stats_now >= smoke.next_gpu_stream_stats_log) {
        const double seconds=smoke.stats_previous_time.time_since_epoch().count()!=0 ?
            std::chrono::duration<double>(stats_now-smoke.stats_previous_time).count():0.0;
        const double fresh_fps=seconds>0 ?
            static_cast<double>(smoke.gpu_stream_frames_copied-smoke.stats_previous_copied)/seconds:0.0;
        impl_->logger->write(
            LogLevel::info,
            "gpu_stream_counters",
            "Optional shared D3D12 stream counters",
            {{"kind", smoke.native_stereo ? "native-stereo":(smoke.auxiliary_theater ? "theater":"mono")},
             {"fresh_fps", seconds>0 ? std::to_string(fresh_fps):"warming-up"},
             {"neural_fps", seconds>0 ? std::to_string(static_cast<double>(smoke.neural_pairs-smoke.stats_previous_neural_pairs)/seconds):"warming-up"},
             {"neural_enabled", smoke.neural && smoke.neural_enabled ? "true":"false"},
             {"open", smoke.gpu_stream.is_open() ? "true" : "false"},
             {"slots", std::to_string(smoke.gpu_stream.slot_count())},
             {"copied", std::to_string(smoke.gpu_stream_frames_copied)},
             {"reused", std::to_string(smoke.gpu_stream_frames_reused)},
             {"fallback", std::to_string(smoke.gpu_stream_fallback_frames)},
             {"errors", std::to_string(smoke.gpu_stream_errors)},
             {"last_consumed",
              std::to_string(smoke.gpu_stream.last_consumed_value())}});
        smoke.next_gpu_stream_stats_log = stats_now + std::chrono::seconds(5);
        smoke.stats_previous_time=stats_now;
        smoke.stats_previous_copied=smoke.gpu_stream_frames_copied;
        smoke.stats_previous_neural_pairs=smoke.neural_pairs;
    }
    return true;
#endif
}

void OpenXrRuntime::shutdown() noexcept {
    if (!impl_) {
        return;
    }
#if KOTORVR_HAS_OPENXR
    // This must precede EVERY resource release, including cached source atlases,
    // immutable descriptor bindings, neural outputs and both XR swapchains.
    if (!impl_->RetireGpuWork()) {
        // Intentionally retain the entire ownership graph (and device/queue)
        // until process exit. Freeing it after a timeout would be a GPU UAF.
        (void)impl_.release();
        return;
    }
    impl_->visible_smoke.ready = false;
    impl_->movie_channel.Close();
    impl_->movie_snapshot={};impl_->movie_image={};impl_->movie_was_active=false;
    impl_->next_movie_open_attempt={};
    impl_->upright_ui_head={}; impl_->ui_head_yaw=0;
    impl_->have_ui_head=impl_->previous_movie_theater=impl_->ui_pose_warning_logged=false;
    impl_->visible_smoke.compositor_depth.Reset();
    impl_->visible_smoke.game_image_reader.Close();
    impl_->visible_smoke.game_image_upload.Shutdown();
    impl_->visible_smoke.gpu_stream.Close();
    impl_->visible_smoke.stereo_metadata.Close();
    impl_->visible_smoke.neural.reset();
    impl_->visible_smoke.reprojection.reset();
    impl_->visible_smoke.game_image_enabled = false;
    if (impl_->visible_smoke.fence_event != nullptr) {
        CloseHandle(impl_->visible_smoke.fence_event);
        impl_->visible_smoke.fence_event = nullptr;
    }
    impl_->visible_smoke.fence.Reset();
    impl_->visible_smoke.command_list.Reset();
    impl_->visible_smoke.command_allocator.Reset();
    impl_->visible_smoke.rtv_heap.Reset();
    impl_->visible_smoke.images.clear();
    if (impl_->visible_smoke.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(impl_->visible_smoke.swapchain);
        impl_->visible_smoke.swapchain = XR_NULL_HANDLE;
    }
    auto& theater=impl_->theater_smoke;
    theater.ready=false;
    theater.compositor_depth.Reset();
    theater.game_image_reader.Close(); theater.game_image_upload.Shutdown();
    theater.gpu_stream.Close(); theater.stereo_metadata.Close();
    theater.neural.reset(); theater.reprojection.reset();
    if (theater.fence_event) { CloseHandle(theater.fence_event); theater.fence_event=nullptr; }
    theater.fence.Reset(); theater.command_list.Reset(); theater.command_allocator.Reset();
    theater.rtv_heap.Reset(); theater.images.clear();
    if (theater.swapchain!=XR_NULL_HANDLE) { xrDestroySwapchain(theater.swapchain); theater.swapchain=XR_NULL_HANDLE; }
    if (impl_->view_space != XR_NULL_HANDLE) {
        xrDestroySpace(impl_->view_space);
        impl_->view_space = XR_NULL_HANDLE;
    }
    if (impl_->local_space != XR_NULL_HANDLE) {
        xrDestroySpace(impl_->local_space);
        impl_->local_space = XR_NULL_HANDLE;
    }
    if (impl_->session != XR_NULL_HANDLE) {
        xrDestroySession(impl_->session);
        impl_->session = XR_NULL_HANDLE;
    }
    if (impl_->instance != XR_NULL_HANDLE) {
        xrDestroyInstance(impl_->instance);
        impl_->instance = XR_NULL_HANDLE;
    }
    if (impl_->retirement_event) {
        CloseHandle(impl_->retirement_event);
        impl_->retirement_event=nullptr;
    }
    impl_->retirement_fence.Reset();
    impl_->retirement_value=0;
    impl_->gpu_queue.Reset();
    impl_->gpu_device.Reset();
#endif
    impl_->graphics=nullptr;
    impl_->available = false;
}

bool OpenXrRuntime::compiled_with_openxr() const noexcept {
    return KOTORVR_HAS_OPENXR != 0;
}

bool OpenXrRuntime::available() const noexcept { return impl_ && impl_->available; }

std::string_view OpenXrRuntime::runtime_name() const noexcept {
    return impl_ ? std::string_view(impl_->runtime_name) : std::string_view{};
}

Extent2D OpenXrRuntime::recommended_view_extent() const noexcept {
#if KOTORVR_HAS_OPENXR
    return impl_ ? impl_->recommended_view_extent : Extent2D{};
#else
    return {};
#endif
}

} // namespace kotorvr::host
