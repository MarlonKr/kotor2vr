#include "kotorvr/host/host_ipc_publisher.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace {

int g_failures = 0;

void Check(bool condition, std::string_view description) {
    if (condition) {
        std::cout << "PASS: " << description << '\n';
    } else {
        std::cerr << "FAIL: " << description << '\n';
        ++g_failures;
    }
}

kotorvr::host::LocatedViews ValidViews(std::int64_t display_time) {
    using namespace kotorvr::host;
    LocatedViews views{};
    views.predicted_display_time_ns = display_time;
    views.orientation_valid = true;
    views.position_valid = true;
    views.views[0].pose.position = {-0.032F, 1.7F, 0.2F};
    views.views[1].pose.position = {0.032F, 1.7F, 0.2F};
    views.views[0].pose.orientation = {0.1F, 0.2F, 0.3F, 0.9F};
    views.views[1].pose.orientation = {0.1F, 0.2F, 0.3F, 0.9F};
    views.views[0].fov = {-0.8F, 0.7F, 0.75F, -0.7F};
    views.views[1].fov = {-0.7F, 0.8F, 0.75F, -0.7F};
    return views;
}

} // namespace

int main() {
    using namespace kotorvr::host;
    using namespace k2vr::ipc;

    constexpr SessionNonce nonce{0x1122334455667788ULL,
                                 0x8877665544332211ULL};
    constexpr std::uint64_t generation = 3;
    constexpr std::int64_t display_time = 99112233;
    constexpr Extent2D extent{2016, 2208};
    XrFrameToken frame{17, display_time, true, true};
    LocatedViews views = ValidViews(display_time);

    const SessionNonceParseResult parsed =
        ParseSessionNonceHex("88776655443322111122334455667788");
    Check(parsed.ok() && parsed.nonce.high == nonce.high &&
              parsed.nonce.low == nonce.low,
          "32 hex digits map deterministically to high then low nonce halves");
    const SessionNonceParseResult lower =
        ParseSessionNonceHex("abcdef0123456789fedcba9876543210");
    Check(lower.ok() && lower.nonce.high == 0xABCDEF0123456789ULL &&
              lower.nonce.low == 0xFEDCBA9876543210ULL,
          "lower-case launcher nonce is accepted");
    Check(ParseSessionNonceHex("0011").status ==
              SessionNonceParseStatus::WrongLength &&
              ParseSessionNonceHex("8877665544332211112233445566778-").status ==
                  SessionNonceParseStatus::InvalidHexCharacter &&
              ParseSessionNonceHex("00000000000000000000000000000000").status ==
                  SessionNonceParseStatus::ZeroNonce,
          "wrong length, non-hex, and all-zero nonces fail closed");
    const std::uint32_t resets = static_cast<std::uint32_t>(BitOr(
        ResetReason::Startup, ResetReason::RuntimeRestart));

    constexpr Pose source_pose{{1.0F, 2.0F, 3.0F},
                               {0.1F, 0.2F, 0.3F, 0.9F}};
    constexpr PoseF32 wire_pose = ToIpcPose(source_pose);
    static_assert(wire_pose.position_x == 1.0F &&
                  wire_pose.orientation_w == 0.9F);
    constexpr FieldOfView source_fov{-0.8F, 0.7F, 0.75F, -0.7F};
    constexpr FovF32 wire_fov = ToIpcFov(source_fov);
    static_assert(wire_fov.angle_left == -0.8F &&
                  wire_fov.angle_down == -0.7F);

    const RenderRequestBuildResult built = BuildRenderRequest(
        views, frame, extent, PresentationState::WorldFirstPerson, nonce,
        generation, resets);
    Check(built.ok(), "valid located stereo views build a request");
    Check(HeaderMatches<RenderRequest>(built.request.header,
                                       MessageType::RenderRequest) &&
              built.request.header.sequence == frame.sequence &&
              built.request.header.session_nonce.low == nonce.low &&
              built.request.header.generation == generation,
          "request header binds frame, nonce, and generation");
    Check(built.request.frame_id == frame.sequence &&
              built.request.predicted_display_time_ns == display_time &&
              built.request.render_width == extent.width &&
              built.request.render_height == extent.height &&
              built.request.history_reset_reasons == resets,
          "frame timing, extent, state reset data map exactly");
    Check(built.request.views[0].pose.position_x == -0.032F &&
              built.request.views[1].pose.position_x == 0.032F &&
              built.request.views[0].fov.angle_left == -0.8F &&
              built.request.views[1].fov.angle_right == 0.8F,
          "left/right pose and asymmetric FOV remain ordered");

    XrFrameToken invalid_frame = frame;
    invalid_frame.begun = false;
    Check(BuildRenderRequest(views, invalid_frame, extent,
                             PresentationState::WorldFirstPerson, nonce,
                             generation).status ==
              RenderRequestBuildStatus::InvalidFrameToken,
          "unbegun XR frame is rejected");
    invalid_frame = frame;
    invalid_frame.should_render = false;
    Check(BuildRenderRequest(views, invalid_frame, extent,
                             PresentationState::WorldFirstPerson, nonce,
                             generation).status ==
              RenderRequestBuildStatus::InvalidFrameToken,
          "non-rendering XR frame does not request game rendering");

    LocatedViews invalid_views = views;
    invalid_views.predicted_display_time_ns++;
    Check(BuildRenderRequest(invalid_views, frame, extent,
                             PresentationState::WorldFirstPerson, nonce,
                             generation).status ==
              RenderRequestBuildStatus::InvalidViews,
          "view/token prediction-time mismatch is rejected");
    invalid_views = views;
    invalid_views.position_valid = false;
    Check(BuildRenderRequest(invalid_views, frame, extent,
                             PresentationState::WorldFirstPerson, nonce,
                             generation).status ==
              RenderRequestBuildStatus::InvalidViews,
          "invalid positional tracking is rejected");
    invalid_views = views;
    invalid_views.views[0].pose.position[1] =
        std::numeric_limits<float>::quiet_NaN();
    Check(BuildRenderRequest(invalid_views, frame, extent,
                             PresentationState::WorldFirstPerson, nonce,
                             generation).status ==
              RenderRequestBuildStatus::InvalidViews,
          "non-finite pose is rejected");
    invalid_views = views;
    invalid_views.views[0].fov.angle_left = 1.0F;
    Check(BuildRenderRequest(invalid_views, frame, extent,
                             PresentationState::WorldFirstPerson, nonce,
                             generation).status ==
              RenderRequestBuildStatus::InvalidViews,
          "inverted FOV is rejected");
    Check(BuildRenderRequest(views, frame, {},
                             PresentationState::WorldFirstPerson, nonce,
                             generation).status ==
              RenderRequestBuildStatus::InvalidExtent,
          "zero render extent is rejected");
    Check(BuildRenderRequest(
              views, frame, extent, static_cast<PresentationState>(99), nonce,
              generation).status ==
              RenderRequestBuildStatus::InvalidPresentationState,
          "unknown presentation enum is rejected");
    Check(BuildRenderRequest(views, frame, extent,
                             PresentationState::WorldFirstPerson, nonce,
                             generation, 0x80000000U).status ==
              RenderRequestBuildStatus::InvalidResetReasons,
          "unknown history reset bit is rejected");

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    SessionNonce live_nonce{
        nonce.low ^ static_cast<std::uint64_t>(qpc.QuadPart),
        nonce.high ^ (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32U)};
    if (!IsValid(live_nonce)) {
        live_nonce.low = 1;
    }

    HostIpcPublisher publisher;
    Check(publisher.Start(live_nonce, generation) == SharedMemoryStatus::Ok,
          "publisher creates mapping and initial heartbeat");
    SharedMemoryChannel game;
    Check(SharedMemoryChannel::OpenGame(live_nonce, generation, game) ==
              SharedMemoryStatus::Ok,
          "game endpoint opens publisher mapping");

    HealthState initial_health{};
    Check(game.ReadPeerHealth(initial_health) == SharedMemoryStatus::Ok &&
              initial_health.host_heartbeat_qpc != 0 &&
              initial_health.header.sequence == 1,
          "Start immediately makes host liveness observable");

    const HostIpcPublishResult publish = publisher.PublishFrame(
        views, frame, extent, PresentationState::DialogueStereo, resets);
    RenderRequest transported{};
    Check(publish.ok() &&
              game.ReadRenderRequest(transported) == SharedMemoryStatus::Ok &&
              transported.frame_id == frame.sequence &&
              transported.presentation_state ==
                  PresentationState::DialogueStereo,
          "publisher transports a converted XR frame to Game32");

    HostHealthUpdate update{};
    update.heartbeat_qpc = 5555;
    update.presented_frame_count = 20;
    update.dropped_frame_count = 2;
    update.timeout_count = 1;
    update.visibility_flags = static_cast<std::uint32_t>(BitOr(
        RuntimeVisibility::Visible, RuntimeVisibility::Focused));
    update.health_flags = static_cast<std::uint32_t>(BitOr(
        RuntimeHealth::RuntimeReady, RuntimeHealth::SwapchainReady));
    Check(publisher.PublishHostHealth(update) == SharedMemoryStatus::Ok,
          "publisher updates host health counters and flags");
    HealthState updated_health{};
    Check(game.ReadPeerHealth(updated_health) == SharedMemoryStatus::Ok &&
              updated_health.host_heartbeat_qpc == 5555 &&
              updated_health.presented_frame_count == 20 &&
              updated_health.dropped_frame_count == 2 &&
              updated_health.header.sequence == 2,
          "game observes exact host health update");
    Check(publisher.Heartbeat() == SharedMemoryStatus::Ok,
          "heartbeat advances without replacing counters");
    HealthState heartbeat{};
    Check(game.ReadPeerHealth(heartbeat) == SharedMemoryStatus::Ok &&
              heartbeat.host_heartbeat_qpc != 0 &&
              heartbeat.presented_frame_count == 20 &&
              heartbeat.header.sequence == 3,
          "heartbeat preserves counters and advances liveness sequence");

    HostHealthUpdate bad_health = update;
    bad_health.health_flags = 0x80000000U;
    Check(publisher.PublishHostHealth(bad_health) ==
              SharedMemoryStatus::InvalidArgument,
          "unknown health flags fail closed");

    HealthState game_health{};
    game_health.header = MakeHeader<HealthState>(
        MessageType::HealthState, 1, live_nonce, generation);
    game_health.game_heartbeat_qpc = 7777;
    Check(game.PublishLocalHealth(game_health) == SharedMemoryStatus::Ok,
          "test game endpoint publishes its heartbeat");
    HealthState observed_game{};
    Check(publisher.ReadGameHealth(observed_game) == SharedMemoryStatus::Ok &&
              observed_game.game_heartbeat_qpc == 7777,
          "publisher reads Game32 health through the same channel");

    publisher.Close();
    Check(!publisher.is_open() &&
              publisher.PublishFrame(views, frame, extent,
                                     PresentationState::UnknownSafe)
                      .transport_status == SharedMemoryStatus::NotOpen,
          "closed publisher rejects frame publication");
    game.Close();

    if (g_failures == 0) {
        std::cout << "All host IPC publisher tests passed.\n";
    }
    return g_failures == 0 ? 0 : 1;
}
