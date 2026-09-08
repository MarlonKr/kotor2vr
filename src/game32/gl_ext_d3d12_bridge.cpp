#include "gl_ext_d3d12_bridge.hpp"

// The external-object loader/import sequence is adapted from the pinned,
// MIT-licensed DLSS5-Feeder `feed_gl.h` implementation.

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_2.h>
#include <gl/GL.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <new>

static_assert(sizeof(void*) == 4,
              "the in-game GL/D3D12 bridge must be built as x86");
static_assert(sizeof(LUID) == 8U);

namespace k2vr::game32 {
namespace {

constexpr GLenum kGlNumExtensions = 0x821DU;
constexpr GLenum kGlRgba8 = 0x8058U;
constexpr GLenum kGlDedicatedMemoryObjectExt = 0x9581U;
constexpr GLenum kGlLayoutGeneralExt = 0x958DU;
constexpr GLenum kGlHandleTypeD3D12ResourceExt = 0x958AU;
constexpr GLenum kGlHandleTypeD3D12FenceExt = 0x9594U;
constexpr GLenum kGlD3D12FenceValueExt = 0x9595U;
constexpr GLenum kGlDeviceLuidExt = 0x9599U;

using GlGetStringi = const GLubyte*(APIENTRY*)(GLenum, GLuint);
using GlGetUnsignedBytevExt = void(APIENTRY*)(GLenum, GLubyte*);
using GlCreateMemoryObjectsExt = void(APIENTRY*)(GLsizei, GLuint*);
using GlDeleteMemoryObjectsExt = void(APIENTRY*)(GLsizei, const GLuint*);
using GlMemoryObjectParameterivExt =
    void(APIENTRY*)(GLuint, GLenum, const GLint*);
using GlTexStorageMem2DExt =
    void(APIENTRY*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint,
                    std::uint64_t);
using GlImportMemoryWin32HandleExt =
    void(APIENTRY*)(GLuint, std::uint64_t, GLenum, void*);
using GlGenSemaphoresExt = void(APIENTRY*)(GLsizei, GLuint*);
using GlDeleteSemaphoresExt = void(APIENTRY*)(GLsizei, const GLuint*);
using GlSemaphoreParameterui64vExt =
    void(APIENTRY*)(GLuint, GLenum, const std::uint64_t*);
using GlWaitSemaphoreExt =
    void(APIENTRY*)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*,
                    const GLenum*);
using GlSignalSemaphoreExt =
    void(APIENTRY*)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*,
                    const GLenum*);
using GlImportSemaphoreWin32HandleExt =
    void(APIENTRY*)(GLuint, GLenum, void*);

template <typename Interface>
void SafeRelease(Interface*& value) noexcept {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

void CopyDetail(char (&destination)[192], const char* const source) noexcept {
    if (source == nullptr) {
        destination[0] = '\0';
        return;
    }
    const std::size_t length =
        (std::min)(std::strlen(source), std::size(destination) - 1U);
    std::memcpy(destination, source, length);
    destination[length] = '\0';
}

[[nodiscard]] GlExtD3D12AdapterLuid PortableLuid(
    const LUID luid) noexcept {
    return {luid.LowPart, static_cast<std::int32_t>(luid.HighPart)};
}

[[nodiscard]] FARPROC ResolveGlProcedure(
    HMODULE const open_gl, const char* const name) noexcept {
    const PROC context_procedure = wglGetProcAddress(name);
    if (IsUsableGlExtD3D12ProcAddressValue(
            reinterpret_cast<std::uintptr_t>(context_procedure))) {
        return reinterpret_cast<FARPROC>(context_procedure);
    }
    const FARPROC exported = GetProcAddress(open_gl, name);
    return IsUsableGlExtD3D12ProcAddressValue(
               reinterpret_cast<std::uintptr_t>(exported))
               ? exported
               : nullptr;
}

[[nodiscard]] GLenum DrainGlErrors() noexcept {
    GLenum first = GL_NO_ERROR;
    for (unsigned attempt = 0U; attempt < 32U; ++attempt) {
        const GLenum error = glGetError();
        if (error == GL_NO_ERROR) {
            break;
        }
        if (first == GL_NO_ERROR) {
            first = error;
        }
    }
    return first;
}

[[nodiscard]] bool CopyObjectName(
    const std::wstring_view source,
    std::array<wchar_t, 128>& destination) noexcept {
    if (!IsValidGlExtD3D12SharedObjectName(source)) {
        return false;
    }
    std::copy(source.begin(), source.end(), destination.begin());
    destination[source.size()] = L'\0';
    return true;
}

} // namespace

struct GlExtD3D12BridgeNativeState {
    HMODULE open_gl{}; // Borrowed: this can be ReShade's proxy opengl32.dll.
    HGLRC owner_context{};
    DWORD owner_thread{};

    GlGetStringi get_string_i{};
    GlGetUnsignedBytevExt get_unsigned_byte_v{};
    GlCreateMemoryObjectsExt create_memory_objects{};
    GlDeleteMemoryObjectsExt delete_memory_objects{};
    GlMemoryObjectParameterivExt memory_object_parameter_i{};
    GlTexStorageMem2DExt texture_storage_memory_2d{};
    GlImportMemoryWin32HandleExt import_memory_handle{};
    GlGenSemaphoresExt generate_semaphores{};
    GlDeleteSemaphoresExt delete_semaphores{};
    GlSemaphoreParameterui64vExt semaphore_parameter_u64{};
    GlWaitSemaphoreExt wait_semaphore{};
    GlSignalSemaphoreExt signal_semaphore{};
    GlImportSemaphoreWin32HandleExt import_semaphore_handle{};

    ID3D12Device* device{};
    std::array<ID3D12Resource*, kGlExtD3D12StreamSlotCount> colors{};
    ID3D12Fence* ready_fence{};
    ID3D12Fence* consumed_fence{};
    std::array<HANDLE, kGlExtD3D12StreamSlotCount> color_handles{};
    HANDLE ready_handle{};
    HANDLE consumed_handle{};

    std::array<GLuint, kGlExtD3D12StreamSlotCount> gl_memories{};
    std::array<GLuint, kGlExtD3D12StreamSlotCount> gl_textures{};
    GLuint gl_ready_semaphore{};
    GLuint gl_consumed_semaphore{};

    GlExtD3D12AdapterLuid adapter_luid{};
    std::uint32_t advertised_extension_mask{};
    std::size_t active_slot_count{};
    std::array<std::uint64_t, kGlExtD3D12StreamSlotCount>
        slot_last_ready_signals{};
    std::array<std::uint64_t, kGlExtD3D12StreamSlotCount>
        slot_last_consumed_waits{};
    std::uint64_t last_ready_signal{};
    std::uint64_t last_consumed_wait{};
    bool live_probe_used{};
};

namespace {

[[nodiscard]] bool ContextMatches(
    const GlExtD3D12BridgeNativeState& state) noexcept {
    return state.owner_context != nullptr &&
           wglGetCurrentContext() == state.owner_context;
}

void SetDiagnostic(
    GlExtD3D12Diagnostic& diagnostic, const GlExtD3D12Status status,
    const char* const detail,
    const GlExtD3D12BridgeNativeState* const state = nullptr,
    const HRESULT hresult = S_OK,
    const DWORD win32_error = ERROR_SUCCESS,
    const GLenum gl_error = GL_NO_ERROR) noexcept {
    diagnostic = {};
    diagnostic.status = status;
    diagnostic.hresult = static_cast<std::int32_t>(hresult);
    diagnostic.win32_error = win32_error;
    diagnostic.gl_error = static_cast<std::uint32_t>(gl_error);
    if (state != nullptr) {
        diagnostic.adapter_luid = state->adapter_luid;
        diagnostic.advertised_extension_mask =
            state->advertised_extension_mask;
        diagnostic.live_probe_used = state->live_probe_used ? 1U : 0U;
    }
    CopyDetail(diagnostic.detail, detail);
}

[[nodiscard]] std::uint32_t SurveyExtensionMask(
    GlExtD3D12BridgeNativeState& state) noexcept {
    constexpr std::array<std::string_view, 4> names{
        "GL_EXT_memory_object", "GL_EXT_memory_object_win32",
        "GL_EXT_semaphore", "GL_EXT_semaphore_win32"};
    constexpr std::array<std::uint32_t, 4> bits{
        GlExtMemoryObject, GlExtMemoryObjectWin32, GlExtSemaphore,
        GlExtSemaphoreWin32};

    std::uint32_t mask = 0U;
    (void)DrainGlErrors();
    if (state.get_string_i != nullptr) {
        GLint count = 0;
        glGetIntegerv(kGlNumExtensions, &count);
        if (DrainGlErrors() == GL_NO_ERROR && count > 0) {
            for (GLint index = 0; index < count; ++index) {
                const auto* const raw =
                    state.get_string_i(GL_EXTENSIONS,
                                       static_cast<GLuint>(index));
                if (raw == nullptr) {
                    continue;
                }
                const std::string_view extension(
                    reinterpret_cast<const char*>(raw));
                for (std::size_t item = 0U; item < names.size(); ++item) {
                    if (extension == names[item]) {
                        mask |= bits[item];
                    }
                }
            }
        }
    }

    const auto* const legacy =
        reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (legacy != nullptr) {
        const std::string_view extensions(legacy);
        for (std::size_t item = 0U; item < names.size(); ++item) {
            if (HasExactGlExtD3D12ExtensionToken(extensions, names[item])) {
                mask |= bits[item];
            }
        }
    }
    (void)DrainGlErrors();
    return mask;
}

[[nodiscard]] GlExtD3D12Status LoadGlFunctions(
    GlExtD3D12BridgeNativeState& state,
    GlExtD3D12Diagnostic& diagnostic) noexcept {
#define K2VR_RESOLVE_GL(member, type, symbol)                                  \
    state.member = reinterpret_cast<type>(                                    \
        ResolveGlProcedure(state.open_gl, symbol));                           \
    if (state.member == nullptr) {                                            \
        SetDiagnostic(diagnostic, GlExtD3D12Status::MissingGlEntryPoint,      \
                      symbol, &state);                                        \
        return GlExtD3D12Status::MissingGlEntryPoint;                         \
    }

    state.get_string_i = reinterpret_cast<GlGetStringi>(
        ResolveGlProcedure(state.open_gl, "glGetStringi"));
    K2VR_RESOLVE_GL(get_unsigned_byte_v, GlGetUnsignedBytevExt,
                    "glGetUnsignedBytevEXT")
    K2VR_RESOLVE_GL(create_memory_objects, GlCreateMemoryObjectsExt,
                    "glCreateMemoryObjectsEXT")
    K2VR_RESOLVE_GL(delete_memory_objects, GlDeleteMemoryObjectsExt,
                    "glDeleteMemoryObjectsEXT")
    K2VR_RESOLVE_GL(memory_object_parameter_i, GlMemoryObjectParameterivExt,
                    "glMemoryObjectParameterivEXT")
    K2VR_RESOLVE_GL(texture_storage_memory_2d, GlTexStorageMem2DExt,
                    "glTexStorageMem2DEXT")
    K2VR_RESOLVE_GL(import_memory_handle, GlImportMemoryWin32HandleExt,
                    "glImportMemoryWin32HandleEXT")
    K2VR_RESOLVE_GL(generate_semaphores, GlGenSemaphoresExt,
                    "glGenSemaphoresEXT")
    K2VR_RESOLVE_GL(delete_semaphores, GlDeleteSemaphoresExt,
                    "glDeleteSemaphoresEXT")
    K2VR_RESOLVE_GL(semaphore_parameter_u64,
                    GlSemaphoreParameterui64vExt,
                    "glSemaphoreParameterui64vEXT")
    K2VR_RESOLVE_GL(wait_semaphore, GlWaitSemaphoreExt,
                    "glWaitSemaphoreEXT")
    K2VR_RESOLVE_GL(signal_semaphore, GlSignalSemaphoreExt,
                    "glSignalSemaphoreEXT")
    K2VR_RESOLVE_GL(import_semaphore_handle,
                    GlImportSemaphoreWin32HandleExt,
                    "glImportSemaphoreWin32HandleEXT")
#undef K2VR_RESOLVE_GL

    state.advertised_extension_mask = SurveyExtensionMask(state);

    // Old games can receive a deliberately capped extension string. A live
    // object probe distinguishes that case from an unsupported driver.
    state.live_probe_used = true;
    (void)DrainGlErrors();
    GLuint memory = 0U;
    state.create_memory_objects(1, &memory);
    const bool memory_ok =
        memory != 0U && DrainGlErrors() == GL_NO_ERROR;
    if (memory != 0U) {
        state.delete_memory_objects(1, &memory);
    }
    GLuint semaphore = 0U;
    state.generate_semaphores(1, &semaphore);
    const bool semaphore_ok =
        semaphore != 0U && DrainGlErrors() == GL_NO_ERROR;
    if (semaphore != 0U) {
        state.delete_semaphores(1, &semaphore);
    }
    const GLenum probe_error = DrainGlErrors();
    if (!memory_ok || !semaphore_ok || probe_error != GL_NO_ERROR) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::InteropProbeFailure,
                      "external memory/semaphore live probe failed", &state,
                      S_OK, ERROR_SUCCESS, probe_error);
        return GlExtD3D12Status::InteropProbeFailure;
    }
    return GlExtD3D12Status::Ok;
}

[[nodiscard]] GlExtD3D12Status QueryGlAdapterLuid(
    GlExtD3D12BridgeNativeState& state,
    GlExtD3D12Diagnostic& diagnostic) noexcept {
    std::array<GLubyte, sizeof(LUID)> bytes{};
    (void)DrainGlErrors();
    state.get_unsigned_byte_v(kGlDeviceLuidExt, bytes.data());
    const GLenum error = DrainGlErrors();
    LUID luid{};
    std::memcpy(&luid, bytes.data(), sizeof(luid));
    state.adapter_luid = PortableLuid(luid);
    if (error != GL_NO_ERROR ||
        !IsValidGlExtD3D12AdapterLuid(state.adapter_luid)) {
        SetDiagnostic(diagnostic,
                      GlExtD3D12Status::GlDeviceLuidQueryFailure,
                      "glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT) failed",
                      &state, S_OK, ERROR_SUCCESS, error);
        return GlExtD3D12Status::GlDeviceLuidQueryFailure;
    }
    return GlExtD3D12Status::Ok;
}

[[nodiscard]] GlExtD3D12Status CreateDeviceForGlAdapter(
    GlExtD3D12BridgeNativeState& state,
    GlExtD3D12Diagnostic& diagnostic) noexcept {
    IDXGIFactory1* factory = nullptr;
    const HRESULT factory_result = CreateDXGIFactory1(
        __uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(factory_result) || factory == nullptr) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::DxgiFactoryFailure,
                      "CreateDXGIFactory1 failed", &state, factory_result);
        return GlExtD3D12Status::DxgiFactoryFailure;
    }

    IDXGIAdapter1* selected = nullptr;
    for (UINT index = 0U;; ++index) {
        IDXGIAdapter1* candidate = nullptr;
        const HRESULT result = factory->EnumAdapters1(index, &candidate);
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(result) || candidate == nullptr) {
            continue;
        }
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(candidate->GetDesc1(&description)) &&
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0U &&
            SameGlExtD3D12AdapterLuid(
                PortableLuid(description.AdapterLuid), state.adapter_luid)) {
            selected = candidate;
            candidate = nullptr;
        }
        SafeRelease(candidate);
        if (selected != nullptr) {
            break;
        }
    }
    SafeRelease(factory);
    if (selected == nullptr) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::AdapterUnavailable,
                      "the OpenGL adapter LUID is unavailable through DXGI",
                      &state);
        return GlExtD3D12Status::AdapterUnavailable;
    }

    const HRESULT device_result = D3D12CreateDevice(
        selected, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
        reinterpret_cast<void**>(&state.device));
    SafeRelease(selected);
    if (FAILED(device_result) || state.device == nullptr) {
        SetDiagnostic(diagnostic,
                      GlExtD3D12Status::D3d12DeviceCreationFailure,
                      "D3D12CreateDevice failed on the OpenGL adapter",
                      &state, device_result);
        return GlExtD3D12Status::D3d12DeviceCreationFailure;
    }
    return GlExtD3D12Status::Ok;
}

void ReleaseD3D12Stream(GlExtD3D12BridgeNativeState& state) noexcept {
    for (HANDLE& color_handle : state.color_handles) {
        if (color_handle != nullptr) {
            CloseHandle(color_handle);
            color_handle = nullptr;
        }
    }
    if (state.ready_handle != nullptr) {
        CloseHandle(state.ready_handle);
        state.ready_handle = nullptr;
    }
    if (state.consumed_handle != nullptr) {
        CloseHandle(state.consumed_handle);
        state.consumed_handle = nullptr;
    }
    for (ID3D12Resource*& color : state.colors) {
        SafeRelease(color);
    }
    SafeRelease(state.ready_fence);
    SafeRelease(state.consumed_fence);
    state.active_slot_count = 0U;
    state.slot_last_ready_signals.fill(0U);
    state.slot_last_consumed_waits.fill(0U);
    state.last_ready_signal = 0U;
    state.last_consumed_wait = 0U;
}

void ReleaseGlStream(GlExtD3D12BridgeNativeState& state) noexcept {
    for (GLuint& gl_texture : state.gl_textures) {
        if (gl_texture != 0U) {
            glDeleteTextures(1, &gl_texture);
            gl_texture = 0U;
        }
    }
    for (GLuint& gl_memory : state.gl_memories) {
        if (gl_memory != 0U) {
            state.delete_memory_objects(1, &gl_memory);
            gl_memory = 0U;
        }
    }
    std::array<GLuint, 2> semaphores{
        state.gl_ready_semaphore, state.gl_consumed_semaphore};
    if (semaphores[0] != 0U || semaphores[1] != 0U) {
        state.delete_semaphores(
            static_cast<GLsizei>(semaphores.size()), semaphores.data());
        state.gl_ready_semaphore = 0U;
        state.gl_consumed_semaphore = 0U;
    }
}

void AbandonState(GlExtD3D12BridgeNativeState& state) noexcept {
    if (ContextMatches(state)) {
        ReleaseGlStream(state);
    }
    ReleaseD3D12Stream(state);
    SafeRelease(state.device);
}

[[nodiscard]] bool HasD3D12Stream(
    const GlExtD3D12BridgeNativeState& state) noexcept {
    if (!IsValidGlExtD3D12StreamSlot(0U, state.active_slot_count) ||
        state.ready_fence == nullptr || state.consumed_fence == nullptr ||
        state.ready_handle == nullptr || state.consumed_handle == nullptr) {
        return false;
    }
    for (std::size_t slot = 0U; slot < state.active_slot_count; ++slot) {
        if (state.colors[slot] == nullptr ||
            state.color_handles[slot] == nullptr) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool HasCompleteStream(
    const GlExtD3D12BridgeNativeState& state) noexcept {
    if (!HasD3D12Stream(state) || state.gl_ready_semaphore == 0U ||
        state.gl_consumed_semaphore == 0U) {
        return false;
    }
    for (std::size_t slot = 0U; slot < state.active_slot_count; ++slot) {
        if (state.gl_memories[slot] == 0U || state.gl_textures[slot] == 0U) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] GlExtD3D12Status FailStreamCreation(
    GlExtD3D12BridgeNativeState& state,
    GlExtD3D12Diagnostic& diagnostic, const GlExtD3D12Status status,
    const char* const detail, const HRESULT hresult = S_OK,
    const DWORD win32_error = ERROR_SUCCESS,
    const GLenum gl_error = GL_NO_ERROR) noexcept {
    ReleaseGlStream(state);
    ReleaseD3D12Stream(state);
    SetDiagnostic(diagnostic, status, detail, &state, hresult, win32_error,
                  gl_error);
    return status;
}

// Import cleanup owns only names created in the current GL context. Shared
// D3D12 objects/handles and fence history survive any failed rebind attempt.
[[nodiscard]] GlExtD3D12Status FailGlImport(
    GlExtD3D12BridgeNativeState& state,
    GlExtD3D12Diagnostic& diagnostic, const GlExtD3D12Status status,
    const char* const detail, const HRESULT hresult = S_OK,
    const DWORD win32_error = ERROR_SUCCESS,
    const GLenum gl_error = GL_NO_ERROR) noexcept {
    ReleaseGlStream(state);
    SetDiagnostic(diagnostic, status, detail, &state, hresult, win32_error,
                  gl_error);
    return status;
}

[[nodiscard]] GlExtD3D12Status ImportD3D12StreamIntoGl(
    GlExtD3D12BridgeNativeState& state,
    GlExtD3D12Diagnostic& diagnostic) noexcept {
    const auto resource = state.colors[0]->GetDesc();
    const auto active_slot_count = state.active_slot_count;
    const D3D12_RESOURCE_ALLOCATION_INFO allocation =
        state.device->GetResourceAllocationInfo(0U, 1U, &resource);
    if (allocation.SizeInBytes == 0U ||
        allocation.SizeInBytes ==
            (std::numeric_limits<std::uint64_t>::max)()) {
        return FailGlImport(
            state, diagnostic, GlExtD3D12Status::TextureCreationFailure,
            "D3D12 resource allocation size is invalid");
    }

    (void)DrainGlErrors();
    state.create_memory_objects(static_cast<GLsizei>(active_slot_count),
                                state.gl_memories.data());
    GLenum gl_error = DrainGlErrors();
    for (std::size_t slot = 0U; slot < active_slot_count; ++slot) {
        if (state.gl_memories[slot] == 0U) {
            return FailGlImport(
                state, diagnostic,
                GlExtD3D12Status::GlMemoryObjectCreationFailure,
                "glCreateMemoryObjectsEXT returned an empty color slot",
                S_OK, ERROR_SUCCESS, gl_error);
        }
    }
    if (gl_error != GL_NO_ERROR) {
        return FailGlImport(
            state, diagnostic,
            GlExtD3D12Status::GlMemoryObjectCreationFailure,
            "glCreateMemoryObjectsEXT failed", S_OK, ERROR_SUCCESS,
            gl_error);
    }

    const GLint dedicated = GL_TRUE;
    for (std::size_t slot = 0U; slot < active_slot_count; ++slot) {
        state.memory_object_parameter_i(
            state.gl_memories[slot], kGlDedicatedMemoryObjectExt,
            &dedicated);
        state.import_memory_handle(
            state.gl_memories[slot], allocation.SizeInBytes,
            kGlHandleTypeD3D12ResourceExt, state.color_handles[slot]);
        gl_error = DrainGlErrors();
        if (gl_error != GL_NO_ERROR) {
            return FailGlImport(
                state, diagnostic,
                GlExtD3D12Status::GlMemoryImportFailure,
                "glImportMemoryWin32HandleEXT failed for a color slot",
                S_OK, ERROR_SUCCESS, gl_error);
        }
    }

    glGenTextures(static_cast<GLsizei>(active_slot_count),
                  state.gl_textures.data());
    gl_error = DrainGlErrors();
    for (std::size_t slot = 0U; slot < active_slot_count; ++slot) {
        if (state.gl_textures[slot] == 0U) {
            return FailGlImport(
                state, diagnostic,
                GlExtD3D12Status::GlTextureCreationFailure,
                "glGenTextures returned an empty color slot", S_OK,
                ERROR_SUCCESS, gl_error);
        }
    }
    if (gl_error != GL_NO_ERROR) {
        return FailGlImport(
            state, diagnostic,
            GlExtD3D12Status::GlTextureCreationFailure,
            "glGenTextures failed", S_OK, ERROR_SUCCESS, gl_error);
    }

    GLint previous_texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    for (std::size_t slot = 0U; slot < active_slot_count; ++slot) {
        glBindTexture(GL_TEXTURE_2D, state.gl_textures[slot]);
        state.texture_storage_memory_2d(
            GL_TEXTURE_2D, 1, kGlRgba8,
            static_cast<GLsizei>(resource.Width),
            static_cast<GLsizei>(resource.Height),
            state.gl_memories[slot], 0U);
        gl_error = DrainGlErrors();
        if (gl_error != GL_NO_ERROR) {
            glBindTexture(GL_TEXTURE_2D,
                          static_cast<GLuint>(previous_texture));
            return FailGlImport(
                state, diagnostic,
                GlExtD3D12Status::GlTextureStorageFailure,
                "glTexStorageMem2DEXT failed for a color slot", S_OK,
                ERROR_SUCCESS, gl_error);
        }
    }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));
    gl_error = DrainGlErrors();
    if (gl_error != GL_NO_ERROR) {
        return FailGlImport(
            state, diagnostic,
            GlExtD3D12Status::GlTextureStorageFailure,
            "restoring the previous GL texture binding failed", S_OK,
            ERROR_SUCCESS, gl_error);
    }

    std::array<GLuint, 2> semaphores{};
    state.generate_semaphores(static_cast<GLsizei>(semaphores.size()),
                              semaphores.data());
    state.gl_ready_semaphore = semaphores[0];
    state.gl_consumed_semaphore = semaphores[1];
    gl_error = DrainGlErrors();
    if (state.gl_ready_semaphore == 0U ||
        state.gl_consumed_semaphore == 0U || gl_error != GL_NO_ERROR) {
        return FailGlImport(
            state, diagnostic,
            GlExtD3D12Status::GlSemaphoreCreationFailure,
            "glGenSemaphoresEXT failed", S_OK, ERROR_SUCCESS, gl_error);
    }
    state.import_semaphore_handle(
        state.gl_ready_semaphore, kGlHandleTypeD3D12FenceExt,
        state.ready_handle);
    state.import_semaphore_handle(
        state.gl_consumed_semaphore, kGlHandleTypeD3D12FenceExt,
        state.consumed_handle);
    gl_error = DrainGlErrors();
    if (gl_error != GL_NO_ERROR) {
        return FailGlImport(
            state, diagnostic,
            GlExtD3D12Status::GlSemaphoreImportFailure,
            "glImportSemaphoreWin32HandleEXT failed", S_OK,
            ERROR_SUCCESS, gl_error);
    }

    SetDiagnostic(diagnostic, GlExtD3D12Status::Ok,
                  active_slot_count == 1U
                      ? "named D3D12 texture and fences are imported into GL"
                      : "three named D3D12 textures and shared fences are imported into GL",
                  &state);
    return GlExtD3D12Status::Ok;
}

[[nodiscard]] GlExtD3D12Status CreateNamedStreamsImpl(
    GlExtD3D12BridgeNativeState& state,
    const GlExtD3D12TextureDescription& description,
    const std::array<std::wstring_view, kGlExtD3D12StreamSlotCount>&
        color_names_view,
    const std::size_t active_slot_count,
    const std::wstring_view ready_name_view,
    const std::wstring_view consumed_name_view,
    GlExtD3D12Diagnostic& diagnostic) noexcept {
    const GlExtD3D12Status decision = DecideGlExtD3D12CreateStream(
        true, HasD3D12Stream(state), ContextMatches(state));
    if (decision != GlExtD3D12Status::Ok) {
        SetDiagnostic(diagnostic, decision, ToString(decision).data(), &state);
        return decision;
    }
    if (!IsValidGlExtD3D12StreamSlot(0U, active_slot_count)) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::InvalidStreamSlot,
                      "the stream must contain between one and three slots",
                      &state);
        return GlExtD3D12Status::InvalidStreamSlot;
    }
    const GlExtD3D12Status description_status =
        ValidateGlExtD3D12TextureDescription(description);
    if (description_status != GlExtD3D12Status::Ok) {
        SetDiagnostic(diagnostic, description_status,
                      "only bounded single-sample RGBA8 is supported",
                      &state);
        return description_status;
    }
    if (!IsValidGlExtD3D12SharedObjectName(ready_name_view) ||
        !IsValidGlExtD3D12SharedObjectName(consumed_name_view)) {
        SetDiagnostic(diagnostic,
                      GlExtD3D12Status::InvalidSharedObjectName,
                      "both shared fence names must be bounded", &state);
        return GlExtD3D12Status::InvalidSharedObjectName;
    }
    if (ready_name_view == consumed_name_view) {
        SetDiagnostic(diagnostic,
                      GlExtD3D12Status::DuplicateSharedObjectName,
                      "the ready and consumed fence names must differ",
                      &state);
        return GlExtD3D12Status::DuplicateSharedObjectName;
    }
    for (std::size_t slot = 0U; slot < active_slot_count; ++slot) {
        if (!IsValidGlExtD3D12SharedObjectName(color_names_view[slot])) {
            SetDiagnostic(diagnostic,
                          GlExtD3D12Status::InvalidSharedObjectName,
                          "every shared color name must be bounded", &state);
            return GlExtD3D12Status::InvalidSharedObjectName;
        }
        if (color_names_view[slot] == ready_name_view ||
            color_names_view[slot] == consumed_name_view) {
            SetDiagnostic(diagnostic,
                          GlExtD3D12Status::DuplicateSharedObjectName,
                          "color and fence names must be distinct", &state);
            return GlExtD3D12Status::DuplicateSharedObjectName;
        }
        for (std::size_t other = slot + 1U; other < active_slot_count;
             ++other) {
            if (color_names_view[slot] == color_names_view[other]) {
                SetDiagnostic(diagnostic,
                              GlExtD3D12Status::DuplicateSharedObjectName,
                              "every color slot needs a distinct name",
                              &state);
                return GlExtD3D12Status::DuplicateSharedObjectName;
            }
        }
    }

    std::array<std::array<wchar_t, 128>, kGlExtD3D12StreamSlotCount>
        color_names{};
    std::array<wchar_t, 128> ready_name{};
    std::array<wchar_t, 128> consumed_name{};
    for (std::size_t slot = 0U; slot < active_slot_count; ++slot) {
        if (!CopyObjectName(color_names_view[slot], color_names[slot])) {
            SetDiagnostic(diagnostic,
                          GlExtD3D12Status::InvalidSharedObjectName,
                          "shared color name copy failed", &state);
            return GlExtD3D12Status::InvalidSharedObjectName;
        }
    }
    if (!CopyObjectName(ready_name_view, ready_name) ||
        !CopyObjectName(consumed_name_view, consumed_name)) {
        SetDiagnostic(diagnostic,
                      GlExtD3D12Status::InvalidSharedObjectName,
                      "shared fence name copy failed", &state);
        return GlExtD3D12Status::InvalidSharedObjectName;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1U;
    heap.VisibleNodeMask = 1U;

    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = description.width;
    resource.Height = description.height;
    resource.DepthOrArraySize = 1U;
    resource.MipLevels = 1U;
    resource.Format = static_cast<DXGI_FORMAT>(description.dxgi_format);
    resource.SampleDesc.Count = 1U;
    resource.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    state.active_slot_count = active_slot_count;
    HRESULT result = S_OK;
    for (std::size_t slot = 0U; slot < active_slot_count; ++slot) {
        result = state.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_SHARED, &resource,
            D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
            reinterpret_cast<void**>(&state.colors[slot]));
        if (FAILED(result) || state.colors[slot] == nullptr) {
            return FailStreamCreation(
                state, diagnostic,
                GlExtD3D12Status::TextureCreationFailure,
                "CreateCommittedResource failed for a color slot", result);
        }
    }

    result = state.device->CreateFence(
        0U, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence),
        reinterpret_cast<void**>(&state.ready_fence));
    if (SUCCEEDED(result)) {
        result = state.device->CreateFence(
            0U, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence),
            reinterpret_cast<void**>(&state.consumed_fence));
    }
    if (FAILED(result) || state.ready_fence == nullptr ||
        state.consumed_fence == nullptr) {
        return FailStreamCreation(
            state, diagnostic, GlExtD3D12Status::FenceCreationFailure,
            "D3D12 shared fence creation failed", result);
    }

    for (std::size_t slot = 0U; slot < active_slot_count; ++slot) {
        result = state.device->CreateSharedHandle(
            state.colors[slot], nullptr, GENERIC_ALL,
            color_names[slot].data(), &state.color_handles[slot]);
        if (FAILED(result) || state.color_handles[slot] == nullptr) {
            return FailStreamCreation(
                state, diagnostic,
                GlExtD3D12Status::SharedHandleCreationFailure,
                "D3D12 named color handle creation failed", result,
                GetLastError());
        }
    }
    result = state.device->CreateSharedHandle(
        state.ready_fence, nullptr, GENERIC_ALL, ready_name.data(),
        &state.ready_handle);
    if (SUCCEEDED(result)) {
        result = state.device->CreateSharedHandle(
            state.consumed_fence, nullptr, GENERIC_ALL,
            consumed_name.data(), &state.consumed_handle);
    }
    if (FAILED(result) || state.ready_handle == nullptr ||
        state.consumed_handle == nullptr) {
        return FailStreamCreation(
            state, diagnostic,
            GlExtD3D12Status::SharedHandleCreationFailure,
            "D3D12 named fence handle creation failed", result,
            GetLastError());
    }

    const auto import_status = ImportD3D12StreamIntoGl(state, diagnostic);
    if (import_status != GlExtD3D12Status::Ok) {
        ReleaseD3D12Stream(state);
    }
    return import_status;
}

} // namespace

std::string_view ToString(const GlExtD3D12Status status) noexcept {
    switch (status) {
    case GlExtD3D12Status::Ok: return "ok";
    case GlExtD3D12Status::AlreadyInitialized: return "already-initialized";
    case GlExtD3D12Status::NotInitialized: return "not-initialized";
    case GlExtD3D12Status::StreamAlreadyCreated:
        return "stream-already-created";
    case GlExtD3D12Status::StreamNotCreated: return "stream-not-created";
    case GlExtD3D12Status::InvalidTextureDescription:
        return "invalid-texture-description";
    case GlExtD3D12Status::InvalidSharedObjectName:
        return "invalid-shared-object-name";
    case GlExtD3D12Status::DuplicateSharedObjectName:
        return "duplicate-shared-object-name";
    case GlExtD3D12Status::InvalidFenceValue: return "invalid-fence-value";
    case GlExtD3D12Status::NoCurrentGlContext:
        return "no-current-gl-context";
    case GlExtD3D12Status::ContextMismatch: return "context-mismatch";
    case GlExtD3D12Status::OpenGlRuntimeUnavailable:
        return "opengl-runtime-unavailable";
    case GlExtD3D12Status::MissingGlEntryPoint:
        return "missing-gl-entry-point";
    case GlExtD3D12Status::InteropProbeFailure:
        return "interop-probe-failure";
    case GlExtD3D12Status::GlDeviceLuidQueryFailure:
        return "gl-device-luid-query-failure";
    case GlExtD3D12Status::DxgiFactoryFailure: return "dxgi-factory-failure";
    case GlExtD3D12Status::AdapterUnavailable: return "adapter-unavailable";
    case GlExtD3D12Status::D3d12DeviceCreationFailure:
        return "d3d12-device-creation-failure";
    case GlExtD3D12Status::TextureCreationFailure:
        return "texture-creation-failure";
    case GlExtD3D12Status::FenceCreationFailure:
        return "fence-creation-failure";
    case GlExtD3D12Status::SharedHandleCreationFailure:
        return "shared-handle-creation-failure";
    case GlExtD3D12Status::GlMemoryObjectCreationFailure:
        return "gl-memory-object-creation-failure";
    case GlExtD3D12Status::GlMemoryImportFailure:
        return "gl-memory-import-failure";
    case GlExtD3D12Status::GlTextureCreationFailure:
        return "gl-texture-creation-failure";
    case GlExtD3D12Status::GlTextureStorageFailure:
        return "gl-texture-storage-failure";
    case GlExtD3D12Status::GlSemaphoreCreationFailure:
        return "gl-semaphore-creation-failure";
    case GlExtD3D12Status::GlSemaphoreImportFailure:
        return "gl-semaphore-import-failure";
    case GlExtD3D12Status::GlSemaphoreSignalFailure:
        return "gl-semaphore-signal-failure";
    case GlExtD3D12Status::GlSemaphoreWaitFailure:
        return "gl-semaphore-wait-failure";
    case GlExtD3D12Status::CleanupFailure: return "cleanup-failure";
    case GlExtD3D12Status::OutOfMemory: return "out-of-memory";
    case GlExtD3D12Status::InvalidStreamSlot:
        return "invalid-stream-slot";
    case GlExtD3D12Status::StreamSlotNotConsumed:
        return "stream-slot-not-consumed";
    case GlExtD3D12Status::AdapterMismatch: return "adapter-mismatch";
    }
    return "unknown";
}

GlExtD3D12Bridge::~GlExtD3D12Bridge() {
    if (state_ != nullptr) {
        AbandonState(*state_);
        delete state_;
        state_ = nullptr;
    }
}

GlExtD3D12Status GlExtD3D12Bridge::Initialize(
    GlExtD3D12Diagnostic& diagnostic) noexcept {
    if (state_ != nullptr) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::AlreadyInitialized,
                      "the bridge is already initialized", state_);
        return GlExtD3D12Status::AlreadyInitialized;
    }
    if (wglGetCurrentContext() == nullptr) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::NoCurrentGlContext,
                      "a WGL context must be current on the render thread");
        return GlExtD3D12Status::NoCurrentGlContext;
    }

    auto* const candidate =
        new (std::nothrow) GlExtD3D12BridgeNativeState{};
    if (candidate == nullptr) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::OutOfMemory,
                      "native bridge state allocation failed");
        return GlExtD3D12Status::OutOfMemory;
    }
    candidate->owner_context = wglGetCurrentContext();
    candidate->owner_thread = GetCurrentThreadId();
    candidate->open_gl = GetModuleHandleW(L"opengl32.dll");
    if (candidate->open_gl == nullptr) {
        const DWORD error = GetLastError();
        SetDiagnostic(diagnostic,
                      GlExtD3D12Status::OpenGlRuntimeUnavailable,
                      "opengl32.dll is not loaded", candidate, S_OK, error);
        delete candidate;
        return GlExtD3D12Status::OpenGlRuntimeUnavailable;
    }

    GlExtD3D12Status status = LoadGlFunctions(*candidate, diagnostic);
    if (status == GlExtD3D12Status::Ok) {
        status = QueryGlAdapterLuid(*candidate, diagnostic);
    }
    if (status == GlExtD3D12Status::Ok) {
        status = CreateDeviceForGlAdapter(*candidate, diagnostic);
    }
    if (status != GlExtD3D12Status::Ok) {
        AbandonState(*candidate);
        delete candidate;
        return status;
    }

    state_ = candidate;
    SetDiagnostic(diagnostic, GlExtD3D12Status::Ok,
                  "GL external objects and matching D3D12 device are ready",
                  state_);
    return GlExtD3D12Status::Ok;
}

GlExtD3D12Status GlExtD3D12Bridge::RebindAfterContextReplacement(
    GlExtD3D12Diagnostic& diagnostic) noexcept {
    if (!initialized()) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::NotInitialized,
                      "context replacement requires an initialized bridge");
        return GlExtD3D12Status::NotInitialized;
    }

    // The caller has witnessed context destruction. Numeric HGLRC/GLuint
    // reuse is possible, so do not compare handles or delete any old GL name.
    // Invalidate even on failure, preventing stale names from being used later.
    state_->gl_memories.fill(0U);
    state_->gl_textures.fill(0U);
    state_->gl_ready_semaphore = 0U;
    state_->gl_consumed_semaphore = 0U;
    state_->owner_context = nullptr;
    state_->owner_thread = 0U;
    state_->slot_last_consumed_waits.fill(0U);
    state_->last_consumed_wait = 0U;

    if (!HasD3D12Stream(*state_)) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::StreamNotCreated,
                      "context replacement requires retained D3D12 stream resources",
                      state_);
        return GlExtD3D12Status::StreamNotCreated;
    }
    const HGLRC replacement = wglGetCurrentContext();
    if (replacement == nullptr) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::NoCurrentGlContext,
                      "make the replacement game context current before retrying; D3D12 retained",
                      state_);
        return GlExtD3D12Status::NoCurrentGlContext;
    }

    // This staging copy only borrows the existing D3D12 pointers and handles;
    // no AbandonState/FailStreamCreation may run on it. Commit GL state only
    // after every import succeeds. Import failure deletes only its new names.
    auto candidate = *state_;
    candidate.owner_context = replacement;
    candidate.owner_thread = GetCurrentThreadId();
    candidate.open_gl = GetModuleHandleW(L"opengl32.dll");
    if (candidate.open_gl == nullptr) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::OpenGlRuntimeUnavailable,
                      "opengl32.dll is not loaded; D3D12 retained", state_,
                      S_OK, GetLastError());
        return GlExtD3D12Status::OpenGlRuntimeUnavailable;
    }
    auto status = LoadGlFunctions(candidate, diagnostic);
    if (status == GlExtD3D12Status::Ok) {
        status = QueryGlAdapterLuid(candidate, diagnostic);
    }
    if (status != GlExtD3D12Status::Ok) {
        return status;
    }
    if (!SameGlExtD3D12AdapterLuid(candidate.adapter_luid,
                                  state_->adapter_luid)) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::AdapterMismatch,
                      "replacement GL adapter differs from retained D3D12 adapter",
                      &candidate);
        return GlExtD3D12Status::AdapterMismatch;
    }
    status = ImportD3D12StreamIntoGl(candidate, diagnostic);
    if (status != GlExtD3D12Status::Ok) {
        return status;
    }
    *state_ = candidate;
    SetDiagnostic(diagnostic, GlExtD3D12Status::Ok,
                  "replacement GL imports ready; D3D12 and ready sequence retained; reacquire used slots",
                  state_);
    return GlExtD3D12Status::Ok;
}

GlExtD3D12Status GlExtD3D12Bridge::CreateNamedStream(
    const GlExtD3D12TextureDescription& description,
    const GlExtD3D12ObjectNames& names,
    GlExtD3D12Diagnostic& diagnostic) noexcept {
    std::array<std::wstring_view, kGlExtD3D12StreamSlotCount> colors{};
    colors[0] = names.color;
    if (state_ == nullptr) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::NotInitialized,
                      "the bridge is not initialized");
        return GlExtD3D12Status::NotInitialized;
    }
    return CreateNamedStreamsImpl(*state_, description, colors, 1U,
                                  names.ready_fence,
                                  names.consumed_fence, diagnostic);
}

GlExtD3D12Status GlExtD3D12Bridge::CreateNamedStreams(
    const GlExtD3D12TextureDescription& description,
    const GlExtD3D12RingObjectNames& names,
    GlExtD3D12Diagnostic& diagnostic) noexcept {
    if (state_ == nullptr) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::NotInitialized,
                      "the bridge is not initialized");
        return GlExtD3D12Status::NotInitialized;
    }
    return CreateNamedStreamsImpl(
        *state_, description, names.colors, names.colors.size(),
        names.ready_fence, names.consumed_fence, diagnostic);
}

GlExtD3D12Status GlExtD3D12Bridge::SignalReady(
    const std::uint64_t value,
    GlExtD3D12Diagnostic& diagnostic) noexcept {
    return SignalReady(0U, value, diagnostic);
}

GlExtD3D12Status GlExtD3D12Bridge::SignalReady(
    const std::size_t slot, const std::uint64_t value,
    GlExtD3D12Diagnostic& diagnostic) noexcept {
    if (state_ == nullptr) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::NotInitialized,
                      "the bridge is not initialized");
        return GlExtD3D12Status::NotInitialized;
    }
    if (!IsValidGlExtD3D12StreamSlot(slot, state_->active_slot_count)) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::InvalidStreamSlot,
                      "the requested color slot is not active", state_);
        return GlExtD3D12Status::InvalidStreamSlot;
    }
    const GlExtD3D12Status decision = DecideGlExtD3D12SemaphoreOperation(
        true, HasCompleteStream(*state_), ContextMatches(*state_), value,
        state_->last_ready_signal);
    if (decision != GlExtD3D12Status::Ok) {
        SetDiagnostic(diagnostic, decision, ToString(decision).data(), state_);
        return decision;
    }
    if (!CanSignalGlExtD3D12StreamSlot(
            state_->slot_last_ready_signals[slot],
            state_->slot_last_consumed_waits[slot])) {
        SetDiagnostic(diagnostic,
                      GlExtD3D12Status::StreamSlotNotConsumed,
                      "WaitConsumed must reacquire this color slot first",
                      state_);
        return GlExtD3D12Status::StreamSlotNotConsumed;
    }

    (void)DrainGlErrors();
    state_->semaphore_parameter_u64(
        state_->gl_ready_semaphore, kGlD3D12FenceValueExt, &value);
    const GLuint texture = state_->gl_textures[slot];
    const GLenum layout = kGlLayoutGeneralExt;
    state_->signal_semaphore(state_->gl_ready_semaphore, 0U, nullptr, 1U,
                             &texture, &layout);
    // SignalSemaphoreEXT implies a flush, and the explicit flush mirrors the
    // locally hardware-validated feeder path.
    glFlush();
    const GLenum error = DrainGlErrors();
    if (error != GL_NO_ERROR) {
        SetDiagnostic(diagnostic,
                      GlExtD3D12Status::GlSemaphoreSignalFailure,
                      "GL failed to release the color slot to D3D12", state_,
                      S_OK, ERROR_SUCCESS, error);
        return GlExtD3D12Status::GlSemaphoreSignalFailure;
    }
    state_->slot_last_ready_signals[slot] = value;
    state_->last_ready_signal = value;
    SetDiagnostic(diagnostic, GlExtD3D12Status::Ok,
                  "the slot ready fence signal was queued", state_);
    return GlExtD3D12Status::Ok;
}

GlExtD3D12Status GlExtD3D12Bridge::WaitConsumed(
    const std::uint64_t value,
    GlExtD3D12Diagnostic& diagnostic) noexcept {
    return WaitConsumed(0U, value, diagnostic);
}

GlExtD3D12Status GlExtD3D12Bridge::WaitConsumed(
    const std::size_t slot, const std::uint64_t value,
    GlExtD3D12Diagnostic& diagnostic) noexcept {
    if (state_ == nullptr) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::NotInitialized,
                      "the bridge is not initialized");
        return GlExtD3D12Status::NotInitialized;
    }
    if (!IsValidGlExtD3D12StreamSlot(slot, state_->active_slot_count)) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::InvalidStreamSlot,
                      "the requested color slot is not active", state_);
        return GlExtD3D12Status::InvalidStreamSlot;
    }
    const GlExtD3D12Status decision = DecideGlExtD3D12SemaphoreOperation(
        true, HasCompleteStream(*state_), ContextMatches(*state_), value,
        state_->slot_last_consumed_waits[slot]);
    if (decision != GlExtD3D12Status::Ok) {
        SetDiagnostic(diagnostic, decision, ToString(decision).data(), state_);
        return decision;
    }
    if (value != state_->slot_last_ready_signals[slot]) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::InvalidFenceValue,
                      "the consumed value does not own this color slot",
                      state_);
        return GlExtD3D12Status::InvalidFenceValue;
    }
    const auto completed = state_->consumed_fence->GetCompletedValue();
    if (completed < value ||
        completed == (std::numeric_limits<std::uint64_t>::max)()) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::GlSemaphoreWaitFailure,
                      "the consumed fence value is not complete", state_);
        return GlExtD3D12Status::GlSemaphoreWaitFailure;
    }

    (void)DrainGlErrors();
    state_->semaphore_parameter_u64(
        state_->gl_consumed_semaphore, kGlD3D12FenceValueExt, &value);
    const GLuint texture = state_->gl_textures[slot];
    const GLenum layout = kGlLayoutGeneralExt;
    state_->wait_semaphore(state_->gl_consumed_semaphore, 0U, nullptr, 1U,
                           &texture, &layout);
    const GLenum error = DrainGlErrors();
    if (error != GL_NO_ERROR) {
        SetDiagnostic(diagnostic,
                      GlExtD3D12Status::GlSemaphoreWaitFailure,
                      "GL failed to reacquire the consumed color slot",
                      state_, S_OK, ERROR_SUCCESS, error);
        return GlExtD3D12Status::GlSemaphoreWaitFailure;
    }
    state_->slot_last_consumed_waits[slot] = value;
    // Texture ownership is per slot: after a rebind an older ready value can
    // legitimately be reacquired after a newer slot. Duplicate waits for that
    // same slot remain invalid, while the shared fence itself stays monotonic.
    state_->last_consumed_wait = (std::max)(state_->last_consumed_wait, value);
    SetDiagnostic(diagnostic, GlExtD3D12Status::Ok,
                  "the slot consumed fence wait was queued", state_);
    return GlExtD3D12Status::Ok;
}

GlExtD3D12Status GlExtD3D12Bridge::ReleaseStream(
    GlExtD3D12Diagnostic& diagnostic) noexcept {
    if (state_ == nullptr) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::NotInitialized,
                      "the bridge is not initialized");
        return GlExtD3D12Status::NotInitialized;
    }
    const bool has_color_resource = std::any_of(
        state_->colors.begin(), state_->colors.end(),
        [](const ID3D12Resource* const color) { return color != nullptr; });
    const bool has_gl_texture = std::any_of(
        state_->gl_textures.begin(), state_->gl_textures.end(),
        [](const GLuint texture) { return texture != 0U; });
    if (!HasCompleteStream(*state_) && !has_color_resource &&
        !has_gl_texture && state_->ready_fence == nullptr &&
        state_->consumed_fence == nullptr) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::Ok,
                      "no stream is allocated", state_);
        return GlExtD3D12Status::Ok;
    }
    if (!ContextMatches(*state_)) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::ContextMismatch,
                      "stream release requires the owning WGL context",
                      state_);
        return GlExtD3D12Status::ContextMismatch;
    }
    (void)DrainGlErrors();
    ReleaseGlStream(*state_);
    const GLenum error = DrainGlErrors();
    ReleaseD3D12Stream(*state_);
    if (error != GL_NO_ERROR) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::CleanupFailure,
                      "GL stream object deletion reported an error", state_,
                      S_OK, ERROR_SUCCESS, error);
        return GlExtD3D12Status::CleanupFailure;
    }
    SetDiagnostic(diagnostic, GlExtD3D12Status::Ok,
                  "shared GL/D3D12 stream resources released", state_);
    return GlExtD3D12Status::Ok;
}

GlExtD3D12Status GlExtD3D12Bridge::Shutdown(
    GlExtD3D12Diagnostic& diagnostic) noexcept {
    if (state_ == nullptr) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::Ok,
                      "the bridge is already shut down");
        return GlExtD3D12Status::Ok;
    }
    if (!ContextMatches(*state_)) {
        SetDiagnostic(diagnostic, GlExtD3D12Status::ContextMismatch,
                      "Shutdown requires the owning WGL context", state_);
        return GlExtD3D12Status::ContextMismatch;
    }
    const GlExtD3D12Status release = ReleaseStream(diagnostic);
    if (release != GlExtD3D12Status::Ok) {
        return release;
    }
    SafeRelease(state_->device);
    delete state_;
    state_ = nullptr;
    SetDiagnostic(diagnostic, GlExtD3D12Status::Ok,
                  "the bridge is shut down");
    return GlExtD3D12Status::Ok;
}

bool GlExtD3D12Bridge::initialized() const noexcept {
    return state_ != nullptr && state_->device != nullptr;
}

bool GlExtD3D12Bridge::stream_created() const noexcept {
    return state_ != nullptr && HasCompleteStream(*state_);
}

bool GlExtD3D12Bridge::owning_gl_context_is_current() const noexcept {
    return state_ != nullptr && ContextMatches(*state_);
}

GlExtD3D12AdapterLuid GlExtD3D12Bridge::adapter_luid() const noexcept {
    return state_ != nullptr ? state_->adapter_luid
                             : GlExtD3D12AdapterLuid{};
}

std::uint32_t GlExtD3D12Bridge::advertised_extension_mask() const noexcept {
    return state_ != nullptr ? state_->advertised_extension_mask : 0U;
}

bool GlExtD3D12Bridge::live_probe_used() const noexcept {
    return state_ != nullptr && state_->live_probe_used;
}

std::uint32_t GlExtD3D12Bridge::gl_texture_name() const noexcept {
    return gl_texture_name(0U);
}

std::uint32_t GlExtD3D12Bridge::gl_texture_name(
    const std::size_t slot) const noexcept {
    return state_ != nullptr &&
                   IsValidGlExtD3D12StreamSlot(
                       slot, state_->active_slot_count)
               ? state_->gl_textures[slot]
               : 0U;
}

std::size_t GlExtD3D12Bridge::active_slot_count() const noexcept {
    return state_ != nullptr ? state_->active_slot_count : 0U;
}

std::uint64_t GlExtD3D12Bridge::consumed_value() const noexcept {
    return state_ != nullptr && state_->consumed_fence != nullptr
               ? state_->consumed_fence->GetCompletedValue()
               : 0U;
}

std::uint64_t GlExtD3D12Bridge::ready_value() const noexcept {
    return state_ != nullptr && state_->ready_fence != nullptr
               ? state_->ready_fence->GetCompletedValue()
               : 0U;
}

} // namespace k2vr::game32

#else

namespace k2vr::game32 {

std::string_view ToString(const GlExtD3D12Status status) noexcept {
    return status == GlExtD3D12Status::Ok ? "ok" : "unsupported-platform";
}

GlExtD3D12Bridge::~GlExtD3D12Bridge() = default;

} // namespace k2vr::game32

#endif
