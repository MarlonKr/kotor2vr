#pragma once

#include "../common/gpu_stream_contract.hpp"

#include <cstdint>
#include <limits>
#include <string_view>

namespace k2vr::game32 {

enum class GpuStreamProducerStatus : std::uint32_t {
    Ok = 0,
    AlreadyStarted,
    InvalidSession,
    OutOfMemory,
    MissingGlEntryPoint,
    InteropInitializationFailure,
    TextureRegistrationFailure,
    FenceCreationFailure,
    FenceShareFailure,
    FramebufferCreationFailure,
    NoCurrentGlContext,
    InvalidViewport,
    ResolveTargetFailure,
    FramebufferIncomplete,
    InteropLockFailure,
    GlBlitFailure,
    InteropUnlockFailure,
    FenceSignalFailure,
    FenceDeviceRemoved,
    FenceValueExhausted,
    NotStarted,
    ContextMismatch,
    CleanupFailure,
    PrimaryInteropWaitFailure,
    PrimaryInteropSignalFailure,
};

inline constexpr std::uint32_t kGpuStreamInteropStatusUnavailable =
    (std::numeric_limits<std::uint32_t>::max)();
inline constexpr std::uint32_t kGpuStreamGlExtInteropStatusTag =
    0x80000000U;

enum class GpuStreamInteropBackend : std::uint8_t {
    None = 0,
    GlExtD3D12,
    NvDxInterop,
};

[[nodiscard]] constexpr std::uint32_t EncodeGpuStreamInteropStatus(
    const GpuStreamInteropBackend backend,
    const std::uint32_t status) noexcept {
    return backend == GpuStreamInteropBackend::GlExtD3D12
               ? kGpuStreamGlExtInteropStatusTag | status
               : status;
}

// The EXT/D3D12 path explicitly reacquires the texture after the host has
// signalled consumption. The legacy NV path performs ownership transfer in
// Lock/Unlock instead.
[[nodiscard]] constexpr bool ShouldWaitForGpuStreamConsumed(
    const GpuStreamInteropBackend backend,
    const std::uint64_t last_produced) noexcept {
    return backend == GpuStreamInteropBackend::GlExtD3D12 &&
           last_produced != 0U;
}

// Small POD result copied into the already nonce-bound CPU snapshot after the
// F7 start attempt. This keeps diagnostics available even when the normal x86
// log path differs from the launcher's environment.
struct GpuStreamProducerDiagnostic {
    GpuStreamProducerStatus status{GpuStreamProducerStatus::NotStarted};
    std::uint32_t interop_status{kGpuStreamInteropStatusUnavailable};
    std::int32_t hresult{};
    std::uint32_t win32_error{};
};

[[nodiscard]] constexpr GpuStreamProducerDiagnostic
MakeGpuStreamProducerDiagnostic(
    const GpuStreamProducerStatus status,
    const std::uint32_t interop_status =
        kGpuStreamInteropStatusUnavailable,
    const std::int32_t hresult = 0,
    const std::uint32_t win32_error = 0) noexcept {
    return {status, interop_status, hresult, win32_error};
}

enum class GpuStreamFrameDisposition : std::uint8_t {
    Produce = 0,
    DropConsumerBusy,
    StopDeviceRemoved,
    StopFenceValueExhausted,
};

struct GpuStreamFrameDecision {
    GpuStreamFrameDisposition disposition{
        GpuStreamFrameDisposition::DropConsumerBusy};
    std::uint64_t next_ready_value{};
};

// GetCompletedValue()==UINT64_MAX means device removal. Value zero is valid for
// the newly created consumed fence. UINT64_MAX is deliberately never signalled.
[[nodiscard]] constexpr GpuStreamFrameDecision DecideGpuStreamFrame(
    const std::uint64_t last_produced,
    const std::uint64_t consumed) noexcept {
    constexpr std::uint64_t invalid =
        (std::numeric_limits<std::uint64_t>::max)();
    if (consumed == invalid) {
        return {GpuStreamFrameDisposition::StopDeviceRemoved, 0U};
    }
    if (last_produced >= invalid - 1U) {
        return {GpuStreamFrameDisposition::StopFenceValueExhausted, 0U};
    }
    if (!ipc::CanProduceGpuStreamFrame(last_produced, consumed)) {
        return {GpuStreamFrameDisposition::DropConsumerBusy, 0U};
    }
    return {GpuStreamFrameDisposition::Produce, last_produced + 1U};
}

[[nodiscard]] constexpr GpuStreamFrameDecision DecideGpuStreamRingFrame(
    const std::uint64_t last_produced,
    const std::uint64_t selected_slot_last_use,
    const std::uint64_t consumed) noexcept {
    constexpr std::uint64_t invalid =
        (std::numeric_limits<std::uint64_t>::max)();
    if (consumed == invalid) {
        return {GpuStreamFrameDisposition::StopDeviceRemoved, 0U};
    }
    if (last_produced >= invalid - 1U) {
        return {GpuStreamFrameDisposition::StopFenceValueExhausted, 0U};
    }
    if (!ipc::CanReuseGpuStreamSlot(selected_slot_last_use, consumed)) {
        return {GpuStreamFrameDisposition::DropConsumerBusy, 0U};
    }
    return {GpuStreamFrameDisposition::Produce, last_produced + 1U};
}

[[nodiscard]] std::string_view ToString(
    GpuStreamProducerStatus status) noexcept;

// All functions are render-thread-only. Start is called after the diagnostic F7
// edge while KOTOR's WGL context is current. Produce never waits for the host;
// an occupied single slot is dropped immediately.
[[nodiscard]] GpuStreamProducerStatus StartGpuStreamProducer(
    ipc::SessionNonce session_nonce,
    GpuStreamProducerDiagnostic* diagnostic = nullptr,
    std::uint32_t width = ipc::kGpuStreamWidth,
    std::uint32_t height = ipc::kGpuStreamHeight) noexcept;
void ProduceGpuStreamAfterScenePass(std::uint64_t scene_frame_id) noexcept;
void NotifyGpuStreamContextDeleted(void* context) noexcept;
void RecoverGpuStreamContext() noexcept;
[[nodiscard]] bool GpuStreamContextRecoveryPending() noexcept;
[[nodiscard]] GpuStreamProducerStatus ShutdownGpuStreamProducer() noexcept;
[[nodiscard]] bool IsGpuStreamProducerRunning() noexcept;

} // namespace k2vr::game32
