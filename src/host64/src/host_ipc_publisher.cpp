#include "kotorvr/host/host_ipc_publisher.hpp"

#include <cmath>
#include <limits>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace kotorvr::host {
namespace {

[[nodiscard]] bool Finite(float value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] bool ValidPose(const Pose& pose) noexcept {
    for (const float value : pose.position) {
        if (!Finite(value)) {
            return false;
        }
    }
    float norm_squared = 0.0F;
    for (const float value : pose.orientation) {
        if (!Finite(value)) {
            return false;
        }
        norm_squared += value * value;
    }
    return Finite(norm_squared) && norm_squared > 1.0e-8F;
}

[[nodiscard]] bool ValidFov(const FieldOfView& fov) noexcept {
    return Finite(fov.angle_left) && Finite(fov.angle_right) &&
           Finite(fov.angle_up) && Finite(fov.angle_down) &&
           fov.angle_left < fov.angle_right &&
           fov.angle_down < fov.angle_up;
}

[[nodiscard]] bool ValidPresentationState(
    k2vr::ipc::PresentationState state) noexcept {
    return state >= k2vr::ipc::PresentationState::UnknownSafe &&
           state <= k2vr::ipc::PresentationState::LoadingTheater;
}

[[nodiscard]] bool ValidResetReasons(std::uint32_t reasons) noexcept {
    constexpr std::uint32_t known =
        static_cast<std::uint32_t>(k2vr::ipc::ResetReason::Startup) |
        static_cast<std::uint32_t>(k2vr::ipc::ResetReason::ResolutionChange) |
        static_cast<std::uint32_t>(k2vr::ipc::ResetReason::SwapchainRecreate) |
        static_cast<std::uint32_t>(k2vr::ipc::ResetReason::RuntimeRestart) |
        static_cast<std::uint32_t>(k2vr::ipc::ResetReason::DialogueCut) |
        static_cast<std::uint32_t>(k2vr::ipc::ResetReason::Recenter) |
        static_cast<std::uint32_t>(k2vr::ipc::ResetReason::CameraModeChange) |
        static_cast<std::uint32_t>(k2vr::ipc::ResetReason::Loading) |
        static_cast<std::uint32_t>(k2vr::ipc::ResetReason::Teleport) |
        static_cast<std::uint32_t>(k2vr::ipc::ResetReason::DeviceReset) |
        static_cast<std::uint32_t>(k2vr::ipc::ResetReason::UserRequested);
    return (reasons & ~known) == 0;
}

[[nodiscard]] bool ValidHealthFlags(const HostHealthUpdate& update) noexcept {
    constexpr std::uint32_t visibility =
        static_cast<std::uint32_t>(k2vr::ipc::RuntimeVisibility::Visible) |
        static_cast<std::uint32_t>(k2vr::ipc::RuntimeVisibility::Focused);
    constexpr std::uint32_t health =
        static_cast<std::uint32_t>(k2vr::ipc::RuntimeHealth::RuntimeReady) |
        static_cast<std::uint32_t>(k2vr::ipc::RuntimeHealth::SwapchainReady) |
        static_cast<std::uint32_t>(k2vr::ipc::RuntimeHealth::AdapterMatched) |
        static_cast<std::uint32_t>(k2vr::ipc::RuntimeHealth::ProducerStalled) |
        static_cast<std::uint32_t>(k2vr::ipc::RuntimeHealth::ConsumerStalled) |
        static_cast<std::uint32_t>(k2vr::ipc::RuntimeHealth::DeviceLost) |
        static_cast<std::uint32_t>(k2vr::ipc::RuntimeHealth::ProtocolError) |
        static_cast<std::uint32_t>(k2vr::ipc::RuntimeHealth::FlatFallbackActive);
    return (update.visibility_flags & ~visibility) == 0 &&
           (update.health_flags & ~health) == 0;
}

} // namespace

SessionNonceParseResult ParseSessionNonceHex(
    const std::string_view value) noexcept {
    SessionNonceParseResult result{};
    if (value.size() != 32) {
        result.status = SessionNonceParseStatus::WrongLength;
        return result;
    }

    const auto nibble = [](const char character) constexpr -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };

    std::uint64_t high{};
    std::uint64_t low{};
    for (std::size_t index = 0; index < value.size(); ++index) {
        const int digit = nibble(value[index]);
        if (digit < 0) {
            result.status = SessionNonceParseStatus::InvalidHexCharacter;
            return result;
        }
        std::uint64_t& half = index < 16 ? high : low;
        half = (half << 4U) | static_cast<std::uint64_t>(digit);
    }

    result.nonce = {low, high};
    if (!k2vr::ipc::IsValid(result.nonce)) {
        result.status = SessionNonceParseStatus::ZeroNonce;
        result.nonce = {};
        return result;
    }
    result.status = SessionNonceParseStatus::Ok;
    return result;
}

RenderRequestBuildResult BuildRenderRequest(
    const LocatedViews& located_views, const XrFrameToken& frame,
    Extent2D extent, k2vr::ipc::PresentationState presentation_state,
    k2vr::ipc::SessionNonce nonce, std::uint64_t generation,
    std::uint32_t history_reset_reasons) noexcept {
    RenderRequestBuildResult result{};
    if (!k2vr::ipc::IsValid(nonce) || generation == 0) {
        result.status = RenderRequestBuildStatus::InvalidSession;
        return result;
    }
    if (!frame.begun || !frame.should_render || frame.sequence == 0 ||
        frame.predicted_display_time_ns <= 0) {
        result.status = RenderRequestBuildStatus::InvalidFrameToken;
        return result;
    }
    if (!located_views.orientation_valid || !located_views.position_valid ||
        located_views.predicted_display_time_ns !=
            frame.predicted_display_time_ns) {
        result.status = RenderRequestBuildStatus::InvalidViews;
        return result;
    }
    for (const EyeView& view : located_views.views) {
        if (!ValidPose(view.pose) || !ValidFov(view.fov)) {
            result.status = RenderRequestBuildStatus::InvalidViews;
            return result;
        }
    }
    if (!extent.valid()) {
        result.status = RenderRequestBuildStatus::InvalidExtent;
        return result;
    }
    if (!ValidPresentationState(presentation_state)) {
        result.status = RenderRequestBuildStatus::InvalidPresentationState;
        return result;
    }
    if (!ValidResetReasons(history_reset_reasons)) {
        result.status = RenderRequestBuildStatus::InvalidResetReasons;
        return result;
    }

    k2vr::ipc::RenderRequest& request = result.request;
    request.header = k2vr::ipc::MakeHeader<k2vr::ipc::RenderRequest>(
        k2vr::ipc::MessageType::RenderRequest, frame.sequence, nonce,
        generation);
    request.frame_id = frame.sequence;
    request.predicted_display_time_ns = frame.predicted_display_time_ns;
    request.render_width = extent.width;
    request.render_height = extent.height;
    request.presentation_state = presentation_state;
    request.history_reset_reasons = history_reset_reasons;
    for (std::size_t index = 0; index < eye_count; ++index) {
        request.views[index].pose = ToIpcPose(located_views.views[index].pose);
        request.views[index].fov = ToIpcFov(located_views.views[index].fov);
    }
    result.status = RenderRequestBuildStatus::Ok;
    return result;
}

std::uint64_t HostIpcPublisher::CurrentQpc() noexcept {
#if defined(_WIN32)
    LARGE_INTEGER now{};
    if (QueryPerformanceCounter(&now) != FALSE && now.QuadPart > 0) {
        return static_cast<std::uint64_t>(now.QuadPart);
    }
#endif
    return 0;
}

k2vr::ipc::SharedMemoryStatus HostIpcPublisher::Start(
    k2vr::ipc::SessionNonce nonce, std::uint64_t generation) noexcept {
    const k2vr::ipc::SharedMemoryStatus created =
        k2vr::ipc::SharedMemoryChannel::CreateHost(nonce, generation, channel_);
    if (created != k2vr::ipc::SharedMemoryStatus::Ok) {
        return created;
    }
    last_health_ = {};
    health_sequence_ = 0;
    const k2vr::ipc::SharedMemoryStatus heartbeat = Heartbeat();
    if (heartbeat != k2vr::ipc::SharedMemoryStatus::Ok) {
        Close();
    }
    return heartbeat;
}

void HostIpcPublisher::Close() noexcept {
    channel_.Close();
    last_health_ = {};
    health_sequence_ = 0;
}

HostIpcPublishResult HostIpcPublisher::PublishFrame(
    const LocatedViews& located_views, const XrFrameToken& frame,
    Extent2D extent, k2vr::ipc::PresentationState presentation_state,
    std::uint32_t history_reset_reasons) noexcept {
    HostIpcPublishResult result{};
    if (!channel_.is_open()) {
        result.transport_status = k2vr::ipc::SharedMemoryStatus::NotOpen;
        return result;
    }
    RenderRequestBuildResult built = BuildRenderRequest(
        located_views, frame, extent, presentation_state,
        channel_.session_nonce(), channel_.generation(), history_reset_reasons);
    result.build_status = built.status;
    if (!built.ok()) {
        result.transport_status = k2vr::ipc::SharedMemoryStatus::InvalidArgument;
        return result;
    }
    result.transport_status = channel_.PublishRenderRequest(built.request);
    return result;
}

k2vr::ipc::SharedMemoryStatus HostIpcPublisher::PublishHostHealth(
    HostHealthUpdate update) noexcept {
    if (!channel_.is_open()) {
        return k2vr::ipc::SharedMemoryStatus::NotOpen;
    }
    if (!ValidHealthFlags(update)) {
        return k2vr::ipc::SharedMemoryStatus::InvalidArgument;
    }
    if (update.heartbeat_qpc == 0) {
        update.heartbeat_qpc = CurrentQpc();
    }
    if (update.heartbeat_qpc == 0 ||
        health_sequence_ == (std::numeric_limits<std::uint64_t>::max)()) {
        return k2vr::ipc::SharedMemoryStatus::InvalidArgument;
    }
    const std::uint64_t next_sequence = health_sequence_ + 1;
    const k2vr::ipc::HealthState health = BuildHostHealthState(
        update, channel_.session_nonce(), channel_.generation(), next_sequence);
    const k2vr::ipc::SharedMemoryStatus published =
        channel_.PublishLocalHealth(health);
    if (published == k2vr::ipc::SharedMemoryStatus::Ok) {
        health_sequence_ = next_sequence;
        last_health_ = update;
    }
    return published;
}

k2vr::ipc::SharedMemoryStatus HostIpcPublisher::Heartbeat() noexcept {
    HostHealthUpdate update = last_health_;
    update.heartbeat_qpc = CurrentQpc();
    return PublishHostHealth(update);
}

k2vr::ipc::SharedMemoryStatus HostIpcPublisher::ReadGameHealth(
    k2vr::ipc::HealthState& output) const noexcept {
    return channel_.ReadPeerHealth(output);
}

} // namespace kotorvr::host
