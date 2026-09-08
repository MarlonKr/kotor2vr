#pragma once

#include "../common/stereo_stream.hpp"
#include "../common/math.hpp"

#include <cmath>
#include <cstdint>

#if defined(_WIN32)
#if defined(K2VR_GAME32_BUILD)
#define K2VR_RENDER_TRACE_EXPORT extern "C" __declspec(dllexport)
#else
#define K2VR_RENDER_TRACE_EXPORT extern "C" __declspec(dllimport)
#endif
#define K2VR_RENDER_TRACE_THREAD_CALL __stdcall
#else
#define K2VR_RENDER_TRACE_EXPORT extern "C"
#define K2VR_RENDER_TRACE_THREAD_CALL
#endif

namespace k2vr::game32 {

enum class RenderTraceResult : std::uint32_t {
    Ok = 0,
    InvalidArgument = 1,
    Busy = 2,
    AlreadyStopped = 3,
    PersistentLogFailure = 4,
    ModulePinFailure = 5,
    BuildVerificationFailure = 6,
    WrongExactBuild = 7,
    CandidateValidationFailure = 8,
    WriterStartFailure = 9,
    HookInstallFailure = 10,
    TimeoutStartFailure = 11,
    StopTimeout = 12,
};

// Pure decisions shared by the runtime code and unit tests. A trace install
// may only replace the exact original pointer. Teardown restores only a cell
// that still contains our wrapper, preserving any later third-party change.
enum class InstallCellDecision : std::uint8_t {
    SwapExpected,
    AlreadyOwned,
    RejectForeign,
};

enum class RestoreCellDecision : std::uint8_t {
    RestoreOwned,
    AlreadyOriginal,
    PreserveForeign,
};

struct DoublePassInputTransition {
    bool next_was_down{false};
    bool should_attempt{false};
};

// Exact Steam/Aspyr Camera storage: position XYZ followed by quaternion WXYZ.
// This POD is copied byte-for-byte at Camera+0xA8 for the temporary HMD pass.
struct EngineCameraPoseWxyz {
    float position_x{};
    float position_y{};
    float position_z{};
    float orientation_w{1.0F};
    float orientation_x{};
    float orientation_y{};
    float orientation_z{};
};

struct HmdCameraPoseResult {
    bool valid{false};
    EngineCameraPoseWxyz pose{};
};

[[nodiscard]] inline bool IsConservativeUnitQuaternion(
    const math::Quat value) noexcept {
    if (!math::IsFinite(value)) {
        return false;
    }
    const float norm_squared = math::Dot(value, value);
    return norm_squared >= 0.25F && norm_squared <= 4.0F;
}

[[nodiscard]] inline math::Quat EngineCameraQuaternion(
    const EngineCameraPoseWxyz& pose) noexcept {
    return {pose.orientation_x, pose.orientation_y, pose.orientation_z,
            pose.orientation_w};
}

[[nodiscard]] inline math::Quat OpenXrQuaternion(
    const ipc::PoseF32& pose) noexcept {
    return {pose.orientation_x, pose.orientation_y, pose.orientation_z,
            pose.orientation_w};
}

// The regular follow camera supplies its real target position; gameplay eye
// height is calibrated explicitly, without relying on a guessed camera distance.
// Remove the monitor camera's downward pitch but preserve its horizontal heading.
[[nodiscard]] inline HmdCameraPoseResult ComposeFirstPersonAnchor(
    const EngineCameraPoseWxyz& authored, math::Vec3 target,
    float units_per_metre, float height_m, float forward_m) noexcept {
    if (!IsConservativeUnitQuaternion(EngineCameraQuaternion(authored)) ||
        !math::IsFinite(target) || !std::isfinite(units_per_metre) || units_per_metre<=0 ||
        !std::isfinite(height_m) || height_m<0.2F || height_m>3.0F ||
        !std::isfinite(forward_m) || forward_m<0 || forward_m>0.5F) return {};
    auto forward=math::Rotate(EngineCameraQuaternion(authored),{0,0,-1});
    forward.z=0;
    const float length=math::Length(forward);
    if (length<0.001F) return {};
    forward=forward/length;
    const auto heading=math::FromAxisAngle({0,0,1},std::atan2(-forward.x,forward.y));
    const auto upright=math::Multiply(heading,math::FromAxisAngle({1,0,0},math::kPi*0.5F));
    target=target+forward*(forward_m*units_per_metre);
    target.z+=height_m*units_per_metre;
    if (!math::IsFinite(target)) return {};
    return {true,{target.x,target.y,target.z,upright.w,upright.x,upright.y,upright.z}};
}

// Camera orientation already maps camera-local OpenGL axes into world space.
// Exact engine 0x435F00 applies its inverse directly (glRotatef / glMultMatrixf).
// XR and GL CAMERA-local axes match; do not apply world-Z-up conversion twice.
[[nodiscard]] inline HmdCameraPoseResult ComposeHmdCameraPose(
    const EngineCameraPoseWxyz& authored,
    const ipc::PoseF32& baseline_xr,
    const ipc::PoseF32& current_xr) noexcept {
    const math::Quat authored_quaternion = EngineCameraQuaternion(authored);
    const math::Quat baseline_quaternion = OpenXrQuaternion(baseline_xr);
    const math::Quat current_quaternion = OpenXrQuaternion(current_xr);
    if (!std::isfinite(authored.position_x) ||
        !std::isfinite(authored.position_y) ||
        !std::isfinite(authored.position_z) ||
        !IsConservativeUnitQuaternion(authored_quaternion) ||
        !IsConservativeUnitQuaternion(baseline_quaternion) ||
        !IsConservativeUnitQuaternion(current_quaternion)) {
        return {};
    }

    const math::Quat relative_xr = math::NormalizeOrIdentity(math::Multiply(
        math::Conjugate(math::NormalizeOrIdentity(baseline_quaternion)),
        math::NormalizeOrIdentity(current_quaternion)));
    const math::Quat composed = math::NormalizeOrIdentity(math::Multiply(
        math::NormalizeOrIdentity(authored_quaternion), relative_xr));
    if (!math::IsFinite(composed)) {
        return {};
    }

    EngineCameraPoseWxyz output = authored;
    output.orientation_w = composed.w;
    output.orientation_x = composed.x;
    output.orientation_y = composed.y;
    output.orientation_z = composed.z;
    return {true, output};
}

static_assert(sizeof(EngineCameraPoseWxyz) == 28);

[[nodiscard]] inline ipc::PoseF32 StereoHeadCenter(const ipc::RenderRequest& r) noexcept {
    ipc::PoseF32 center = r.views[0].pose;
    center.position_x = (center.position_x + r.views[1].pose.position_x) * 0.5F;
    center.position_y = (center.position_y + r.views[1].pose.position_y) * 0.5F;
    center.position_z = (center.position_z + r.views[1].pose.position_z) * 0.5F;
    return center;
}

// Recenter changes heading and seated origin, while gravity remains the XR
// runtime's up axis. Capturing pitch/roll here tilts the room after F11 when
// the user looks down or rests their head on one side. Never filter HMD pose.
[[nodiscard]] inline ipc::PoseF32 UprightRecenterPose(
    const ipc::PoseF32& current, const ipc::PoseF32& previous = {}) noexcept {
    auto result=current;
    const auto forward=math::Rotate(math::NormalizeOrIdentity(OpenXrQuaternion(current)),{0,0,-1});
    auto heading=math::Rotate(math::NormalizeOrIdentity(OpenXrQuaternion(previous)),{0,0,-1});
    // Looking nearly vertically has no stable forward heading. Keep the last
    // calibrated yaw in that case; the position still recenters immediately.
    if (forward.x*forward.x+forward.z*forward.z>0.0001F) heading=forward;
    const auto yaw=math::FromAxisAngle({0,1,0},std::atan2(-heading.x,-heading.z));
    result.orientation_x=0; result.orientation_z=0;
    result.orientation_y=yaw.y; result.orientation_w=yaw.w;
    return result;
}

[[nodiscard]] inline HmdCameraPoseResult ComposeStereoEyePose(
    const EngineCameraPoseWxyz& authored, const ipc::PoseF32& baseline,
    const ipc::RenderRequest& request, std::size_t eye, float units_per_metre = 1.F) noexcept {
    if (eye >= 2 || !std::isfinite(units_per_metre) || units_per_metre <= 0.F)
        return {};
    const auto center = StereoHeadCenter(request);
    auto result = ComposeHmdCameraPose(authored, baseline, request.views[eye].pose);
    if (!result.valid) return result;
    const math::Quat inverse_baseline = math::Conjugate(math::NormalizeOrIdentity(OpenXrQuaternion(baseline)));
    const math::Vec3 center_delta{center.position_x-baseline.position_x,
                                  center.position_y-baseline.position_y,
                                  center.position_z-baseline.position_z};
    math::Vec3 lean = math::Rotate(inverse_baseline, center_delta);
    if (!math::IsFinite(lean)) return {};
    const float horizontal = std::sqrt(lean.x*lean.x + lean.z*lean.z);
    if (horizontal > 0.20F) { lean.x *= 0.20F/horizontal; lean.z *= 0.20F/horizontal; }
    lean.y = std::clamp(lean.y, -0.10F, 0.10F);
    const auto& p = request.views[eye].pose;
    const math::Vec3 offset = math::Rotate(inverse_baseline,
        math::Vec3{p.position_x-center.position_x,p.position_y-center.position_y,p.position_z-center.position_z});
    if (!math::IsFinite(offset) || math::Length(offset) > 0.15F) return {};
    const math::Vec3 world = math::Rotate(EngineCameraQuaternion(authored), (lean+offset)*units_per_metre);
    result.pose.position_x += world.x;
    result.pose.position_y += world.y;
    result.pose.position_z += world.z;
    return result;
}

// Accept clips from the actual glFrustum call only when the scene's captured
// projection still matches that asymmetric frustum. No engine-units inference
// from a combined VP matrix and no hard-coded clip distances.
[[nodiscard]] inline std::optional<ipc::DepthRangeF32> NativeStereoDepthRange(
    const math::Matrix4x4& projection,const ipc::FovF32& fov,
    double near_engine,double far_engine,float units_per_metre,
    double window_near,double window_far) noexcept {
    const auto range=ipc::StereoDepthRangeFromOpenGl(near_engine,far_engine,
        units_per_metre,window_near,window_far);
    if (!range) return std::nullopt;
    const auto expected=math::OpenGlProjection(
        {fov.angle_left,fov.angle_right,fov.angle_up,fov.angle_down},
        static_cast<float>(near_engine),static_cast<float>(far_engine));
    if (!expected) return std::nullopt;
    for (std::size_t i=0;i<16;++i) {
        const float actual=projection.value[i],target=expected->value[i];
        if (!std::isfinite(actual) || std::abs(actual-target)>1.0e-5F*(1.0F+std::abs(target)))
            return std::nullopt;
    }
    return range;
}

// The runtime updates the sampled F8 state atomically.  A press that was
// already held when the dangerous diagnostic was armed is deliberately not an
// edge; the user must release and press F8 again.  A claimed session can never
// request another extra pass.
[[nodiscard]] constexpr DoublePassInputTransition EvaluateDoublePassInput(
    bool enabled, bool already_claimed, bool was_down,
    bool is_down) noexcept {
    return {is_down,
            enabled && !already_claimed && !was_down && is_down};
}

[[nodiscard]] constexpr InstallCellDecision DecideInstallCell(
    std::uintptr_t current, std::uintptr_t expected_original,
    std::uintptr_t wrapper) noexcept {
    if (current == expected_original) {
        return InstallCellDecision::SwapExpected;
    }
    if (current == wrapper) {
        return InstallCellDecision::AlreadyOwned;
    }
    return InstallCellDecision::RejectForeign;
}

[[nodiscard]] constexpr RestoreCellDecision DecideRestoreCell(
    std::uintptr_t current, std::uintptr_t expected_original,
    std::uintptr_t wrapper) noexcept {
    if (current == wrapper) {
        return RestoreCellDecision::RestoreOwned;
    }
    if (current == expected_original) {
        return RestoreCellDecision::AlreadyOriginal;
    }
    return RestoreCellDecision::PreserveForeign;
}

} // namespace k2vr::game32

// Explicit opt-in entry points. Both signatures are compatible with
// LPTHREAD_START_ROUTINE and protocol v1 requires a null argument.
K2VR_RENDER_TRACE_EXPORT std::uint32_t K2VR_RENDER_TRACE_THREAD_CALL
K2VR_RenderTraceBootstrap(void* reserved) noexcept;

// Separate dangerous diagnostic entry point.  Unlike the ordinary render
// trace, this arms exactly one user-triggered original Scene+0xB8 extra call.
K2VR_RENDER_TRACE_EXPORT std::uint32_t K2VR_RENDER_TRACE_THREAD_CALL
K2VR_RenderDoublePassBootstrap(void* reserved) noexcept;

K2VR_RENDER_TRACE_EXPORT std::uint32_t K2VR_RENDER_TRACE_THREAD_CALL
K2VR_RenderTraceStop(void* reserved) noexcept;
