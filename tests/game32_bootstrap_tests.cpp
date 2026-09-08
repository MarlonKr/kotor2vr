#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "probe.hpp"
#include "game_image_capture.hpp"
#include "render_trace.hpp"
#include "shared_memory_channel.hpp"
#include "vr_bridge.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const char* label) {
    if (condition) {
        std::cout << "PASS: " << label << '\n';
    } else {
        std::cerr << "FAIL: " << label << '\n';
        ++g_failures;
    }
}

std::wstring ReadEnvironment(const wchar_t* name, bool& existed) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        existed = false;
        return {};
    }
    existed = true;
    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    value.resize(copied);
    return value;
}

bool FileContains(const std::filesystem::path& path,
                  std::string_view marker) {
    if (!std::filesystem::exists(path)) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
    return text.find(marker) != std::string::npos;
}

} // namespace

int main() {
    static_assert(sizeof(void*) == 4,
                  "bootstrap integration test must execute as Win32/x86");
    static_assert(k2vr::game32::SamplerStopWasObserved(WAIT_OBJECT_0));
    static_assert(!k2vr::game32::SamplerStopWasObserved(WAIT_TIMEOUT));
    static_assert(!k2vr::game32::SamplerStopWasObserved(WAIT_FAILED));
    static_assert(sizeof(k2vr::game32::VrBridgeBootstrapV1) == 32);
    static_assert(sizeof(k2vr::game32::GameImageSmokeBootstrapV1) == 32);
    static_assert(sizeof(k2vr::game32::GameImageSharedHeaderV1) == 64);
    static_assert(k2vr::game32::VrBridgeWorkerStopWasObserved(WAIT_OBJECT_0));
    static_assert(
        !k2vr::game32::VrBridgeWorkerStopWasObserved(WAIT_TIMEOUT));

    wchar_t temporary_base[MAX_PATH]{};
    const DWORD temporary_length =
        GetTempPathW(static_cast<DWORD>(std::size(temporary_base)),
                     temporary_base);
    Check(temporary_length != 0 && temporary_length < std::size(temporary_base),
          "temporary directory is available");

    const std::filesystem::path sandbox =
        std::filesystem::path(temporary_base) /
        (L"kotor2vr-bootstrap-test-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(sandbox);

    bool local_app_data_existed = false;
    const std::wstring previous_local_app_data =
        ReadEnvironment(L"LOCALAPPDATA", local_app_data_existed);
    Check(SetEnvironmentVariableW(L"LOCALAPPDATA", sandbox.c_str()) != FALSE,
          "isolated LOCALAPPDATA is installed");

    const std::filesystem::path log_path =
        sandbox / L"Kotor2VR" / L"logs" / L"game32-probe.log";
    const std::filesystem::path bridge_log_path =
        sandbox / L"Kotor2VR" / L"logs" / L"vr-bridge.log";

    wchar_t executable_path[32768]{};
    const DWORD executable_length = GetModuleFileNameW(
        nullptr, executable_path, static_cast<DWORD>(std::size(executable_path)));
    Check(executable_length != 0 && executable_length < std::size(executable_path),
          "test executable path is available");
    const std::filesystem::path module_path =
        std::filesystem::path(executable_path).parent_path() /
        L"kotor2vr-game32.dll";
    HMODULE module = LoadLibraryW(module_path.c_str());
    Check(module != nullptr, "game32 module loads from the test output directory");
    Sleep(50);
    Check(!std::filesystem::exists(log_path),
          "DllMain performs no asynchronous bootstrap or file I/O");
    Check(!std::filesystem::exists(bridge_log_path),
          "DllMain does not start or log the VR bridge");

    using BootstrapFunction = std::uint32_t(WINAPI*)(void*);
    using ShutdownFunction = void(__cdecl*)();
    const auto bootstrap = reinterpret_cast<BootstrapFunction>(
        GetProcAddress(module, "K2VR_ProbeBootstrap"));
    const auto shutdown = reinterpret_cast<ShutdownFunction>(
        GetProcAddress(module, "K2VR_ProbeShutdown"));
    const auto render_trace_bootstrap = reinterpret_cast<BootstrapFunction>(
        GetProcAddress(module, "K2VR_RenderTraceBootstrap"));
    const auto render_double_pass_bootstrap =
        reinterpret_cast<BootstrapFunction>(
            GetProcAddress(module, "K2VR_RenderDoublePassBootstrap"));
    const auto render_trace_stop = reinterpret_cast<BootstrapFunction>(
        GetProcAddress(module, "K2VR_RenderTraceStop"));
    const auto game_image_smoke_bootstrap =
        reinterpret_cast<BootstrapFunction>(
            GetProcAddress(module, "K2VR_GameImageSmokeBootstrap"));
    const auto vr_bridge_bootstrap = reinterpret_cast<BootstrapFunction>(
        GetProcAddress(module, "K2VR_VrBridgeBootstrap"));
    const auto vr_bridge_stop = reinterpret_cast<BootstrapFunction>(
        GetProcAddress(module, "K2VR_VrBridgeStop"));
    Check(bootstrap != nullptr,
          "stable undecorated WINAPI bootstrap export is discoverable");
    Check(shutdown != nullptr, "shutdown export is discoverable");
    Check(render_trace_bootstrap != nullptr,
          "stable undecorated render-trace bootstrap export is discoverable");
    Check(render_double_pass_bootstrap != nullptr,
          "stable undecorated render double-pass bootstrap export is discoverable");
    Check(render_trace_stop != nullptr,
          "stable undecorated render-trace stop export is discoverable");
    Check(game_image_smoke_bootstrap != nullptr,
          "stable undecorated game-image smoke bootstrap export is discoverable");
    Check(vr_bridge_bootstrap != nullptr,
          "stable undecorated VR-bridge bootstrap export is discoverable");
    Check(vr_bridge_stop != nullptr,
          "stable undecorated VR-bridge stop export is discoverable");
    if (bootstrap == nullptr || shutdown == nullptr ||
        render_trace_bootstrap == nullptr ||
        render_double_pass_bootstrap == nullptr ||
        render_trace_stop == nullptr || game_image_smoke_bootstrap == nullptr ||
        vr_bridge_bootstrap == nullptr ||
        vr_bridge_stop == nullptr) {
        return 1;
    }

    Check(static_cast<k2vr::game32::RenderTraceResult>(
              render_trace_bootstrap(reinterpret_cast<void*>(1))) ==
              k2vr::game32::RenderTraceResult::InvalidArgument,
          "render-trace bootstrap rejects a non-null protocol-v1 argument");
    Check(static_cast<k2vr::game32::RenderTraceResult>(
              render_double_pass_bootstrap(reinterpret_cast<void*>(1))) ==
              k2vr::game32::RenderTraceResult::InvalidArgument,
          "render double-pass bootstrap rejects a non-null protocol-v1 argument");
    Check(static_cast<k2vr::game32::RenderTraceResult>(
              render_trace_stop(reinterpret_cast<void*>(1))) ==
              k2vr::game32::RenderTraceResult::InvalidArgument,
          "render-trace stop rejects a non-null protocol-v1 argument");
    Check(static_cast<k2vr::game32::GameImageCaptureResult>(
              game_image_smoke_bootstrap(nullptr)) ==
              k2vr::game32::GameImageCaptureResult::InvalidArgument,
          "game-image smoke requires a non-null packed bootstrap value");

    using k2vr::game32::VrBridgeBootstrapV1;
    using k2vr::game32::VrBridgeResult;
    using namespace k2vr::ipc;
    Check(static_cast<VrBridgeResult>(vr_bridge_bootstrap(nullptr)) ==
              VrBridgeResult::InvalidArgument,
          "VR bridge requires a non-null packed bootstrap value");
    VrBridgeBootstrapV1 wrong_version{
        sizeof(VrBridgeBootstrapV1),
        k2vr::game32::kVrBridgeBootstrapMajor,
        static_cast<std::uint16_t>(
            k2vr::game32::kVrBridgeBootstrapMinor + 1),
        {1, 2},
        1};
    Check(static_cast<VrBridgeResult>(
              vr_bridge_bootstrap(&wrong_version)) ==
              VrBridgeResult::VersionMismatch,
          "VR bridge rejects an unknown packed bootstrap version");
    VrBridgeBootstrapV1 invalid_session{
        sizeof(VrBridgeBootstrapV1),
        k2vr::game32::kVrBridgeBootstrapMajor,
        k2vr::game32::kVrBridgeBootstrapMinor,
        {},
        0};
    Check(static_cast<VrBridgeResult>(
              vr_bridge_bootstrap(&invalid_session)) ==
              VrBridgeResult::InvalidSession,
          "VR bridge rejects a zero nonce and generation");
    Check(static_cast<VrBridgeResult>(
              vr_bridge_stop(reinterpret_cast<void*>(1))) ==
              VrBridgeResult::InvalidArgument,
          "VR bridge stop rejects a non-null reserved argument");

    LARGE_INTEGER nonce_counter{};
    QueryPerformanceCounter(&nonce_counter);
    SessionNonce bridge_nonce{
        static_cast<std::uint64_t>(nonce_counter.QuadPart) ^
            static_cast<std::uint64_t>(GetCurrentProcessId()),
        (static_cast<std::uint64_t>(GetTickCount64()) << 16U) ^
            0x4B32565242524731ULL};
    if (!IsValid(bridge_nonce)) {
        bridge_nonce.low = 1;
    }
    constexpr std::uint64_t bridge_generation = 9;
    SharedMemoryChannel bridge_host;
    Check(SharedMemoryChannel::CreateHost(
              bridge_nonce, bridge_generation, bridge_host) ==
              SharedMemoryStatus::Ok,
          "test host creates the exact VR-bridge mapping");
    VrBridgeBootstrapV1 bridge_arguments{
        sizeof(VrBridgeBootstrapV1),
        k2vr::game32::kVrBridgeBootstrapMajor,
        k2vr::game32::kVrBridgeBootstrapMinor,
        bridge_nonce,
        bridge_generation};
    Check(static_cast<VrBridgeResult>(
              vr_bridge_bootstrap(&bridge_arguments)) ==
              VrBridgeResult::Ok,
          "VR bridge opens the mapping and acknowledges its first heartbeat");
    Check(static_cast<VrBridgeResult>(
              vr_bridge_bootstrap(&bridge_arguments)) ==
              VrBridgeResult::AlreadyRunning,
          "second bootstrap cannot create a second VR-bridge worker");

    HealthState observed_game_health{};
    SharedMemoryStatus health_status = SharedMemoryStatus::SnapshotUnavailable;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        health_status = bridge_host.ReadPeerHealth(observed_game_health);
        if (health_status == SharedMemoryStatus::Ok) {
            break;
        }
        Sleep(5);
    }
    Check(health_status == SharedMemoryStatus::Ok &&
              observed_game_health.game_heartbeat_qpc != 0 &&
              observed_game_health.header.generation == bridge_generation &&
              HasFlag(static_cast<RuntimeHealth>(
                          observed_game_health.health_flags),
                      RuntimeHealth::FlatFallbackActive),
          "worker publishes game heartbeat and explicit no-render fallback health");

    RenderRequest bridge_request{};
    bridge_request.header = MakeHeader<RenderRequest>(
        MessageType::RenderRequest, 1, bridge_nonce, bridge_generation);
    bridge_request.frame_id = 42;
    bridge_request.predicted_display_time_ns = 123456789;
    bridge_request.render_width = 2016;
    bridge_request.render_height = 2208;
    bridge_request.presentation_state =
        PresentationState::WorldFirstPerson;
    Check(bridge_host.PublishRenderRequest(bridge_request) ==
              SharedMemoryStatus::Ok,
          "test host publishes a consistent RenderRequest");
    bool frame_witness = false;
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        if (FileContains(bridge_log_path, "frame id=42 seq=1")) {
            frame_witness = true;
            break;
        }
        Sleep(5);
    }
    Check(frame_witness,
          "worker logs a newly observed frame ID without touching engine state");
    Check(static_cast<VrBridgeResult>(vr_bridge_stop(nullptr)) ==
              VrBridgeResult::Ok,
          "VR bridge teardown observes worker exit within its bounded wait");
    Check(static_cast<VrBridgeResult>(vr_bridge_stop(nullptr)) ==
              VrBridgeResult::AlreadyStopped,
          "VR bridge teardown is terminal and cannot race a second worker");
    bridge_host.Close();

    const auto invalid_argument = static_cast<k2vr::game32::ProbeResult>(
        bootstrap(reinterpret_cast<void*>(1)));
    Check(invalid_argument ==
              k2vr::game32::ProbeResult::InvalidBootstrapArgument,
          "non-null protocol-v1 bootstrap argument fails closed");
    Check(!std::filesystem::exists(log_path),
          "rejected bootstrap has no persistent side effect");

    const auto first = static_cast<k2vr::game32::ProbeResult>(
        bootstrap(nullptr));
    Check(first != k2vr::game32::ProbeResult::OkExactBuildProbeOnly &&
              first != k2vr::game32::ProbeResult::AlreadyInitialized,
          "explicit bootstrap fails closed for the non-game test executable");
    Check(std::filesystem::exists(log_path),
          "explicit bootstrap creates a persistent log witness");

    std::string log;
    if (std::filesystem::exists(log_path)) {
        std::ifstream input(log_path, std::ios::binary);
        log.assign(std::istreambuf_iterator<char>(input),
                   std::istreambuf_iterator<char>());
    }
    Check(log.find("bootstrap-entry source=explicit-export pid=") !=
              std::string::npos,
          "witness identifies explicit export and process");
    Check(log.find("build verification failed; no address dereference and no hooks") !=
              std::string::npos,
          "unknown executable failure is persisted before return");

    const auto size_before_repeat = std::filesystem::file_size(log_path);
    const auto repeated = static_cast<k2vr::game32::ProbeResult>(
        bootstrap(nullptr));
    Check(repeated == first, "explicit bootstrap result is idempotent");
    Check(std::filesystem::file_size(log_path) == size_before_repeat,
          "repeated bootstrap does not duplicate probe work");

    shutdown();

    const std::filesystem::path blocked_local_app_data =
        sandbox / L"not-a-directory";
    {
        std::ofstream blocker(blocked_local_app_data, std::ios::binary);
        blocker << "block directory creation";
    }
    Check(SetEnvironmentVariableW(L"LOCALAPPDATA",
                                  blocked_local_app_data.c_str()) != FALSE,
          "unwritable log fixture is installed");
    const auto log_failure = static_cast<k2vr::game32::ProbeResult>(
        bootstrap(nullptr));
    Check(log_failure == k2vr::game32::ProbeResult::PersistentLogFailure,
          "bootstrap fails closed when persistent logging is unavailable");
    Check(static_cast<k2vr::game32::ProbeResult>(bootstrap(nullptr)) ==
              log_failure,
          "persistent-log failure is idempotent");
    shutdown();

    if (local_app_data_existed) {
        SetEnvironmentVariableW(L"LOCALAPPDATA", previous_local_app_data.c_str());
    } else {
        SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(sandbox, cleanup_error);
    Check(!cleanup_error, "test sandbox cleanup succeeds");

    if (g_failures == 0) {
        std::cout << "All game32 bootstrap tests passed.\n";
    }
    return g_failures == 0 ? 0 : 1;
}
