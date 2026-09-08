#include "shared_memory_channel.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

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

} // namespace

int main() {
    using namespace k2vr::ipc;

    Check(sizeof(SharedMemoryRegion) == 512,
          "shared-memory ABI is exactly 512 bytes");
    SharedMemoryChannel unopened;
    RenderRequest unread{};
    Check(unopened.ReadRenderRequest(unread) == SharedMemoryStatus::NotOpen,
          "closed channel rejects reads");

#if defined(_WIN32)
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    SessionNonce nonce{
        static_cast<std::uint64_t>(now.QuadPart) ^
            static_cast<std::uint64_t>(GetCurrentProcessId()),
        (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32U) ^
            static_cast<std::uint64_t>(GetTickCount64()) ^
            0x4B32565249504331ULL};
    if (!IsValid(nonce)) {
        nonce.low = 1;
    }
    constexpr std::uint64_t generation = 7;

    const SharedMemoryObjectName object_name =
        MakeSharedMemoryObjectName(nonce);
    Check(object_name.valid(), "valid nonce produces a Local named object");
    Check(wcsncmp(object_name.c_str(), L"Local\\Kotor2VR-ipc-v1-", 22) == 0,
          "mapping name is session-local and protocol-scoped");
    Check(!MakeSharedMemoryObjectName({}).valid(),
          "zero nonce cannot name a mapping");

    SharedMemoryChannel host;
    Check(SharedMemoryChannel::CreateHost(nonce, generation, host) ==
              SharedMemoryStatus::Ok,
          "host creates and initializes the mapping");
    Check(host.is_open() && host.role() == SharedMemoryRole::HostCreator &&
              host.generation() == generation,
          "host retains role, nonce, and generation");

    SharedMemoryChannel duplicate_host;
    Check(SharedMemoryChannel::CreateHost(nonce, generation, duplicate_host) ==
              SharedMemoryStatus::MappingAlreadyExists,
          "second creator cannot attach to an existing session");

    SharedMemoryChannel wrong_generation;
    Check(SharedMemoryChannel::OpenGame(nonce, generation + 1,
                                        wrong_generation) ==
              SharedMemoryStatus::GenerationMismatch &&
              !wrong_generation.is_open(),
          "opener fails closed on generation mismatch");

    SharedMemoryChannel game;
    Check(SharedMemoryChannel::OpenGame(nonce, generation, game) ==
              SharedMemoryStatus::Ok,
          "game opens the exact host mapping");
    Check(game.ReadRenderRequest(unread) ==
              SharedMemoryStatus::SnapshotUnavailable,
          "unpublished request is unavailable rather than zero-valued data");

    RenderRequest request{};
    request.header = MakeHeader<RenderRequest>(
        MessageType::RenderRequest, 1, nonce, generation);
    request.frame_id = 42;
    request.predicted_display_time_ns = 123456789;
    request.render_width = 2016;
    request.render_height = 2208;
    request.presentation_state = PresentationState::WorldFirstPerson;

    Check(game.PublishRenderRequest(request) ==
              SharedMemoryStatus::RoleViolation,
          "game role cannot publish host-owned render requests");
    Check(host.PublishRenderRequest(request) == SharedMemoryStatus::Ok,
          "host publishes a render request without a mutex");
    RenderRequest received{};
    Check(game.ReadRenderRequest(received) == SharedMemoryStatus::Ok &&
              received.frame_id == request.frame_id &&
              received.render_width == request.render_width &&
              received.header.generation == generation,
          "game reads the complete consistent request snapshot");

    RenderRequest bad_request = request;
    bad_request.header.session_nonce.low ^= 1;
    Check(host.PublishRenderRequest(bad_request) ==
              SharedMemoryStatus::MessageHeaderMismatch,
          "publisher rejects a request for another nonce");
    bad_request = request;
    bad_request.header.generation = generation + 1;
    Check(host.PublishRenderRequest(bad_request) ==
              SharedMemoryStatus::MessageHeaderMismatch,
          "publisher rejects a request from another generation");

    HealthState host_health{};
    host_health.header = MakeHeader<HealthState>(
        MessageType::HealthState, 2, nonce, generation);
    host_health.host_heartbeat_qpc = 1001;
    host_health.presented_frame_count = 17;
    Check(host.PublishLocalHealth(host_health) == SharedMemoryStatus::Ok,
          "host publishes its heartbeat slot");
    HealthState peer_health{};
    Check(game.ReadPeerHealth(peer_health) == SharedMemoryStatus::Ok &&
              peer_health.host_heartbeat_qpc == 1001 &&
              peer_health.presented_frame_count == 17,
          "game reads host heartbeat and counters");

    HealthState game_health{};
    game_health.header = MakeHeader<HealthState>(
        MessageType::HealthState, 3, nonce, generation);
    game_health.game_heartbeat_qpc = 2002;
    game_health.dropped_frame_count = 4;
    Check(game.PublishLocalHealth(game_health) == SharedMemoryStatus::Ok,
          "game publishes its independent heartbeat slot");
    peer_health = {};
    Check(host.ReadPeerHealth(peer_health) == SharedMemoryStatus::Ok &&
              peer_health.game_heartbeat_qpc == 2002 &&
              peer_health.dropped_frame_count == 4,
          "host reads game heartbeat and counters");

    HANDLE raw_mapping = OpenFileMappingW(
        FILE_MAP_READ | FILE_MAP_WRITE, FALSE, object_name.c_str());
    auto* raw_region = raw_mapping != nullptr
                           ? static_cast<SharedMemoryRegion*>(MapViewOfFile(
                                 raw_mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                                 0, 0, sizeof(SharedMemoryRegion)))
                           : nullptr;
    Check(raw_mapping != nullptr && raw_region != nullptr,
          "test obtains an independent view for deterministic seqlock faults");
    if (raw_region != nullptr) {
        const RenderRequest stable_request = raw_region->render_request.payload;
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(
            &raw_region->render_request.sequence));
        raw_region->render_request.payload.frame_id = 999;
        RenderRequest sentinel{};
        sentinel.frame_id = 777;
        Check(game.ReadRenderRequest(sentinel) ==
                  SharedMemoryStatus::SnapshotContended &&
                  sentinel.frame_id == 777,
              "odd seqlock never exposes a torn payload or alters output");
        raw_region->render_request.payload = stable_request;
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(
            &raw_region->render_request.sequence));
        Check(game.ReadRenderRequest(received) == SharedMemoryStatus::Ok &&
                  received.frame_id == 42,
              "reader recovers after writer publishes the next even sequence");

        const std::uint16_t saved_abi_minor = raw_region->header.abi_minor;
        raw_region->header.abi_minor = saved_abi_minor + 1;
        SharedMemoryChannel bad_version;
        Check(SharedMemoryChannel::OpenGame(nonce, generation, bad_version) ==
                  SharedMemoryStatus::VersionMismatch,
              "opener rejects an ABI/protocol version mismatch");
        raw_region->header.abi_minor = saved_abi_minor;

        const SessionNonce saved_nonce = raw_region->header.session_nonce;
        raw_region->header.session_nonce.low ^= 1;
        SharedMemoryChannel bad_nonce;
        Check(SharedMemoryChannel::OpenGame(nonce, generation, bad_nonce) ==
                  SharedMemoryStatus::NonceMismatch,
              "opener rejects a header nonce mismatch");
        raw_region->header.session_nonce = saved_nonce;

        const std::uint32_t saved_size = raw_region->header.region_size;
        raw_region->header.region_size = saved_size + 1;
        SharedMemoryChannel bad_size;
        Check(SharedMemoryChannel::OpenGame(nonce, generation, bad_size) ==
                  SharedMemoryStatus::SizeMismatch,
              "opener rejects a region ABI size mismatch");
        raw_region->header.region_size = saved_size;
    }
    if (raw_region != nullptr) {
        UnmapViewOfFile(raw_region);
    }
    if (raw_mapping != nullptr) {
        CloseHandle(raw_mapping);
    }

    SharedMemoryChannel moved(std::move(game));
    Check(moved.is_open() && !game.is_open() &&
              moved.role() == SharedMemoryRole::GameOpener,
          "move transfers mapping ownership exactly once");
    moved.Close();
    moved.Close();
    Check(!moved.is_open(), "Close is idempotent");
    host.Close();

    SharedMemoryChannel after_close;
    Check(SharedMemoryChannel::OpenGame(nonce, generation, after_close) ==
              SharedMemoryStatus::MappingOpenFailure,
          "named mapping disappears after all handles close");
#else
    SharedMemoryChannel channel;
    Check(SharedMemoryChannel::CreateHost({1, 2}, 1, channel) ==
              SharedMemoryStatus::UnsupportedPlatform,
          "non-Windows implementation fails closed");
#endif

    if (g_failures == 0) {
        std::cout << "All shared-memory IPC tests passed.\n";
    }
    return g_failures == 0 ? 0 : 1;
}
