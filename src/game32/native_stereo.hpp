#pragma once
#include "../common/stereo_stream.hpp"
#include <string_view>
#include <span>
namespace k2vr::game32 {
struct EngineCameraPoseWxyz;
struct EgoMeshSelection {
    math::Vec3 feet{};
    bool short_model{};
    std::array<math::Vec3,2> head_pivots{};
    unsigned head_pivot_mask{};
};
void ConfigureNativeStereoEgoVisibility(const void* target,const EgoMeshSelection& selection,
    const EngineCameraPoseWxyz& eye,float units,bool enabled) noexcept;
struct EgoHeadAttachmentLink {
    const void* child{};
    const void* parent{};
    const void* node{};
    bool supported{};
};
inline constexpr std::size_t kMaxEgoHeadAttachmentDepth=8;
// A separate head Gob is bound to the controlled body's exact HeadHook node.
// Head accessories may descend from that Gob. Body and hand/weapon attachment
// chains never qualify, regardless of their shader, vertex count or position.
[[nodiscard]] inline bool ControlledHeadAttachment(const void* target,const void* head_hook,
    const void* drawing,std::span<const EgoHeadAttachmentLink> chain) noexcept {
    if (!target || !head_hook || !drawing || drawing==target) return false;
    const void* expected=drawing;
    for (std::size_t i=0;i<chain.size() && i<kMaxEgoHeadAttachmentDepth;++i) {
        const auto& link=chain[i];
        if (!link.supported || link.child!=expected || !link.parent || !link.node ||
            link.child==link.parent) return false;
        for (std::size_t j=0;j<i;++j) if (chain[j].child==link.child) return false;
        if (link.parent==target) return link.node==head_hook;
        expected=link.parent;
    }
    return false;
}
// Dispatch by authored model height in engine units, not user world-scale calibration.
[[nodiscard]] inline bool ShortEgoModel(math::Vec3 feet,math::Vec3 model_eye) noexcept {
    return math::IsFinite(feet) && math::IsFinite(model_eye) &&
        model_eye.z-feet.z>=0.05F && model_eye.z-feet.z<1.2F;
}
[[nodiscard]] inline bool NamedEgoHeadPivot(const EgoMeshSelection& selection,math::Vec3 world) noexcept {
    if (!selection.short_model || !math::IsFinite(world)) return false;
    // A small engine-space tolerance covers float roundoff and slight pose
    // changes between render-entry hook sampling and mesh drawing. This is a node-pivot
    // match, not a head-height column or a radius around the headset.
    for (unsigned i=0;i<selection.head_pivots.size();++i) {
        if (!(selection.head_pivot_mask&(1U<<i)) || !math::IsFinite(selection.head_pivots[i])) continue;
        const auto delta=world-selection.head_pivots[i];
        if (math::Dot(delta,delta)<=0.02F*0.02F) return true;
    }
    return false;
}
// The first eye records the engine draw stream; the second eye replays it.
// Pointer equality identifies the controlled graphics object, independent of
// model size or nearby scene geometry. The short-model path additionally
// requires a named head-pivot match; human heads use their attachment chain.
[[nodiscard]] constexpr bool ShouldHideFirstPersonObjectDraw(bool enabled,int recording_eye,
    const void* target,const void* drawing_object) noexcept {
    return enabled && recording_eye==0 && target && target==drawing_object;
}
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
[[nodiscard]] bool NativeStereoContextRecoveryPending() noexcept;
}
