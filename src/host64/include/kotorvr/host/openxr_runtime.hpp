#pragma once

#include "kotorvr/host/d3d12_context.hpp"
#include "kotorvr/host/logger.hpp"
#include "kotorvr/host/session_state_machine.hpp"
#include "kotorvr/host/types.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace kotorvr::host {

struct RuntimeStartResult {
    bool success{};
    bool compiled_with_openxr{};
    std::string message;
};

struct RuntimePollResult {
    bool success{true};
    bool exit_requested{};
    bool history_reset_requested{};
    // A local-space change invalidates both the physical tracking origin and
    // view-dependent temporal data. change_time is the signed OpenXR XrTime.
    bool tracking_reset_requested{};
    std::int64_t tracking_change_time_ns{};
};

struct LocatedViews {
    std::int64_t predicted_display_time_ns{};
    std::array<EyeView, eye_count> views{};
    bool position_valid{};
    bool orientation_valid{};
};

struct XrFrameToken {
    std::uint64_t sequence{};
    std::int64_t predicted_display_time_ns{};
    bool should_render{};
    bool begun{};
};

class OpenXrRuntime final {
public:
    OpenXrRuntime();
    ~OpenXrRuntime();

    OpenXrRuntime(const OpenXrRuntime&) = delete;
    OpenXrRuntime& operator=(const OpenXrRuntime&) = delete;

    [[nodiscard]] RuntimeStartResult start(D3D12Context& graphics,
                                           SessionStateMachine& state_machine,
                                           Logger& logger);
    [[nodiscard]] RuntimePollResult poll_events();
    [[nodiscard]] bool wait_begin_frame(XrFrameToken& token, FrameTiming& timing);
    [[nodiscard]] bool locate_views(const XrFrameToken& token, LocatedViews& views);
    [[nodiscard]] bool end_frame(XrFrameToken& token, FrameTiming& timing);
    [[nodiscard]] bool initialize_visible_smoke(
        std::optional<k2vr::ipc::SessionNonce> game_image_nonce = std::nullopt,
        bool native_stereo = false,
        bool auxiliary_theater = false);
    [[nodiscard]] bool end_frame_visible_smoke(XrFrameToken& token,
                                               FrameTiming& timing);
    void shutdown() noexcept;

    [[nodiscard]] bool compiled_with_openxr() const noexcept;
    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] std::string_view runtime_name() const noexcept;
    [[nodiscard]] Extent2D recommended_view_extent() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kotorvr::host
