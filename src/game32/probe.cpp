#include "probe.hpp"

#include "../common/build_descriptor.hpp"
#include "../common/hook_validation.hpp"
#include "../common/ipc_protocol.hpp"
#include "../common/math.hpp"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

static_assert(sizeof(void*) == 4,
              "kotor2vr_game32 must be built for Win32/x86, never x64");

namespace k2vr::game32 {
namespace {

HMODULE g_self_module = nullptr;
HANDLE g_log_file = INVALID_HANDLE_VALUE;
HANDLE g_workspace_log = INVALID_HANDLE_VALUE;
SRWLOCK g_log_lock = SRWLOCK_INIT;
SRWLOCK g_state_lock = SRWLOCK_INIT;
SRWLOCK g_bootstrap_lock = SRWLOCK_INIT;
LONG g_initialization_state = 0; // Protected by g_state_lock.
ProbeResult g_initialization_result = ProbeResult::NotInitialized;
bool g_bootstrap_complete = false; // Protected by g_bootstrap_lock.
ProbeResult g_bootstrap_result = ProbeResult::NotInitialized;
HANDLE g_sampler_thread = nullptr; // Protected by g_bootstrap_lock.
std::optional<builds::VerifiedBuild> g_verified_build;

class ExclusiveSrwGuard final {
public:
    explicit ExclusiveSrwGuard(SRWLOCK& lock) noexcept : lock_(lock) {
        AcquireSRWLockExclusive(&lock_);
    }
    ~ExclusiveSrwGuard() { ReleaseSRWLockExclusive(&lock_); }
    ExclusiveSrwGuard(const ExclusiveSrwGuard&) = delete;
    ExclusiveSrwGuard& operator=(const ExclusiveSrwGuard&) = delete;

private:
    SRWLOCK& lock_;
};

class SharedSrwGuard final {
public:
    explicit SharedSrwGuard(SRWLOCK& lock) noexcept : lock_(lock) {
        AcquireSRWLockShared(&lock_);
    }
    ~SharedSrwGuard() { ReleaseSRWLockShared(&lock_); }
    SharedSrwGuard(const SharedSrwGuard&) = delete;
    SharedSrwGuard& operator=(const SharedSrwGuard&) = delete;

private:
    SRWLOCK& lock_;
};

[[nodiscard]] bool PinSelfModuleForProcessLifetime() noexcept {
    if (g_self_module == nullptr) {
        return false;
    }
    HMODULE pinned_module = nullptr;
    const BOOL pinned = GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&g_self_module), &pinned_module);
    return pinned != FALSE && pinned_module == g_self_module;
}

void DebugLine(std::string_view message) noexcept {
    std::string terminated(message);
    if (terminated.empty() || terminated.back() != '\n') {
        terminated.push_back('\n');
    }
    OutputDebugStringA(terminated.c_str());
}

[[nodiscard]] bool OpenProbeLog() noexcept {
    // Mirror diagnostics next to the injected DLL so the test has one explicit
    // workspace path for each locally built test DLL.
    if (g_workspace_log==INVALID_HANDLE_VALUE && g_self_module) {
        wchar_t module_path[MAX_PATH]{};
        const DWORD n=GetModuleFileNameW(g_self_module,module_path,MAX_PATH);
        if (n && n<MAX_PATH) {
            std::wstring path(module_path,n);
            const auto slash=path.find_last_of(L"\\");
            if (slash!=std::wstring::npos) {
                path.resize(slash); path+=L"\\logs";
                (void)CreateDirectoryW(path.c_str(),nullptr);
                path+=L"\\game32-live.log";
                g_workspace_log=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,
                    nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            }
        }
    }
    if (g_log_file != INVALID_HANDLE_VALUE) {
        return true;
    }
    std::array<wchar_t, 32768> local_app_data{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local_app_data.data(),
        static_cast<DWORD>(local_app_data.size()));
    if (length == 0 || length >= local_app_data.size()) {
        DebugLine("[K2VR] LOCALAPPDATA unavailable; logging to debugger only.");
        return false;
    }

    std::wstring directory(local_app_data.data(), length);
    directory += L"\\Kotor2VR";
    if (!CreateDirectoryW(directory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        DebugLine("[K2VR] Could not create local probe-log directory.");
        return false;
    }
    directory += L"\\logs";
    if (!CreateDirectoryW(directory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        DebugLine("[K2VR] Could not create local probe-log directory.");
        return false;
    }

    const std::wstring path = directory + L"\\game32-probe.log";
    g_log_file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (g_log_file == INVALID_HANDLE_VALUE) {
        DebugLine("[K2VR] Could not open probe log; using debugger only.");
        return false;
    }
    return true;
}

bool LogLine(std::string_view message) noexcept {
    std::string line("[K2VR t=");
    line.append(std::to_string(GetTickCount64()));
    line.append("ms] ");
    line.append(message);
    if (line.empty() || line.back() != '\n') {
        line.push_back('\n');
    }
    OutputDebugStringA(line.c_str());

    AcquireSRWLockExclusive(&g_log_lock);
    bool persisted = false;
    if (g_log_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        persisted =
            WriteFile(g_log_file, line.data(),
                      static_cast<DWORD>(line.size()), &written, nullptr) !=
                FALSE &&
            written == line.size() && FlushFileBuffers(g_log_file) != FALSE;
    }
    if (g_workspace_log != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        const bool mirrored = WriteFile(g_workspace_log, line.data(),
            static_cast<DWORD>(line.size()), &written, nullptr) != FALSE &&
            written == line.size() && FlushFileBuffers(g_workspace_log) != FALSE;
        persisted = persisted || mirrored;
    }
    ReleaseSRWLockExclusive(&g_log_lock);
    return persisted;
}

bool LogFormat(const char* format, ...) noexcept {
    std::array<char, 1024> buffer{};
    va_list arguments;
    va_start(arguments, format);
    const int count = std::vsnprintf(buffer.data(), buffer.size(), format, arguments);
    va_end(arguments);
    if (count < 0) {
        return LogLine("log formatting failed");
    }
    const std::size_t safe_count =
        static_cast<std::size_t>(count) < buffer.size()
            ? static_cast<std::size_t>(count)
            : buffer.size() - 1;
    return LogLine(std::string_view(buffer.data(), safe_count));
}

[[nodiscard]] bool IsReadableProtection(DWORD protection) noexcept {
    if ((protection & PAGE_GUARD) != 0 ||
        (protection & PAGE_NOACCESS) != 0) {
        return false;
    }
    switch (protection & 0xFFU) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool IsReadableRange(std::uintptr_t address,
                                   std::size_t size) noexcept {
    if (address == 0 || size == 0 || address + size < address) {
        return false;
    }
    const std::uintptr_t end = address + size;
    std::uintptr_t cursor = address;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory,
                         sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT ||
            !IsReadableProtection(memory.Protect)) {
            return false;
        }
        const std::uintptr_t region_end =
            reinterpret_cast<std::uintptr_t>(memory.BaseAddress) +
            memory.RegionSize;
        if (region_end <= cursor ||
            region_end < reinterpret_cast<std::uintptr_t>(memory.BaseAddress)) {
            return false;
        }
        cursor = region_end;
    }
    return true;
}

[[nodiscard]] bool SafeReadBytes(
    std::uintptr_t address, std::span<std::uint8_t> output) noexcept {
    if (output.empty() || !IsReadableRange(address, output.size())) {
        return false;
    }
    SIZE_T bytes_read = 0;
    return ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<const void*>(address),
                             output.data(), output.size(), &bytes_read) != FALSE &&
           bytes_read == output.size();
}

template <typename Value>
[[nodiscard]] bool SafeRead(std::uintptr_t address, Value& output) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    return SafeReadBytes(
        address, {reinterpret_cast<std::uint8_t*>(&output), sizeof(output)});
}

[[nodiscard]] bool ReadPointer(std::uintptr_t address,
                               std::uintptr_t& output) noexcept {
    std::uint32_t pointer32 = 0;
    if (!SafeRead(address, pointer32) || pointer32 == 0) {
        output = 0;
        return false;
    }
    output = pointer32;
    return true;
}

[[nodiscard]] bool ReadPointerAtOffset(std::uintptr_t base,
                                       std::uint32_t offset,
                                       std::uintptr_t& output) noexcept {
    if (base == 0 ||
        base > (std::numeric_limits<std::uintptr_t>::max)() - offset) {
        output = 0;
        return false;
    }
    return ReadPointer(base + offset, output);
}

void LogKnownAddresses() noexcept {
    const builds::VerifiedBuild& verified = *g_verified_build;
    const builds::ExactBuildDescriptor& build = verified.descriptor();
    for (const builds::KnownAddress& address : build.known_addresses) {
        const std::uintptr_t runtime_address = builds::ToRuntimeAddress(
            build, address, verified.runtime_image_base());
        MEMORY_BASIC_INFORMATION memory{};
        const bool mapped =
            VirtualQuery(reinterpret_cast<const void*>(runtime_address), &memory,
                         sizeof(memory)) == sizeof(memory) &&
            memory.State == MEM_COMMIT;
        LogFormat("symbol=%.*s preferred_va=0x%08lX runtime=0x%08lX mapped=%s protect=0x%08lX",
                  static_cast<int>(builds::ToString(address.symbol).size()),
                  builds::ToString(address.symbol).data(),
                  static_cast<unsigned long>(address.preferred_virtual_address),
                  static_cast<unsigned long>(runtime_address),
                  mapped ? "yes" : "no",
                  mapped ? static_cast<unsigned long>(memory.Protect) : 0UL);

        if (mapped && address.kind == builds::AddressKind::GlobalPointerSlot) {
            std::uintptr_t value = 0;
            LogFormat("  pointer-slot value=%s0x%08lX",
                      ReadPointer(runtime_address, value) ? "" : "unreadable/",
                      static_cast<unsigned long>(value));
        } else if (mapped && address.kind == builds::AddressKind::GlobalValue) {
            std::uint32_t value = 0;
            if (SafeRead(runtime_address, value)) {
                LogFormat("  global-value=0x%08lX",
                          static_cast<unsigned long>(value));
            }
        }
    }
}

void LogProbePatterns() noexcept {
    const builds::VerifiedBuild& verified = *g_verified_build;
    const builds::ExactBuildDescriptor& build = verified.descriptor();
    for (const hooks::HookCandidate& candidate : hooks::KnownProbeCandidates()) {
        if (candidate.build_id != build.id) {
            continue;
        }
        std::vector<std::uint8_t> observed(candidate.expected.bytes.size());
        const bool inside_image =
            candidate.rva <= verified.runtime_image_size() &&
            observed.size() <= verified.runtime_image_size() - candidate.rva &&
            verified.runtime_image_base() <=
                (std::numeric_limits<std::uintptr_t>::max)() - candidate.rva;
        const std::uintptr_t runtime_address =
            inside_image ? verified.runtime_image_base() + candidate.rva : 0;
        const bool readable =
            inside_image && SafeReadBytes(runtime_address, observed);
        const hooks::PatternStatus status =
            readable ? hooks::MatchPattern(observed, candidate.expected)
                     : hooks::PatternStatus::NoMatch;
        LogFormat("probe-pattern=%.*s rva=0x%08lX readable=%s match=%s installable=no",
                  static_cast<int>(candidate.name.size()), candidate.name.data(),
                  static_cast<unsigned long>(candidate.rva),
                  readable ? "yes" : "no",
                  status == hooks::PatternStatus::Match ? "yes" : "no");
    }
}

void LogCameraProbe() noexcept {
    const builds::VerifiedBuild& verified = *g_verified_build;
    const builds::ExactBuildDescriptor& build = verified.descriptor();
    const builds::CameraProbeLayout& layout = build.camera_probe;
    if (!layout.available) {
        LogLine("camera-probe layout unavailable for this exact build");
        return;
    }
    const builds::KnownAddress* app_manager_address = builds::FindAddress(
        build, builds::SymbolId::AppManagerPointer);
    if (app_manager_address == nullptr) {
        LogLine("camera-probe blocked: APP_MANAGER_PTR not described");
        return;
    }

    std::uintptr_t app_manager = 0;
    std::uintptr_t facade = 0;
    std::uintptr_t internal = 0;
    std::uintptr_t module = 0;
    std::uintptr_t camera = 0;
    const std::uintptr_t app_manager_slot = builds::ToRuntimeAddress(
        build, *app_manager_address, verified.runtime_image_base());
    if (!ReadPointer(app_manager_slot, app_manager) ||
        !ReadPointerAtOffset(app_manager, layout.app_manager_to_facade,
                             facade) ||
        !ReadPointerAtOffset(facade, layout.facade_to_internal, internal) ||
        !ReadPointerAtOffset(internal, layout.internal_to_module, module) ||
        !ReadPointerAtOffset(module, layout.module_to_camera, camera)) {
        LogLine("camera-probe unavailable (expected before a world/module is loaded)");
        return;
    }

    math::Pose pose{};
    if (camera > (std::numeric_limits<std::uintptr_t>::max)() -
                     layout.camera_position ||
        camera > (std::numeric_limits<std::uintptr_t>::max)() -
                     layout.camera_orientation ||
        !SafeRead(camera + layout.camera_position, pose.position) ||
        !SafeRead(camera + layout.camera_orientation, pose.orientation) ||
        !math::IsFinite(pose)) {
        LogLine("camera-probe pose unreadable or non-finite");
        return;
    }

    const float quaternion_length =
        std::sqrt(math::Dot(pose.orientation, pose.orientation));
    LogFormat("camera-probe camera=0x%08lX pos=(%.5f,%.5f,%.5f) quat_xyzw=(%.5f,%.5f,%.5f,%.5f) qlen=%.5f",
              static_cast<unsigned long>(camera), pose.position.x,
              pose.position.y, pose.position.z, pose.orientation.x,
              pose.orientation.y, pose.orientation.z, pose.orientation.w,
              quaternion_length);
}

DWORD WINAPI RunReadOnlySampler(LPVOID) noexcept {
    constexpr DWORD sample_interval_ms = 500;
    constexpr std::uint32_t maximum_samples = 1200; // Ten minutes.
    LogFormat("sampler-running interval_ms=%lu maximum_samples=%lu",
              static_cast<unsigned long>(sample_interval_ms),
              static_cast<unsigned long>(maximum_samples));
    std::uint32_t completed_samples = 0;
    ProbeResult terminal_result = ProbeResult::OkExactBuildProbeOnly;
    for (; completed_samples < maximum_samples; ++completed_samples) {
        Sleep(sample_interval_ms);
        terminal_result = RunReadOnlyCameraSample();
        if (terminal_result != ProbeResult::OkExactBuildProbeOnly) {
            break;
        }
    }
    LogFormat("sampler-stopped completed_samples=%lu result=%lu",
              static_cast<unsigned long>(completed_samples),
              static_cast<unsigned long>(terminal_result));
    return static_cast<DWORD>(terminal_result);
}

} // namespace

void SetSelfModule(void* module) noexcept {
    g_self_module = static_cast<HMODULE>(module);
}

bool AppendPersistentProbeLogLine(std::string_view message) noexcept {
    return OpenProbeLog() && LogLine(message);
}

ProbeResult InitializeProbe(std::string_view source) noexcept {
    ExclusiveSrwGuard state_guard(g_state_lock);
    if (g_initialization_state != 0) {
        if (g_initialization_state == 3) {
            return g_initialization_result;
        }
        return ProbeResult::AlreadyInitialized;
    }
    g_initialization_state = 1;
    const auto finish = [](ProbeResult result) noexcept {
        g_initialization_result = result;
        g_initialization_state = 2;
        return result;
    };

    // KPM's injection path loads DLL-only patches with LoadLibrary and retains
    // that reference for the process lifetime. Pinning here, after DllMain and
    // therefore outside the loader lock, also makes an accidental later
    // FreeLibrary unable to unload code while this worker or an export runs.
    if (!PinSelfModuleForProcessLifetime()) {
        OutputDebugStringA(
            "[K2VR] could not pin game32 module; probe refused.\n");
        return finish(ProbeResult::LifetimePinFailure);
    }

    if (!OpenProbeLog()) {
        return finish(ProbeResult::PersistentLogFailure);
    }
    if (!LogFormat("bootstrap-entry source=%.*s pid=%lu tid=%lu",
                   static_cast<int>(source.size()), source.data(),
                   static_cast<unsigned long>(GetCurrentProcessId()),
                   static_cast<unsigned long>(GetCurrentThreadId()))) {
        DebugLine("[K2VR] persistent bootstrap witness failed; probe refused.");
        return finish(ProbeResult::PersistentLogFailure);
    }
    LogLine("probe initialization; hook installation is intentionally disabled");
    LogFormat("dll=0x%08lX pointer_width=%lu protocol=%u.%u",
              static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(
                  g_self_module)),
              static_cast<unsigned long>(sizeof(void*) * 8),
              static_cast<unsigned>(ipc::kProtocolMajor),
              static_cast<unsigned>(ipc::kProtocolMinor));

    builds::BuildVerificationResult verification =
        builds::VerifyMainExecutable();
    const auto digest_hex = builds::ToHex(verification.executable_sha256);
    LogFormat("verification=%.*s exe_sha256=%s machine=0x%04X image_size=0x%08lX file_size=0x%llX",
              static_cast<int>(builds::ToString(verification.status).size()),
              builds::ToString(verification.status).data(), digest_hex.data(),
              static_cast<unsigned>(verification.pe_machine),
              static_cast<unsigned long>(verification.runtime_image_size),
              static_cast<unsigned long long>(
                  verification.executable_file_size));

    if (verification.pe_machine != 0 &&
        verification.pe_machine != builds::kPeMachineI386) {
        LogFormat("unsupported PE machine=0x%04X; expected x86 0x014C",
                  static_cast<unsigned>(verification.pe_machine));
        return finish(ProbeResult::UnsupportedArchitecture);
    }

    if (!verification.IsVerified()) {
        LogLine("build verification failed; no address dereference and no hooks");
        switch (verification.status) {
        case builds::BuildVerificationStatus::HashFailure:
        case builds::BuildVerificationStatus::ExecutablePathUnavailable:
        case builds::BuildVerificationStatus::FileOpenFailure:
            return finish(ProbeResult::HashFailure);
        case builds::BuildVerificationStatus::LoadedImageInspectionFailure:
        case builds::BuildVerificationStatus::FileInspectionFailure:
        case builds::BuildVerificationStatus::LoadedImageDoesNotMatchFile:
            return finish(ProbeResult::PeInspectionFailure);
        default:
            return finish(ProbeResult::UnknownExecutable);
        }
    }

    g_verified_build = std::move(verification.verified_build);
    const builds::ExactBuildDescriptor& exact_build =
        g_verified_build->descriptor();
    LogFormat("exact_build=%.*s support=probe-only",
              static_cast<int>(exact_build.name.size()),
              exact_build.name.data());
    return finish(ProbeResult::OkExactBuildProbeOnly);
}

ProbeResult CurrentInitializationResult() noexcept {
    SharedSrwGuard state_guard(g_state_lock);
    return g_initialization_state == 2 ? g_initialization_result
                                       : ProbeResult::NotInitialized;
}

ProbeResult RunReadOnlyProbe() noexcept {
    SharedSrwGuard state_guard(g_state_lock);
    if (g_initialization_state != 2) {
        return ProbeResult::NotInitialized;
    }
    if (!g_verified_build.has_value()) {
        LogLine("read-only probe refused: executable is not an exact known build");
        return ProbeResult::UnknownExecutable;
    }

    LogLine("begin read-only probe snapshot");
    LogKnownAddresses();
    LogProbePatterns();
    LogCameraProbe();
    LogLine("end read-only probe snapshot; no memory was modified");
    return ProbeResult::OkExactBuildProbeOnly;
}

ProbeResult RunReadOnlyCameraSample() noexcept {
    SharedSrwGuard state_guard(g_state_lock);
    if (g_initialization_state != 2) {
        return ProbeResult::NotInitialized;
    }
    if (!g_verified_build.has_value()) {
        return ProbeResult::UnknownExecutable;
    }
    LogCameraProbe();
    return ProbeResult::OkExactBuildProbeOnly;
}

ProbeResult BootstrapProbe(void* reserved) noexcept {
    if (reserved != nullptr) {
        return ProbeResult::InvalidBootstrapArgument;
    }

    ExclusiveSrwGuard bootstrap_guard(g_bootstrap_lock);
    if (g_bootstrap_complete) {
        return g_bootstrap_result;
    }

    ProbeResult initialized = InitializeProbe("explicit-export");
    if (initialized == ProbeResult::AlreadyInitialized) {
        initialized = CurrentInitializationResult();
    }
    if (initialized != ProbeResult::OkExactBuildProbeOnly) {
        LogFormat("bootstrap-failed stage=initialize result=%lu",
                  static_cast<unsigned long>(initialized));
        g_bootstrap_result = initialized;
        g_bootstrap_complete = true;
        return g_bootstrap_result;
    }

    const ProbeResult snapshot = RunReadOnlyProbe();
    if (snapshot != ProbeResult::OkExactBuildProbeOnly) {
        LogFormat("bootstrap-failed stage=snapshot result=%lu",
                  static_cast<unsigned long>(snapshot));
        g_bootstrap_result = snapshot;
        g_bootstrap_complete = true;
        return g_bootstrap_result;
    }

    g_sampler_thread = CreateThread(nullptr, 0, RunReadOnlySampler, nullptr, 0,
                                    nullptr);
    if (g_sampler_thread == nullptr) {
        LogFormat("bootstrap-failed stage=sampler-create win32_error=%lu",
                  static_cast<unsigned long>(GetLastError()));
        g_bootstrap_result = ProbeResult::SamplerStartFailure;
        g_bootstrap_complete = true;
        return g_bootstrap_result;
    }

    if (!LogFormat("bootstrap-complete pid=%lu sampler=started hooks=disabled",
                   static_cast<unsigned long>(GetCurrentProcessId()))) {
        DebugLine("[K2VR] persistent bootstrap completion witness failed.");
        g_bootstrap_result = ProbeResult::PersistentLogFailure;
        g_bootstrap_complete = true;
        return g_bootstrap_result;
    }

    g_bootstrap_result = ProbeResult::OkExactBuildProbeOnly;
    g_bootstrap_complete = true;
    return g_bootstrap_result;
}

void ShutdownProbe() noexcept {
    ExclusiveSrwGuard bootstrap_guard(g_bootstrap_lock);
    {
        ExclusiveSrwGuard state_guard(g_state_lock);
        // A non-ready state makes the bounded sampler stop on its next 500 ms
        // iteration. No thread is terminated and no engine state is touched.
        g_initialization_state = 3;
        g_initialization_result = ProbeResult::NotInitialized;
        g_verified_build.reset();
    }

    if (g_sampler_thread != nullptr) {
        const DWORD wait_result = WaitForSingleObject(g_sampler_thread, 2000);
        if (!SamplerStopWasObserved(wait_result)) {
            const DWORD wait_error =
                wait_result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
            LogFormat(
                "shutdown-incomplete sampler_wait=0x%08lX win32_error=%lu; "
                "bootstrap remains terminal",
                static_cast<unsigned long>(wait_result),
                static_cast<unsigned long>(wait_error));
            {
                ExclusiveSrwGuard state_guard(g_state_lock);
                g_initialization_result = ProbeResult::SamplerStopTimeout;
            }
            g_bootstrap_result = ProbeResult::SamplerStopTimeout;
            g_bootstrap_complete = true;
            // The worker may still execute code and write its terminal log line.
            // Keep both handles alive, retain state 3, and refuse rebootstrap.
            return;
        }
        CloseHandle(g_sampler_thread);
        g_sampler_thread = nullptr;
    }

    {
        ExclusiveSrwGuard log_guard(g_log_lock);
        if (g_log_file != INVALID_HANDLE_VALUE) {
            CloseHandle(g_log_file);
            g_log_file = INVALID_HANDLE_VALUE;
        }
    }
    {
        ExclusiveSrwGuard state_guard(g_state_lock);
        g_initialization_result = ProbeResult::NotInitialized;
        g_initialization_state = 0;
    }
    g_bootstrap_result = ProbeResult::NotInitialized;
    g_bootstrap_complete = false;
}

} // namespace k2vr::game32

std::uint32_t K2VR_GAME32_THREAD_CALL
K2VR_ProbeBootstrap(void* reserved) noexcept {
    return static_cast<std::uint32_t>(
        k2vr::game32::BootstrapProbe(reserved));
}

std::uint32_t K2VR_GAME32_CALL K2VR_ProbeInitialize() noexcept {
    return static_cast<std::uint32_t>(
        k2vr::game32::InitializeProbe("manual-export"));
}

std::uint32_t K2VR_GAME32_CALL K2VR_RunReadOnlyProbe() noexcept {
    return static_cast<std::uint32_t>(k2vr::game32::RunReadOnlyProbe());
}

void K2VR_GAME32_CALL K2VR_ProbeShutdown() noexcept {
    k2vr::game32::ShutdownProbe();
}

std::uint32_t K2VR_GAME32_CALL K2VR_ProtocolVersion() noexcept {
    return (static_cast<std::uint32_t>(k2vr::ipc::kProtocolMajor) << 16U) |
           static_cast<std::uint32_t>(k2vr::ipc::kProtocolMinor);
}

#else

namespace k2vr::game32 {
void SetSelfModule(void*) noexcept {}
ProbeResult RunReadOnlyCameraSample() noexcept {
    return ProbeResult::ProbeUnavailable;
}
bool AppendPersistentProbeLogLine(std::string_view) noexcept {
    return false;
}
} // namespace k2vr::game32

std::uint32_t K2VR_GAME32_THREAD_CALL
K2VR_ProbeBootstrap(void*) noexcept {
    return static_cast<std::uint32_t>(
        k2vr::game32::ProbeResult::UnsupportedArchitecture);
}

std::uint32_t K2VR_GAME32_CALL K2VR_ProbeInitialize() noexcept {
    return static_cast<std::uint32_t>(
        k2vr::game32::ProbeResult::UnsupportedArchitecture);
}

std::uint32_t K2VR_GAME32_CALL K2VR_RunReadOnlyProbe() noexcept {
    return static_cast<std::uint32_t>(
        k2vr::game32::ProbeResult::ProbeUnavailable);
}

void K2VR_GAME32_CALL K2VR_ProbeShutdown() noexcept {}

std::uint32_t K2VR_GAME32_CALL K2VR_ProtocolVersion() noexcept {
    return (static_cast<std::uint32_t>(k2vr::ipc::kProtocolMajor) << 16U) |
           static_cast<std::uint32_t>(k2vr::ipc::kProtocolMinor);
}

#endif
