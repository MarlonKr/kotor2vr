#include "vr_bridge.hpp"

#include "../common/shared_memory_channel.hpp"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

static_assert(sizeof(void*) == 4,
              "the game-side VR bridge is an in-process Win32/x86 component");

namespace k2vr::game32 {
namespace {

constexpr DWORD kWorkerPollMs = 5;
constexpr DWORD kWorkerReadyWaitMs = 2000;
constexpr DWORD kWorkerStopWaitMs = 3000;
constexpr ULONGLONG kHeartbeatIntervalMs = 100;

enum class BridgeState : LONG {
    Idle = 0,
    Starting = 1,
    Running = 2,
    Stopping = 3,
    Stopped = 4,
    Failed = 5,
};

volatile LONG g_bridge_state = static_cast<LONG>(BridgeState::Idle);
volatile LONG g_worker_start_status =
    static_cast<LONG>(ipc::SharedMemoryStatus::NotOpen);

VrBridgeBootstrapV1 g_bootstrap{};
ipc::SharedMemoryChannel g_channel;
detail::VrBridgeRenderRequestSeqlock g_latest_render_request;
HANDLE g_worker_thread = nullptr;
HANDLE g_stop_event = nullptr;
HANDLE g_ready_event = nullptr;
HANDLE g_log_file = INVALID_HANDLE_VALUE;
SRWLOCK g_log_lock = SRWLOCK_INIT;

[[nodiscard]] LONG AtomicRead(volatile LONG* value) noexcept {
    return InterlockedCompareExchange(value, 0, 0);
}

[[nodiscard]] std::uint64_t ReadQpc() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) != FALSE
               ? static_cast<std::uint64_t>(value.QuadPart)
               : 0;
}

[[nodiscard]] bool PinBridgeModule() noexcept {
    HMODULE pinned = nullptr;
    return GetModuleHandleExW(
               GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                   GET_MODULE_HANDLE_EX_FLAG_PIN,
               reinterpret_cast<LPCWSTR>(&K2VR_VrBridgeBootstrap),
               &pinned) != FALSE &&
           pinned != nullptr;
}

[[nodiscard]] bool TryCopyBootstrap(
    const void* source, VrBridgeBootstrapV1& output) noexcept {
    if (source == nullptr) {
        return false;
    }
    __try {
        std::memcpy(&output, source, sizeof(output));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output = {};
        return false;
    }
    return true;
}

[[nodiscard]] bool OpenBridgeLog() noexcept {
    if (g_log_file != INVALID_HANDLE_VALUE) {
        return true;
    }
    std::array<wchar_t, 32768> local_app_data{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local_app_data.data(),
        static_cast<DWORD>(local_app_data.size()));
    if (length == 0 || length >= local_app_data.size()) {
        return false;
    }

    std::wstring directory(local_app_data.data(), length);
    directory += L"\\Kotor2VR";
    if (!CreateDirectoryW(directory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    directory += L"\\logs";
    if (!CreateDirectoryW(directory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }

    const std::wstring path = directory + L"\\vr-bridge.log";
    g_log_file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_log_file != INVALID_HANDLE_VALUE;
}

[[nodiscard]] bool LogFormat(bool flush, const char* format, ...) noexcept {
    std::array<char, 512> message{};
    va_list arguments;
    va_start(arguments, format);
    const int count =
        std::vsnprintf(message.data(), message.size(), format, arguments);
    va_end(arguments);
    if (count < 0 || static_cast<std::size_t>(count) >= message.size()) {
        return false;
    }

    std::array<char, 640> line{};
    const int line_count = std::snprintf(
        line.data(), line.size(), "[K2VR-BRIDGE t=%llu] %s\n",
        static_cast<unsigned long long>(GetTickCount64()), message.data());
    if (line_count < 0 ||
        static_cast<std::size_t>(line_count) >= line.size()) {
        return false;
    }

    AcquireSRWLockExclusive(&g_log_lock);
    DWORD written = 0;
    const bool succeeded =
        g_log_file != INVALID_HANDLE_VALUE &&
        WriteFile(g_log_file, line.data(), static_cast<DWORD>(line_count),
                  &written, nullptr) != FALSE &&
        written == static_cast<DWORD>(line_count) &&
        (!flush || FlushFileBuffers(g_log_file) != FALSE);
    ReleaseSRWLockExclusive(&g_log_lock);
    return succeeded;
}

void CloseBridgeLog() noexcept {
    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log_file);
        CloseHandle(g_log_file);
        g_log_file = INVALID_HANDLE_VALUE;
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

[[nodiscard]] ipc::HealthState MakeGameHealth(
    std::uint64_t sequence, std::uint64_t game_heartbeat_qpc,
    std::uint64_t host_heartbeat_qpc,
    std::uint64_t duplicate_or_stale_count,
    ipc::SharedMemoryStatus last_error) noexcept {
    ipc::HealthState health{};
    health.header = ipc::MakeHeader<ipc::HealthState>(
        ipc::MessageType::HealthState, sequence,
        g_bootstrap.session_nonce, g_bootstrap.generation);
    health.game_heartbeat_qpc = game_heartbeat_qpc;
    health.host_heartbeat_qpc = host_heartbeat_qpc;
    health.duplicate_or_stale_count = duplicate_or_stale_count;
    health.error_state = last_error == ipc::SharedMemoryStatus::Ok
                             ? 0U
                             : static_cast<std::uint32_t>(last_error);
    health.health_flags = static_cast<std::uint32_t>(
        ipc::RuntimeHealth::FlatFallbackActive);
    if (last_error != ipc::SharedMemoryStatus::Ok) {
        health.health_flags |=
            static_cast<std::uint32_t>(ipc::RuntimeHealth::ProtocolError);
    }
    return health;
}

[[nodiscard]] ipc::SharedMemoryStatus PublishHealth(
    std::uint64_t sequence, std::uint64_t duplicate_or_stale_count,
    ipc::SharedMemoryStatus last_error) noexcept {
    std::uint64_t host_heartbeat_qpc = 0;
    ipc::HealthState host_health{};
    if (g_channel.ReadPeerHealth(host_health) ==
        ipc::SharedMemoryStatus::Ok) {
        host_heartbeat_qpc = host_health.host_heartbeat_qpc;
    }
    const ipc::HealthState game_health = MakeGameHealth(
        sequence, ReadQpc(), host_heartbeat_qpc,
        duplicate_or_stale_count, last_error);
    return g_channel.PublishLocalHealth(game_health);
}

DWORD WINAPI BridgeWorker(LPVOID) noexcept {
    std::uint64_t health_sequence = 1;
    std::uint64_t last_request_sequence = 0;
    std::uint64_t last_frame_id = 0;
    std::uint64_t duplicate_or_stale_count = 0;
    ipc::SharedMemoryStatus last_error = ipc::SharedMemoryStatus::Ok;

    const ipc::SharedMemoryStatus initial_health =
        PublishHealth(health_sequence++, duplicate_or_stale_count, last_error);
    InterlockedExchange(&g_worker_start_status,
                        static_cast<LONG>(initial_health));
    SetEvent(g_ready_event);
    if (initial_health != ipc::SharedMemoryStatus::Ok) {
        (void)LogFormat(true, "worker initial-health failed status=%s",
                        ipc::ToString(initial_health));
        return static_cast<DWORD>(initial_health);
    }

    (void)LogFormat(true, "worker started poll_ms=%lu heartbeat_ms=%llu writes=disabled",
                    static_cast<unsigned long>(kWorkerPollMs),
                    static_cast<unsigned long long>(kHeartbeatIntervalMs));
    ULONGLONG next_heartbeat = GetTickCount64() + kHeartbeatIntervalMs;

    for (;;) {
        if (WaitForSingleObject(g_stop_event, kWorkerPollMs) == WAIT_OBJECT_0) {
            break;
        }

        ipc::RenderRequest request{};
        const ipc::SharedMemoryStatus read_status =
            g_channel.ReadRenderRequest(request);
        if (read_status == ipc::SharedMemoryStatus::Ok) {
            if (!IsValidVrBridgeRenderRequest(
                    request, g_bootstrap.session_nonce,
                    g_bootstrap.generation)) {
                last_error =
                    ipc::SharedMemoryStatus::MessageHeaderMismatch;
            } else {
                last_error = ipc::SharedMemoryStatus::Ok;
            }
            const bool should_publish =
                last_error == ipc::SharedMemoryStatus::Ok &&
                ShouldPublishVrBridgeRenderRequest(
                    request, last_request_sequence, last_frame_id);
            if (last_error == ipc::SharedMemoryStatus::Ok &&
                request.header.sequence != last_request_sequence) {
                last_request_sequence = request.header.sequence;
                if (should_publish) {
                    last_frame_id = request.frame_id;
                    detail::VrBridgeRenderRequestSnapshotPod snapshot{};
                    snapshot.request = request;
                    snapshot.expected_nonce = g_bootstrap.session_nonce;
                    snapshot.expected_generation = g_bootstrap.generation;
                    snapshot.published_at_ms = GetTickCount64();
                    snapshot.valid = 1U;
                    g_latest_render_request.Publish(snapshot);
                    (void)LogFormat(
                        false,
                        "frame id=%llu seq=%llu state=%lu size=%lux%lu",
                        static_cast<unsigned long long>(request.frame_id),
                        static_cast<unsigned long long>(
                            request.header.sequence),
                        static_cast<unsigned long>(
                            request.presentation_state),
                        static_cast<unsigned long>(request.render_width),
                        static_cast<unsigned long>(request.render_height));
                } else {
                    ++duplicate_or_stale_count;
                }
            }
        } else if (read_status !=
                       ipc::SharedMemoryStatus::SnapshotUnavailable &&
                   read_status !=
                       ipc::SharedMemoryStatus::SnapshotContended) {
            last_error = read_status;
        }

        const ULONGLONG now = GetTickCount64();
        if (now >= next_heartbeat) {
            const ipc::SharedMemoryStatus publish_status = PublishHealth(
                health_sequence++, duplicate_or_stale_count, last_error);
            if (publish_status != ipc::SharedMemoryStatus::Ok) {
                last_error = publish_status;
            }
            next_heartbeat = now + kHeartbeatIntervalMs;
        }
    }

    // The worker is the only live publisher, so invalidating here makes Stop
    // linearizable without introducing a second seqlock writer.
    g_latest_render_request.Invalidate();

    const ipc::SharedMemoryStatus final_health = PublishHealth(
        health_sequence, duplicate_or_stale_count, last_error);
    (void)LogFormat(true,
                    "worker stopped last_frame=%llu stale=%llu health=%s",
                    static_cast<unsigned long long>(last_frame_id),
                    static_cast<unsigned long long>(
                        duplicate_or_stale_count),
                    ipc::ToString(final_health));
    return final_health == ipc::SharedMemoryStatus::Ok
               ? 0U
               : static_cast<DWORD>(final_health);
}

void CloseStoppedResources() noexcept {
    // All callers have either not created a worker or have joined it first.
    // Invalidate before unmapping the channel so later render-thread reads can
    // never depend on mapping lifetime.
    g_latest_render_request.Invalidate();
    if (g_worker_thread != nullptr) {
        CloseHandle(g_worker_thread);
        g_worker_thread = nullptr;
    }
    if (g_ready_event != nullptr) {
        CloseHandle(g_ready_event);
        g_ready_event = nullptr;
    }
    if (g_stop_event != nullptr) {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
    }
    g_channel.Close();
    g_bootstrap = {};
    CloseBridgeLog();
}

[[nodiscard]] VrBridgeResult FailStartupAfterWorkerCreation(
    VrBridgeResult failure) noexcept {
    if (g_stop_event != nullptr) {
        SetEvent(g_stop_event);
    }
    const DWORD wait = g_worker_thread != nullptr
                           ? WaitForSingleObject(g_worker_thread,
                                                 kWorkerStopWaitMs)
                           : WAIT_OBJECT_0;
    if (!VrBridgeWorkerStopWasObserved(wait)) {
        InterlockedExchange(&g_bridge_state,
                            static_cast<LONG>(BridgeState::Failed));
        return VrBridgeResult::StopTimeout;
    }
    CloseStoppedResources();
    InterlockedExchange(&g_bridge_state,
                        static_cast<LONG>(BridgeState::Failed));
    return failure;
}

[[nodiscard]] VrBridgeResult BootstrapBridge(void* raw_bootstrap) noexcept {
    VrBridgeBootstrapV1 copied{};
    if (!TryCopyBootstrap(raw_bootstrap, copied)) {
        return VrBridgeResult::InvalidArgument;
    }
    const VrBridgeResult validation = ValidateVrBridgeBootstrap(copied);
    if (validation != VrBridgeResult::Ok) {
        return validation;
    }

    const LONG observed = InterlockedCompareExchange(
        &g_bridge_state, static_cast<LONG>(BridgeState::Starting),
        static_cast<LONG>(BridgeState::Idle));
    if (observed == static_cast<LONG>(BridgeState::Running)) {
        return VrBridgeResult::AlreadyRunning;
    }
    if (observed != static_cast<LONG>(BridgeState::Idle)) {
        return VrBridgeResult::Busy;
    }

    g_latest_render_request.Invalidate();
    g_bootstrap = copied;
    if (!PinBridgeModule()) {
        InterlockedExchange(&g_bridge_state,
                            static_cast<LONG>(BridgeState::Failed));
        return VrBridgeResult::ModulePinFailure;
    }
    if (!OpenBridgeLog()) {
        InterlockedExchange(&g_bridge_state,
                            static_cast<LONG>(BridgeState::Failed));
        return VrBridgeResult::PersistentLogFailure;
    }
    if (!LogFormat(true,
                   "bootstrap pid=%lu tid=%lu version=%u.%u nonce=%016llX%016llX generation=%llu",
                   static_cast<unsigned long>(GetCurrentProcessId()),
                   static_cast<unsigned long>(GetCurrentThreadId()),
                   static_cast<unsigned>(copied.version_major),
                   static_cast<unsigned>(copied.version_minor),
                   static_cast<unsigned long long>(
                       copied.session_nonce.high),
                   static_cast<unsigned long long>(
                       copied.session_nonce.low),
                   static_cast<unsigned long long>(copied.generation))) {
        CloseBridgeLog();
        InterlockedExchange(&g_bridge_state,
                            static_cast<LONG>(BridgeState::Failed));
        return VrBridgeResult::PersistentLogFailure;
    }

    ipc::SharedMemoryChannel channel;
    const ipc::SharedMemoryStatus open_status =
        ipc::SharedMemoryChannel::OpenGame(
            copied.session_nonce, copied.generation, channel);
    if (open_status != ipc::SharedMemoryStatus::Ok) {
        (void)LogFormat(true, "bootstrap channel-open failed status=%s",
                        ipc::ToString(open_status));
        CloseBridgeLog();
        g_bootstrap = {};
        InterlockedExchange(&g_bridge_state,
                            static_cast<LONG>(BridgeState::Failed));
        return VrBridgeResult::ChannelOpenFailure;
    }

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop_event == nullptr || g_ready_event == nullptr) {
        channel.Close();
        CloseStoppedResources();
        InterlockedExchange(&g_bridge_state,
                            static_cast<LONG>(BridgeState::Failed));
        return VrBridgeResult::SynchronizationFailure;
    }

    g_channel = std::move(channel);
    InterlockedExchange(
        &g_worker_start_status,
        static_cast<LONG>(ipc::SharedMemoryStatus::NotOpen));
    g_worker_thread =
        CreateThread(nullptr, 0, BridgeWorker, nullptr, 0, nullptr);
    if (g_worker_thread == nullptr) {
        CloseStoppedResources();
        InterlockedExchange(&g_bridge_state,
                            static_cast<LONG>(BridgeState::Failed));
        return VrBridgeResult::WorkerStartFailure;
    }

    if (WaitForSingleObject(g_ready_event, kWorkerReadyWaitMs) !=
        WAIT_OBJECT_0) {
        return FailStartupAfterWorkerCreation(
            VrBridgeResult::WorkerReadyTimeout);
    }
    if (static_cast<ipc::SharedMemoryStatus>(
            AtomicRead(&g_worker_start_status)) !=
        ipc::SharedMemoryStatus::Ok) {
        return FailStartupAfterWorkerCreation(
            VrBridgeResult::InitialHealthFailure);
    }
    if (!LogFormat(true,
                   "bootstrap complete worker=1 camera_writes=0 render_writes=0")) {
        return FailStartupAfterWorkerCreation(
            VrBridgeResult::PersistentLogFailure);
    }

    InterlockedExchange(&g_bridge_state,
                        static_cast<LONG>(BridgeState::Running));
    return VrBridgeResult::Ok;
}

[[nodiscard]] VrBridgeResult StopBridge(void* reserved) noexcept {
    if (reserved != nullptr) {
        return VrBridgeResult::InvalidArgument;
    }

    const LONG observed = InterlockedCompareExchange(
        &g_bridge_state, static_cast<LONG>(BridgeState::Stopping),
        static_cast<LONG>(BridgeState::Running));
    if (observed == static_cast<LONG>(BridgeState::Idle) ||
        observed == static_cast<LONG>(BridgeState::Stopped)) {
        return VrBridgeResult::AlreadyStopped;
    }
    if (observed != static_cast<LONG>(BridgeState::Running)) {
        return VrBridgeResult::Busy;
    }

    SetEvent(g_stop_event);
    const DWORD wait = WaitForSingleObject(g_worker_thread,
                                           kWorkerStopWaitMs);
    if (!VrBridgeWorkerStopWasObserved(wait)) {
        (void)LogFormat(true,
                        "stop timeout wait=0x%08lX; resources retained",
                        static_cast<unsigned long>(wait));
        InterlockedExchange(&g_bridge_state,
                            static_cast<LONG>(BridgeState::Failed));
        return VrBridgeResult::StopTimeout;
    }

    (void)LogFormat(true, "stop complete");
    CloseStoppedResources();
    InterlockedExchange(&g_bridge_state,
                        static_cast<LONG>(BridgeState::Stopped));
    return VrBridgeResult::Ok;
}

} // namespace

bool TryReadLatestRenderRequest(ipc::RenderRequest& output,
                                std::uint32_t max_age_ms) noexcept {
    if (AtomicRead(&g_bridge_state) !=
        static_cast<LONG>(BridgeState::Running)) {
        return false;
    }

    detail::VrBridgeRenderRequestSnapshotPod snapshot{};
    if (!g_latest_render_request.TryRead(snapshot) ||
        !detail::IsValidVrBridgeRenderRequestSnapshot(
            snapshot, GetTickCount64(), max_age_ms)) {
        return false;
    }

    // Stop changes state before waiting for the worker. Rechecking after the
    // copy prevents an overlapping reader from accepting the stopped session;
    // the local POD itself is already independent of channel unmapping.
    if (AtomicRead(&g_bridge_state) !=
        static_cast<LONG>(BridgeState::Running)) {
        return false;
    }
    output = snapshot.request;
    return true;
}

} // namespace k2vr::game32

std::uint32_t K2VR_VR_BRIDGE_THREAD_CALL
K2VR_VrBridgeBootstrap(void* bootstrap_v1) noexcept {
    return static_cast<std::uint32_t>(
        k2vr::game32::BootstrapBridge(bootstrap_v1));
}

std::uint32_t K2VR_VR_BRIDGE_THREAD_CALL
K2VR_VrBridgeStop(void* reserved) noexcept {
    return static_cast<std::uint32_t>(
        k2vr::game32::StopBridge(reserved));
}

#else

namespace k2vr::game32 {

bool TryReadLatestRenderRequest(ipc::RenderRequest&,
                                std::uint32_t) noexcept {
    return false;
}

} // namespace k2vr::game32

std::uint32_t K2VR_VR_BRIDGE_THREAD_CALL
K2VR_VrBridgeBootstrap(void*) noexcept {
    return static_cast<std::uint32_t>(
        k2vr::game32::VrBridgeResult::ChannelOpenFailure);
}

std::uint32_t K2VR_VR_BRIDGE_THREAD_CALL
K2VR_VrBridgeStop(void*) noexcept {
    return static_cast<std::uint32_t>(
        k2vr::game32::VrBridgeResult::AlreadyStopped);
}

#endif
