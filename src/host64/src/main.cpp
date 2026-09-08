#include "kotorvr/host/d3d12_context.hpp"
#include "kotorvr/host/direct_neural_eye.hpp"
#include "kotorvr/host/dlss_backend.hpp"
#include "kotorvr/host/frame_ring.hpp"
#include "kotorvr/host/host_ipc_publisher.hpp"
#include "kotorvr/host/logger.hpp"
#include "kotorvr/host/openxr_runtime.hpp"
#include "kotorvr/host/session_state_machine.hpp"
#include "kotorvr/host/stereo_resolution.hpp"
#include "stereo_stream.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace kotorvr::host;

constexpr std::uint64_t ipc_generation = 1;

std::string_view nonce_parse_status_name(
    const SessionNonceParseStatus status) noexcept {
    switch (status) {
    case SessionNonceParseStatus::Ok: return "ok";
    case SessionNonceParseStatus::WrongLength: return "wrong-length";
    case SessionNonceParseStatus::InvalidHexCharacter: return "invalid-hex-character";
    case SessionNonceParseStatus::ZeroNonce: return "zero-nonce";
    }
    return "unknown";
}

struct SessionNonceEnvironment {
    bool present{};
    std::string value;
};

SessionNonceEnvironment read_session_nonce_environment() {
    std::array<char, 64> buffer{};
    SetLastError(ERROR_SUCCESS);
    const DWORD copied = GetEnvironmentVariableA(
        "KOTOR2VR_SESSION_NONCE", buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (copied == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        return {};
    }
    if (copied == 0) {
        return {true, {}};
    }
    if (copied >= buffer.size()) {
        return {true, "too-long"};
    }
    return {true, std::string(buffer.data(), copied)};
}

std::atomic_bool stop_requested{};

BOOL WINAPI console_handler(const DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        stop_requested.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}

struct Options {
    bool self_test{};
    bool visible_smoke{};
    bool game_image_smoke{};
    bool native_stereo{};
    bool help{};
    std::filesystem::path log_file{"logs/kotorvr-host64.jsonl"};
    std::filesystem::path config_file{};
    std::uint64_t max_frames{};
};

std::optional<Options> parse_options(const int argc, char** argv) {
    Options options{};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--self-test") {
            options.self_test = true;
        } else if (argument == "--visible-smoke") {
            options.visible_smoke = true;
        } else if (argument == "--game-image-smoke") {
            options.visible_smoke = true;
            options.game_image_smoke = true;
        } else if (argument == "--native-stereo") {
            options.visible_smoke = true;
            options.game_image_smoke = true;
            options.native_stereo = true;
        } else if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--log-file" && index + 1 < argc) {
            options.log_file = argv[++index];
        } else if (argument == "--config" && index + 1 < argc) {
            options.config_file = argv[++index];
        } else if (argument == "--max-frames" && index + 1 < argc) {
            try {
                options.max_frames = std::stoull(argv[++index]);
            } catch (...) {
                std::cerr << "Invalid --max-frames value\n";
                return std::nullopt;
            }
        } else {
            std::cerr << "Unknown or incomplete argument: " << argument << '\n';
            return std::nullopt;
        }
    }
    return options;
}

void print_help() {
    std::cout
        << "kotor2vr-host64 [--self-test] [--visible-smoke|--game-image-smoke] "
           "[--config PATH] "
           "[--log-file PATH] [--max-frames N]\n\n"
        << "  --self-test     Validate state, stereo ring, and DLSS-history contracts\n"
        << "                  without an OpenXR runtime or headset.\n"
        << "  --visible-smoke Start OpenXR without the game and show a world-locked\n"
        << "                  four-color test quad about 1.5 m in front of the HMD.\n"
        << "  --game-image-smoke  Show the four-color quad until a matching x86\n"
        << "                  BGRA8 game snapshot becomes available, then show it.\n"
        << "  --log-file      JSON-lines diagnostic log path.\n"
        << "  --config        Validate and record the launcher-selected TOML path.\n"
        << "                  Runtime option wiring remains gated with projection IPC.\n"
        << "  --max-frames    Stop after N OpenXR frames (0 = unlimited).\n";
    std::cout << "  KOTOR2VR_EYE_PERCENT  Native VR eye dimensions, 50..100 percent of\n"
                 "                  OpenXR recommendation (default 75; next launch only).\n";
}

SubmittedEyeFrame test_frame(const Eye eye,
                             const std::uint64_t frame_id,
                             const std::uint64_t generation,
                             const Extent2D extent) {
    const auto eye_offset = static_cast<std::uint64_t>(eye_index(eye));
    SubmittedEyeFrame frame{};
    frame.eye = eye;
    frame.frame_id = frame_id;
    frame.stream_generation = generation;
    frame.predicted_display_time_ns = static_cast<std::int64_t>(10'000'000ULL * frame_id);
    frame.producer_fence_handle = 0x1000ULL + eye_offset;
    frame.producer_fence_value = frame_id;
    frame.color = {0x2000ULL + eye_offset, 28, extent};
    frame.depth = {0x3000ULL + eye_offset, 40, extent};
    frame.motion_vectors = {0x4000ULL + eye_offset, 34, extent};
    frame.render_view.pose.position[0] = eye == Eye::left ? -0.032F : 0.032F;
    return frame;
}

bool run_self_test(Logger& logger) {
    logger.write(LogLevel::info, "self_test_start", "Host contract self-test started");

    SessionStateMachine state_machine(logger);
    const RuntimeSessionState startup_states[]{RuntimeSessionState::idle,
                                               RuntimeSessionState::ready,
                                               RuntimeSessionState::synchronized,
                                               RuntimeSessionState::visible,
                                               RuntimeSessionState::focused};
    for (const auto state : startup_states) {
        if (state == RuntimeSessionState::ready) {
            const SessionActions invalid =
                state_machine.apply(RuntimeSessionState::focused);
            if (invalid.transition_valid ||
                state_machine.state() != RuntimeSessionState::idle) {
                logger.write(LogLevel::error,
                             "self_test_invalid_transition_mutated_state",
                             "Rejected OpenXR transition changed internal state");
                return false;
            }
        }
        const SessionActions actions = state_machine.apply(state);
        if (!actions.transition_valid) {
            return false;
        }
        if (actions.begin_session) {
            state_machine.mark_session_begun();
        }
    }
    if (!state_machine.should_drive_frames()) {
        return false;
    }

    constexpr Extent2D extent{1440, 1600};
    constexpr std::uint64_t generation = 1;
    StereoFrameRing ring;
    std::string error;
    if (!ring.reset(generation, error)) {
        return false;
    }
    for (const Eye eye : {Eye::left, Eye::right}) {
        const auto lease = ring.begin_produce(eye, 1, generation);
        if (!lease || !ring.publish(*lease, test_frame(eye, 1, generation, extent), error)) {
            logger.write(LogLevel::error,
                         "self_test_ring_publish_failed",
                         error.empty() ? "Unable to acquire a producer slot" : error,
                         {{"eye", std::string(to_string(eye))}});
            return false;
        }
        if (eye == Eye::left && ring.acquire_latest_complete_pair().has_value()) {
            logger.write(LogLevel::error,
                         "self_test_unpaired_eye_accepted",
                         "Ring exposed a frame before both eyes were ready");
            return false;
        }
    }
    const auto pair = ring.acquire_latest_complete_pair();
    if (!pair || pair->left().frame_id != pair->right().frame_id ||
        pair->left().stream_generation != pair->right().stream_generation) {
        logger.write(LogLevel::error,
                     "self_test_stereo_pair_failed",
                     "Ring did not return an atomic same-generation stereo pair");
        return false;
    }

    SafeDlssStub dlss;
    if (!dlss.initialize(extent, generation, logger)) {
        return false;
    }
    const auto left_token = dlss.history_token(Eye::left);
    const auto right_token = dlss.history_token(Eye::right);
    if (left_token.identity == right_token.identity || left_token.eye == right_token.eye) {
        logger.write(LogLevel::error,
                     "self_test_history_alias",
                     "Left and right temporal histories are not isolated");
        return false;
    }

    for (const Eye eye : {Eye::left, Eye::right}) {
        const auto& source = pair->eyes[eye_index(eye)];
        DlssFrameRequest request{};
        request.eye = eye;
        request.frame_id = source.frame_id;
        request.stream_generation = source.stream_generation;
        request.history = dlss.history_token(eye);
        request.reset_reason = HistoryResetReason::startup;
        request.input_color = source.color;
        request.input_depth = source.depth;
        request.input_motion_vectors = source.motion_vectors;
        request.output_color = {
            0x5000ULL + static_cast<std::uint64_t>(eye_index(eye)), 28, extent};
        request.current_view = source.render_view;
        request.previous_view = source.render_view;
        if (!dlss.evaluate(request).accepted()) {
            return false;
        }
    }

    const auto stale_left_token = dlss.history_token(Eye::left);
    if (!dlss.reset_histories(extent, generation + 1,
                              HistoryResetReason::camera_cut)) {
        return false;
    }
    const auto reset_left_token = dlss.history_token(Eye::left);
    const auto reset_right_token = dlss.history_token(Eye::right);
    if (reset_left_token.generation != stale_left_token.generation + 1 ||
        reset_right_token.generation != right_token.generation + 1) {
        return false;
    }
    DlssFrameRequest stale_request{};
    stale_request.eye = Eye::left;
    stale_request.frame_id = 1;
    stale_request.stream_generation = generation + 1;
    stale_request.history = stale_left_token;
    stale_request.reset_reason = HistoryResetReason::camera_cut;
    stale_request.input_color = test_frame(Eye::left, 1, generation + 1, extent).color;
    stale_request.input_depth = test_frame(Eye::left, 1, generation + 1, extent).depth;
    stale_request.input_motion_vectors =
        test_frame(Eye::left, 1, generation + 1, extent).motion_vectors;
    stale_request.output_color = {0x5000ULL, 28, extent};
    if (dlss.evaluate(stale_request).accepted()) {
        logger.write(LogLevel::error,
                     "self_test_stale_history_accepted",
                     "DLSS contract accepted a token from before a stereo reset");
        return false;
    }
    for (const Eye eye : {Eye::left, Eye::right}) {
        auto reset_request = stale_request;
        reset_request.eye = eye;
        reset_request.history = dlss.history_token(eye);
        const auto source = test_frame(eye, 1, generation + 1, extent);
        reset_request.input_color = source.color;
        reset_request.input_depth = source.depth;
        reset_request.input_motion_vectors = source.motion_vectors;
        reset_request.output_color.shared_handle =
            0x6000ULL + static_cast<std::uint64_t>(eye_index(eye));
        if (!dlss.evaluate(reset_request).accepted()) {
            return false;
        }
    }

    if (!ring.release_pair(*pair, 1, error)) {
        logger.write(LogLevel::error,
                     "self_test_ring_release_failed",
                     error);
        return false;
    }

    // Submission is not retirement: the producer must not reuse either slot
    // until the D3D12 consumer fence has actually completed.
    ring.retire_completed(0);
    const auto pending_a = ring.begin_produce(Eye::left, 2, generation);
    const auto pending_b = ring.begin_produce(Eye::left, 3, generation);
    if (!pending_a || !pending_b ||
        ring.begin_produce(Eye::left, 4, generation).has_value()) {
        logger.write(LogLevel::error,
                     "self_test_fence_reuse",
                     "A consumer-pending slot was reused before its fence completed");
        return false;
    }
    if (!ring.cancel_produce(*pending_a, error) ||
        !ring.cancel_produce(*pending_b, error)) {
        return false;
    }
    ring.retire_completed(1);

    if (ring.begin_produce(Eye::left, 1, generation).has_value()) {
        logger.write(LogLevel::error,
                     "self_test_duplicate_frame_accepted",
                     "Ring accepted a duplicate or out-of-order frame id");
        return false;
    }

    // A rejected publication must surrender its lease instead of poisoning a
    // slot forever. Exercise that contract in the dependency-free self-test.
    const auto bad_lease = ring.begin_produce(Eye::left, 2, generation);
    if (!bad_lease) {
        return false;
    }
    auto bad_frame = test_frame(Eye::right, 2, generation, extent);
    if (ring.publish(*bad_lease, bad_frame, error)) {
        logger.write(LogLevel::error,
                     "self_test_invalid_frame_accepted",
                     "Ring accepted a frame for the wrong eye");
        return false;
    }
    const auto replacement_lease = ring.begin_produce(Eye::left, 2, generation);
    if (!replacement_lease || !ring.cancel_produce(*replacement_lease, error)) {
        logger.write(LogLevel::error,
                     "self_test_cancel_failed",
                     error.empty() ? "Rejected publish did not release its slot" : error);
        return false;
    }

    StereoFrameRing mismatch_ring;
    if (!mismatch_ring.reset(generation, error)) {
        return false;
    }
    const auto mismatch_left = mismatch_ring.begin_produce(Eye::left, 1, generation);
    const auto mismatch_right = mismatch_ring.begin_produce(Eye::right, 1, generation);
    auto wrong_extent = test_frame(Eye::right, 1, generation, {1441, 1600});
    if (!mismatch_left || !mismatch_right ||
        !mismatch_ring.publish(*mismatch_left,
                               test_frame(Eye::left, 1, generation, extent), error) ||
        !mismatch_ring.publish(*mismatch_right, wrong_extent, error) ||
        mismatch_ring.acquire_latest_complete_pair().has_value()) {
        logger.write(LogLevel::error,
                     "self_test_mismatched_pair_accepted",
                     "Ring paired eyes with incompatible metadata");
        return false;
    }

    if (ring.reset(generation, error) || !ring.reset(generation + 1, error) ||
        ring.release_pair(*pair, 2, error)) {
        logger.write(LogLevel::error,
                     "self_test_stale_pair_accepted",
                     "Generation reset accepted stale state or an old consumer pair");
        return false;
    }

    const auto invalid_eye = static_cast<Eye>(99);
    if (ring.begin_produce(invalid_eye, 1, generation + 1).has_value() ||
        dlss.history_token(invalid_eye).identity != 0) {
        logger.write(LogLevel::error,
                     "self_test_invalid_eye_accepted",
                     "An invalid wire eye value reached an array index");
        return false;
    }

    const SessionActions stopping = state_machine.apply(RuntimeSessionState::stopping);
    if (!stopping.end_session) {
        return false;
    }
    state_machine.mark_session_ended();
    if (!state_machine.apply(RuntimeSessionState::idle).transition_valid) {
        return false;
    }

    const auto stats = ring.stats();
    logger.write(LogLevel::info,
                 "self_test_passed",
                 "State, triple-ring, stereo pairing, and history-isolation contracts passed",
                  {{"published", std::to_string(stats.published)},
                   {"acquired_pairs", std::to_string(stats.acquired_pairs)},
                   {"retired", std::to_string(stats.retired)},
                   {"dlss_mode", "validated_passthrough"}});
    return true;
}

int run_openxr_host(const Options& options,
                    Logger& logger,
                    HostIpcPublisher* const ipc_publisher) {
    SetConsoleCtrlHandler(console_handler, TRUE);

    SessionStateMachine state_machine(logger);
    D3D12Context graphics;
    OpenXrRuntime runtime;
    const RuntimeStartResult started = runtime.start(graphics, state_machine, logger);
    if (!started.success) {
        log_health(logger,
                   {.xr_available = runtime.available(), .xr_rendering = false});
        logger.write(LogLevel::error,
                     "host_start_failed",
                     started.message,
                     {{"compiled_with_openxr",
                       started.compiled_with_openxr ? "true" : "false"}});
        runtime.shutdown();
        graphics.shutdown();
        return started.compiled_with_openxr ? 2 : 3;
    }
    const std::optional<k2vr::ipc::SessionNonce> game_image_nonce =
        options.game_image_smoke && ipc_publisher != nullptr
            ? std::optional<k2vr::ipc::SessionNonce>(
                  ipc_publisher->session_nonce())
            : std::nullopt;
    if (options.visible_smoke &&
        !runtime.initialize_visible_smoke(game_image_nonce,options.native_stereo)) {
        logger.write(LogLevel::error,
                     "visible_smoke_start_failed",
                     "Unable to initialize the explicit visible OpenXR smoke mode");
        runtime.shutdown();
        graphics.shutdown();
        return 5;
    }

    HealthSnapshot health{};
    health.xr_available = true;
    auto next_health_log = std::chrono::steady_clock::now();
    auto next_ipc_heartbeat = std::chrono::steady_clock::now();
    auto next_ipc_health = std::chrono::steady_clock::now();
    std::uint32_t pending_reset_reasons =
        static_cast<std::uint32_t>(k2vr::ipc::ResetReason::Startup);

    while (!stop_requested.load(std::memory_order_relaxed) &&
           !state_machine.exit_requested()) {
        const RuntimePollResult poll = runtime.poll_events();
        if (!poll.success || poll.exit_requested) {
            break;
        }
        if (poll.history_reset_requested) {
            ++health.history_resets;
            if (health.frames_seen != 0) {
                pending_reset_reasons |= static_cast<std::uint32_t>(
                    k2vr::ipc::ResetReason::RuntimeRestart);
            }
            if (poll.tracking_reset_requested) {
                pending_reset_reasons |= static_cast<std::uint32_t>(
                    k2vr::ipc::ResetReason::Recenter);
            }
            logger.write(LogLevel::info,
                         "temporal_reset_pending",
                         "Runtime lifecycle requires both eye histories to reset before next image");
        }

        if (!state_machine.should_drive_frames()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            XrFrameToken frame{};
            FrameTiming timing{};
            if (runtime.wait_begin_frame(frame, timing)) {
                ++health.frames_seen;
                const auto locate_start = std::chrono::steady_clock::now();
                LocatedViews views{};
                const bool located = runtime.locate_views(frame, views);
                timing.render_wait_ms = std::chrono::duration<double, std::milli>(
                                            std::chrono::steady_clock::now() -
                                            locate_start)
                                            .count();
                if ((!located || !views.position_valid) && frame.should_render) {
                    ++health.validation_failures;
                    logger.write(
                        LogLevel::warning,
                        "view_pose_invalid",
                        "Frame has no valid tracked stereo pose",
                        {{"frame_id", std::to_string(frame.sequence)}});
                }
                if (ipc_publisher != nullptr && frame.should_render && located &&
                    views.position_valid) {
                    const HostIpcPublishResult published =
                        ipc_publisher->PublishFrame(
                            views,
                            frame,
                            options.native_stereo ? Extent2D{
                                NativeStereoEyeWidth(runtime.recommended_view_extent().width),
                                NativeStereoEyeHeight(runtime.recommended_view_extent().height)} :
                                runtime.recommended_view_extent(),
                            options.native_stereo ? k2vr::ipc::PresentationState::WorldThirdPerson :
                                k2vr::ipc::PresentationState::UnknownSafe,
                            pending_reset_reasons);
                    if (published.ok()) {
                        pending_reset_reasons = 0;
                    } else {
                        ++health.validation_failures;
                        logger.write(
                            LogLevel::error,
                            "host_ipc_frame_publish_failed",
                            "Unable to publish a valid OpenXR frame to Game32",
                            {{"build_status",
                              std::to_string(static_cast<std::uint32_t>(
                                  published.build_status))},
                             {"transport_status",
                              k2vr::ipc::ToString(published.transport_status)},
                             {"frame_id", std::to_string(frame.sequence)}});
                        state_machine.mark_failed(
                            "Host IPC render-request publication failed");
                    }
                }
                const bool submitted = options.visible_smoke
                                           ? runtime.end_frame_visible_smoke(frame, timing)
                                           : runtime.end_frame(frame, timing);
                if (submitted) {
                    ++health.frames_submitted;
                    log_frame_timing(logger, timing);
                }
            }
        }

        health.xr_rendering = state_machine.should_drive_frames();
        const auto now = std::chrono::steady_clock::now();
        if (ipc_publisher != nullptr && now >= next_ipc_health) {
            HostHealthUpdate update{};
            update.presented_frame_count = health.frames_submitted;
            update.dropped_frame_count = health.dropped_frames;
            update.timeout_count = 0;
            update.duplicate_or_stale_count = 0;
            if (state_machine.state() == RuntimeSessionState::visible ||
                state_machine.state() == RuntimeSessionState::focused) {
                update.visibility_flags |= static_cast<std::uint32_t>(
                    k2vr::ipc::RuntimeVisibility::Visible);
            }
            if (state_machine.state() == RuntimeSessionState::focused) {
                update.visibility_flags |= static_cast<std::uint32_t>(
                    k2vr::ipc::RuntimeVisibility::Focused);
            }
            update.health_flags = static_cast<std::uint32_t>(
                                      k2vr::ipc::RuntimeHealth::RuntimeReady) |
                                  static_cast<std::uint32_t>(
                                      k2vr::ipc::RuntimeHealth::AdapterMatched);
            if (options.visible_smoke) {
                update.health_flags |= static_cast<std::uint32_t>(
                    k2vr::ipc::RuntimeHealth::SwapchainReady);
            }
            const k2vr::ipc::SharedMemoryStatus status =
                ipc_publisher->PublishHostHealth(update);
            if (status != k2vr::ipc::SharedMemoryStatus::Ok) {
                logger.write(LogLevel::error,
                             "host_ipc_health_publish_failed",
                             "Unable to publish periodic host health",
                             {{"transport_status", k2vr::ipc::ToString(status)}});
                state_machine.mark_failed("Host IPC health publication failed");
            }
            next_ipc_health = now + std::chrono::seconds(1);
            next_ipc_heartbeat = now + std::chrono::milliseconds(100);
        } else if (ipc_publisher != nullptr && now >= next_ipc_heartbeat) {
            const k2vr::ipc::SharedMemoryStatus status =
                ipc_publisher->Heartbeat();
            if (status != k2vr::ipc::SharedMemoryStatus::Ok) {
                logger.write(LogLevel::error,
                             "host_ipc_heartbeat_failed",
                             "Unable to refresh the host heartbeat",
                             {{"transport_status", k2vr::ipc::ToString(status)}});
                state_machine.mark_failed("Host IPC heartbeat failed");
            }
            next_ipc_heartbeat = now + std::chrono::milliseconds(100);
        }
        if (now >= next_health_log) {
            health.log_lines_dropped = logger.dropped_lines();
            log_health(logger, health);
            next_health_log = now + std::chrono::seconds(1);
        }
        if (options.max_frames != 0 && health.frames_submitted >= options.max_frames) {
            break;
        }
    }

    runtime.shutdown();
    graphics.shutdown();
    health.xr_rendering = false;
    log_health(logger, health);
    logger.write(LogLevel::info,
                 "host_stopped",
                 state_machine.failed() ? "Host stopped after a runtime failure"
                                        : "Host stopped cleanly");
    return state_machine.failed() ? 4 : 0;
}

} // namespace

int main(const int argc, char** argv) {
    const kotorvr::host::DirectNeuralSessionScope directNeuralSession;
    const auto options = parse_options(argc, argv);
    if (!options) {
        print_help();
        return 64;
    }
    if (options->help) {
        print_help();
        return 0;
    }
    std::error_code config_error;
    if (!options->config_file.empty()) {
        const bool regular =
            std::filesystem::is_regular_file(options->config_file, config_error);
        if (!regular || config_error) {
            std::cerr << "Config file is unavailable: "
                      << options->config_file.string();
            if (config_error) {
                std::cerr << " (" << config_error.message() << ')';
            }
            std::cerr << '\n';
            return 66;
        }
    }

    Logger logger(options->log_file);
    logger.write(LogLevel::info,
                 "host_process_start",
                 "KOTOR II VR x64 host starting",
                 {{"build_openxr", KOTORVR_HAS_OPENXR ? "true" : "false"},
                  {"dlss_backend", "safe_stub"},
                  {"visible_smoke", options->visible_smoke ? "true" : "false"},
                  {"game_image_smoke",
                   options->game_image_smoke ? "true" : "false"},
                  {"config",
                   options->config_file.empty()
                       ? "not-supplied"
                       : std::filesystem::absolute(options->config_file,
                                                   config_error).string()}});

    const auto& eye_resolution=NativeStereoEyeResolutionSetting();
    if (eye_resolution.fixed && !eye_resolution.valid) {
        const char* error="Invalid KOTOR2VR_EYE_RESOLUTION: expected WIDTHxHEIGHT, even dimensions 256..4096, height <=3328 for the 8192-row transport atlas";
        logger.write(LogLevel::error,"native_stereo_eye_resolution_invalid",error);
        std::cerr << error << '\n';
        return 64;
    }
    if (options->native_stereo) {
        logger.write(eye_resolution.valid ? LogLevel::info:LogLevel::warning,
            "native_stereo_eye_resolution",
            eye_resolution.valid ? "Native eye resolution selected for this session":
                "Invalid KOTOR2VR_EYE_PERCENT; using the unchanged 75 percent default",
            {{"mode",eye_resolution.fixed ? "fixed":"percent"},
             {"eye_width",eye_resolution.fixed ? std::to_string(eye_resolution.width):"runtime-scaled"},
             {"eye_height",eye_resolution.fixed ? std::to_string(eye_resolution.height):"runtime-scaled"},
             {"eye_percent",eye_resolution.fixed ? "not-applicable":std::to_string(eye_resolution.percent)},
             {"source",eye_resolution.from_environment ? "environment":"default"}});
    }

    HostIpcPublisher ipc_publisher;
    HostIpcPublisher* active_ipc = nullptr;
    const SessionNonceEnvironment environment_nonce =
        read_session_nonce_environment();
    if (environment_nonce.present) {
        const SessionNonceParseResult parsed =
            ParseSessionNonceHex(environment_nonce.value);
        if (!parsed.ok()) {
            logger.write(LogLevel::error,
                         "host_ipc_nonce_invalid",
                         "KOTOR2VR_SESSION_NONCE must contain exactly 32 non-zero hexadecimal digits",
                         {{"parse_status",
                           std::string(nonce_parse_status_name(parsed.status))}});
            return 67;
        }
        const k2vr::ipc::SharedMemoryStatus started =
            ipc_publisher.Start(parsed.nonce, ipc_generation);
        if (started != k2vr::ipc::SharedMemoryStatus::Ok) {
            logger.write(LogLevel::error,
                         "host_ipc_mapping_start_failed",
                         "Unable to create the launcher-selected IPC mapping",
                         {{"transport_status", k2vr::ipc::ToString(started)},
                          {"generation", std::to_string(ipc_generation)}});
            return 68;
        }
        active_ipc = &ipc_publisher;
        logger.write(LogLevel::info,
                     "host_ipc_mapping_ready",
                     "Host IPC mapping and initial heartbeat are ready",
                     {{"generation", std::to_string(ipc_generation)}});
    }
    if (options->game_image_smoke && active_ipc == nullptr) {
        logger.write(LogLevel::error,
                     "game_image_nonce_missing",
                     "--game-image-smoke requires KOTOR2VR_SESSION_NONCE");
        return 69;
    }

    int exit_code{};
    if (options->self_test) {
        exit_code = run_self_test(logger) ? 0 : 1;
    } else {
        exit_code = run_openxr_host(*options, logger, active_ipc);
    }
    ipc_publisher.Close();
    return exit_code;
}
