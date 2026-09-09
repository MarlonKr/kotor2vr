#include "render_trace.hpp"

#include "game_image_capture.hpp"
#include "gpu_stream_producer.hpp"
#include "vr_bridge.hpp"
#include "native_stereo.hpp"
#include "scene_replay.hpp"
#include "../common/vr_input.hpp"
#include "probe.hpp"
#include "../common/build_descriptor.hpp"

#if defined(_WIN32)

#include "gl_gpu_timing.hpp"


#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cstdlib>
#include <limits>
#include <string>

static_assert(sizeof(void*) == 4,
              "the render tracer is an in-process Win32/x86 component");

namespace k2vr::game32 {
namespace {

constexpr DWORD kTraceDurationMs = 600000;
constexpr DWORD kStopWaitMs = 35000;
constexpr DWORD kWriterWaitMs = 5000;
constexpr std::size_t kRingCapacity = 16384;
constexpr std::size_t kRingMask = kRingCapacity - 1;
constexpr std::size_t kMaximumTraceDepth = 64;
static_assert((kRingCapacity & kRingMask) == 0);

enum class TraceState : LONG {
    Idle = 0,
    Installing = 1,
    Active = 2,
    Stopping = 3,
    Stopped = 4,
    Failed = 5,
};

enum class HookKind : std::uint8_t {
    CameraRenderScene = 0,
    SceneRender = 1,
    SceneRenderSinglePass = 2,
};

enum class TracePhase : std::uint8_t {
    Entry = 0,
    Exit = 1,
};

enum class DoublePassEvent : std::uint8_t {
    None = 0,
    Attempt = 1,
    Begin = 2,
    End = 3,
};

struct TraceRecord {
    std::uint64_t qpc{0};
    std::uint64_t duration_qpc{0};
    std::uint32_t call_id{0};
    std::uint32_t parent_call_id{0};
    std::uint32_t thread_id{0};
    std::uint32_t object_address{0};
    std::uint16_t depth{0};
    HookKind hook{HookKind::CameraRenderScene};
    TracePhase phase{TracePhase::Entry};
    DoublePassEvent double_pass_event{DoublePassEvent::None};
    std::uint64_t primary_return{0};
    std::uint64_t extra_return{0};
    std::uint32_t exception_code{0};
    bool extra_succeeded{false};
};

struct RingSlot {
    volatile LONG sequence{0};
    TraceRecord record{};
};

struct ThreadTraceState {
    std::uint32_t depth{0};
    std::array<std::uint32_t, kMaximumTraceDepth> call_stack{};
};

// The exact methods take no stack arguments. Camera +0x08 has a verified
// caller that propagates EAX, and a 64-bit opaque result preserves EDX:EAX as
// well. This avoids inventing a narrower return ABI while the semantic return
// type is still unknown.
using VirtualMethod = std::uint64_t(__thiscall*)(void*);
using WrapperMethod = std::uint64_t(__fastcall*)(void*, void*);

struct HookCandidateRuntime {
    const char* name;
    std::uint32_t cell_preferred_va;
    std::uint32_t target_preferred_va;
    const std::uint8_t* expected_prologue;
    std::size_t expected_prologue_size;
    WrapperMethod wrapper;
    std::uintptr_t runtime_cell{0};
    std::uintptr_t runtime_target{0};
    bool installed{false};
};

alignas(64) std::array<RingSlot, kRingCapacity> g_ring{};
alignas(8) LARGE_INTEGER g_qpc_frequency{};
volatile LONG g_enqueue_position = 0;
LONG g_dequeue_position = 0;
volatile LONG g_next_call_id = 0;
volatile LONG g_dropped_records = 0;
volatile LONG g_written_records = 0;
volatile LONG g_active_wrappers = 0;
volatile LONG g_accepting_records = 0;
volatile LONG g_writer_error = 0;
volatile LONG g_trace_state = static_cast<LONG>(TraceState::Idle);
volatile LONG g_stop_result =
    static_cast<LONG>(RenderTraceResult::AlreadyStopped);
volatile LONG g_stop_reason = 0; // 1 = explicit, 2 = ten-minute timeout.
volatile LONG g_double_pass_enabled = 0;
volatile LONG g_double_pass_claimed = 0;
volatile LONG g_f8_was_down = 0;
volatile LONG g_hmd_camera_enabled = 0;
volatile LONG g_hmd_render_thread = 0;
volatile LONG g_persistent_camera_hooks = 0;

std::uint64_t g_double_pass_armed_qpc = 0;
DWORD g_double_pass_armed_thread_id = 0;
bool g_double_pass_initial_f8_down = false;

HANDLE g_trace_file = INVALID_HANDLE_VALUE;
HANDLE g_writer_thread = nullptr;
HANDLE g_timeout_thread = nullptr;
HANDLE g_writer_ready_event = nullptr;
HANDLE g_writer_stop_event = nullptr;
HANDLE g_stop_request_event = nullptr;
HANDLE g_stop_complete_event = nullptr;

VirtualMethod g_original_camera_render_scene = nullptr;
VirtualMethod g_original_scene_render = nullptr;
VirtualMethod g_original_scene_render_single_pass = nullptr;

thread_local ThreadTraceState g_thread_trace{};
thread_local bool g_hmd_camera_reentry = false;
thread_local bool g_hmd_baseline_valid = false;
thread_local bool g_f11_was_down = false;
thread_local void* g_hmd_baseline_camera = nullptr;
thread_local ipc::PoseF32 g_hmd_baseline{};
thread_local std::uint64_t g_native_camera_frame = 0;
float g_engine_units_per_metre=1.0F;
float g_first_person_forward_m=0.10F;
bool g_first_person_enabled=true;
bool g_recenter_yaw_only=true;
bool g_hide_near_self_mesh=false;
bool g_disable_monitor_vsync=true;
bool g_stereo_request_gate=false;
bool g_stereo_request_cadence=false;
thread_local bool g_camera_toggle_was_down=false;
thread_local bool g_first_person_applied=false;
thread_local std::uintptr_t g_last_behavior_table=UINTPTR_MAX;

void LoadCameraSettings() noexcept {
    // Default OFF even if module/path lookup fails. An explicit game setting
    // must not be overridden by Steam's inherited environment. This loader runs
    // before BootstrapRenderTrace installs the scene hooks.
    gpu_timing::Configure(false);
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&LoadCameraSettings),&module)) return;
    wchar_t path[MAX_PATH]{},value[64]{};
    const DWORD length=GetModuleFileNameW(module,path,MAX_PATH);
    if (!length || length>=MAX_PATH) return;
    auto* slash=std::wcsrchr(path,L'\\');
    if (!slash || slash-path+1+std::wcslen(L"kotor2vr-camera.ini")>=MAX_PATH) return;
    wcscpy_s(slash+1,static_cast<std::size_t>(MAX_PATH-(slash+1-path)),L"kotor2vr-camera.ini");
    GetPrivateProfileStringW(L"camera",L"engine_units_per_metre",L"1.0",value,64,path);
    wchar_t* end{};
    const double parsed=std::wcstod(value,&end);
    if (end!=value && *end==L'\0' && std::isfinite(parsed) && parsed>=0.01 && parsed<=100.0)
        g_engine_units_per_metre=static_cast<float>(parsed);
    g_first_person_enabled=GetPrivateProfileIntW(L"camera",L"first_person",1,path)!=0;
    g_recenter_yaw_only=GetPrivateProfileIntW(L"camera",L"recenter_yaw_only",1,path)!=0;
    g_hide_near_self_mesh=GetPrivateProfileIntW(L"camera",L"experimental_hide_self",0,path)!=0;
    g_disable_monitor_vsync=GetPrivateProfileIntW(L"camera",L"disable_monitor_vsync",1,path)!=0;
    g_stereo_request_gate=GetPrivateProfileIntW(L"camera",L"stereo_request_gate",0,path)!=0;
    g_stereo_request_cadence=GetPrivateProfileIntW(L"camera",L"stereo_request_cadence",0,path)!=0;
    gpu_timing::Configure(GetPrivateProfileIntW(L"camera",L"gl_gpu_timing",0,path)!=0);
    GetPrivateProfileStringW(L"camera",L"first_person_forward_m",L"0.10",value,64,path);
    const double forward=std::wcstod(value,&end);
    if (end!=value && *end==L'\0' && std::isfinite(forward) && forward>=0.0 && forward<=0.5)
        g_first_person_forward_m=static_cast<float>(forward);
}

std::uint64_t __fastcall CameraRenderSceneWrapper(void* self, void*) noexcept;
std::uint64_t __fastcall SceneRenderWrapper(void* self, void*) noexcept;
std::uint64_t __fastcall SceneRenderSinglePassWrapper(void* self,
                                                      void*) noexcept;

constexpr std::array<std::uint8_t, 15> kCameraRenderScenePrologue{{
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x38, 0x03, 0x00,
    0x00, 0x89, 0x8D, 0xEC, 0xFC, 0xFF, 0xFF,
}};
constexpr std::array<std::uint8_t, 14> kSceneRenderPrologue{{
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x89,
    0x4D, 0xEC, 0xE8, 0xE2, 0x82, 0x01, 0x00,
}};
constexpr std::array<std::uint8_t, 15> kSceneRenderSinglePassPrologue{{
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xD4, 0x00, 0x00,
    0x00, 0x89, 0x8D, 0x2C, 0xFF, 0xFF, 0xFF,
}};

std::array<HookCandidateRuntime, 3> g_candidates{{
    {"camera+0x08", 0x0098C464U, 0x0047F320U,
     kCameraRenderScenePrologue.data(), kCameraRenderScenePrologue.size(),
     CameraRenderSceneWrapper},
    {"scene+0x18", 0x0098BB5CU, 0x0046C2B0U,
     kSceneRenderPrologue.data(), kSceneRenderPrologue.size(),
     SceneRenderWrapper},
    {"scene+0xB8", 0x0098BBFCU, 0x0046C5C0U,
     kSceneRenderSinglePassPrologue.data(),
     kSceneRenderSinglePassPrologue.size(), SceneRenderSinglePassWrapper},
}};

[[nodiscard]] LONG AtomicRead(volatile LONG* value) noexcept {
    return InterlockedCompareExchange(value, 0, 0);
}

[[nodiscard]] bool ControlLogFormat(const char* format, ...) noexcept {
    std::array<char, 512> buffer{};
    va_list arguments;
    va_start(arguments, format);
    const int count =
        std::vsnprintf(buffer.data(), buffer.size(), format, arguments);
    va_end(arguments);
    if (count < 0) {
        return false;
    }
    const std::size_t length =
        static_cast<std::size_t>(count) < buffer.size()
            ? static_cast<std::size_t>(count)
            : buffer.size() - 1;
    return AppendPersistentProbeLogLine(
        std::string_view(buffer.data(), length));
}

[[nodiscard]] bool PinTraceModule() noexcept {
    HMODULE pinned = nullptr;
    return GetModuleHandleExW(
               GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                   GET_MODULE_HANDLE_EX_FLAG_PIN,
               reinterpret_cast<LPCWSTR>(&CameraRenderSceneWrapper),
               &pinned) != FALSE &&
           pinned != nullptr;
}

[[nodiscard]] bool OpenTraceFile() noexcept {
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

    const std::wstring path = directory + L"\\render-trace.jsonl";
    g_trace_file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_trace_file != INVALID_HANDLE_VALUE;
}

[[nodiscard]] bool WriteTraceBytes(const char* bytes,
                                   std::size_t size) noexcept {
    if (g_trace_file == INVALID_HANDLE_VALUE || bytes == nullptr || size == 0 ||
        size > (std::numeric_limits<DWORD>::max)()) {
        return false;
    }
    DWORD written = 0;
    return WriteFile(g_trace_file, bytes, static_cast<DWORD>(size), &written,
                     nullptr) != FALSE &&
           written == size;
}

[[nodiscard]] bool WriteTraceFormat(const char* format, ...) noexcept {
    std::array<char, 640> buffer{};
    va_list arguments;
    va_start(arguments, format);
    const int count =
        std::vsnprintf(buffer.data(), buffer.size(), format, arguments);
    va_end(arguments);
    if (count < 0 || static_cast<std::size_t>(count) >= buffer.size()) {
        return false;
    }
    return WriteTraceBytes(buffer.data(), static_cast<std::size_t>(count));
}

[[nodiscard]] std::uint64_t ReadQpc() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) != FALSE
               ? static_cast<std::uint64_t>(value.QuadPart)
               : 0;
}

[[nodiscard]] std::uint64_t TicksToMicroseconds(
    std::uint64_t ticks) noexcept {
    const std::uint64_t frequency =
        static_cast<std::uint64_t>(g_qpc_frequency.QuadPart);
    if (frequency == 0) {
        return 0;
    }
    return (ticks / frequency) * 1000000ULL +
           ((ticks % frequency) * 1000000ULL) / frequency;
}

[[nodiscard]] const char* HookName(HookKind hook) noexcept {
    switch (hook) {
    case HookKind::CameraRenderScene:
        return "camera+0x08";
    case HookKind::SceneRender:
        return "scene+0x18";
    case HookKind::SceneRenderSinglePass:
        return "scene+0xB8";
    }
    return "unknown";
}

[[nodiscard]] bool EnqueueRecord(const TraceRecord& record) noexcept {
    LONG position = AtomicRead(&g_enqueue_position);
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        RingSlot& slot = g_ring[static_cast<std::size_t>(position) & kRingMask];
        const LONG sequence = AtomicRead(&slot.sequence);
        const LONG difference = sequence - position;
        if (difference == 0) {
            const LONG observed = InterlockedCompareExchange(
                &g_enqueue_position, position + 1, position);
            if (observed == position) {
                slot.record = record;
                MemoryBarrier();
                InterlockedExchange(&slot.sequence, position + 1);
                return true;
            }
            position = observed;
            continue;
        }
        if (difference < 0) {
            InterlockedIncrement(&g_dropped_records);
            return false;
        }
        position = AtomicRead(&g_enqueue_position);
    }
    InterlockedIncrement(&g_dropped_records);
    return false;
}

[[nodiscard]] bool DequeueRecord(TraceRecord& output) noexcept {
    RingSlot& slot =
        g_ring[static_cast<std::size_t>(g_dequeue_position) & kRingMask];
    const LONG sequence = AtomicRead(&slot.sequence);
    if (sequence - (g_dequeue_position + 1) != 0) {
        return false;
    }
    output = slot.record;
    MemoryBarrier();
    InterlockedExchange(&slot.sequence,
                        g_dequeue_position +
                            static_cast<LONG>(kRingCapacity));
    ++g_dequeue_position;
    return true;
}

[[nodiscard]] bool WriteRecord(const TraceRecord& record) noexcept {
    if (record.double_pass_event != DoublePassEvent::None) {
        const char* type = "double_pass_unknown";
        switch (record.double_pass_event) {
        case DoublePassEvent::Attempt:
            type = "double_pass_attempt";
            break;
        case DoublePassEvent::Begin:
            type = "double_pass_begin";
            break;
        case DoublePassEvent::End:
            type = "double_pass_end";
            break;
        case DoublePassEvent::None:
            break;
        }

        if (record.double_pass_event != DoublePassEvent::End) {
            return WriteTraceFormat(
                "{\"type\":\"%s\",\"qpc\":%llu,\"this\":\"0x%08lX\","
                "\"tid\":%lu,\"primary_return\":\"0x%016llX\","
                "\"extra_return\":null,\"duration_qpc\":0,"
                "\"duration_us\":0}\n",
                type, static_cast<unsigned long long>(record.qpc),
                static_cast<unsigned long>(record.object_address),
                static_cast<unsigned long>(record.thread_id),
                static_cast<unsigned long long>(record.primary_return));
        }

        if (record.extra_succeeded) {
            return WriteTraceFormat(
                "{\"type\":\"double_pass_end\",\"qpc\":%llu,"
                "\"this\":\"0x%08lX\",\"tid\":%lu,"
                "\"primary_return\":\"0x%016llX\","
                "\"extra_return\":\"0x%016llX\","
                "\"duration_qpc\":%llu,\"duration_us\":%llu,"
                "\"status\":\"ok\"}\n",
                static_cast<unsigned long long>(record.qpc),
                static_cast<unsigned long>(record.object_address),
                static_cast<unsigned long>(record.thread_id),
                static_cast<unsigned long long>(record.primary_return),
                static_cast<unsigned long long>(record.extra_return),
                static_cast<unsigned long long>(record.duration_qpc),
                static_cast<unsigned long long>(
                    TicksToMicroseconds(record.duration_qpc)));
        }
        return WriteTraceFormat(
            "{\"type\":\"double_pass_end\",\"qpc\":%llu,"
            "\"this\":\"0x%08lX\",\"tid\":%lu,"
            "\"primary_return\":\"0x%016llX\","
            "\"extra_return\":null,\"duration_qpc\":%llu,"
            "\"duration_us\":%llu,\"status\":\"seh_exception\","
            "\"exception_code\":\"0x%08lX\"}\n",
            static_cast<unsigned long long>(record.qpc),
            static_cast<unsigned long>(record.object_address),
            static_cast<unsigned long>(record.thread_id),
            static_cast<unsigned long long>(record.primary_return),
            static_cast<unsigned long long>(record.duration_qpc),
            static_cast<unsigned long long>(
                TicksToMicroseconds(record.duration_qpc)),
            static_cast<unsigned long>(record.exception_code));
    }

    const char* phase = record.phase == TracePhase::Entry ? "entry" : "exit";
    return WriteTraceFormat(
        "{\"type\":\"call\",\"hook\":\"%s\",\"phase\":\"%s\","
        "\"call_id\":%lu,\"parent\":%lu,\"depth\":%u,\"tid\":%lu,"
        "\"qpc\":%llu,\"this\":\"0x%08lX\",\"duration_qpc\":%llu,"
        "\"duration_us\":%llu}\n",
        HookName(record.hook), phase,
        static_cast<unsigned long>(record.call_id),
        static_cast<unsigned long>(record.parent_call_id),
        static_cast<unsigned>(record.depth),
        static_cast<unsigned long>(record.thread_id),
        static_cast<unsigned long long>(record.qpc),
        static_cast<unsigned long>(record.object_address),
        static_cast<unsigned long long>(record.duration_qpc),
        static_cast<unsigned long long>(
            TicksToMicroseconds(record.duration_qpc)));
}

DWORD WINAPI WriterThread(LPVOID) noexcept {
    const bool double_pass_enabled = AtomicRead(&g_double_pass_enabled) != 0;
    bool header_written = WriteTraceFormat(
        "{\"type\":\"session\",\"pid\":%lu,\"qpc_frequency\":%lld,"
        "\"hooks\":3,\"duration_ms\":%lu,\"double_pass\":%s}\n",
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<long long>(g_qpc_frequency.QuadPart),
        static_cast<unsigned long>(kTraceDurationMs),
        double_pass_enabled ? "true" : "false");
    if (header_written && double_pass_enabled) {
        header_written = WriteTraceFormat(
            "{\"type\":\"double_pass_armed\",\"qpc\":%llu,"
            "\"this\":null,\"tid\":%lu,\"primary_return\":null,"
            "\"extra_return\":null,\"duration_qpc\":0,"
            "\"duration_us\":0,\"key\":\"F8\",\"initial_down\":%s}\n",
            static_cast<unsigned long long>(g_double_pass_armed_qpc),
            static_cast<unsigned long>(g_double_pass_armed_thread_id),
            g_double_pass_initial_f8_down ? "true" : "false");
    }
    if (!header_written) {
        InterlockedExchange(&g_writer_error, 1);
    }
    SetEvent(g_writer_ready_event);
    if (!header_written) {
        return 1;
    }

    for (;;) {
        bool wrote_any = false;
        TraceRecord record{};
        while (DequeueRecord(record)) {
            if (!WriteRecord(record)) {
                InterlockedExchange(&g_writer_error, 1);
            } else {
                InterlockedIncrement(&g_written_records);
            }
            wrote_any = true;
        }

        if (WaitForSingleObject(g_writer_stop_event, wrote_any ? 0 : 10) ==
            WAIT_OBJECT_0) {
            if (!DequeueRecord(record)) {
                break;
            }
            if (!WriteRecord(record)) {
                InterlockedExchange(&g_writer_error, 1);
            } else {
                InterlockedIncrement(&g_written_records);
            }
        }
    }

    (void)WriteTraceFormat(
        "{\"type\":\"summary\",\"written\":%lu,\"drops\":%lu,"
        "\"reason\":\"%s\"}\n",
        static_cast<unsigned long>(AtomicRead(&g_written_records)),
        static_cast<unsigned long>(AtomicRead(&g_dropped_records)),
        AtomicRead(&g_stop_reason) == 2 ? "timeout" : "explicit");
    FlushFileBuffers(g_trace_file);
    return AtomicRead(&g_writer_error) == 0 ? 0U : 1U;
}

struct TraceScope final {
    HookKind hook;
    void* object;
    std::uint64_t started_qpc{0};
    std::uint32_t call_id{0};
    std::uint32_t parent_call_id{0};
    std::uint16_t depth{0};
    bool recorded{false};

    TraceScope(HookKind hook_value, void* object_value) noexcept
        : hook(hook_value), object(object_value) {
        InterlockedIncrement(&g_active_wrappers);
        ThreadTraceState& thread = g_thread_trace;
        recorded = AtomicRead(&g_accepting_records) != 0 || thread.depth != 0;
        if (!recorded) {
            return;
        }

        const LONG next = InterlockedIncrement(&g_next_call_id);
        call_id = static_cast<std::uint32_t>(next);
        const std::uint32_t current_depth = thread.depth;
        depth = static_cast<std::uint16_t>(
            current_depth <= (std::numeric_limits<std::uint16_t>::max)()
                ? current_depth
                : (std::numeric_limits<std::uint16_t>::max)());
        if (current_depth != 0 && current_depth <= kMaximumTraceDepth) {
            parent_call_id = thread.call_stack[current_depth - 1];
        }
        if (current_depth < kMaximumTraceDepth) {
            thread.call_stack[current_depth] = call_id;
        }
        ++thread.depth;
        started_qpc = ReadQpc();
        (void)EnqueueRecord({started_qpc,
                       0,
                       call_id,
                       parent_call_id,
                       GetCurrentThreadId(),
                       static_cast<std::uint32_t>(
                           reinterpret_cast<std::uintptr_t>(object)),
                       depth,
                       hook,
                       TracePhase::Entry});
    }

    ~TraceScope() {
        if (recorded) {
            const std::uint64_t ended_qpc = ReadQpc();
            const std::uint64_t duration =
                ended_qpc >= started_qpc ? ended_qpc - started_qpc : 0;
            (void)EnqueueRecord({ended_qpc,
                           duration,
                           call_id,
                           parent_call_id,
                           GetCurrentThreadId(),
                           static_cast<std::uint32_t>(
                               reinterpret_cast<std::uintptr_t>(object)),
                           depth,
                           hook,
                           TracePhase::Exit});
            if (g_thread_trace.depth != 0) {
                --g_thread_trace.depth;
            }
        }
        InterlockedDecrement(&g_active_wrappers);
    }

    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;
};

[[nodiscard]] bool EnqueueDoublePassEvent(
    DoublePassEvent event, void* self, std::uint64_t qpc,
    std::uint64_t primary_return, std::uint64_t extra_return = 0,
    std::uint64_t duration_qpc = 0, bool extra_succeeded = false,
    DWORD exception_code = 0) noexcept {
    TraceRecord record{};
    record.qpc = qpc;
    record.duration_qpc = duration_qpc;
    record.thread_id = GetCurrentThreadId();
    record.object_address = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(self));
    record.double_pass_event = event;
    record.primary_return = primary_return;
    record.extra_return = extra_return;
    record.exception_code = exception_code;
    record.extra_succeeded = extra_succeeded;
    return EnqueueRecord(record);
}

[[nodiscard]] bool ClaimDoublePassOnF8PressEdge() noexcept {
    const bool enabled = AtomicRead(&g_double_pass_enabled) != 0 &&
                         AtomicRead(&g_accepting_records) != 0;
    if (!enabled) {
        return false;
    }

    const bool is_down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    const bool was_down =
        InterlockedExchange(&g_f8_was_down, is_down ? 1 : 0) != 0;
    const bool already_claimed = AtomicRead(&g_double_pass_claimed) != 0;
    const DoublePassInputTransition transition = EvaluateDoublePassInput(
        enabled, already_claimed, was_down, is_down);
    if (!transition.should_attempt) {
        return false;
    }

    return InterlockedCompareExchange(&g_double_pass_claimed, 1, 0) == 0;
}

struct ExtraPassOutcome {
    std::uint64_t result{0};
    DWORD exception_code{0};
    bool succeeded{false};
};

// Keep SEH isolated from the wrapper's TraceScope (which requires C++ object
// unwinding and therefore cannot share a function with __try under MSVC x86).
[[nodiscard]] ExtraPassOutcome InvokeExtraPassSeh(void* self) noexcept {
    ExtraPassOutcome outcome{};
    __try {
        outcome.result = g_original_scene_render_single_pass(self);
        outcome.succeeded = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outcome.exception_code = GetExceptionCode();
    }
    return outcome;
}

[[nodiscard]] bool ReadCameraPoseSeh(
    void* self, EngineCameraPoseWxyz& pose) noexcept {
    if (self == nullptr) return false;
    __try {
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto* app=*reinterpret_cast<unsigned char* const*>(base+0x00A1B4A4U-0x00400000U);
        const auto* facade=*reinterpret_cast<unsigned char* const*>(app+4);
        const auto* internal=*reinterpret_cast<unsigned char* const*>(facade+4);
        const auto* module=*reinterpret_cast<unsigned char* const*>(internal+0x18);
        if (*reinterpret_cast<void* const*>(module+0x40) != self) return false;
        const auto* bytes = static_cast<const unsigned char*>(self);
        if (*reinterpret_cast<void* const*>(bytes + 0x98) == nullptr)
            return false;
        std::memcpy(&pose, bytes + 0xA8, sizeof(pose));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// All temporary engine writes and TLS routing are undone even if the original
// engine method raises an SEH exception. Keep this free of C++ destructors.
[[nodiscard]] ExtraPassOutcome InvokeHmdCameraSeh(
    void* self, const EngineCameraPoseWxyz& authored,
    const EngineCameraPoseWxyz& hmd) noexcept {
    ExtraPassOutcome outcome{};
    const GameImageCapturePolicy previous = GetGameImageCapturePolicy();
    __try {
        __try {
            g_hmd_camera_reentry = true;
            std::memcpy(static_cast<unsigned char*>(self) + 0xA8,
                        &hmd, sizeof(hmd));
            (void)SetGameImageCapturePolicy(GameImageCapturePolicy::VrCapture);
            outcome.result = g_original_camera_render_scene(self);
            outcome.succeeded = true;
        } __finally {
            std::memcpy(static_cast<unsigned char*>(self) + 0xA8,
                        &authored, sizeof(authored));
            (void)SetGameImageCapturePolicy(previous);
            g_hmd_camera_reentry = false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outcome.exception_code = GetExceptionCode();
    }
    return outcome;
}

enum class FirstPersonRead : unsigned { Disabled, NonGameplay, MissingTarget, UnsupportedAbi,
    InvalidFeet, MissingOrInvalidHook, ReadFault, Ready, InvalidAnchor };
struct FirstPersonSample {
    std::uintptr_t target{};
    math::Vec3 feet{};
    FirstPersonEye eye{};
    EgoMeshSelection visibility{};
    FirstPersonRead status{FirstPersonRead::NonGameplay};
};
thread_local std::uintptr_t g_last_first_person_target=UINTPTR_MAX;
thread_local FirstPersonHook g_last_first_person_hook=FirstPersonHook::None;
thread_local FirstPersonRead g_last_first_person_status=FirstPersonRead::Disabled;

[[nodiscard]] FirstPersonSample ReadFirstPersonTargetSeh(void* self) noexcept {
    FirstPersonSample sample{};
    __try {
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto* behavior=*reinterpret_cast<unsigned char* const*>(static_cast<unsigned char*>(self)+0x1B8);
        const auto table=behavior ? *reinterpret_cast<const std::uintptr_t*>(behavior):0;
        if (table!=g_last_behavior_table) {
            char line[160]{};
            std::snprintf(line,sizeof(line),"camera-behavior preferred_vtable=0x%08llX on_a_stick=%u",
                static_cast<unsigned long long>(table ? table-base+0x00400000U:0),
                table==base+0x009A1F70U-0x00400000U ? 1U:0U);
            (void)AppendPersistentProbeLogLine(line); g_last_behavior_table=table;
        }
        // Exact CSWCameraOnAStick and its CAurCamera owner. Authored cameras
        // remain authoritative during dialog, death, free-look and unknown modes.
        if (!behavior || table!=base+0x009A1F70U-0x00400000U ||
            *reinterpret_cast<void* const*>(behavior+0x1C)!=static_cast<unsigned char*>(self)+4) return sample;
        auto* object=*reinterpret_cast<void* const*>(behavior+0x14);
        sample.target=reinterpret_cast<std::uintptr_t>(object);
        sample.status=FirstPersonRead::MissingTarget;
        if (!object) return sample;
        const auto* vtable=*reinterpret_cast<const std::uintptr_t* const*>(object);
        const auto position_address=vtable[0x64/4], hook_address=vtable[0x98/4];
        sample.status=FirstPersonRead::UnsupportedAbi;
        // Exact Gob implementations, confirmed against the supported executable.
        const unsigned char position_prefix[]={0x55,0x8b,0xec,0x51,0x89,0x4d,0xfc};
        const unsigned char hook_prefix[]={0x55,0x8b,0xec,0x83,0xec,0x24,0x89,0x4d,0xdc};
        if (position_address!=base+0x00059710U || hook_address!=base+0x00062B90U ||
            std::memcmp(reinterpret_cast<const void*>(position_address),position_prefix,sizeof(position_prefix))!=0 ||
            std::memcmp(reinterpret_cast<const void*>(hook_address),hook_prefix,sizeof(hook_prefix))!=0) return sample;
        using GetPosition=math::Vec3*(__thiscall*)(void*,math::Vec3*);
        using GetHook=int(__thiscall*)(void*,const char*,math::Vec3*,void*);
        sample.status=FirstPersonRead::InvalidFeet;
        if (reinterpret_cast<GetPosition>(position_address)(object,&sample.feet)!=&sample.feet ||
            !math::IsFinite(sample.feet)) return sample;
        const float nan=std::numeric_limits<float>::quiet_NaN();
        math::Vec3 free_look{nan,nan,nan}, camera{nan,nan,nan};
        const auto get_hook=reinterpret_cast<GetHook>(hook_address);
        // Same ordered lookup as CSWCameraFreeLook::Control, 0x7E2B16..0x7E2BC2.
        // GetHook allows a null quaternion output; its position is already world-space.
        const bool free_found=get_hook(object,"FreeLookHook",&free_look,nullptr)!=0;
        sample.eye=SelectFirstPersonEye(sample.feet,free_found,free_look,false,camera);
        if (sample.eye.hook==FirstPersonHook::None) {
            const bool camera_found=get_hook(object,"CameraHook",&camera,nullptr)!=0;
            sample.eye=SelectFirstPersonEye(sample.feet,free_found,free_look,camera_found,camera);
        }
        sample.status=sample.eye.hook==FirstPersonHook::None ?
            FirstPersonRead::MissingOrInvalidHook:FirstPersonRead::Ready;
        sample.visibility.feet=sample.feet;
        sample.visibility.short_model=sample.status==FirstPersonRead::Ready &&
            ShortEgoModel(sample.feet,sample.eye.world);
        return sample;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        sample.eye={}; sample.status=FirstPersonRead::ReadFault; return sample;
    }
}

[[nodiscard]] bool InvokeStereoCameraSeh(void* self,
    const EngineCameraPoseWxyz& authored, const EngineCameraPoseWxyz& vr_anchor,
    const ipc::RenderRequest& request,const FirstPersonSample& sample) noexcept {
    EngineCameraPoseWxyz eyes[2]{};
    for (std::size_t i=0;i<2;++i) {
        const auto composed=ComposeStereoEyePose(vr_anchor,g_hmd_baseline,request,i,g_engine_units_per_metre);
        if (!composed.valid) return false;
        eyes[i]=composed.pose;
    }
    unsigned char original_projection[36]{}; // +204..+227; includes viewport and clips
    ConfigureNativeStereoEgoVisibility(reinterpret_cast<const void*>(sample.target),sample.visibility,
        eyes[0],g_engine_units_per_metre,
        g_hide_near_self_mesh && request.presentation_state==ipc::PresentationState::WorldFirstPerson);
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* framebuffer_effects=reinterpret_cast<std::int32_t*>(base+0x009F4E74U-0x00400000U);
    auto* soft_shadows=reinterpret_cast<std::int32_t*>(base+0x009F5DA8U-0x00400000U);
    std::int32_t original_framebuffer_effects{},original_soft_shadows{};
    bool saved=false, success=false;
    __try {
        __try {
            std::memcpy(original_projection,static_cast<unsigned char*>(self)+0x204,36);
            original_framebuffer_effects=*framebuffer_effects;
            original_soft_shadows=*soft_shadows;
            saved=true; g_hmd_camera_reentry=true;
            if (BeginSceneReplayFrame() && BeginNativeStereoPair(request,++g_native_camera_frame,g_disable_monitor_vsync,eyes,self,g_stereo_request_gate,g_engine_units_per_metre,g_stereo_request_cadence)) {
                // Exact Frame Buffer setting: INI reader 0x7BA9E1 calls
                // 0x7BC330; 0x478D20/30 set this render flag. The effect at
                // 0x4373B8 is gated by it and copies 3440x1440 desktop pixels
                // into legacy screen textures (0x437532..0x43754F). Those
                // textures are not per-eye; use the engine's ordinary path
                // for native eyes, restoring the setting before the monitor.
                *framebuffer_effects=0;
                // Soft Shadows is a separate screen-space composite, not
                // gated by Frame Buffer. INI 0x7BA4F1 -> 0x7BC260 ->
                // 0x478CF0/0x478D10 controls this flag. 0x4480E0 copies the
                // desktop-sized scene into shared rectangle textures before
                // a 512x512 blur. Keep ordinary stencil shadows in the eyes.
                *soft_shadows=0;
                // SceneRender runs game/animation code once, with its ordinary
                // delta. The right eye and monitor replay its GL commands.
                success=true;
                for (std::size_t i=0;i<2;++i) {
                    auto* bytes=static_cast<unsigned char*>(self);
                    std::memcpy(bytes+0xA8,&eyes[i],sizeof(eyes[i]));
                    *reinterpret_cast<float*>(bytes+0x204)=ipc::StereoCullingFovDegrees(request);
                    const std::int32_t viewport[4]={0,0,static_cast<std::int32_t>(request.render_width),
                                                       static_cast<std::int32_t>(request.render_height)};
                    std::memcpy(bytes+0x218,viewport,sizeof(viewport));
                    if (!BeginNativeStereoEye(i)) { success=false; break; }
                    (void)g_original_camera_render_scene(self);
                    CaptureNativeStereoEye();
                    EndNativeStereoEye();
                }
            }
        } __finally {
            EndNativeStereoEye();
            success=EndNativeStereoPair(success && !AbnormalTermination());
            if (saved) {
                *framebuffer_effects=original_framebuffer_effects;
                *soft_shadows=original_soft_shadows;
                std::memcpy(static_cast<unsigned char*>(self)+0x204,original_projection,36);
                std::memcpy(static_cast<unsigned char*>(self)+0xA8,&authored,sizeof(authored));
            }
            g_hmd_camera_reentry=false;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_hmd_camera_enabled,0);
        success=false;
    }
    return success;
}

[[nodiscard]] std::uint64_t InvokeMonitorReplaySeh(void* self,bool mirror) noexcept {
    std::uint64_t result{};
    __try {
        SetSceneReplayFinalPass(mirror ? &TryNativeStereoMonitorMirror:nullptr);
        result=g_original_camera_render_scene(self);
    } __finally {
        FinishSceneReplayFrame();
    }
    return result;
}

std::uint64_t __fastcall CameraRenderSceneWrapper(void* self,
                                                  void*) noexcept {
    TraceScope trace(HookKind::CameraRenderScene, self);
    if (!g_hmd_camera_reentry && (AtomicRead(&g_hmd_camera_enabled)==0 || !IsGpuStreamProducerRunning()))
        RestoreNativeStereoPacing();
    if (g_hmd_camera_reentry || NativeStereoHudActive() || GetGameImageCapturePolicy()==GameImageCapturePolicy::Suppressed ||
        AtomicRead(&g_hmd_camera_enabled) == 0 ||
        !IsGpuStreamProducerRunning()) {
        return g_original_camera_render_scene(self);
    }
    const LONG thread = static_cast<LONG>(GetCurrentThreadId());
    const LONG owner = InterlockedCompareExchange(&g_hmd_render_thread, thread, 0);
    if (owner != 0 && owner != thread) return g_original_camera_render_scene(self);

    ipc::RenderRequest request{};
    EngineCameraPoseWxyz authored{};
    if (!TryReadLatestRenderRequest(request)) {
        g_hmd_baseline_valid = false;
        RestoreNativeStereoPacing();
        return g_original_camera_render_scene(self);
    }
    if (!ReadCameraPoseSeh(self,authored)) return g_original_camera_render_scene(self);
    const ipc::PoseF32 current = StereoHeadCenter(request);
    if (!IsConservativeUnitQuaternion(OpenXrQuaternion(current)))
        return g_original_camera_render_scene(self);
    const bool recenter_down = input::RecenterDown();
    if (!g_hmd_baseline_valid || g_hmd_baseline_camera != self ||
        (request.history_reset_reasons & static_cast<std::uint32_t>(ipc::ResetReason::RuntimeRestart)) != 0 ||
        (request.history_reset_reasons & static_cast<std::uint32_t>(ipc::ResetReason::Recenter)) != 0 ||
        (recenter_down && !g_f11_was_down)) {
        g_hmd_baseline = g_recenter_yaw_only ? UprightRecenterPose(current,g_hmd_baseline) : current;
        g_hmd_baseline_valid = true;
        g_hmd_baseline_camera = self;
        request.history_reset_reasons |= static_cast<std::uint32_t>(ipc::ResetReason::Recenter);
    }
    g_f11_was_down = recenter_down;
    if (request.presentation_state == ipc::PresentationState::WorldThirdPerson &&
        ipc::ValidStereoRequest(request)) {
        const bool toggle=input::CameraToggleDown();
        if (toggle && !g_camera_toggle_was_down) g_first_person_enabled=!g_first_person_enabled;
        g_camera_toggle_was_down=toggle;
        EngineCameraPoseWxyz vr_anchor=authored;
        FirstPersonSample sample{};
        sample.status=FirstPersonRead::Disabled;
        bool first_person=false;
        if (g_first_person_enabled) {
            sample=ReadFirstPersonTargetSeh(self);
            if (sample.status==FirstPersonRead::Ready) {
                const auto anchor=ComposeFirstPersonAnchor(authored,sample.eye.world,
                    g_engine_units_per_metre,g_first_person_forward_m);
                if (anchor.valid) { vr_anchor=anchor.pose; first_person=true; }
                else sample.status=FirstPersonRead::InvalidAnchor;
            }
        }
        if (first_person!=g_first_person_applied || sample.target!=g_last_first_person_target ||
            sample.eye.hook!=g_last_first_person_hook) {
            request.history_reset_reasons |= static_cast<std::uint32_t>(ipc::ResetReason::CameraModeChange);
        }
        if (first_person!=g_first_person_applied || sample.target!=g_last_first_person_target ||
            sample.eye.hook!=g_last_first_person_hook || sample.status!=g_last_first_person_status) {
            const char* reasons[]={"disabled","non-gameplay","missing-target","unsupported-abi",
                "invalid-feet","missing-or-invalid-hook","read-fault","ready","invalid-anchor"};
            const char* hooks[]={"none","FreeLookHook","CameraHook"};
            char line[256]{};
            std::snprintf(line,sizeof(line),"camera-mode %s target=0x%08llX hook=%s reason=%s height_engine=%.3f",
                first_person ? "first-person":"authored-camera",
                static_cast<unsigned long long>(sample.target),hooks[static_cast<unsigned>(sample.eye.hook)],
                reasons[static_cast<unsigned>(sample.status)],first_person ? sample.eye.world.z-sample.feet.z:0.0F);
            (void)AppendPersistentProbeLogLine(line);
        }
        g_first_person_applied=first_person;
        g_last_first_person_target=sample.target;
        g_last_first_person_hook=sample.eye.hook;
        g_last_first_person_status=sample.status;
        if (first_person) request.presentation_state=ipc::PresentationState::WorldFirstPerson;
        const bool stereo_ready=InvokeStereoCameraSeh(self,authored,vr_anchor,request,sample);
        ScopedGameImageCapturePolicy monitor(GameImageCapturePolicy::Suppressed);
        const auto result=InvokeMonitorReplaySeh(self,stereo_ready && NativeStereoMonitorMirrorEnabled());
        BeginNativeStereoHud();
        return result;
    }
    const HmdCameraPoseResult composed =
        ComposeHmdCameraPose(authored, g_hmd_baseline, current);
    if (!composed.valid) return g_original_camera_render_scene(self);

    const ExtraPassOutcome extra = InvokeHmdCameraSeh(self, authored, composed.pose);
    if (!extra.succeeded) {
        InterlockedExchange(&g_hmd_camera_enabled, 0);
        (void)EnqueueDoublePassEvent(DoublePassEvent::End, self, ReadQpc(),
            0, extra.result, 0, false, extra.exception_code);
    }
    // Rebuild the authored camera's culling planes and derived state, preserve
    // its opaque EDX:EAX return, and leave the normal monitor camera intact.
    ScopedGameImageCapturePolicy monitor(GameImageCapturePolicy::Suppressed);
    return g_original_camera_render_scene(self);
}

std::uint64_t __fastcall SceneRenderWrapper(void* self, void*) noexcept {
    TraceScope trace(HookKind::SceneRender, self);
    TraceNativeStereoSceneMatrices();
    return RenderSceneWithReplay(self,g_original_scene_render);
}

std::uint64_t __fastcall SceneRenderSinglePassWrapper(void* self,
                                                      void*) noexcept {
    TraceScope trace(HookKind::SceneRenderSinglePass, self);
    const std::uint64_t primary_return =
        g_original_scene_render_single_pass(self);
    GameImageCaptureAfterScenePass();
    if (!ClaimDoublePassOnF8PressEdge()) {
        return primary_return;
    }

    const std::uint64_t attempt_qpc = ReadQpc();
    (void)EnqueueDoublePassEvent(DoublePassEvent::Attempt, self, attempt_qpc,
                                 primary_return);
    const std::uint64_t begin_qpc = ReadQpc();
    (void)EnqueueDoublePassEvent(DoublePassEvent::Begin, self, begin_qpc,
                                 primary_return);

    // Call the captured original address directly. Calling through the VTable
    // would recurse through this wrapper and could create an unbounded pass.
    const ExtraPassOutcome extra = InvokeExtraPassSeh(self);
    const std::uint64_t end_qpc = ReadQpc();
    const std::uint64_t duration_qpc =
        end_qpc >= begin_qpc ? end_qpc - begin_qpc : 0;
    (void)EnqueueDoublePassEvent(
        DoublePassEvent::End, self, end_qpc, primary_return, extra.result,
        duration_qpc, extra.succeeded, extra.exception_code);

    // The diagnostic result must never replace the engine's primary result.
    return primary_return;
}

[[nodiscard]] bool IsExecutableProtection(DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    switch (protection & 0xFFU) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool ReadCurrentPointer(std::uintptr_t address,
                                      std::uintptr_t& output) noexcept {
    std::uint32_t value = 0;
    SIZE_T read = 0;
    if (ReadProcessMemory(GetCurrentProcess(),
                          reinterpret_cast<const void*>(address), &value,
                          sizeof(value), &read) == FALSE ||
        read != sizeof(value)) {
        return false;
    }
    output = value;
    return true;
}

[[nodiscard]] bool ValidateCandidates(
    const builds::VerifiedBuild& verified) noexcept {
    const builds::ExactBuildDescriptor& build = verified.descriptor();
    const std::uintptr_t runtime_base = verified.runtime_image_base();
    const std::uint32_t image_size = verified.runtime_image_size();

    for (HookCandidateRuntime& candidate : g_candidates) {
        if (candidate.cell_preferred_va < build.preferred_image_base ||
            candidate.target_preferred_va < build.preferred_image_base) {
            return false;
        }
        const std::uint32_t cell_rva =
            candidate.cell_preferred_va - build.preferred_image_base;
        const std::uint32_t target_rva =
            candidate.target_preferred_va - build.preferred_image_base;
        if (cell_rva > image_size || sizeof(void*) > image_size - cell_rva ||
            target_rva > image_size ||
            candidate.expected_prologue_size > image_size - target_rva ||
            runtime_base >
                (std::numeric_limits<std::uintptr_t>::max)() - cell_rva ||
            runtime_base >
                (std::numeric_limits<std::uintptr_t>::max)() - target_rva) {
            return false;
        }

        candidate.runtime_cell = runtime_base + cell_rva;
        candidate.runtime_target = runtime_base + target_rva;
        std::uintptr_t current = 0;
        if (!ReadCurrentPointer(candidate.runtime_cell, current) ||
            DecideInstallCell(current, candidate.runtime_target,
                              reinterpret_cast<std::uintptr_t>(
                                  candidate.wrapper)) !=
                InstallCellDecision::SwapExpected) {
            (void)ControlLogFormat(
                "render-trace-validation-failed candidate=%s cell=0x%08lX "
                "current=0x%08lX expected=0x%08lX",
                candidate.name,
                static_cast<unsigned long>(candidate.runtime_cell),
                static_cast<unsigned long>(current),
                static_cast<unsigned long>(candidate.runtime_target));
            return false;
        }

        std::array<std::uint8_t, 16> observed{};
        SIZE_T read = 0;
        if (candidate.expected_prologue_size > observed.size() ||
            ReadProcessMemory(
                GetCurrentProcess(),
                reinterpret_cast<const void*>(candidate.runtime_target),
                observed.data(), candidate.expected_prologue_size, &read) ==
                FALSE ||
            read != candidate.expected_prologue_size ||
            std::memcmp(observed.data(), candidate.expected_prologue,
                        candidate.expected_prologue_size) != 0) {
            (void)ControlLogFormat(
                "render-trace-validation-failed candidate=%s prologue=mismatch",
                candidate.name);
            return false;
        }

        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<const void*>(candidate.runtime_target),
                         &memory, sizeof(memory)) != sizeof(memory) ||
            memory.State != MEM_COMMIT ||
            !IsExecutableProtection(memory.Protect)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool MutateCell(HookCandidateRuntime& candidate,
                              bool install) noexcept {
    const std::uintptr_t original = candidate.runtime_target;
    const std::uintptr_t wrapper =
        reinterpret_cast<std::uintptr_t>(candidate.wrapper);
    std::uintptr_t current = 0;
    if (!ReadCurrentPointer(candidate.runtime_cell, current)) {
        return false;
    }

    if (install) {
        if (DecideInstallCell(current, original, wrapper) !=
            InstallCellDecision::SwapExpected) {
            return false;
        }
    } else {
        const RestoreCellDecision decision =
            DecideRestoreCell(current, original, wrapper);
        if (decision == RestoreCellDecision::AlreadyOriginal) {
            candidate.installed = false;
            return true;
        }
        if (decision == RestoreCellDecision::PreserveForeign) {
            (void)ControlLogFormat(
                "render-trace-restore-preserved-foreign candidate=%s "
                "current=0x%08lX",
                candidate.name, static_cast<unsigned long>(current));
            candidate.installed = false;
            return true;
        }
    }

    DWORD old_protection = 0;
    if (VirtualProtect(reinterpret_cast<void*>(candidate.runtime_cell),
                       sizeof(void*), PAGE_READWRITE,
                       &old_protection) == FALSE) {
        return false;
    }

    void* const replacement = reinterpret_cast<void*>(install ? wrapper : original);
    void* const expected = reinterpret_cast<void*>(install ? original : wrapper);
    void* const previous = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(candidate.runtime_cell), replacement,
        expected);
    DWORD ignored = 0;
    const bool protection_restored =
        VirtualProtect(reinterpret_cast<void*>(candidate.runtime_cell),
                       sizeof(void*), old_protection, &ignored) != FALSE;
    const bool exchanged = previous == expected;
    if (install && exchanged) {
        candidate.installed = true;
    } else if (!install && exchanged) {
        candidate.installed = false;
    }
    return exchanged && protection_restored;
}

[[nodiscard]] bool RestoreInstalledHooks() noexcept {
    bool complete = true;
    for (auto iterator = g_candidates.rbegin();
         iterator != g_candidates.rend(); ++iterator) {
        if (iterator->installed && !MutateCell(*iterator, false)) {
            complete = false;
        }
    }
    return complete;
}

void InitializeQueue() noexcept {
    for (std::size_t index = 0; index < g_ring.size(); ++index) {
        g_ring[index].sequence = static_cast<LONG>(index);
    }
    g_enqueue_position = 0;
    g_dequeue_position = 0;
    g_next_call_id = 0;
    g_dropped_records = 0;
    g_written_records = 0;
    g_active_wrappers = 0;
    g_accepting_records = 0;
    g_writer_error = 0;
    g_double_pass_claimed = 0;
}

void StopWriterAfterFailedInstall() noexcept {
    InterlockedExchange(&g_accepting_records, 0);
    (void)RestoreInstalledHooks();
    for (unsigned attempt = 0;
         attempt < 3000 && AtomicRead(&g_active_wrappers) != 0; ++attempt) {
        Sleep(1);
    }
    if (g_writer_stop_event != nullptr) {
        SetEvent(g_writer_stop_event);
    }
    if (g_writer_thread != nullptr) {
        WaitForSingleObject(g_writer_thread, kWriterWaitMs);
    }
}

DWORD WINAPI TimeoutAndStopThread(LPVOID) noexcept {
    DWORD wait = WaitForSingleObject(g_stop_request_event,
                                           kTraceDurationMs);
    if (wait == WAIT_TIMEOUT && AtomicRead(&g_persistent_camera_hooks) != 0) {
        // Bound the diagnostic file, not the VR session. Hooks remain installed
        // until explicitly stopped; their active-wrapper accounting stays live.
        InterlockedExchange(&g_accepting_records, 0);
        wait=WaitForSingleObject(g_stop_request_event, INFINITE);
    }
    InterlockedExchange(&g_hmd_camera_enabled, 0);
    InterlockedExchange(&g_stop_reason,
                        wait == WAIT_TIMEOUT ? 2 : 1);
    InterlockedExchange(&g_trace_state,
                        static_cast<LONG>(TraceState::Stopping));
    InterlockedExchange(&g_accepting_records, 0);

    const bool restored = RestoreInstalledHooks();
    DWORD active_wait_ms = 0;
    while (AtomicRead(&g_active_wrappers) != 0 &&
           active_wait_ms < 30000) {
        Sleep(1);
        ++active_wait_ms;
    }

    RenderTraceResult result = RenderTraceResult::Ok;
    if (!restored || AtomicRead(&g_active_wrappers) != 0) {
        result = RenderTraceResult::StopTimeout;
    } else {
        SetEvent(g_writer_stop_event);
        if (WaitForSingleObject(g_writer_thread, kWriterWaitMs) !=
            WAIT_OBJECT_0) {
            result = RenderTraceResult::StopTimeout;
        }
    }

    (void)ControlLogFormat(
        "render-trace-stop-complete pid=%lu reason=%s result=%lu drops=%lu",
        static_cast<unsigned long>(GetCurrentProcessId()),
        AtomicRead(&g_stop_reason) == 2 ? "timeout" : "explicit",
        static_cast<unsigned long>(result),
        static_cast<unsigned long>(AtomicRead(&g_dropped_records)));
    InterlockedExchange(&g_stop_result, static_cast<LONG>(result));
    InterlockedExchange(
        &g_trace_state,
        static_cast<LONG>(result == RenderTraceResult::Ok ? TraceState::Stopped
                                                          : TraceState::Failed));
    SetEvent(g_stop_complete_event);
    return static_cast<DWORD>(result);
}

[[nodiscard]] bool CreateTraceSynchronization() noexcept {
    g_writer_ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_writer_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_stop_request_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_stop_complete_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return g_writer_ready_event != nullptr && g_writer_stop_event != nullptr &&
           g_stop_request_event != nullptr && g_stop_complete_event != nullptr;
}

[[nodiscard]] RenderTraceResult BootstrapRenderTrace(
    void* reserved, bool enable_double_pass) noexcept {
    if (reserved != nullptr) {
        return RenderTraceResult::InvalidArgument;
    }

    const LONG observed = InterlockedCompareExchange(
        &g_trace_state, static_cast<LONG>(TraceState::Installing),
        static_cast<LONG>(TraceState::Idle));
    if (observed == static_cast<LONG>(TraceState::Active)) {
        return (AtomicRead(&g_double_pass_enabled) != 0) == enable_double_pass
                   ? RenderTraceResult::Ok
                   : RenderTraceResult::Busy;
    }

    InterlockedExchange(&g_double_pass_enabled,
                        enable_double_pass ? 1 : 0);
    if (observed != static_cast<LONG>(TraceState::Idle)) {
        return observed == static_cast<LONG>(TraceState::Stopped)
                   ? RenderTraceResult::AlreadyStopped
                   : RenderTraceResult::Busy;
    }

    if (!ControlLogFormat("render-trace-bootstrap-entry pid=%lu tid=%lu",
                          static_cast<unsigned long>(GetCurrentProcessId()),
                          static_cast<unsigned long>(GetCurrentThreadId()))) {
        InterlockedExchange(&g_trace_state,
                            static_cast<LONG>(TraceState::Failed));
        return RenderTraceResult::PersistentLogFailure;
    }
    if (!PinTraceModule()) {
        InterlockedExchange(&g_trace_state,
                            static_cast<LONG>(TraceState::Failed));
        return RenderTraceResult::ModulePinFailure;
    }

    builds::BuildVerificationResult verification =
        builds::VerifyMainExecutable();
    if (!verification.IsVerified()) {
        (void)ControlLogFormat(
            "render-trace-bootstrap-failed stage=verify status=%.*s",
            static_cast<int>(builds::ToString(verification.status).size()),
            builds::ToString(verification.status).data());
        InterlockedExchange(&g_trace_state,
                            static_cast<LONG>(TraceState::Failed));
        return RenderTraceResult::BuildVerificationFailure;
    }
    if (verification.verified_build->descriptor().id !=
        builds::GameBuildId::SteamAspyr2015Build817494) {
        InterlockedExchange(&g_trace_state,
                            static_cast<LONG>(TraceState::Failed));
        return RenderTraceResult::WrongExactBuild;
    }
    if (!ValidateCandidates(*verification.verified_build)) {
        InterlockedExchange(&g_trace_state,
                            static_cast<LONG>(TraceState::Failed));
        return RenderTraceResult::CandidateValidationFailure;
    }
    if (!OpenTraceFile() || !QueryPerformanceFrequency(&g_qpc_frequency) ||
        g_qpc_frequency.QuadPart <= 0 || !CreateTraceSynchronization()) {
        InterlockedExchange(&g_trace_state,
                            static_cast<LONG>(TraceState::Failed));
        return RenderTraceResult::PersistentLogFailure;
    }

    InitializeQueue();
    g_double_pass_initial_f8_down =
        (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    InterlockedExchange(&g_f8_was_down,
                        g_double_pass_initial_f8_down ? 1 : 0);
    g_double_pass_armed_qpc = ReadQpc();
    g_double_pass_armed_thread_id = GetCurrentThreadId();
    g_writer_thread = CreateThread(nullptr, 0, WriterThread, nullptr, 0, nullptr);
    if (g_writer_thread == nullptr ||
        WaitForSingleObject(g_writer_ready_event, kWriterWaitMs) !=
            WAIT_OBJECT_0 ||
        AtomicRead(&g_writer_error) != 0) {
        StopWriterAfterFailedInstall();
        InterlockedExchange(&g_trace_state,
                            static_cast<LONG>(TraceState::Failed));
        return RenderTraceResult::WriterStartFailure;
    }

    g_original_camera_render_scene = reinterpret_cast<VirtualMethod>(
        g_candidates[0].runtime_target);
    g_original_scene_render = reinterpret_cast<VirtualMethod>(
        g_candidates[1].runtime_target);
    g_original_scene_render_single_pass = reinterpret_cast<VirtualMethod>(
        g_candidates[2].runtime_target);
    InterlockedExchange(&g_accepting_records, 1);
    for (HookCandidateRuntime& candidate : g_candidates) {
        if (!MutateCell(candidate, true)) {
            StopWriterAfterFailedInstall();
            InterlockedExchange(&g_trace_state,
                                static_cast<LONG>(TraceState::Failed));
            return RenderTraceResult::HookInstallFailure;
        }
    }

    g_timeout_thread =
        CreateThread(nullptr, 0, TimeoutAndStopThread, nullptr, 0, nullptr);
    if (g_timeout_thread == nullptr) {
        StopWriterAfterFailedInstall();
        InterlockedExchange(&g_trace_state,
                            static_cast<LONG>(TraceState::Failed));
        return RenderTraceResult::TimeoutStartFailure;
    }

    InterlockedExchange(&g_trace_state, static_cast<LONG>(TraceState::Active));
    if (!ControlLogFormat(
            "render-trace-bootstrap-complete pid=%lu hooks=3 duration_ms=600000 "
            "double_pass=%s",
            static_cast<unsigned long>(GetCurrentProcessId()),
            enable_double_pass ? "armed-f8-once" : "disabled")) {
        SetEvent(g_stop_request_event);
        WaitForSingleObject(g_stop_complete_event, kStopWaitMs);
        InterlockedExchange(&g_trace_state,
                            static_cast<LONG>(TraceState::Failed));
        return RenderTraceResult::PersistentLogFailure;
    }
    return RenderTraceResult::Ok;
}

[[nodiscard]] RenderTraceResult StopRenderTrace(void* reserved) noexcept {
    if (reserved != nullptr) {
        return RenderTraceResult::InvalidArgument;
    }
    const TraceState state =
        static_cast<TraceState>(AtomicRead(&g_trace_state));
    if (state == TraceState::Stopped) {
        return RenderTraceResult::Ok;
    }
    if (state != TraceState::Active && state != TraceState::Stopping) {
        return state == TraceState::Idle ? RenderTraceResult::AlreadyStopped
                                         : RenderTraceResult::Busy;
    }
    SetEvent(g_stop_request_event);
    if (WaitForSingleObject(g_stop_complete_event, kStopWaitMs) !=
        WAIT_OBJECT_0) {
        return RenderTraceResult::StopTimeout;
    }
    return static_cast<RenderTraceResult>(AtomicRead(&g_stop_result));
}

} // namespace
} // namespace k2vr::game32

std::uint32_t K2VR_RENDER_TRACE_THREAD_CALL
K2VR_RenderTraceBootstrap(void* reserved) noexcept {
    return static_cast<std::uint32_t>(
        k2vr::game32::BootstrapRenderTrace(reserved, false));
}

std::uint32_t K2VR_RENDER_TRACE_THREAD_CALL
K2VR_RenderDoublePassBootstrap(void* reserved) noexcept {
    return static_cast<std::uint32_t>(
        k2vr::game32::BootstrapRenderTrace(reserved, true));
}

std::uint32_t K2VR_GAME_IMAGE_THREAD_CALL
K2VR_GameImageSmokeBootstrap(void* bootstrap_v1) noexcept {
    const k2vr::game32::GameImageCaptureResult capture =
        k2vr::game32::ArmGameImageCapture(bootstrap_v1);
    if (capture != k2vr::game32::GameImageCaptureResult::Ok) {
        return static_cast<std::uint32_t>(capture);
    }

    static_assert(sizeof(k2vr::game32::GameImageSmokeBootstrapV1) ==
                  sizeof(k2vr::game32::VrBridgeBootstrapV1));
    const std::uint32_t bridge = K2VR_VrBridgeBootstrap(bootstrap_v1);
    if (bridge != static_cast<std::uint32_t>(k2vr::game32::VrBridgeResult::Ok)) {
        k2vr::game32::CancelGameImageCaptureArm();
        return 0x200U + bridge;
    }
    InterlockedExchange(&k2vr::game32::g_persistent_camera_hooks, 1);
    k2vr::game32::LoadCameraSettings();

    const k2vr::game32::RenderTraceResult trace =
        k2vr::game32::BootstrapRenderTrace(nullptr, false);
    if (trace != k2vr::game32::RenderTraceResult::Ok) {
        k2vr::game32::CancelGameImageCaptureArm();
        (void)K2VR_VrBridgeStop(nullptr);
        return 0x100U + static_cast<std::uint32_t>(trace);
    }
    if (!k2vr::game32::ControlLogFormat(
            "game-image-smoke-complete pid=%lu hooks=3 key=F7",
            static_cast<unsigned long>(GetCurrentProcessId()))) {
        k2vr::game32::CancelGameImageCaptureArm();
        (void)k2vr::game32::StopRenderTrace(nullptr);
        (void)K2VR_VrBridgeStop(nullptr);
        return static_cast<std::uint32_t>(
            k2vr::game32::GameImageCaptureResult::PersistentLogFailure);
    }
    if (!k2vr::game32::InstallGameImagePresentCapture()) {
        k2vr::game32::CancelGameImageCaptureArm();
        (void)k2vr::game32::StopRenderTrace(nullptr);
        (void)K2VR_VrBridgeStop(nullptr);
        return 0x300U;
    }
    InterlockedExchange(&k2vr::game32::g_hmd_camera_enabled, 1);
    return 0;
}

std::uint32_t K2VR_RENDER_TRACE_THREAD_CALL
K2VR_RenderTraceStop(void* reserved) noexcept {
    return static_cast<std::uint32_t>(
        k2vr::game32::StopRenderTrace(reserved));
}

#else

std::uint32_t K2VR_RENDER_TRACE_THREAD_CALL
K2VR_RenderTraceBootstrap(void*) noexcept {
    return static_cast<std::uint32_t>(
        k2vr::game32::RenderTraceResult::WrongExactBuild);
}

std::uint32_t K2VR_RENDER_TRACE_THREAD_CALL
K2VR_RenderDoublePassBootstrap(void*) noexcept {
    return static_cast<std::uint32_t>(
        k2vr::game32::RenderTraceResult::WrongExactBuild);
}

std::uint32_t K2VR_GAME_IMAGE_THREAD_CALL
K2VR_GameImageSmokeBootstrap(void*) noexcept {
    return static_cast<std::uint32_t>(
        k2vr::game32::GameImageCaptureResult::InvalidSession);
}

std::uint32_t K2VR_RENDER_TRACE_THREAD_CALL
K2VR_RenderTraceStop(void*) noexcept {
    return static_cast<std::uint32_t>(
        k2vr::game32::RenderTraceResult::WrongExactBuild);
}

#endif
