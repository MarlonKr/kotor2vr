#pragma once
#include "../common/stereo_stream.hpp"
#include <string_view>
namespace k2vr::game32 {
struct EngineCameraPoseWxyz;
void ConfigureNativeStereoEgoVisibility(math::Vec3 target,const EngineCameraPoseWxyz& eye,float units,bool enabled) noexcept;
[[nodiscard]] bool BeginNativeStereoPair(const ipc::RenderRequest& request,
                                        std::uint64_t camera_frame,
                                        bool disable_monitor_vsync,
                                        const EngineCameraPoseWxyz* eyes,
                                        const void* camera,bool request_gate_enabled,
                                        float engine_units_per_metre,
                                        bool request_cadence=false) noexcept;
void RestoreNativeStereoPacing() noexcept;
[[nodiscard]] bool BeginNativeStereoEye(std::size_t eye) noexcept;
void CaptureNativeStereoEye() noexcept;
void TraceNativeStereoSceneMatrices() noexcept;
void EndNativeStereoEye() noexcept;
[[nodiscard]] bool EndNativeStereoPair(bool success) noexcept;
[[nodiscard]] bool NativeStereoEyeActive() noexcept;
[[nodiscard]] bool NativeStereoHudActive() noexcept;
void BeginNativeStereoHud() noexcept;
void FinishNativeStereoPresent() noexcept;
[[nodiscard]] bool NativeStereoMonitorMirrorEnabled() noexcept;
[[nodiscard]] bool TryNativeStereoMonitorMirror() noexcept;
[[nodiscard]] constexpr bool MonitorMirrorOption(std::string_view value) noexcept { return value=="1"; }
struct MonitorMirrorRect { int x{},y{},width{},height{}; };
// Integer fit, no crop/stretch. Shared by the GL path and CPU smoke checks.
[[nodiscard]] constexpr MonitorMirrorRect FitMonitorMirror(int width,int height,int source_width,int source_height) noexcept {
    if (width<=0 || height<=0 || source_width<=0 || source_height<=0) return {};
    int w=width,h=height;
    if (static_cast<std::int64_t>(width)*source_height>static_cast<std::int64_t>(height)*source_width)
        w=static_cast<int>(static_cast<std::int64_t>(height)*source_width/source_height);
    else h=static_cast<int>(static_cast<std::int64_t>(width)*source_height/source_width);
    return {(width-w)/2,(height-h)/2,w,h};
}
// Called on the game render thread after successful wglDeleteContext, and
// subsequently at the replacement game window's first SwapBuffers.
void NotifyNativeStereoContextDeleted(void* context) noexcept;
void RecoverNativeStereoContext() noexcept;
}
