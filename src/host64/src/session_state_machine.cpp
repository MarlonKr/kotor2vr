#include "kotorvr/host/session_state_machine.hpp"

#include <string>

namespace kotorvr::host {

SessionStateMachine::SessionStateMachine(Logger& logger) : logger_(logger) {}

SessionActions SessionStateMachine::apply(const RuntimeSessionState next) {
    const RuntimeSessionState previous = state_;
    SessionActions actions{};
    actions.transition_valid = allowed(previous, next);

    if (!actions.transition_valid) {
        logger_.write(LogLevel::warning,
                      "xr_session_transition_unexpected",
                      "Runtime reported an unexpected session transition",
                      {{"from", std::string(to_string(previous))},
                       {"to", std::string(to_string(next))}});
        return actions;
    }

    if (previous != next) {
        logger_.write(LogLevel::info,
                      "xr_session_transition",
                      "OpenXR session state changed",
                      {{"from", std::string(to_string(previous))},
                       {"to", std::string(to_string(next))}});
    }

    state_ = next;
    if (next == RuntimeSessionState::ready && !session_running_) {
        actions.begin_session = true;
        actions.reset_temporal_histories = true;
    }
    if (next == RuntimeSessionState::stopping && session_running_) {
        actions.end_session = true;
    }
    if (next == RuntimeSessionState::loss_pending ||
        next == RuntimeSessionState::exiting) {
        actions.request_exit = true;
        actions.reset_temporal_histories = true;
        exit_requested_ = true;
    }
    return actions;
}

void SessionStateMachine::mark_session_begun() {
    session_running_ = true;
    logger_.write(LogLevel::info, "xr_session_begun", "xrBeginSession succeeded");
}

void SessionStateMachine::mark_session_ended() {
    session_running_ = false;
    logger_.write(LogLevel::info, "xr_session_ended", "xrEndSession succeeded");
}

void SessionStateMachine::mark_failed(const std::string_view reason) {
    failed_ = true;
    exit_requested_ = true;
    logger_.write(LogLevel::error, "xr_session_failed", reason);
}

RuntimeSessionState SessionStateMachine::state() const noexcept { return state_; }
bool SessionStateMachine::session_running() const noexcept { return session_running_; }

bool SessionStateMachine::should_drive_frames() const noexcept {
    if (!session_running_) {
        return false;
    }
    return state_ == RuntimeSessionState::ready ||
           state_ == RuntimeSessionState::synchronized ||
           state_ == RuntimeSessionState::visible ||
           state_ == RuntimeSessionState::focused;
}

bool SessionStateMachine::exit_requested() const noexcept { return exit_requested_; }
bool SessionStateMachine::failed() const noexcept { return failed_; }

bool SessionStateMachine::allowed(const RuntimeSessionState from,
                                  const RuntimeSessionState to) noexcept {
    if (from == to) {
        return true;
    }
    if (to == RuntimeSessionState::loss_pending || to == RuntimeSessionState::exiting) {
        return true;
    }
    switch (from) {
    case RuntimeSessionState::unknown:
        return to == RuntimeSessionState::idle || to == RuntimeSessionState::ready;
    case RuntimeSessionState::idle:
        return to == RuntimeSessionState::ready;
    case RuntimeSessionState::ready:
        return to == RuntimeSessionState::synchronized ||
               to == RuntimeSessionState::stopping;
    case RuntimeSessionState::synchronized:
        return to == RuntimeSessionState::visible ||
               to == RuntimeSessionState::stopping ||
               to == RuntimeSessionState::idle;
    case RuntimeSessionState::visible:
        return to == RuntimeSessionState::focused ||
               to == RuntimeSessionState::synchronized ||
               to == RuntimeSessionState::stopping;
    case RuntimeSessionState::focused:
        return to == RuntimeSessionState::visible ||
               to == RuntimeSessionState::stopping;
    case RuntimeSessionState::stopping:
        return to == RuntimeSessionState::idle;
    case RuntimeSessionState::loss_pending:
    case RuntimeSessionState::exiting:
        return false;
    }
    return false;
}

} // namespace kotorvr::host
