#include "game_image_capture.hpp"
#include "native_stereo.hpp"
#include "scene_replay.hpp"
#include "controller_input.hpp"
#include "vr_bridge.hpp"
#include "../common/vr_input.hpp"

#include "gpu_stream_producer.hpp"
#include "probe.hpp"
#include "bink_movie.hpp"

#if defined(_WIN32)

#include "gl_gpu_timing.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <gl/GL.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

static_assert(sizeof(void*) == 4,
              "game image capture is an in-process Win32/x86 component");

namespace k2vr::game32 {
namespace {

enum class CaptureState : LONG {
    Idle = 0,
    Arming = 1,
    Armed = 2,
    Capturing = 3,
    Published = 4,
    Failed = 5,
};

constexpr GLenum kGlBgra = 0x80E1U;

volatile LONG g_capture_state = static_cast<LONG>(CaptureState::Idle);
volatile LONG g_f7_was_down = 0;
volatile LONG g_scene_frame_id = 0;
GameImageSmokeBootstrapV1 g_bootstrap{};
std::array<wchar_t, 128> g_mapping_name{};
HANDLE g_mapping = nullptr;
std::atomic_bool g_capture_at_present=false;
using SwapBuffersFunction=BOOL(WINAPI*)(HDC);
SwapBuffersFunction g_original_swap_buffers{};
using DeleteContextFunction=BOOL(WINAPI*)(HGLRC);
DeleteContextFunction g_original_delete_context{};
std::atomic_bool g_log_context_recovery_surface=false;
[[nodiscard]] bool LogFormat(const char* format, ...) noexcept;
BOOL WINAPI CaptureDeleteContext(HGLRC context) {
    const BOOL result=g_original_delete_context(context);
    const DWORD error=GetLastError();
    if (result) {
        NotifyGpuStreamContextDeleted(context);
        NotifyNativeStereoContextDeleted(context);
        NotifySceneReplayContextDeleted(context);
        g_log_context_recovery_surface=true;
    }
    SetLastError(error);
    return result;
}
void RecoverContextsForWindow(HDC dc,const char* source) noexcept {
    if (!NativeStereoContextRecoveryPending() && !GpuStreamContextRecoveryPending()) return;
    const HDC current_dc=wglGetCurrentDC();
    const HGLRC context=wglGetCurrentContext();
    const HWND current_window=WindowFromDC(current_dc);
    const HWND window=WindowFromDC(dc);
    DWORD process{};
    const DWORD thread=window ? GetWindowThreadProcessId(window,&process) : 0;
    const bool eligible=IsGameContextRecoverySurface(
        reinterpret_cast<std::uintptr_t>(context),
        reinterpret_cast<std::uintptr_t>(current_window),
        reinterpret_cast<std::uintptr_t>(window),
        window && IsWindowVisible(window),process,GetCurrentProcessId());
    if (g_log_context_recovery_surface.exchange(false)) {
        (void)LogFormat("gl-context-recovery-surface source=%s eligible=%u context=%p current_dc=%p target_dc=%p current_window=%p target_window=%p window_thread=%lu render_thread=%lu window_process=%lu",
            source,eligible ? 1U:0U,context,current_dc,dc,current_window,window,
            thread,GetCurrentThreadId(),process);
    }
    // Neither a transient offscreen context nor another window's present can
    // select a replacement. Producers additionally require confirmed deletion
    // and their original GL owner thread; numeric context reuse is allowed.
    if (eligible) {
        RecoverGpuStreamContext();
        RecoverNativeStereoContext();
    }
}
BOOL WINAPI CaptureSwapBuffers(HDC dc) {
    RecoverContextsForWindow(dc,"present");
    FinishNativeStereoPresent();
    if (g_capture_at_present && IsGpuStreamProducerRunning()) {
        const auto id=static_cast<std::uint32_t>(InterlockedIncrement(&g_scene_frame_id));
        ProduceGpuStreamAfterScenePass(id);
    }
    return g_original_swap_buffers(dc);
}
void* g_mapping_view = nullptr;

[[nodiscard]] LONG AtomicRead(volatile LONG* value) noexcept {
    return InterlockedCompareExchange(value, 0, 0);
}

[[nodiscard]] bool LogFormat(const char* format, ...) noexcept {
    std::array<char, 768> buffer{};
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

[[nodiscard]] bool CopyBootstrapSeh(
    const void* source, GameImageSmokeBootstrapV1& destination) noexcept {
    if (source == nullptr) {
        return false;
    }
    __try {
        std::memcpy(&destination, source, sizeof(destination));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] bool MakeMappingName(ipc::SessionNonce nonce) noexcept {
    const int written = swprintf_s(
        g_mapping_name.data(), g_mapping_name.size(),
        L"Local\\Kotor2VR-game-image-v1-%016llX%016llX",
        static_cast<unsigned long long>(nonce.high),
        static_cast<unsigned long long>(nonce.low));
    return written > 0 &&
           static_cast<std::size_t>(written) < g_mapping_name.size();
}

[[nodiscard]] bool QueryViewportSeh(GLint (&viewport)[4],
                                    DWORD& seh_code) noexcept {
    seh_code = 0;
    __try {
        glGetIntegerv(GL_VIEWPORT, viewport);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh_code = GetExceptionCode();
        return false;
    }
}

[[nodiscard]] bool ReadBackBufferSeh(
    GLint x, GLint y, GLsizei width, GLsizei height, void* destination,
    DWORD& seh_code) noexcept {
    GLint old_alignment = 4;
    GLint old_row_length = 0;
    GLint old_skip_rows = 0;
    GLint old_skip_pixels = 0;
    GLint old_read_buffer = GL_BACK;
    bool completed = false;
    seh_code = 0;

    __try {
        glGetIntegerv(GL_PACK_ALIGNMENT, &old_alignment);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &old_row_length);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &old_skip_rows);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &old_skip_pixels);
        glGetIntegerv(GL_READ_BUFFER, &old_read_buffer);
        __try {
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            glPixelStorei(GL_PACK_SKIP_ROWS, 0);
            glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
            glReadBuffer(GL_BACK);
            glReadPixels(x, y, width, height, kGlBgra, GL_UNSIGNED_BYTE,
                         destination);
            completed = true;
        } __finally {
            glReadBuffer(static_cast<GLenum>(old_read_buffer));
            glPixelStorei(GL_PACK_SKIP_PIXELS, old_skip_pixels);
            glPixelStorei(GL_PACK_SKIP_ROWS, old_skip_rows);
            glPixelStorei(GL_PACK_ROW_LENGTH, old_row_length);
            glPixelStorei(GL_PACK_ALIGNMENT, old_alignment);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh_code = GetExceptionCode();
        completed = false;
    }
    return completed;
}

[[nodiscard]] bool CaptureAndPublish(std::uint64_t frame_id) noexcept {
    if (wglGetCurrentContext() == nullptr) {
        (void)LogFormat(
            "game-image-capture-failed pid=%lu frame_id=%llu stage=no-gl-context",
            static_cast<unsigned long>(GetCurrentProcessId()),
            static_cast<unsigned long long>(frame_id));
        return false;
    }

    GLint viewport[4]{};
    DWORD exception_code = 0;
    if (!QueryViewportSeh(viewport, exception_code)) {
        (void)LogFormat(
            "game-image-capture-failed pid=%lu frame_id=%llu "
            "stage=query-viewport exception=0x%08lX",
            static_cast<unsigned long>(GetCurrentProcessId()),
            static_cast<unsigned long long>(frame_id),
            static_cast<unsigned long>(exception_code));
        return false;
    }
    if (viewport[0] < 0 || viewport[1] < 0 || viewport[2] <= 0 ||
        viewport[3] <= 0) {
        (void)LogFormat(
            "game-image-capture-failed pid=%lu frame_id=%llu "
            "stage=invalid-viewport x=%ld y=%ld width=%ld height=%ld",
            static_cast<unsigned long>(GetCurrentProcessId()),
            static_cast<unsigned long long>(frame_id),
            static_cast<long>(viewport[0]), static_cast<long>(viewport[1]),
            static_cast<long>(viewport[2]), static_cast<long>(viewport[3]));
        return false;
    }

    const auto width = static_cast<std::uint32_t>(viewport[2]);
    const auto height = static_cast<std::uint32_t>(viewport[3]);
    const GameImageLayout layout = ComputeGameImageLayout(width, height);
    if (!layout.ok()) {
        (void)LogFormat(
            "game-image-capture-failed pid=%lu frame_id=%llu "
            "stage=invalid-layout width=%lu height=%lu status=%u",
            static_cast<unsigned long>(GetCurrentProcessId()),
            static_cast<unsigned long long>(frame_id),
            static_cast<unsigned long>(width),
            static_cast<unsigned long>(height),
            static_cast<unsigned>(layout.status));
        return false;
    }

    void* const bottom_up = HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY,
        static_cast<SIZE_T>(layout.pixel_bytes));
    if (bottom_up == nullptr) {
        (void)LogFormat(
            "game-image-capture-failed pid=%lu frame_id=%llu stage=heap-alloc",
            static_cast<unsigned long>(GetCurrentProcessId()),
            static_cast<unsigned long long>(frame_id));
        return false;
    }

    const bool read = ReadBackBufferSeh(
        viewport[0], viewport[1], viewport[2], viewport[3], bottom_up,
        exception_code);
    if (!read) {
        HeapFree(GetProcessHeap(), 0, bottom_up);
        (void)LogFormat(
            "game-image-capture-failed pid=%lu frame_id=%llu "
            "stage=read-pixels exception=0x%08lX",
            static_cast<unsigned long>(GetCurrentProcessId()),
            static_cast<unsigned long long>(frame_id),
            static_cast<unsigned long>(exception_code));
        return false;
    }

    HANDLE mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        layout.mapping_bytes, g_mapping_name.data());
    const DWORD mapping_error = GetLastError();
    if (mapping == nullptr || mapping_error == ERROR_ALREADY_EXISTS) {
        if (mapping != nullptr) {
            CloseHandle(mapping);
        }
        HeapFree(GetProcessHeap(), 0, bottom_up);
        (void)LogFormat(
            "game-image-capture-failed pid=%lu frame_id=%llu "
            "stage=create-mapping error=%lu",
            static_cast<unsigned long>(GetCurrentProcessId()),
            static_cast<unsigned long long>(frame_id),
            static_cast<unsigned long>(mapping_error));
        return false;
    }

    void* const view = MapViewOfFile(
        mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
        static_cast<SIZE_T>(layout.mapping_bytes));
    if (view == nullptr) {
        const DWORD error = GetLastError();
        CloseHandle(mapping);
        HeapFree(GetProcessHeap(), 0, bottom_up);
        (void)LogFormat(
            "game-image-capture-failed pid=%lu frame_id=%llu "
            "stage=map-view error=%lu",
            static_cast<unsigned long>(GetCurrentProcessId()),
            static_cast<unsigned long long>(frame_id),
            static_cast<unsigned long>(error));
        return false;
    }

    std::memset(view, 0, layout.mapping_bytes);
    auto* const header = static_cast<GameImageSharedHeaderV1*>(view);
    InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&header->sequence), 1);
    header->magic = kGameImageMagic;
    header->version_major = kGameImageMajor;
    header->version_minor = kGameImageMinor;
    header->header_size = sizeof(GameImageSharedHeaderV1);
    header->mapping_size = layout.mapping_bytes;
    header->width = width;
    header->height = height;
    header->stride = layout.stride;
    header->pixel_format = kGameImagePixelFormatBgra8Unorm;
    header->frame_id = frame_id;

    auto* const destination =
        static_cast<std::uint8_t*>(view) + sizeof(GameImageSharedHeaderV1);
    const auto* const source = static_cast<const std::uint8_t*>(bottom_up);
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint32_t source_row = SourceRowForTopDown(row, height);
        std::memcpy(destination + static_cast<std::size_t>(row) * layout.stride,
                    source + static_cast<std::size_t>(source_row) * layout.stride,
                    layout.stride);
    }
    HeapFree(GetProcessHeap(), 0, bottom_up);

    MemoryBarrier();
    InterlockedExchange(
        reinterpret_cast<volatile LONG*>(&header->sequence), 2);
    // Retain both for process lifetime so a host that opens slightly after F7
    // can still consume the one immutable published frame.
    g_mapping = mapping;
    g_mapping_view = view;
    (void)LogFormat(
        "game-image-captured pid=%lu frame_id=%llu width=%lu height=%lu "
        "stride=%lu sequence=2 mapping=%ls",
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long long>(frame_id),
        static_cast<unsigned long>(width),
        static_cast<unsigned long>(height),
        static_cast<unsigned long>(layout.stride), g_mapping_name.data());
    return true;
}

void PublishGpuStreamDiagnostic(
    const GpuStreamProducerDiagnostic& diagnostic) noexcept {
    if (g_mapping_view == nullptr) {
        return;
    }

    auto* const header =
        static_cast<GameImageSharedHeaderV1*>(g_mapping_view);
    auto* const sequence =
        reinterpret_cast<volatile LONG*>(&header->sequence);
    const LONG before = AtomicRead(sequence);
    if (before <= 0 || (before & 1) != 0 ||
        before > (std::numeric_limits<LONG>::max)() - 2) {
        return;
    }

    InterlockedExchange(sequence, before + 1);
    const GameImageGpuDiagnosticV1 wire{
        kGameImageGpuDiagnosticMagic,
        static_cast<std::uint32_t>(diagnostic.status),
        diagnostic.interop_status,
        static_cast<std::uint32_t>(diagnostic.hresult),
        diagnostic.win32_error};
    std::memcpy(header->reserved, &wire, sizeof(wire));
    MemoryBarrier();
    InterlockedExchange(sequence, before + 2);
}

} // namespace

void RecoverGameImageContextsForScene() noexcept {
    RecoverContextsForWindow(wglGetCurrentDC(),"world-camera");
}

GameImageCaptureResult ArmGameImageCapture(void* bootstrap_v1) noexcept {
    GameImageSmokeBootstrapV1 copied{};
    if (!CopyBootstrapSeh(bootstrap_v1, copied)) {
        return GameImageCaptureResult::InvalidArgument;
    }
    const GameImageCaptureResult validation =
        ValidateGameImageSmokeBootstrap(copied);
    if (validation != GameImageCaptureResult::Ok) {
        return validation;
    }

    const LONG observed = InterlockedCompareExchange(
        &g_capture_state, static_cast<LONG>(CaptureState::Arming),
        static_cast<LONG>(CaptureState::Idle));
    if (observed != static_cast<LONG>(CaptureState::Idle)) {
        return GameImageCaptureResult::Busy;
    }

    g_bootstrap = copied;
    if (!MakeMappingName(copied.session_nonce)) {
        InterlockedExchange(&g_capture_state,
                            static_cast<LONG>(CaptureState::Failed));
        return GameImageCaptureResult::InvalidSession;
    }
    g_scene_frame_id = 0;
    const bool initially_down = input::StartStreamDown();
    InterlockedExchange(&g_f7_was_down, initially_down ? 1 : 0);
    if (!LogFormat(
            "game-image-smoke-entry pid=%lu tid=%lu version=1.0 "
            "nonce=%016llX%016llX generation=%llu key=F7",
            static_cast<unsigned long>(GetCurrentProcessId()),
            static_cast<unsigned long>(GetCurrentThreadId()),
            static_cast<unsigned long long>(copied.session_nonce.high),
            static_cast<unsigned long long>(copied.session_nonce.low),
            static_cast<unsigned long long>(copied.generation))) {
        InterlockedExchange(&g_capture_state,
                            static_cast<LONG>(CaptureState::Failed));
        return GameImageCaptureResult::PersistentLogFailure;
    }

    InterlockedExchange(&g_capture_state,
                        static_cast<LONG>(CaptureState::Armed));
    return GameImageCaptureResult::Ok;
}

void CancelGameImageCaptureArm() noexcept {
    g_capture_at_present=false;
    (void)InterlockedCompareExchange(
        &g_capture_state, static_cast<LONG>(CaptureState::Failed),
        static_cast<LONG>(CaptureState::Armed));
}

void GameImageCaptureAfterScenePass() noexcept {
    // This check must remain first: a suppressed camera re-entry is invisible
    // to F7 edge tracking, frame numbering, CPU readback, and the GPU stream.
    if (GetGameImageCapturePolicy() ==
        GameImageCapturePolicy::Suppressed) {
        return;
    }

    // Native capture occurs after the entire Camera pass, including Scene
    // post-processing. Inner Scene callbacks must not publish a partial eye.
    if (NativeStereoEyeActive()) return;

    const CaptureState capture_state =
        static_cast<CaptureState>(AtomicRead(&g_capture_state));
    if (capture_state != CaptureState::Armed &&
        !IsGpuStreamProducerRunning()) {
        return;
    }

    const std::uint64_t frame_id = static_cast<std::uint32_t>(
        InterlockedIncrement(&g_scene_frame_id));
    if (capture_state == CaptureState::Armed) {
        const bool is_down = input::StartStreamDown();
        const bool was_down =
            InterlockedExchange(&g_f7_was_down, is_down ? 1 : 0) != 0;
        const GameImageInputTransition transition = EvaluateGameImageInput(
            true, false, was_down, is_down);
        ipc::RenderRequest initial_request{};
        const bool auto_start=frame_id>=3 && TryReadLatestRenderRequest(initial_request) &&
            initial_request.presentation_state==ipc::PresentationState::WorldThirdPerson;
        if ((transition.should_capture || auto_start) &&
            InterlockedCompareExchange(
                &g_capture_state,
                static_cast<LONG>(CaptureState::Capturing),
                static_cast<LONG>(CaptureState::Armed)) ==
                static_cast<LONG>(CaptureState::Armed)) {
            // Patch between polls on the game's window/input thread only.
            DWORD window_pid{};
            const HWND window=WindowFromDC(wglGetCurrentDC());
            const DWORD window_thread=GetWindowThreadProcessId(window,&window_pid);
            if (initial_request.presentation_state==ipc::PresentationState::WorldThirdPerson && window_pid==GetCurrentProcessId() &&
                window_thread==GetCurrentThreadId()) {
                const auto installed=InstallControllerInputFilter();
                (void)LogFormat("controller-input-install result=%u thread=%lu",
                    static_cast<unsigned>(installed),window_thread);
            }
            if (auto_start) (void)LogFormat("native-vr-auto-start frame=%llu",static_cast<unsigned long long>(frame_id));
            const bool published = CaptureAndPublish(frame_id);
            // Start only after the immutable CPU smoke image was attempted. The
            // CPU mapping remains usable even if continuous GPU interop fails.
            GpuStreamProducerDiagnostic gpu_diagnostic{};
            ipc::RenderRequest request{};
            g_capture_at_present=TryReadLatestRenderRequest(request) &&
                request.presentation_state==ipc::PresentationState::WorldThirdPerson;
            (void)StartGpuStreamProducer(g_bootstrap.session_nonce,
                &gpu_diagnostic,g_capture_at_present ? 1920U : ipc::kGpuStreamWidth,
                g_capture_at_present ? 1080U : ipc::kGpuStreamHeight);
            if (published) {
                PublishGpuStreamDiagnostic(gpu_diagnostic);
            }
            InterlockedExchange(
                &g_capture_state,
                static_cast<LONG>(published ? CaptureState::Published
                                            : CaptureState::Failed));
        }
    }

    if (!g_capture_at_present) ProduceGpuStreamAfterScenePass(frame_id);
}

bool InstallGameImagePresentCapture() noexcept {
    // Revalidate the actual IAT cells even on repeated calls. Original function
    // pointers alone do not prove that a previous partial install succeeded.
    bool present_ready=false,deletion_ready=false;
    // Called only after the exact-build trace validator succeeded. Locate the
    // named import instead of guessing an IAT cell or replacing ReShade's proxy.
    auto* base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
    const auto directory=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto* imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+directory.VirtualAddress);
    for (;imports->Name;++imports) {
        const auto* module=reinterpret_cast<const char*>(base+imports->Name);
        if ((_stricmp(module,"gdi32.dll")!=0 && _stricmp(module,"opengl32.dll")!=0) || !imports->OriginalFirstThunk) continue;
        auto* names=reinterpret_cast<IMAGE_THUNK_DATA32*>(base+imports->OriginalFirstThunk);
        auto* cells=reinterpret_cast<IMAGE_THUNK_DATA32*>(base+imports->FirstThunk);
        for (;names->u1.AddressOfData;++names,++cells) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            auto* name=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData);
            const auto* symbol=reinterpret_cast<const char*>(name->Name);
            const bool present=_stricmp(module,"gdi32.dll")==0 && std::strcmp(symbol,"SwapBuffers")==0;
            const bool deletion=_stricmp(module,"opengl32.dll")==0 && std::strcmp(symbol,"wglDeleteContext")==0;
            if (!present && !deletion) continue;
            const auto wrapper=present ? reinterpret_cast<void*>(&CaptureSwapBuffers) : reinterpret_cast<void*>(&CaptureDeleteContext);
            if (reinterpret_cast<void*>(cells->u1.Function)==wrapper) {
                if (present) present_ready=g_original_swap_buffers!=nullptr;
                else deletion_ready=g_original_delete_context!=nullptr;
                if ((present && !present_ready) || (deletion && !deletion_ready)) return false;
                continue;
            }
            const auto expected=reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA(module),symbol));
            if (!expected || reinterpret_cast<void*>(cells->u1.Function)!=expected) return false;
            DWORD old{},ignored{};
            if (!VirtualProtect(&cells->u1.Function,sizeof(void*),PAGE_READWRITE,&old)) return false;
            if (present) g_original_swap_buffers=reinterpret_cast<SwapBuffersFunction>(expected);
            else g_original_delete_context=reinterpret_cast<DeleteContextFunction>(expected);
            const bool swapped=InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&cells->u1.Function),
                wrapper,expected)==expected;
            const bool restored=VirtualProtect(&cells->u1.Function,sizeof(void*),old,&ignored)!=FALSE;
            if (!swapped) {
                if (present) g_original_swap_buffers=nullptr;
                else g_original_delete_context=nullptr;
            }
            if (!swapped || !restored) return false;
            if (present) present_ready=true;
            else deletion_ready=true;
        }
    }
    const bool ready=present_ready && deletion_ready &&
        g_original_swap_buffers && g_original_delete_context;
    if (ready) gpu_timing::ArmAfterDeletionHookInstalled();
    return ready && InstallBinkMovieHooks(g_bootstrap.session_nonce);
}

} // namespace k2vr::game32

#else

namespace k2vr::game32 {

GameImageCaptureResult ArmGameImageCapture(void*) noexcept {
    return GameImageCaptureResult::InvalidSession;
}

void CancelGameImageCaptureArm() noexcept {}
void RecoverGameImageContextsForScene() noexcept {}
void GameImageCaptureAfterScenePass() noexcept {}

} // namespace k2vr::game32

#endif
