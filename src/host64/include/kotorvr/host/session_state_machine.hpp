#pragma once

#include "kotorvr/host/logger.hpp"

#include <cstdint>
#include <string_view>

namespace kotorvr::host {

// Mirrors the semantic OpenXR states without exposing OpenXR headers to the
// state machine or to stub builds.
enum class RuntimeSessionState : std::uint8_t {
    unknown,
    idle,
    ready,
    synchronized,
    visible,
    focused,
    stopping,
    loss_pending,
    exiting,
};

[[nodiscard]] constexpr std::string_view to_string(RuntimeSessionState state) noexcept {
    switch (state) {
    case RuntimeSessionState::unknown: return "unknown";
    case RuntimeSessionState::idle: return "idle";
    case RuntimeSessionState::ready: return "ready";
    case RuntimeSessionState::synchronized: return "synchronized";
    case RuntimeSessionState::visible: return "visible";
    case RuntimeSessionState::focused: return "focused";
    case RuntimeSessionState::stopping: return "stopping";
    case RuntimeSessionState::loss_pending: return "loss_pending";
    case RuntimeSessionState::exiting: return "exiting";
    }
    return "invalid";
}

struct SessionActions {
    bool begin_session{};
    bool end_session{};
    bool request_exit{};
    bool reset_temporal_histories{};
    bool transition_valid{};
};

class SessionStateMachine final {
public:
    explicit SessionStateMachine(Logger& logger);

    [[nodiscard]] SessionActions apply(RuntimeSessionState next);
    void mark_session_begun();
    void mark_session_ended();
    void mark_failed(std::string_view reason);

    [[nodiscard]] RuntimeSessionState state() const noexcept;
    [[nodiscard]] bool session_running() const noexcept;
    [[nodiscard]] bool should_drive_frames() const noexcept;
    [[nodiscard]] bool exit_requested() const noexcept;
    [[nodiscard]] bool failed() const noexcept;

private:
    [[nodiscard]] static bool allowed(RuntimeSessionState from,
                                      RuntimeSessionState to) noexcept;

    Logger& logger_;
    RuntimeSessionState state_{RuntimeSessionState::unknown};
    bool session_running_{};
    bool exit_requested_{};
    bool failed_{};
};

} // namespace kotorvr::host

