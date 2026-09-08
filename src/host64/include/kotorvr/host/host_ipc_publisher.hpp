#pragma once

#include "kotorvr/host/openxr_runtime.hpp"
#include "shared_memory_channel.hpp"

#include <cstdint>
#include <string_view>

namespace kotorvr::host {

enum class SessionNonceParseStatus : std::uint32_t {
    Ok = 0,
    WrongLength,
    InvalidHexCharacter,
    ZeroNonce,
};

struct SessionNonceParseResult {
    SessionNonceParseStatus status{SessionNonceParseStatus::WrongLength};
    k2vr::ipc::SessionNonce nonce{};

    [[nodiscard]] constexpr bool ok() const noexcept {
        return status == SessionNonceParseStatus::Ok;
    }
};

// Parses the launcher's Guid "N" representation. The first 16 digits become
// nonce.high and the final 16 become nonce.low, so MakeSharedMemoryObjectName
// reproduces the original 32 digits exactly (apart from upper-case rendering).
[[nodiscard]] SessionNonceParseResult
ParseSessionNonceHex(std::string_view value) noexcept;

enum class RenderRequestBuildStatus : std::uint32_t {
    Ok = 0,
    InvalidSession,
    InvalidFrameToken,
    InvalidViews,
    InvalidExtent,
    InvalidPresentationState,
    InvalidResetReasons,
};

struct RenderRequestBuildResult {
    RenderRequestBuildStatus status{RenderRequestBuildStatus::InvalidSession};
    k2vr::ipc::RenderRequest request{};

    [[nodiscard]] constexpr bool ok() const noexcept {
        return status == RenderRequestBuildStatus::Ok;
    }
};

struct HostHealthUpdate {
    // Zero asks HostIpcPublisher to sample QueryPerformanceCounter at publish
    // time. Tests and replay tools may provide an explicit deterministic QPC.
    std::uint64_t heartbeat_qpc{};
    std::uint64_t presented_frame_count{};
    std::uint64_t dropped_frame_count{};
    std::uint64_t timeout_count{};
    std::uint64_t duplicate_or_stale_count{};
    std::uint32_t error_state{};
    std::uint32_t visibility_flags{};
    std::uint32_t health_flags{};
};

struct HostIpcPublishResult {
    RenderRequestBuildStatus build_status{
        RenderRequestBuildStatus::InvalidSession};
    k2vr::ipc::SharedMemoryStatus transport_status{
        k2vr::ipc::SharedMemoryStatus::NotOpen};

    [[nodiscard]] constexpr bool ok() const noexcept {
        return build_status == RenderRequestBuildStatus::Ok &&
               transport_status == k2vr::ipc::SharedMemoryStatus::Ok;
    }
};

// Pure, OpenXR-header-free wire conversions. The source types are the host's
// stable POD view types, not XrPosef/XrFovf.
[[nodiscard]] constexpr k2vr::ipc::PoseF32 ToIpcPose(
    const Pose& pose) noexcept {
    return {pose.position[0], pose.position[1], pose.position[2],
            pose.orientation[0], pose.orientation[1], pose.orientation[2],
            pose.orientation[3]};
}

[[nodiscard]] constexpr k2vr::ipc::FovF32 ToIpcFov(
    const FieldOfView& fov) noexcept {
    return {fov.angle_left, fov.angle_right, fov.angle_up, fov.angle_down};
}

[[nodiscard]] RenderRequestBuildResult BuildRenderRequest(
    const LocatedViews& located_views, const XrFrameToken& frame,
    Extent2D extent, k2vr::ipc::PresentationState presentation_state,
    k2vr::ipc::SessionNonce nonce, std::uint64_t generation,
    std::uint32_t history_reset_reasons = 0) noexcept;

[[nodiscard]] constexpr k2vr::ipc::HealthState BuildHostHealthState(
    const HostHealthUpdate& update, k2vr::ipc::SessionNonce nonce,
    std::uint64_t generation, std::uint64_t sequence) noexcept {
    k2vr::ipc::HealthState health{};
    health.header = k2vr::ipc::MakeHeader<k2vr::ipc::HealthState>(
        k2vr::ipc::MessageType::HealthState, sequence, nonce, generation);
    health.host_heartbeat_qpc = update.heartbeat_qpc;
    health.presented_frame_count = update.presented_frame_count;
    health.dropped_frame_count = update.dropped_frame_count;
    health.timeout_count = update.timeout_count;
    health.duplicate_or_stale_count = update.duplicate_or_stale_count;
    health.error_state = update.error_state;
    health.visibility_flags = update.visibility_flags;
    health.health_flags = update.health_flags;
    return health;
}

class HostIpcPublisher final {
public:
    HostIpcPublisher() noexcept = default;
    ~HostIpcPublisher() = default;

    HostIpcPublisher(const HostIpcPublisher&) = delete;
    HostIpcPublisher& operator=(const HostIpcPublisher&) = delete;
    HostIpcPublisher(HostIpcPublisher&&) noexcept = default;
    HostIpcPublisher& operator=(HostIpcPublisher&&) noexcept = default;

    // Creates the host mapping and immediately publishes an initial heartbeat.
    [[nodiscard]] k2vr::ipc::SharedMemoryStatus Start(
        k2vr::ipc::SessionNonce nonce, std::uint64_t generation) noexcept;
    void Close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return channel_.is_open(); }
    [[nodiscard]] k2vr::ipc::SessionNonce session_nonce() const noexcept {
        return channel_.session_nonce();
    }
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return channel_.generation();
    }

    [[nodiscard]] HostIpcPublishResult PublishFrame(
        const LocatedViews& located_views, const XrFrameToken& frame,
        Extent2D extent, k2vr::ipc::PresentationState presentation_state,
        std::uint32_t history_reset_reasons = 0) noexcept;

    [[nodiscard]] k2vr::ipc::SharedMemoryStatus PublishHostHealth(
        HostHealthUpdate update) noexcept;

    // Reuses the last counters/flags and advances only time + message sequence.
    [[nodiscard]] k2vr::ipc::SharedMemoryStatus Heartbeat() noexcept;

    [[nodiscard]] k2vr::ipc::SharedMemoryStatus ReadGameHealth(
        k2vr::ipc::HealthState& output) const noexcept;

private:
    [[nodiscard]] static std::uint64_t CurrentQpc() noexcept;

    k2vr::ipc::SharedMemoryChannel channel_;
    HostHealthUpdate last_health_{};
    std::uint64_t health_sequence_{};
};

} // namespace kotorvr::host
