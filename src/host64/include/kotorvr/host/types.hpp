#pragma once

#include "ipc_protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace kotorvr::host {

using Eye = k2vr::ipc::Eye;

inline constexpr std::size_t eye_count = k2vr::ipc::kStereoEyeCount;
inline constexpr std::size_t ring_slot_count = k2vr::ipc::kEyeRingSlotCount;

[[nodiscard]] constexpr std::size_t eye_index(const Eye eye) noexcept {
    return static_cast<std::size_t>(eye);
}

[[nodiscard]] constexpr bool valid_eye(const Eye eye) noexcept {
    return eye == Eye::left || eye == Eye::right;
}

[[nodiscard]] constexpr std::string_view to_string(const Eye eye) noexcept {
    switch (eye) {
    case Eye::left: return "left";
    case Eye::right: return "right";
    }
    return "invalid";
}

struct Extent2D {
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return width != 0 && height != 0;
    }

    friend constexpr bool operator==(const Extent2D&, const Extent2D&) = default;
};

struct Pose {
    std::array<float, 3> position{};
    std::array<float, 4> orientation{0.0F, 0.0F, 0.0F, 1.0F};
};

struct FieldOfView {
    float angle_left{};
    float angle_right{};
    float angle_up{};
    float angle_down{};
};

struct EyeView {
    Pose pose{};
    FieldOfView fov{};
};

struct NativeTexture {
    // Cross-process handles, not process-local pointers. Ownership remains with
    // the producer unless the negotiated IPC protocol says otherwise.
    std::uint64_t shared_handle{};
    std::uint32_t format{};
    Extent2D extent{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return shared_handle != 0 && format != 0 && extent.valid();
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return shared_handle == 0 && format == 0 && extent.width == 0 &&
               extent.height == 0;
    }
};

struct SubmittedEyeFrame {
    Eye eye{Eye::left};
    std::uint64_t frame_id{};
    std::uint64_t stream_generation{};
    std::int64_t predicted_display_time_ns{};
    std::uint64_t producer_fence_handle{};
    std::uint64_t producer_fence_value{};
    NativeTexture color{};
    NativeTexture depth{};
    NativeTexture motion_vectors{};
    EyeView render_view{};
};

struct StereoFramePair {
    std::array<SubmittedEyeFrame, eye_count> eyes{};
    std::array<std::uint32_t, eye_count> slot_indices{};
    std::array<std::uint64_t, eye_count> consumer_lease_ids{};

    [[nodiscard]] constexpr const SubmittedEyeFrame& left() const noexcept {
        return eyes[eye_index(Eye::left)];
    }

    [[nodiscard]] constexpr const SubmittedEyeFrame& right() const noexcept {
        return eyes[eye_index(Eye::right)];
    }
};

} // namespace kotorvr::host
