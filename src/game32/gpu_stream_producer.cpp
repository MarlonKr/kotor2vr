#include "gpu_stream_producer.hpp"
#include "gl_context_recovery.hpp"
#include "../common/ui_mapping.hpp"

#include "gl_ext_d3d12_bridge.hpp"
#include "nv_dx_interop_bridge.hpp"
#include "probe.hpp"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <d3d11_4.h>
#include <gl/GL.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <new>

static_assert(sizeof(void*) == 4,
              "GPU stream producer belongs in the injected x86 module");
static_assert(k2vr::ipc::kGpuStreamSlotCount ==
              k2vr::game32::kGlExtD3D12StreamSlotCount);

namespace k2vr::game32 {
namespace {

constexpr GLenum kGlReadFramebuffer = 0x8CA8U;
constexpr GLenum kGlDrawFramebuffer = 0x8CA9U;
constexpr GLenum kGlReadFramebufferBinding = 0x8CAAU;
constexpr GLenum kGlDrawFramebufferBinding = 0x8CA6U;
constexpr GLenum kGlColorAttachment0 = 0x8CE0U;
constexpr GLenum kGlFramebufferComplete = 0x8CD5U;
constexpr GLenum kGlFramebufferSrgb = 0x8DB9U;
constexpr GLenum kGlRgba8 = 0x8058U;

using GlGenFramebuffers = void(APIENTRY*)(GLsizei count, GLuint* framebuffers);
using GlDeleteFramebuffers = void(APIENTRY*)(GLsizei count,
                                             const GLuint* framebuffers);
using GlBindFramebuffer = void(APIENTRY*)(GLenum target, GLuint framebuffer);
using GlFramebufferTexture2D = void(APIENTRY*)(GLenum target,
                                               GLenum attachment,
                                               GLenum texture_target,
                                               GLuint texture, GLint level);
using GlCheckFramebufferStatus = GLenum(APIENTRY*)(GLenum target);
using GlBlitFramebuffer = void(APIENTRY*)(GLint source_x0, GLint source_y0,
                                          GLint source_x1, GLint source_y1,
                                          GLint destination_x0,
                                          GLint destination_y0,
                                          GLint destination_x1,
                                          GLint destination_y1,
                                          GLbitfield mask, GLenum filter);

enum class ProducerLifecycle : LONG {
    Dormant = 0,
    Starting = 1,
    Running = 2,
    Failed = 3,
    Stopped = 4,
};

struct GlFunctions {
    GlGenFramebuffers gen_framebuffers{};
    GlDeleteFramebuffers delete_framebuffers{};
    GlBindFramebuffer bind_framebuffer{};
    GlFramebufferTexture2D framebuffer_texture_2d{};
    GlCheckFramebufferStatus check_framebuffer_status{};
    GlBlitFramebuffer blit_framebuffer{};
};

struct ProducerState {
    HGLRC owner_context{};
    DWORD owner_thread{};
    bool context_deleted{};
    GlContextRecoveryRetry context_recovery;
    std::uint32_t width{ipc::kGpuStreamWidth},height{ipc::kGpuStreamHeight};
    GlExtD3D12Bridge gl_ext_bridge;
    NvDxInteropBridge nv_bridge;
    GpuStreamInteropBackend backend{GpuStreamInteropBackend::None};
    GlFunctions gl{};
    ID3D11Fence* ready_fence{};
    ID3D11Fence* consumed_fence{};
    HANDLE ready_handle{};
    HANDLE consumed_handle{};
    GLuint output_fbo{};
    GLuint resolve_fbo{};
    GLuint resolve_texture{};
    std::uint32_t resolve_width{};
    std::uint32_t resolve_height{};
    std::uint64_t last_produced{};
    std::array<std::uint64_t, ipc::kGpuStreamSlotCount> slot_last_use{};
    std::uint64_t dropped_frames{};
    bool first_frame_logged{};
};

volatile LONG g_lifecycle = static_cast<LONG>(ProducerLifecycle::Dormant);
ProducerState* g_producer = nullptr;

[[nodiscard]] LONG AtomicRead(volatile LONG* const value) noexcept {
    return InterlockedCompareExchange(value, 0, 0);
}

void StoreDiagnostic(
    GpuStreamProducerDiagnostic* const destination,
    const GpuStreamProducerStatus status,
    const NvDxInteropDiagnostic* const interop = nullptr) noexcept {
    if (destination == nullptr) {
        return;
    }
    *destination = MakeGpuStreamProducerDiagnostic(
        status,
        interop != nullptr
            ? static_cast<std::uint32_t>(interop->status)
            : kGpuStreamInteropStatusUnavailable,
        interop != nullptr ? interop->hresult : 0,
        interop != nullptr ? interop->win32_error : 0U);
}

void StoreDiagnostic(
    GpuStreamProducerDiagnostic* const destination,
    const GpuStreamProducerStatus status,
    const GlExtD3D12Diagnostic* const interop) noexcept {
    if (destination == nullptr) {
        return;
    }
    *destination = MakeGpuStreamProducerDiagnostic(
        status,
        interop != nullptr
            ? EncodeGpuStreamInteropStatus(
                  GpuStreamInteropBackend::GlExtD3D12,
                  static_cast<std::uint32_t>(interop->status))
            : kGpuStreamInteropStatusUnavailable,
        interop != nullptr ? interop->hresult : 0,
        interop != nullptr ? interop->win32_error : 0U);
}

[[nodiscard]] const char* BackendName(
    const GpuStreamInteropBackend backend) noexcept {
    switch (backend) {
    case GpuStreamInteropBackend::GlExtD3D12: return "gl-ext-d3d12";
    case GpuStreamInteropBackend::NvDxInterop: return "nv-dx-interop";
    case GpuStreamInteropBackend::None: return "none";
    }
    return "unknown";
}

[[nodiscard]] bool OwningGlContextIsCurrent(
    const ProducerState& state) noexcept {
    switch (state.backend) {
    case GpuStreamInteropBackend::GlExtD3D12:
        return state.gl_ext_bridge.owning_gl_context_is_current();
    case GpuStreamInteropBackend::NvDxInterop:
        return state.nv_bridge.owning_gl_context_is_current();
    case GpuStreamInteropBackend::None:
        return (!state.gl_ext_bridge.initialized() ||
                state.gl_ext_bridge.owning_gl_context_is_current()) &&
               (!state.nv_bridge.initialized() ||
                state.nv_bridge.owning_gl_context_is_current());
    }
    return false;
}

[[nodiscard]] GLuint OutputTextureName(
    const ProducerState& state, const std::size_t slot) noexcept {
    return state.backend == GpuStreamInteropBackend::GlExtD3D12
               ? static_cast<GLuint>(state.gl_ext_bridge.gl_texture_name(slot))
               : state.nv_bridge.gl_texture_name();
}

[[nodiscard]] bool LogFormat(const char* const format, ...) noexcept {
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
            : buffer.size() - 1U;
    return AppendPersistentProbeLogLine(
        std::string_view(buffer.data(), length));
}

template <typename Interface>
void SafeRelease(Interface*& value) noexcept {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

[[nodiscard]] FARPROC ResolveGlProcedure(HMODULE const open_gl,
                                         const char* const name) noexcept {
    const PROC context_procedure = wglGetProcAddress(name);
    if (IsUsableWglProcAddressValue(
            reinterpret_cast<std::uintptr_t>(context_procedure))) {
        return reinterpret_cast<FARPROC>(context_procedure);
    }
    const FARPROC exported = GetProcAddress(open_gl, name);
    return IsUsableWglProcAddressValue(
               reinterpret_cast<std::uintptr_t>(exported))
               ? exported
               : nullptr;
}

[[nodiscard]] GpuStreamProducerStatus LoadGlFunctions(
    GlFunctions& gl, const char*& missing) noexcept {
    HMODULE const open_gl = GetModuleHandleW(L"opengl32.dll");
    if (open_gl == nullptr) {
        missing = "opengl32.dll";
        return GpuStreamProducerStatus::MissingGlEntryPoint;
    }
#define K2VR_LOAD_GL(member, type, symbol)                                     \
    gl.member = reinterpret_cast<type>(ResolveGlProcedure(open_gl, symbol));   \
    if (gl.member == nullptr) {                                                \
        missing = symbol;                                                      \
        return GpuStreamProducerStatus::MissingGlEntryPoint;                  \
    }
    K2VR_LOAD_GL(gen_framebuffers, GlGenFramebuffers, "glGenFramebuffers")
    K2VR_LOAD_GL(delete_framebuffers, GlDeleteFramebuffers,
                 "glDeleteFramebuffers")
    K2VR_LOAD_GL(bind_framebuffer, GlBindFramebuffer, "glBindFramebuffer")
    K2VR_LOAD_GL(framebuffer_texture_2d, GlFramebufferTexture2D,
                 "glFramebufferTexture2D")
    K2VR_LOAD_GL(check_framebuffer_status, GlCheckFramebufferStatus,
                 "glCheckFramebufferStatus")
    K2VR_LOAD_GL(blit_framebuffer, GlBlitFramebuffer, "glBlitFramebuffer")
#undef K2VR_LOAD_GL
    missing = nullptr;
    return GpuStreamProducerStatus::Ok;
}

void DrainGlErrors() noexcept {
    for (unsigned attempt = 0; attempt < 16U; ++attempt) {
        if (glGetError() == GL_NO_ERROR) {
            break;
        }
    }
}

struct GlStateGuard {
    explicit GlStateGuard(const GlFunctions& functions) noexcept
        : gl(functions) {
        glGetIntegerv(kGlReadFramebufferBinding, &read_fbo);
        glGetIntegerv(kGlDrawFramebufferBinding, &draw_fbo);
        glGetIntegerv(GL_READ_BUFFER, &read_buffer);
        glGetIntegerv(GL_DRAW_BUFFER, &draw_buffer);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_2d);
        scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
        srgb_enabled = glIsEnabled(kGlFramebufferSrgb);
        if (scissor_enabled != GL_FALSE) {
            glDisable(GL_SCISSOR_TEST);
        }
        if (srgb_enabled != GL_FALSE) {
            glDisable(kGlFramebufferSrgb);
        }
    }

    ~GlStateGuard() {
        gl.bind_framebuffer(kGlReadFramebuffer,
                            static_cast<GLuint>(read_fbo));
        gl.bind_framebuffer(kGlDrawFramebuffer,
                            static_cast<GLuint>(draw_fbo));
        glReadBuffer(static_cast<GLenum>(read_buffer));
        glDrawBuffer(static_cast<GLenum>(draw_buffer));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_2d));
        if (scissor_enabled != GL_FALSE) {
            glEnable(GL_SCISSOR_TEST);
        }
        if (srgb_enabled != GL_FALSE) {
            glEnable(kGlFramebufferSrgb);
        }
    }

    GlStateGuard(const GlStateGuard&) = delete;
    GlStateGuard& operator=(const GlStateGuard&) = delete;

    const GlFunctions& gl;
    GLint read_fbo{};
    GLint draw_fbo{};
    GLint read_buffer{};
    GLint draw_buffer{};
    GLint texture_2d{};
    GLboolean scissor_enabled{GL_FALSE};
    GLboolean srgb_enabled{GL_FALSE};
};

[[nodiscard]] bool EnsureResolveTarget(ProducerState& state,
                                       const std::uint32_t width,
                                       const std::uint32_t height) noexcept {
    if (state.resolve_texture != 0U && state.resolve_fbo != 0U &&
        state.resolve_width == width && state.resolve_height == height) {
        return true;
    }
    if (state.resolve_fbo != 0U) {
        state.gl.delete_framebuffers(1, &state.resolve_fbo);
        state.resolve_fbo = 0U;
    }
    if (state.resolve_texture != 0U) {
        glDeleteTextures(1, &state.resolve_texture);
        state.resolve_texture = 0U;
    }
    state.resolve_width = 0U;
    state.resolve_height = 0U;

    DrainGlErrors();
    glGenTextures(1, &state.resolve_texture);
    if (state.resolve_texture == 0U) {
        return false;
    }
    glBindTexture(GL_TEXTURE_2D, state.resolve_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(kGlRgba8),
                 static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    state.gl.gen_framebuffers(1, &state.resolve_fbo);
    if (state.resolve_fbo == 0U || glGetError() != GL_NO_ERROR) {
        if (state.resolve_fbo != 0U) {
            state.gl.delete_framebuffers(1, &state.resolve_fbo);
            state.resolve_fbo = 0U;
        }
        glDeleteTextures(1, &state.resolve_texture);
        state.resolve_texture = 0U;
        return false;
    }

    state.gl.bind_framebuffer(kGlDrawFramebuffer, state.resolve_fbo);
    state.gl.framebuffer_texture_2d(
        kGlDrawFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D,
        state.resolve_texture, 0);
    glDrawBuffer(kGlColorAttachment0);
    if (state.gl.check_framebuffer_status(kGlDrawFramebuffer) !=
            kGlFramebufferComplete ||
        glGetError() != GL_NO_ERROR) {
        state.gl.delete_framebuffers(1, &state.resolve_fbo);
        state.resolve_fbo = 0U;
        glDeleteTextures(1, &state.resolve_texture);
        state.resolve_texture = 0U;
        return false;
    }

    state.resolve_width = width;
    state.resolve_height = height;
    return true;
}

[[nodiscard]] GpuStreamProducerStatus BlitBackBuffer(
    ProducerState& state, const std::size_t slot) noexcept {
    if (wglGetCurrentContext() == nullptr) {
        return GpuStreamProducerStatus::NoCurrentGlContext;
    }
    GLint viewport[4]{};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[0] < 0 || viewport[1] < 0 || viewport[2] <= 0 ||
        viewport[3] <= 0) {
        return GpuStreamProducerStatus::InvalidViewport;
    }
    const auto source_width = static_cast<std::uint32_t>(viewport[2]);
    const auto source_height = static_cast<std::uint32_t>(viewport[3]);
    const auto fit=ui::FitLogicalSurface({0,0,static_cast<float>(state.width),static_cast<float>(state.height)},
        source_width,source_height);
    if (!fit) return GpuStreamProducerStatus::InvalidViewport;

    NvDxInteropDiagnostic interop_diagnostic{};
    GpuStreamProducerStatus result = GpuStreamProducerStatus::Ok;
    bool object_locked = false;
    {
        GlStateGuard guard(state.gl);
        if (!EnsureResolveTarget(state, source_width, source_height)) {
            return GpuStreamProducerStatus::ResolveTargetFailure;
        }

        if (state.backend == GpuStreamInteropBackend::NvDxInterop) {
            if (state.nv_bridge.Lock(interop_diagnostic) !=
                NvDxInteropStatus::Ok) {
                return GpuStreamProducerStatus::InteropLockFailure;
            }
            object_locked = true;
        }
        DrainGlErrors();

        // Resolve GL_BACK at its native dimensions first. KOTOR commonly uses a
        // multisampled default framebuffer, for which resolving and scaling in
        // one glBlitFramebuffer call is illegal.
        state.gl.bind_framebuffer(kGlReadFramebuffer, 0U);
        glReadBuffer(GL_BACK);
        state.gl.bind_framebuffer(kGlDrawFramebuffer, state.resolve_fbo);
        glDrawBuffer(kGlColorAttachment0);
        state.gl.blit_framebuffer(
            viewport[0], viewport[1], viewport[0] + viewport[2],
            viewport[1] + viewport[3], 0, 0, viewport[2], viewport[3],
            GL_COLOR_BUFFER_BIT, GL_NEAREST);

        state.gl.bind_framebuffer(kGlReadFramebuffer, state.resolve_fbo);
        glReadBuffer(kGlColorAttachment0);
        state.gl.bind_framebuffer(kGlDrawFramebuffer, state.output_fbo);
        state.gl.framebuffer_texture_2d(
            kGlDrawFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D,
            OutputTextureName(state, slot), 0);
        glDrawBuffer(kGlColorAttachment0);
        if (state.gl.check_framebuffer_status(kGlDrawFramebuffer) !=
            kGlFramebufferComplete) {
            result = GpuStreamProducerStatus::FramebufferIncomplete;
        } else {
            // Keep ultrawide and 4:3 interfaces undistorted on the 16:9 theater.
            glPushAttrib(GL_COLOR_BUFFER_BIT);
            glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE); glClearColor(0,0,0,1);
            glClear(GL_COLOR_BUFFER_BIT); glPopAttrib();
            // Reversing the destination Y coordinates makes D3D row zero the
            // visual top of the image consumed by the x64 host.
            state.gl.blit_framebuffer(
                0, 0, viewport[2], viewport[3], static_cast<GLint>(std::lround(fit->x)),
                static_cast<GLint>(std::lround(fit->y+fit->height)),
                static_cast<GLint>(std::lround(fit->x+fit->width)), static_cast<GLint>(std::lround(fit->y)),
                GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glFlush();
            if (glGetError() != GL_NO_ERROR) {
                result = GpuStreamProducerStatus::GlBlitFailure;
            }
        }

        // Do not leave a shared texture attached while either backend releases
        // ownership to the host.
        state.gl.framebuffer_texture_2d(
            kGlDrawFramebuffer, kGlColorAttachment0, GL_TEXTURE_2D, 0U, 0);
    }

    if (object_locked) {
        // Flush after state restoration as well, so every command touching the
        // registered object is submitted before ownership returns to D3D11.
        glFlush();
        if (state.nv_bridge.Unlock(interop_diagnostic) !=
            NvDxInteropStatus::Ok) {
            return GpuStreamProducerStatus::InteropUnlockFailure;
        }
    }
    return result;
}

[[nodiscard]] bool CleanupProducer(ProducerState& state) noexcept {
    bool clean = true;
    if (OwningGlContextIsCurrent(state)) {
        if (state.output_fbo != 0U) {
            state.gl.delete_framebuffers(1, &state.output_fbo);
            state.output_fbo = 0U;
        }
        if (state.resolve_fbo != 0U) {
            state.gl.delete_framebuffers(1, &state.resolve_fbo);
            state.resolve_fbo = 0U;
        }
        if (state.resolve_texture != 0U) {
            glDeleteTextures(1, &state.resolve_texture);
            state.resolve_texture = 0U;
        }
    } else if (state.output_fbo != 0U || state.resolve_fbo != 0U ||
               state.resolve_texture != 0U) {
        clean = false;
    }
    if (state.ready_handle != nullptr) {
        CloseHandle(state.ready_handle);
        state.ready_handle = nullptr;
    }
    if (state.consumed_handle != nullptr) {
        CloseHandle(state.consumed_handle);
        state.consumed_handle = nullptr;
    }
    SafeRelease(state.ready_fence);
    SafeRelease(state.consumed_fence);

    GlExtD3D12Diagnostic gl_ext_diagnostic{};
    if (state.gl_ext_bridge.Shutdown(gl_ext_diagnostic) !=
        GlExtD3D12Status::Ok) {
        clean = false;
    }
    NvDxInteropDiagnostic nv_diagnostic{};
    if (state.nv_bridge.Shutdown(nv_diagnostic) != NvDxInteropStatus::Ok) {
        clean = false;
    }
    state.backend = GpuStreamInteropBackend::None;
    return clean;
}

[[nodiscard]] GpuStreamProducerStatus FailStart(
    ProducerState* const state, const GpuStreamProducerStatus status,
    const char* const detail, const NvDxInteropDiagnostic* const interop =
                                   nullptr,
    GpuStreamProducerDiagnostic* const diagnostic = nullptr) noexcept {
    StoreDiagnostic(diagnostic, status, interop);
    if (interop != nullptr) {
        (void)LogFormat(
            "gpu-stream-producer-failed pid=%lu status=%s stage=%s "
            "interop=%s hr=0x%08lX error=%lu detail=%s",
            static_cast<unsigned long>(GetCurrentProcessId()),
            ToString(status).data(), detail,
            ToString(interop->status).data(),
            static_cast<unsigned long>(interop->hresult),
            static_cast<unsigned long>(interop->win32_error),
            interop->detail);
    } else {
        (void)LogFormat(
            "gpu-stream-producer-failed pid=%lu status=%s stage=%s",
            static_cast<unsigned long>(GetCurrentProcessId()),
            ToString(status).data(), detail);
    }
    if (state != nullptr) {
        (void)CleanupProducer(*state);
        delete state;
    }
    g_producer = nullptr;
    InterlockedExchange(&g_lifecycle,
                        static_cast<LONG>(ProducerLifecycle::Failed));
    return status;
}

void DisableAfterRuntimeFailure(const GpuStreamProducerStatus status,
                                const std::uint64_t scene_frame_id) noexcept {
    if (InterlockedCompareExchange(
            &g_lifecycle, static_cast<LONG>(ProducerLifecycle::Failed),
            static_cast<LONG>(ProducerLifecycle::Running)) !=
        static_cast<LONG>(ProducerLifecycle::Running)) {
        return;
    }
    (void)LogFormat(
        "gpu-stream-runtime-failed pid=%lu scene_frame=%llu status=%s "
        "produced=%llu dropped=%llu",
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long long>(scene_frame_id),
        ToString(status).data(),
        static_cast<unsigned long long>(
            g_producer != nullptr ? g_producer->last_produced : 0U),
        static_cast<unsigned long long>(
            g_producer != nullptr ? g_producer->dropped_frames : 0U));
}

} // namespace

std::string_view ToString(const GpuStreamProducerStatus status) noexcept {
    switch (status) {
    case GpuStreamProducerStatus::Ok: return "ok";
    case GpuStreamProducerStatus::AlreadyStarted: return "already-started";
    case GpuStreamProducerStatus::InvalidSession: return "invalid-session";
    case GpuStreamProducerStatus::OutOfMemory: return "out-of-memory";
    case GpuStreamProducerStatus::MissingGlEntryPoint:
        return "missing-gl-entry-point";
    case GpuStreamProducerStatus::InteropInitializationFailure:
        return "interop-initialization-failure";
    case GpuStreamProducerStatus::TextureRegistrationFailure:
        return "texture-registration-failure";
    case GpuStreamProducerStatus::FenceCreationFailure:
        return "fence-creation-failure";
    case GpuStreamProducerStatus::FenceShareFailure:
        return "fence-share-failure";
    case GpuStreamProducerStatus::FramebufferCreationFailure:
        return "framebuffer-creation-failure";
    case GpuStreamProducerStatus::NoCurrentGlContext:
        return "no-current-gl-context";
    case GpuStreamProducerStatus::InvalidViewport: return "invalid-viewport";
    case GpuStreamProducerStatus::ResolveTargetFailure:
        return "resolve-target-failure";
    case GpuStreamProducerStatus::FramebufferIncomplete:
        return "framebuffer-incomplete";
    case GpuStreamProducerStatus::InteropLockFailure:
        return "interop-lock-failure";
    case GpuStreamProducerStatus::GlBlitFailure: return "gl-blit-failure";
    case GpuStreamProducerStatus::InteropUnlockFailure:
        return "interop-unlock-failure";
    case GpuStreamProducerStatus::FenceSignalFailure:
        return "fence-signal-failure";
    case GpuStreamProducerStatus::FenceDeviceRemoved:
        return "fence-device-removed";
    case GpuStreamProducerStatus::FenceValueExhausted:
        return "fence-value-exhausted";
    case GpuStreamProducerStatus::NotStarted: return "not-started";
    case GpuStreamProducerStatus::ContextMismatch: return "context-mismatch";
    case GpuStreamProducerStatus::CleanupFailure: return "cleanup-failure";
    case GpuStreamProducerStatus::PrimaryInteropWaitFailure:
        return "primary-interop-wait-failure";
    case GpuStreamProducerStatus::PrimaryInteropSignalFailure:
        return "primary-interop-signal-failure";
    }
    return "unknown";
}

GpuStreamProducerStatus StartGpuStreamProducer(
    const ipc::SessionNonce session_nonce,
    GpuStreamProducerDiagnostic* const diagnostic,
    const std::uint32_t width, const std::uint32_t height) noexcept {
    if (diagnostic != nullptr) {
        *diagnostic = MakeGpuStreamProducerDiagnostic(
            GpuStreamProducerStatus::NotStarted);
    }
    if (!ipc::IsValid(session_nonce) || width==0 || height==0 || width>4096 || height>4096) {
        StoreDiagnostic(diagnostic, GpuStreamProducerStatus::InvalidSession);
        return GpuStreamProducerStatus::InvalidSession;
    }
    if (InterlockedCompareExchange(
            &g_lifecycle, static_cast<LONG>(ProducerLifecycle::Starting),
            static_cast<LONG>(ProducerLifecycle::Dormant)) !=
        static_cast<LONG>(ProducerLifecycle::Dormant)) {
        StoreDiagnostic(diagnostic, GpuStreamProducerStatus::AlreadyStarted);
        return GpuStreamProducerStatus::AlreadyStarted;
    }
    if (wglGetCurrentContext() == nullptr) {
        return FailStart(nullptr,
                         GpuStreamProducerStatus::NoCurrentGlContext,
                         "no-current-wgl-context", nullptr, diagnostic);
    }

    auto* const state = new (std::nothrow) ProducerState{};
    if (state == nullptr) {
        return FailStart(nullptr, GpuStreamProducerStatus::OutOfMemory,
                         "producer-state-allocation", nullptr, diagnostic);
    }
    const char* missing = nullptr;
    state->width=width; state->height=height;
    if (LoadGlFunctions(state->gl, missing) !=
        GpuStreamProducerStatus::Ok) {
        return FailStart(state,
                         GpuStreamProducerStatus::MissingGlEntryPoint,
                         missing != nullptr ? missing : "unknown-gl-entry",
                         nullptr, diagnostic);
    }

    const ipc::GpuStreamObjectName legacy_color_name =
        ipc::MakeGpuStreamObjectName(session_nonce,
                                     ipc::GpuStreamObjectKind::Color);
    std::array<ipc::GpuStreamObjectName, ipc::kGpuStreamSlotCount>
        color_names{};
    std::array<std::wstring_view, ipc::kGpuStreamSlotCount>
        color_name_views{};
    for (std::size_t slot = 0; slot < color_names.size(); ++slot) {
        color_names[slot] =
            ipc::MakeGpuStreamColorObjectName(session_nonce, slot);
        color_name_views[slot] = color_names[slot].c_str();
    }
    const ipc::GpuStreamObjectName ready_name =
        ipc::MakeGpuStreamObjectName(session_nonce,
                                     ipc::GpuStreamObjectKind::ReadyFence);
    const ipc::GpuStreamObjectName consumed_name =
        ipc::MakeGpuStreamObjectName(session_nonce,
                                     ipc::GpuStreamObjectKind::ConsumedFence);
    bool color_names_valid = legacy_color_name.valid();
    for (const auto& name : color_names) {
        color_names_valid = color_names_valid && name.valid();
    }
    if (!color_names_valid || !ready_name.valid() ||
        !consumed_name.valid()) {
        return FailStart(state, GpuStreamProducerStatus::InvalidSession,
                         "gpu-stream-object-name", nullptr, diagnostic);
    }

    GlExtD3D12Diagnostic gl_ext_diagnostic{};
    GlExtD3D12Status gl_ext_status =
        state->gl_ext_bridge.Initialize(gl_ext_diagnostic);
    if (gl_ext_status == GlExtD3D12Status::Ok) {
        gl_ext_status = state->gl_ext_bridge.CreateNamedStreams(
            {width, height,
             ipc::kGpuStreamDxgiFormatRgba8Unorm},
            {color_name_views, ready_name.c_str(), consumed_name.c_str()},
            gl_ext_diagnostic);
    }
    if (gl_ext_status == GlExtD3D12Status::Ok) {
        state->backend = GpuStreamInteropBackend::GlExtD3D12;
    } else {
        (void)LogFormat(
            "gpu-stream-primary-unavailable pid=%lu backend=gl-ext-d3d12 "
            "interop=%s hr=0x%08lX error=%lu gl=0x%08lX detail=%s "
            "fallback=nv-dx-interop",
            static_cast<unsigned long>(GetCurrentProcessId()),
            ToString(gl_ext_diagnostic.status).data(),
            static_cast<unsigned long>(gl_ext_diagnostic.hresult),
            static_cast<unsigned long>(gl_ext_diagnostic.win32_error),
            static_cast<unsigned long>(gl_ext_diagnostic.gl_error),
            gl_ext_diagnostic.detail);
        GlExtD3D12Diagnostic cleanup_diagnostic{};
        if (state->gl_ext_bridge.Shutdown(cleanup_diagnostic) !=
            GlExtD3D12Status::Ok) {
            StoreDiagnostic(diagnostic,
                            GpuStreamProducerStatus::CleanupFailure,
                            &cleanup_diagnostic);
            return FailStart(state,
                             GpuStreamProducerStatus::CleanupFailure,
                             "gl-ext-primary-cleanup", nullptr,
                             diagnostic);
        }

        NvDxInteropDiagnostic interop_diagnostic{};
        if (state->nv_bridge.Initialize({}, interop_diagnostic) !=
            NvDxInteropStatus::Ok) {
            return FailStart(
                state,
                GpuStreamProducerStatus::InteropInitializationFailure,
                "nv-dx-fallback-initialize", &interop_diagnostic,
                diagnostic);
        }
        state->backend = GpuStreamInteropBackend::NvDxInterop;
        const NvDxInteropTextureDescription texture_description{
            width, height,
            ipc::kGpuStreamDxgiFormatRgba8Unorm,
            NvDxInteropAccess::WriteDiscard};
        if (state->nv_bridge.RegisterSharedTexture(
                texture_description, legacy_color_name.c_str(),
                interop_diagnostic) != NvDxInteropStatus::Ok) {
            return FailStart(
                state, GpuStreamProducerStatus::TextureRegistrationFailure,
                "nv-dx-fallback-color-texture", &interop_diagnostic,
                diagnostic);
        }

        auto* const device = static_cast<ID3D11Device5*>(
            state->nv_bridge.d3d11_device5_native());
        HRESULT result = device->CreateFence(
            0U, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence),
            reinterpret_cast<void**>(&state->ready_fence));
        if (SUCCEEDED(result)) {
            result = device->CreateFence(
                0U, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence),
                reinterpret_cast<void**>(&state->consumed_fence));
        }
        if (FAILED(result) || state->ready_fence == nullptr ||
            state->consumed_fence == nullptr) {
            interop_diagnostic = {};
            interop_diagnostic.hresult = static_cast<std::int32_t>(result);
            return FailStart(state,
                             GpuStreamProducerStatus::FenceCreationFailure,
                             "d3d11-fallback-create-fence",
                             &interop_diagnostic, diagnostic);
        }

        result = state->ready_fence->CreateSharedHandle(
            nullptr, GENERIC_ALL, ready_name.c_str(), &state->ready_handle);
        if (SUCCEEDED(result)) {
            result = state->consumed_fence->CreateSharedHandle(
                nullptr, GENERIC_ALL, consumed_name.c_str(),
                &state->consumed_handle);
        }
        if (FAILED(result) || state->ready_handle == nullptr ||
            state->consumed_handle == nullptr) {
            interop_diagnostic = {};
            interop_diagnostic.hresult = static_cast<std::int32_t>(result);
            return FailStart(state,
                             GpuStreamProducerStatus::FenceShareFailure,
                             "d3d11-fallback-named-fences",
                             &interop_diagnostic, diagnostic);
        }
    }

    DrainGlErrors();
    state->gl.gen_framebuffers(1, &state->output_fbo);
    if (state->output_fbo == 0U || glGetError() != GL_NO_ERROR) {
        return FailStart(
            state, GpuStreamProducerStatus::FramebufferCreationFailure,
            "output-framebuffer", nullptr, diagnostic);
    }

    state->owner_context=wglGetCurrentContext();
    state->owner_thread=GetCurrentThreadId();
    g_producer = state;
    InterlockedExchange(&g_lifecycle,
                        static_cast<LONG>(ProducerLifecycle::Running));
    (void)LogFormat(
        "gpu-stream-producer-ready pid=%lu tid=%lu backend=%s "
        "slots=%lu width=%lu height=%lu "
        "format=%lu adapter=%08lX:%08lX color0=%ls ready=%ls consumed=%ls",
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId()),
        BackendName(state->backend),
        static_cast<unsigned long>(
            state->backend == GpuStreamInteropBackend::GlExtD3D12
                ? state->gl_ext_bridge.active_slot_count()
                : 1U),
        static_cast<unsigned long>(width),
        static_cast<unsigned long>(height),
        static_cast<unsigned long>(ipc::kGpuStreamDxgiFormatRgba8Unorm),
        static_cast<unsigned long>(
            state->backend == GpuStreamInteropBackend::GlExtD3D12
                ? state->gl_ext_bridge.adapter_luid().high_part
                : state->nv_bridge.adapter_luid().high_part),
        static_cast<unsigned long>(
            state->backend == GpuStreamInteropBackend::GlExtD3D12
                ? state->gl_ext_bridge.adapter_luid().low_part
                : state->nv_bridge.adapter_luid().low_part),
        state->backend == GpuStreamInteropBackend::GlExtD3D12
            ? color_names[0].c_str()
            : legacy_color_name.c_str(),
        ready_name.c_str(), consumed_name.c_str());
    if (state->backend == GpuStreamInteropBackend::GlExtD3D12) {
        StoreDiagnostic(diagnostic, GpuStreamProducerStatus::Ok,
                        &gl_ext_diagnostic);
    } else {
        StoreDiagnostic(diagnostic, GpuStreamProducerStatus::Ok);
    }
    return GpuStreamProducerStatus::Ok;
}

void NotifyGpuStreamContextDeleted(void* context) noexcept {
    if (!g_producer || g_producer->owner_context!=context ||
        g_producer->owner_thread!=GetCurrentThreadId()) return;
    g_producer->context_deleted=true;
    g_producer->context_recovery.Reset();
    // Dead-context names must never be deleted in the replacement context.
    g_producer->output_fbo=g_producer->resolve_fbo=g_producer->resolve_texture=0;
    g_producer->resolve_width=g_producer->resolve_height=0;
    (void)LogFormat("gpu-stream-context-deleted context=%p last_ready=%llu",context,
        static_cast<unsigned long long>(g_producer->last_produced));
}

void RecoverGpuStreamContext() noexcept {
    if (!IsGpuStreamProducerRunning() || !g_producer || !g_producer->context_deleted ||
        g_producer->owner_thread!=GetCurrentThreadId() || !wglGetCurrentContext()) return;
    auto& state=*g_producer;
    if (!state.context_recovery.BeginAttempt(GetTickCount64(),
        OwningGlContextIsCurrent(state))) return;
    const char* missing{};
    GlExtD3D12Diagnostic diagnostic{};
    if (!state.context_recovery.imported() &&
        state.backend==GpuStreamInteropBackend::GlExtD3D12 &&
        LoadGlFunctions(state.gl,missing)==GpuStreamProducerStatus::Ok &&
        state.gl_ext_bridge.RebindAfterContextReplacement(diagnostic)==GlExtD3D12Status::Ok) {
        state.context_recovery.MarkImported();
        state.owner_context=wglGetCurrentContext();
    }
    if (!state.context_recovery.imported()) {
        (void)LogFormat("gpu-stream-context-recovery-pending stage=reimport retry_ms=1000 detail=%s",diagnostic.detail);
        return;
    }
    DrainGlErrors();
    if (!state.output_fbo) state.gl.gen_framebuffers(1,&state.output_fbo);
    if (!state.output_fbo || glGetError()!=GL_NO_ERROR) {
        (void)LogFormat("gpu-stream-context-recovery-pending stage=output-fbo retry_ms=1000");
        return;
    }
    state.context_deleted=false;
    (void)LogFormat("gpu-stream-context-recovered context=%p last_ready=%llu",state.owner_context,
        static_cast<unsigned long long>(state.last_produced));
}
bool GpuStreamContextRecoveryPending() noexcept {
    return IsGpuStreamProducerRunning() && g_producer->context_deleted;
}

void ProduceGpuStreamAfterScenePass(
    const std::uint64_t scene_frame_id) noexcept {
    if (AtomicRead(&g_lifecycle) !=
            static_cast<LONG>(ProducerLifecycle::Running) ||
        g_producer == nullptr) {
        return;
    }
    ProducerState& state = *g_producer;
    // Graphics menus can briefly make another WGL context current. Its draws
    // do not belong to this stream; resume when the owning context returns.
    if (state.context_deleted || !OwningGlContextIsCurrent(state)) return;
    const std::uint64_t consumed =
        state.backend == GpuStreamInteropBackend::GlExtD3D12
            ? state.gl_ext_bridge.consumed_value()
            : state.consumed_fence->GetCompletedValue();
    std::size_t slot = 0U;
    GpuStreamFrameDecision decision{};
    if (state.backend == GpuStreamInteropBackend::GlExtD3D12) {
        const std::uint64_t candidate = state.last_produced + 1U;
        slot = ipc::GpuStreamSlotForSequence(candidate);
        decision = DecideGpuStreamRingFrame(
            state.last_produced, state.slot_last_use[slot], consumed);
    } else {
        decision = DecideGpuStreamFrame(state.last_produced, consumed);
    }
    switch (decision.disposition) {
    case GpuStreamFrameDisposition::DropConsumerBusy:
        ++state.dropped_frames;
        return;
    case GpuStreamFrameDisposition::StopDeviceRemoved:
        DisableAfterRuntimeFailure(
            GpuStreamProducerStatus::FenceDeviceRemoved, scene_frame_id);
        return;
    case GpuStreamFrameDisposition::StopFenceValueExhausted:
        DisableAfterRuntimeFailure(
            GpuStreamProducerStatus::FenceValueExhausted, scene_frame_id);
        return;
    case GpuStreamFrameDisposition::Produce:
        break;
    }

    const std::uint64_t selected_slot_last_use =
        state.backend == GpuStreamInteropBackend::GlExtD3D12
            ? state.slot_last_use[slot]
            : 0U;
    if (state.backend == GpuStreamInteropBackend::GlExtD3D12 &&
        selected_slot_last_use != 0U) {
        GlExtD3D12Diagnostic interop_diagnostic{};
        if (state.gl_ext_bridge.WaitConsumed(
                slot, selected_slot_last_use, interop_diagnostic) !=
            GlExtD3D12Status::Ok) {
            (void)LogFormat(
                "gpu-stream-primary-wait-failed pid=%lu scene_frame=%llu "
                "interop=%s hr=0x%08lX error=%lu gl=0x%08lX detail=%s",
                static_cast<unsigned long>(GetCurrentProcessId()),
                static_cast<unsigned long long>(scene_frame_id),
                ToString(interop_diagnostic.status).data(),
                static_cast<unsigned long>(interop_diagnostic.hresult),
                static_cast<unsigned long>(interop_diagnostic.win32_error),
                static_cast<unsigned long>(interop_diagnostic.gl_error),
                interop_diagnostic.detail);
            DisableAfterRuntimeFailure(
                GpuStreamProducerStatus::PrimaryInteropWaitFailure,
                scene_frame_id);
            return;
        }
    }

    const GpuStreamProducerStatus blit_status =
        BlitBackBuffer(state, slot);
    if (blit_status != GpuStreamProducerStatus::Ok) {
        DisableAfterRuntimeFailure(blit_status, scene_frame_id);
        return;
    }
    if (state.backend == GpuStreamInteropBackend::GlExtD3D12) {
        GlExtD3D12Diagnostic interop_diagnostic{};
        if (state.gl_ext_bridge.SignalReady(
                slot, decision.next_ready_value, interop_diagnostic) !=
            GlExtD3D12Status::Ok) {
            (void)LogFormat(
                "gpu-stream-primary-signal-failed pid=%lu "
                "scene_frame=%llu interop=%s hr=0x%08lX error=%lu "
                "gl=0x%08lX detail=%s",
                static_cast<unsigned long>(GetCurrentProcessId()),
                static_cast<unsigned long long>(scene_frame_id),
                ToString(interop_diagnostic.status).data(),
                static_cast<unsigned long>(interop_diagnostic.hresult),
                static_cast<unsigned long>(interop_diagnostic.win32_error),
                static_cast<unsigned long>(interop_diagnostic.gl_error),
                interop_diagnostic.detail);
            DisableAfterRuntimeFailure(
                GpuStreamProducerStatus::PrimaryInteropSignalFailure,
                scene_frame_id);
            return;
        }
    } else {
        auto* const context = static_cast<ID3D11DeviceContext4*>(
            state.nv_bridge.d3d11_context4_native());
        const HRESULT signal_result =
            context->Signal(state.ready_fence, decision.next_ready_value);
        if (FAILED(signal_result)) {
            DisableAfterRuntimeFailure(
                GpuStreamProducerStatus::FenceSignalFailure,
                scene_frame_id);
            return;
        }
        context->Flush();
    }
    state.last_produced = decision.next_ready_value;
    if (state.backend == GpuStreamInteropBackend::GlExtD3D12) {
        state.slot_last_use[slot] = decision.next_ready_value;
    }
    if (!state.first_frame_logged) {
        state.first_frame_logged = true;
        (void)LogFormat(
            "gpu-stream-first-frame pid=%lu scene_frame=%llu backend=%s "
            "ready=%llu slot=%lu",
            static_cast<unsigned long>(GetCurrentProcessId()),
            static_cast<unsigned long long>(scene_frame_id),
            BackendName(state.backend),
            static_cast<unsigned long long>(state.last_produced),
            static_cast<unsigned long>(slot));
    }
}

GpuStreamProducerStatus ShutdownGpuStreamProducer() noexcept {
    const LONG lifecycle = AtomicRead(&g_lifecycle);
    if (lifecycle == static_cast<LONG>(ProducerLifecycle::Dormant) ||
        lifecycle == static_cast<LONG>(ProducerLifecycle::Stopped)) {
        return GpuStreamProducerStatus::NotStarted;
    }
    if (g_producer == nullptr) {
        InterlockedExchange(&g_lifecycle,
                            static_cast<LONG>(ProducerLifecycle::Stopped));
        return GpuStreamProducerStatus::Ok;
    }
    if (!OwningGlContextIsCurrent(*g_producer)) {
        return GpuStreamProducerStatus::ContextMismatch;
    }
    if (!CleanupProducer(*g_producer)) {
        return GpuStreamProducerStatus::CleanupFailure;
    }
    delete g_producer;
    g_producer = nullptr;
    InterlockedExchange(&g_lifecycle,
                        static_cast<LONG>(ProducerLifecycle::Stopped));
    return GpuStreamProducerStatus::Ok;
}

bool IsGpuStreamProducerRunning() noexcept {
    return AtomicRead(&g_lifecycle) ==
               static_cast<LONG>(ProducerLifecycle::Running) &&
           g_producer != nullptr;
}

} // namespace k2vr::game32

#else

namespace k2vr::game32 {

std::string_view ToString(const GpuStreamProducerStatus) noexcept {
    return "unsupported-platform";
}

GpuStreamProducerStatus StartGpuStreamProducer(
    ipc::SessionNonce,
    GpuStreamProducerDiagnostic* const diagnostic, std::uint32_t, std::uint32_t) noexcept {
    if (diagnostic != nullptr) {
        *diagnostic = MakeGpuStreamProducerDiagnostic(
            GpuStreamProducerStatus::InteropInitializationFailure);
    }
    return GpuStreamProducerStatus::InteropInitializationFailure;
}

void ProduceGpuStreamAfterScenePass(std::uint64_t) noexcept {}

GpuStreamProducerStatus ShutdownGpuStreamProducer() noexcept {
    return GpuStreamProducerStatus::NotStarted;
}

bool IsGpuStreamProducerRunning() noexcept { return false; }
bool GpuStreamContextRecoveryPending() noexcept { return false; }

} // namespace k2vr::game32

#endif
