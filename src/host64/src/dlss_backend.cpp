#include "kotorvr/host/dlss_backend.hpp"

#include <sstream>
#include <utility>

namespace kotorvr::host {
namespace {

constexpr std::array<std::pair<HistoryResetReason, std::string_view>, 7> reset_names{{
    {HistoryResetReason::startup, "startup"},
    {HistoryResetReason::resolution_change, "resolution_change"},
    {HistoryResetReason::swapchain_recreation, "swapchain_recreation"},
    {HistoryResetReason::runtime_restart, "runtime_restart"},
    {HistoryResetReason::camera_cut, "camera_cut"},
    {HistoryResetReason::tracking_discontinuity, "tracking_discontinuity"},
    {HistoryResetReason::user_requested, "user_requested"},
}};

constexpr std::uint64_t left_history_identity = 0x4B56524C00000001ULL;
constexpr std::uint64_t right_history_identity = 0x4B56525200000002ULL;

} // namespace

std::string to_string(const HistoryResetReason reasons) {
    if (reasons == HistoryResetReason::none) {
        return "none";
    }
    std::ostringstream stream;
    bool first = true;
    for (const auto& [reason, name] : reset_names) {
        if (!has_reset_reason(reasons, reason)) {
            continue;
        }
        if (!first) {
            stream << '|';
        }
        stream << name;
        first = false;
    }
    return stream.str();
}

bool SafeDlssStub::initialize(const Extent2D render_extent,
                              const std::uint64_t stream_generation,
                              Logger& logger) {
    shutdown();
    if (!render_extent.valid() || stream_generation == 0) {
        logger.write(LogLevel::error,
                     "dlss_stub_init_invalid",
                     "DLSS stub requires a valid extent and non-zero stream generation");
        return false;
    }

    logger_ = &logger;
    histories_[eye_index(Eye::left)].token =
        {Eye::left, left_history_identity, 1};
    histories_[eye_index(Eye::right)].token =
        {Eye::right, right_history_identity, 1};
    for (auto& history : histories_) {
        history.stream_generation = stream_generation;
        history.extent = render_extent;
        history.pending_reset = HistoryResetReason::startup;
    }
    initialized_ = true;
    logger.write(LogLevel::info,
                 "dlss_stub_ready",
                 "Safe validation-only DLSS backend initialized; output is passthrough",
                 {{"width", std::to_string(render_extent.width)},
                  {"height", std::to_string(render_extent.height)},
                  {"stream_generation", std::to_string(stream_generation)},
                  {"proprietary_modules_loaded", "false"}});
    return true;
}

DlssHistoryToken SafeDlssStub::history_token(const Eye eye) const {
    if (!valid_eye(eye)) {
        return {};
    }
    return histories_[eye_index(eye)].token;
}

bool SafeDlssStub::reset_histories(const Extent2D render_extent,
                                   const std::uint64_t stream_generation,
                                   const HistoryResetReason reason) {
    if (!initialized_ || !render_extent.valid() || stream_generation == 0 ||
        reason == HistoryResetReason::none) {
        if (logger_) {
            logger_->write(LogLevel::error,
                           "dlss_history_reset_rejected",
                           "A stereo history reset requires initialization, a generation, and a reason");
        }
        return false;
    }

    for (auto& history : histories_) {
        ++history.token.generation;
        history.stream_generation = stream_generation;
        history.extent = render_extent;
        history.last_frame_id = 0;
        history.has_frame = false;
        history.pending_reset = history.pending_reset | reason;
    }
    logger_->write(LogLevel::info,
                   "dlss_stereo_histories_reset",
                   "Both eye temporal histories invalidated atomically",
                   {{"left_history_generation",
                     std::to_string(histories_[eye_index(Eye::left)].token.generation)},
                    {"right_history_generation",
                     std::to_string(histories_[eye_index(Eye::right)].token.generation)},
                    {"stream_generation", std::to_string(stream_generation)},
                    {"width", std::to_string(render_extent.width)},
                    {"height", std::to_string(render_extent.height)},
                    {"reason", to_string(reason)}});
    return true;
}

DlssResult SafeDlssStub::evaluate(const DlssFrameRequest& request) {
    if (!initialized_) {
        return invalid(request.eye, "backend is not initialized");
    }
    if (!valid_eye(request.eye)) {
        return invalid(request.eye, "eye identity is outside the negotiated stereo view set");
    }

    EyeHistory& history = histories_[eye_index(request.eye)];
    const Eye other_eye = request.eye == Eye::left ? Eye::right : Eye::left;
    const EyeHistory& other = histories_[eye_index(other_eye)];

    if (history.token.identity == other.token.identity) {
        return invalid(request.eye, "left and right history identities collided");
    }
    if (request.history.eye != request.eye || request.history.identity != history.token.identity ||
        request.history.generation != history.token.generation) {
        return invalid(request.eye, "history token does not belong to this eye or generation");
    }
    if (request.stream_generation != history.stream_generation) {
        return invalid(request.eye, "stream generation changed without reset_histories");
    }
    if (!request.input_color.valid() || !request.input_depth.valid() ||
        !request.input_motion_vectors.valid() || !request.output_color.valid()) {
        return invalid(request.eye,
                       "color, depth, motion-vector, and output textures are required");
    }
    if (request.input_color.extent != history.extent ||
        request.output_color.extent != history.extent ||
        request.input_depth.extent != history.extent ||
        request.input_motion_vectors.extent != history.extent) {
        return invalid(request.eye, "texture extent changed without a resolution reset");
    }
    if (history.has_frame && request.frame_id <= history.last_frame_id) {
        return invalid(request.eye, "frame id must increase monotonically within each eye history");
    }

    if (history.pending_reset != HistoryResetReason::none) {
        const auto required = static_cast<std::uint32_t>(history.pending_reset);
        const auto supplied = static_cast<std::uint32_t>(request.reset_reason);
        if ((required & supplied) != required) {
            return invalid(request.eye, "first frame after reset did not carry every reset reason");
        }
    } else if (request.reset_reason != HistoryResetReason::none) {
        return invalid(request.eye, "frame supplied a reset reason without reset_histories");
    }

    history.last_frame_id = request.frame_id;
    history.has_frame = true;
    history.pending_reset = HistoryResetReason::none;
    if (logger_) {
        logger_->write(LogLevel::trace,
                       "dlss_stub_passthrough",
                       "Stereo history contract accepted; no neural module was invoked",
                       {{"eye", std::string(to_string(request.eye))},
                        {"frame_id", std::to_string(request.frame_id)},
                        {"history_identity", std::to_string(request.history.identity)},
                        {"history_generation", std::to_string(request.history.generation)}});
    }
    return {DlssResultCode::passthrough, "validated passthrough"};
}

void SafeDlssStub::shutdown() noexcept {
    histories_ = {};
    logger_ = nullptr;
    initialized_ = false;
}

DlssResult SafeDlssStub::invalid(const Eye eye, std::string message) {
    if (logger_) {
        logger_->write(LogLevel::error,
                       "dlss_request_rejected",
                       message,
                       {{"eye", std::string(to_string(eye))}});
    }
    return {DlssResultCode::invalid_request, std::move(message)};
}

} // namespace kotorvr::host
