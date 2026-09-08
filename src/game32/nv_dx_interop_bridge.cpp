#include "nv_dx_interop_bridge.hpp"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <gl/GL.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <new>

namespace k2vr::game32 {
namespace {

static_assert(sizeof(void*) == 4,
              "NV_DX_interop bridge belongs in the injected x86 module");

using WglDxOpenDeviceNv = HANDLE(WINAPI*)(void* device);
using WglDxCloseDeviceNv = BOOL(WINAPI*)(HANDLE device);
using WglDxSetResourceShareHandleNv = BOOL(WINAPI*)(void* object,
                                                    HANDLE share_handle);
using WglDxRegisterObjectNv = HANDLE(WINAPI*)(HANDLE device, void* object,
                                              GLuint name, GLenum type,
                                              GLenum access);
using WglDxUnregisterObjectNv = BOOL(WINAPI*)(HANDLE device, HANDLE object);
using WglDxLockObjectsNv = BOOL(WINAPI*)(HANDLE device, GLint count,
                                        HANDLE* objects);
using WglDxUnlockObjectsNv = BOOL(WINAPI*)(HANDLE device, GLint count,
                                          HANDLE* objects);
using WglGetExtensionsStringArb = const char*(WINAPI*)(HDC device_context);
using WglGetExtensionsStringExt = const char*(WINAPI*)();

constexpr char kInterop2Extension[] = "WGL_NV_DX_interop2";

void CopyDetail(char (&destination)[192], const char* const source) noexcept {
    destination[0] = '\0';
    if (source == nullptr) {
        return;
    }
    const std::size_t length =
        (std::min)(std::strlen(source), sizeof(destination) - 1U);
    std::memcpy(destination, source, length);
    destination[length] = '\0';
}

[[nodiscard]] FARPROC ResolveWglProcedure(HMODULE const open_gl,
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

template <typename Interface>
void SafeRelease(Interface*& value) noexcept {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

[[nodiscard]] NvDxAdapterLuid ConvertLuid(const LUID luid) noexcept {
    return {luid.LowPart, static_cast<std::int32_t>(luid.HighPart)};
}

} // namespace

struct NvDxInteropBridgeNativeState {
    HMODULE open_gl{}; // Borrowed: ReShade's already-loaded opengl32.dll.
    HGLRC owner_context{};
    DWORD owner_thread{};

    WglDxOpenDeviceNv open_device{};
    WglDxCloseDeviceNv close_device{};
    WglDxSetResourceShareHandleNv set_resource_share_handle{};
    WglDxRegisterObjectNv register_object{};
    WglDxUnregisterObjectNv unregister_object{};
    WglDxLockObjectsNv lock_objects{};
    WglDxUnlockObjectsNv unlock_objects{};

    ID3D11Device5* device{};
    ID3D11DeviceContext4* context{};
    ID3D11Texture2D* texture{};
    HANDLE shared_handle{};
    HANDLE interop_device{};
    HANDLE interop_object{};
    GLuint gl_texture{};

    NvDxAdapterLuid adapter_luid{};
    bool extension_advertised{};
    bool object_locked{};
};

namespace {

void SetDiagnostic(NvDxInteropDiagnostic& diagnostic,
                   const NvDxInteropStatus status, const char* const detail,
                   const NvDxInteropBridgeNativeState* const state = nullptr,
                   const HRESULT hresult = S_OK,
                   const DWORD win32_error = ERROR_SUCCESS) noexcept {
    diagnostic = {};
    diagnostic.status = status;
    diagnostic.hresult = static_cast<std::int32_t>(hresult);
    diagnostic.win32_error = win32_error;
    if (state != nullptr) {
        diagnostic.adapter_luid = state->adapter_luid;
        diagnostic.extension_advertised =
            state->extension_advertised ? 1U : 0U;
    }
    CopyDetail(diagnostic.detail, detail);
}

[[nodiscard]] bool ContextMatches(
    const NvDxInteropBridgeNativeState& state) noexcept {
    return wglGetCurrentContext() == state.owner_context &&
           state.owner_context != nullptr;
}

void DrainGlErrors() noexcept {
    for (unsigned attempt = 0; attempt < 16U; ++attempt) {
        if (glGetError() == GL_NO_ERROR) {
            return;
        }
    }
}

void ReleaseComAndHandles(
    NvDxInteropBridgeNativeState& state) noexcept {
    if (state.shared_handle != nullptr) {
        CloseHandle(state.shared_handle);
        state.shared_handle = nullptr;
    }
    SafeRelease(state.texture);
    SafeRelease(state.context);
    SafeRelease(state.device);
}

// Destructor-only fallback. The public Shutdown path deliberately retains state
// on a context mismatch so its caller can retry on the render thread. During DLL
// teardown there may be no surviving context, so release every CPU/COM handle and
// let the OpenGL driver reclaim any stranded registration with the process.
void AbandonNativeState(NvDxInteropBridgeNativeState& state) noexcept {
    if (ContextMatches(state)) {
        if (state.object_locked && state.interop_object != nullptr &&
            state.interop_device != nullptr) {
            HANDLE object = state.interop_object;
            if (state.unlock_objects(state.interop_device, 1, &object) != FALSE) {
                state.object_locked = false;
            }
        }
        if (!state.object_locked && state.interop_object != nullptr &&
            state.interop_device != nullptr) {
            (void)state.unregister_object(state.interop_device,
                                          state.interop_object);
            state.interop_object = nullptr;
        }
        if (state.gl_texture != 0U) {
            glDeleteTextures(1, &state.gl_texture);
            state.gl_texture = 0U;
        }
    }
    if (state.interop_device != nullptr) {
        (void)state.close_device(state.interop_device);
        state.interop_device = nullptr;
    }
    ReleaseComAndHandles(state);
}

[[nodiscard]] NvDxInteropStatus ResolveInteropFunctions(
    NvDxInteropBridgeNativeState& state,
    NvDxInteropDiagnostic& diagnostic) noexcept {
#define K2VR_RESOLVE_WGL(member, type, symbol)                                  \
    state.member = reinterpret_cast<type>(                                     \
        ResolveWglProcedure(state.open_gl, symbol));                           \
    if (state.member == nullptr) {                                             \
        SetDiagnostic(diagnostic, NvDxInteropStatus::MissingWglEntryPoint,     \
                      symbol, &state);                                         \
        return NvDxInteropStatus::MissingWglEntryPoint;                        \
    }

    K2VR_RESOLVE_WGL(open_device, WglDxOpenDeviceNv, "wglDXOpenDeviceNV")
    K2VR_RESOLVE_WGL(close_device, WglDxCloseDeviceNv, "wglDXCloseDeviceNV")
    K2VR_RESOLVE_WGL(set_resource_share_handle,
                     WglDxSetResourceShareHandleNv,
                     "wglDXSetResourceShareHandleNV")
    K2VR_RESOLVE_WGL(register_object, WglDxRegisterObjectNv,
                     "wglDXRegisterObjectNV")
    K2VR_RESOLVE_WGL(unregister_object, WglDxUnregisterObjectNv,
                     "wglDXUnregisterObjectNV")
    K2VR_RESOLVE_WGL(lock_objects, WglDxLockObjectsNv,
                     "wglDXLockObjectsNV")
    K2VR_RESOLVE_WGL(unlock_objects, WglDxUnlockObjectsNv,
                     "wglDXUnlockObjectsNV")
#undef K2VR_RESOLVE_WGL
    return NvDxInteropStatus::Ok;
}

void SurveyInteropExtension(NvDxInteropBridgeNativeState& state) noexcept {
    const auto get_arb = reinterpret_cast<WglGetExtensionsStringArb>(
        ResolveWglProcedure(state.open_gl, "wglGetExtensionsStringARB"));
    const auto get_ext = reinterpret_cast<WglGetExtensionsStringExt>(
        ResolveWglProcedure(state.open_gl, "wglGetExtensionsStringEXT"));
    const char* extensions = nullptr;
    if (get_arb != nullptr) {
        extensions = get_arb(wglGetCurrentDC());
    }
    if (extensions == nullptr && get_ext != nullptr) {
        extensions = get_ext();
    }
    state.extension_advertised =
        extensions != nullptr &&
        HasExactExtensionToken(extensions, kInterop2Extension);
}

[[nodiscard]] NvDxInteropStatus SelectNvidiaAdapter(
    const NvDxAdapterLuid requested_luid, IDXGIAdapter1*& selected,
    NvDxAdapterLuid& selected_luid, NvDxInteropDiagnostic& diagnostic) noexcept {
    IDXGIFactory1* factory = nullptr;
    const HRESULT factory_result =
        CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                           reinterpret_cast<void**>(&factory));
    if (FAILED(factory_result) || factory == nullptr) {
        SetDiagnostic(diagnostic, NvDxInteropStatus::DxgiFactoryFailure,
                      "CreateDXGIFactory1 failed", nullptr, factory_result);
        return NvDxInteropStatus::DxgiFactoryFailure;
    }

    bool saw_hardware_nvidia = false;
    SIZE_T selected_memory = 0U;
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* candidate = nullptr;
        const HRESULT enumerate_result =
            factory->EnumAdapters1(index, &candidate);
        if (enumerate_result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(enumerate_result) || candidate == nullptr) {
            continue;
        }

        DXGI_ADAPTER_DESC1 description{};
        const HRESULT description_result = candidate->GetDesc1(&description);
        if (SUCCEEDED(description_result)) {
            const NvDxAdapterCandidate identity{
                description.VendorId, description.Flags,
                ConvertLuid(description.AdapterLuid)};
            if (identity.vendor_id == kNvidiaPciVendorId &&
                (identity.flags & kDxgiAdapterSoftwareFlag) == 0U) {
                saw_hardware_nvidia = true;
            }
            if (IsEligibleNvDxAdapter(identity, requested_luid) &&
                (selected == nullptr ||
                 description.DedicatedVideoMemory > selected_memory)) {
                SafeRelease(selected);
                selected = candidate;
                selected_memory = description.DedicatedVideoMemory;
                selected_luid = identity.luid;
                candidate = nullptr;
            }
        }
        SafeRelease(candidate);
    }
    SafeRelease(factory);

    if (selected != nullptr) {
        return NvDxInteropStatus::Ok;
    }
    const NvDxInteropStatus status =
        IsValidAdapterLuid(requested_luid) && saw_hardware_nvidia
            ? NvDxInteropStatus::RequestedAdapterUnavailable
            : NvDxInteropStatus::NvidiaAdapterUnavailable;
    SetDiagnostic(diagnostic, status,
                  status == NvDxInteropStatus::RequestedAdapterUnavailable
                      ? "the requested NVIDIA adapter LUID was not found"
                      : "no hardware NVIDIA DXGI adapter was found");
    return status;
}

} // namespace

std::string_view ToString(const NvDxInteropStatus status) noexcept {
    switch (status) {
    case NvDxInteropStatus::Ok: return "ok";
    case NvDxInteropStatus::AlreadyInitialized: return "already-initialized";
    case NvDxInteropStatus::NotInitialized: return "not-initialized";
    case NvDxInteropStatus::InvalidTextureDescription:
        return "invalid-texture-description";
    case NvDxInteropStatus::NoCurrentGlContext: return "no-current-gl-context";
    case NvDxInteropStatus::OpenGlRuntimeUnavailable:
        return "opengl-runtime-unavailable";
    case NvDxInteropStatus::MissingWglEntryPoint:
        return "missing-wgl-entry-point";
    case NvDxInteropStatus::DxgiFactoryFailure: return "dxgi-factory-failure";
    case NvDxInteropStatus::NvidiaAdapterUnavailable:
        return "nvidia-adapter-unavailable";
    case NvDxInteropStatus::RequestedAdapterUnavailable:
        return "requested-adapter-unavailable";
    case NvDxInteropStatus::D3d11DeviceCreationFailure:
        return "d3d11-device-creation-failure";
    case NvDxInteropStatus::D3d11FenceInterfaceUnavailable:
        return "d3d11-fence-interface-unavailable";
    case NvDxInteropStatus::InteropDeviceOpenFailure:
        return "interop-device-open-failure";
    case NvDxInteropStatus::TextureAlreadyRegistered:
        return "texture-already-registered";
    case NvDxInteropStatus::InvalidSharedObjectName:
        return "invalid-shared-object-name";
    case NvDxInteropStatus::UnsupportedTextureFormat:
        return "unsupported-texture-format";
    case NvDxInteropStatus::TextureCreationFailure:
        return "texture-creation-failure";
    case NvDxInteropStatus::SharedResourceInterfaceUnavailable:
        return "shared-resource-interface-unavailable";
    case NvDxInteropStatus::SharedHandleCreationFailure:
        return "shared-handle-creation-failure";
    case NvDxInteropStatus::GlTextureCreationFailure:
        return "gl-texture-creation-failure";
    case NvDxInteropStatus::ShareHandleAssociationFailure:
        return "share-handle-association-failure";
    case NvDxInteropStatus::TextureRegistrationFailure:
        return "texture-registration-failure";
    case NvDxInteropStatus::ContextMismatch: return "context-mismatch";
    case NvDxInteropStatus::TextureNotRegistered:
        return "texture-not-registered";
    case NvDxInteropStatus::AlreadyLocked: return "already-locked";
    case NvDxInteropStatus::NotLocked: return "not-locked";
    case NvDxInteropStatus::LockFailure: return "lock-failure";
    case NvDxInteropStatus::UnlockFailure: return "unlock-failure";
    case NvDxInteropStatus::CleanupFailure: return "cleanup-failure";
    case NvDxInteropStatus::OutOfMemory: return "out-of-memory";
    }
    return "unknown";
}

NvDxInteropBridge::~NvDxInteropBridge() {
    if (state_ == nullptr) {
        return;
    }
    NvDxInteropDiagnostic ignored{};
    if (Shutdown(ignored) != NvDxInteropStatus::Ok && state_ != nullptr) {
        AbandonNativeState(*state_);
        delete state_;
        state_ = nullptr;
    }
}

NvDxInteropStatus NvDxInteropBridge::Initialize(
    const NvDxAdapterLuid requested_luid,
    NvDxInteropDiagnostic& diagnostic) noexcept {
    if (state_ != nullptr) {
        SetDiagnostic(diagnostic, NvDxInteropStatus::AlreadyInitialized,
                      "the bridge is already initialized", state_);
        return NvDxInteropStatus::AlreadyInitialized;
    }
    if (wglGetCurrentContext() == nullptr) {
        SetDiagnostic(diagnostic, NvDxInteropStatus::NoCurrentGlContext,
                      "Initialize requires a current WGL context");
        return NvDxInteropStatus::NoCurrentGlContext;
    }

    auto* const candidate = new (std::nothrow) NvDxInteropBridgeNativeState{};
    if (candidate == nullptr) {
        SetDiagnostic(diagnostic, NvDxInteropStatus::OutOfMemory,
                      "NativeState allocation failed");
        return NvDxInteropStatus::OutOfMemory;
    }
    candidate->owner_context = wglGetCurrentContext();
    candidate->owner_thread = GetCurrentThreadId();
    candidate->open_gl = GetModuleHandleW(L"opengl32.dll");
    if (candidate->open_gl == nullptr) {
        SetDiagnostic(diagnostic,
                      NvDxInteropStatus::OpenGlRuntimeUnavailable,
                      "opengl32.dll is not loaded", candidate,
                      S_OK, GetLastError());
        delete candidate;
        return NvDxInteropStatus::OpenGlRuntimeUnavailable;
    }

    SurveyInteropExtension(*candidate);
    const NvDxInteropStatus resolve_status =
        ResolveInteropFunctions(*candidate, diagnostic);
    if (resolve_status != NvDxInteropStatus::Ok) {
        delete candidate;
        return resolve_status;
    }

    IDXGIAdapter1* adapter = nullptr;
    NvDxAdapterLuid adapter_luid{};
    const NvDxInteropStatus adapter_status = SelectNvidiaAdapter(
        requested_luid, adapter, adapter_luid, diagnostic);
    if (adapter_status != NvDxInteropStatus::Ok) {
        delete candidate;
        return adapter_status;
    }
    candidate->adapter_luid = adapter_luid;

    constexpr D3D_FEATURE_LEVEL feature_levels[]{
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device* base_device = nullptr;
    ID3D11DeviceContext* base_context = nullptr;
    HRESULT device_result = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, feature_levels,
        static_cast<UINT>(std::size(feature_levels)), D3D11_SDK_VERSION,
        &base_device, nullptr, &base_context);
    if (device_result == E_INVALIDARG) {
        SafeRelease(base_context);
        SafeRelease(base_device);
        constexpr D3D_FEATURE_LEVEL fallback_levels[]{D3D_FEATURE_LEVEL_11_0};
        device_result = D3D11CreateDevice(
            adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, fallback_levels,
            static_cast<UINT>(std::size(fallback_levels)), D3D11_SDK_VERSION,
            &base_device, nullptr, &base_context);
    }
    SafeRelease(adapter);
    if (FAILED(device_result) || base_device == nullptr ||
        base_context == nullptr) {
        SafeRelease(base_context);
        SafeRelease(base_device);
        SetDiagnostic(diagnostic,
                      NvDxInteropStatus::D3d11DeviceCreationFailure,
                      "D3D11CreateDevice failed", candidate, device_result);
        delete candidate;
        return NvDxInteropStatus::D3d11DeviceCreationFailure;
    }

    const HRESULT device5_result = base_device->QueryInterface(
        __uuidof(ID3D11Device5),
        reinterpret_cast<void**>(&candidate->device));
    const HRESULT context4_result = base_context->QueryInterface(
        __uuidof(ID3D11DeviceContext4),
        reinterpret_cast<void**>(&candidate->context));
    SafeRelease(base_context);
    SafeRelease(base_device);
    if (FAILED(device5_result) || FAILED(context4_result) ||
        candidate->device == nullptr || candidate->context == nullptr) {
        ReleaseComAndHandles(*candidate);
        const HRESULT interface_result = FAILED(device5_result)
                                             ? device5_result
                                             : context4_result;
        SetDiagnostic(diagnostic,
                      NvDxInteropStatus::D3d11FenceInterfaceUnavailable,
                      "ID3D11Device5/ID3D11DeviceContext4 is unavailable",
                      candidate, interface_result);
        delete candidate;
        return NvDxInteropStatus::D3d11FenceInterfaceUnavailable;
    }

    candidate->interop_device = candidate->open_device(candidate->device);
    if (candidate->interop_device == nullptr) {
        const DWORD error = GetLastError();
        ReleaseComAndHandles(*candidate);
        SetDiagnostic(diagnostic,
                      NvDxInteropStatus::InteropDeviceOpenFailure,
                      "wglDXOpenDeviceNV rejected the private D3D11 device",
                      candidate, S_OK, error);
        delete candidate;
        return NvDxInteropStatus::InteropDeviceOpenFailure;
    }

    state_ = candidate;
    SetDiagnostic(
        diagnostic, NvDxInteropStatus::Ok,
        candidate->extension_advertised
            ? "WGL_NV_DX_interop2 device is ready"
            : "WGL_NV_DX_interop2 live entry points work; token was not advertised",
        candidate);
    return NvDxInteropStatus::Ok;
}

NvDxInteropStatus NvDxInteropBridge::RegisterSharedTexture(
    const NvDxInteropTextureDescription& description,
    const std::wstring_view shared_object_name,
    NvDxInteropDiagnostic& diagnostic) noexcept {
    if (state_ == nullptr) {
        SetDiagnostic(diagnostic, NvDxInteropStatus::NotInitialized,
                      "Initialize must succeed before texture registration");
        return NvDxInteropStatus::NotInitialized;
    }
    if (ValidateNvDxTextureDescription(description) !=
        NvDxInteropStatus::Ok) {
        SetDiagnostic(diagnostic,
                      NvDxInteropStatus::InvalidTextureDescription,
                      "only non-zero RGBA8 textures up to 8192x8192 are supported",
                      state_);
        return NvDxInteropStatus::InvalidTextureDescription;
    }
    if (!IsValidNvDxSharedObjectName(shared_object_name)) {
        SetDiagnostic(diagnostic,
                      NvDxInteropStatus::InvalidSharedObjectName,
                      "the shared texture requires a bounded non-empty name",
                      state_);
        return NvDxInteropStatus::InvalidSharedObjectName;
    }
    if (state_->texture != nullptr || state_->interop_object != nullptr) {
        SetDiagnostic(diagnostic,
                      NvDxInteropStatus::TextureAlreadyRegistered,
                      "release the current shared texture before rebuilding",
                      state_);
        return NvDxInteropStatus::TextureAlreadyRegistered;
    }
    if (!ContextMatches(*state_)) {
        SetDiagnostic(diagnostic, NvDxInteropStatus::ContextMismatch,
                      "the initializing WGL context is not current", state_);
        return NvDxInteropStatus::ContextMismatch;
    }

    UINT format_support = 0U;
    const auto format = static_cast<DXGI_FORMAT>(description.dxgi_format);
    const HRESULT support_result =
        state_->device->CheckFormatSupport(format, &format_support);
    constexpr UINT required_support =
        D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_RENDER_TARGET;
    if (FAILED(support_result) ||
        (format_support & required_support) != required_support) {
        SetDiagnostic(diagnostic,
                      NvDxInteropStatus::UnsupportedTextureFormat,
                      "the private D3D11 device cannot render to this format",
                      state_, support_result);
        return NvDxInteropStatus::UnsupportedTextureFormat;
    }

    D3D11_TEXTURE2D_DESC texture_description{};
    texture_description.Width = description.width;
    texture_description.Height = description.height;
    texture_description.MipLevels = 1U;
    texture_description.ArraySize = 1U;
    texture_description.Format = format;
    texture_description.SampleDesc.Count = 1U;
    texture_description.Usage = D3D11_USAGE_DEFAULT;
    texture_description.BindFlags =
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    // This is the exact combination used by the locally validated feeder for
    // resources opened by its D3D12 host.
    texture_description.MiscFlags =
        D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    wchar_t shared_name[128]{};
    std::copy(shared_object_name.begin(), shared_object_name.end(),
              shared_name);

    const auto release_attempt = [](ID3D11Texture2D*& texture,
                                    HANDLE& shared_handle,
                                    GLuint& gl_texture) noexcept {
        if (gl_texture != 0U) {
            glDeleteTextures(1, &gl_texture);
            gl_texture = 0U;
        }
        if (shared_handle != nullptr) {
            CloseHandle(shared_handle);
            shared_handle = nullptr;
        }
        SafeRelease(texture);
    };

    const auto create_attempt = [&]() noexcept {
        struct Attempt {
            ID3D11Texture2D* texture{};
            HANDLE shared_handle{};
            GLuint gl_texture{};
            NvDxInteropStatus status{NvDxInteropStatus::Ok};
        } attempt;

        const HRESULT texture_result = state_->device->CreateTexture2D(
            &texture_description, nullptr, &attempt.texture);
        if (FAILED(texture_result) || attempt.texture == nullptr) {
            SetDiagnostic(diagnostic,
                          NvDxInteropStatus::TextureCreationFailure,
                          "ID3D11Device::CreateTexture2D failed", state_,
                          texture_result);
            attempt.status = NvDxInteropStatus::TextureCreationFailure;
            return attempt;
        }

        IDXGIResource1* shared_resource = nullptr;
        const HRESULT resource_result = attempt.texture->QueryInterface(
            __uuidof(IDXGIResource1),
            reinterpret_cast<void**>(&shared_resource));
        if (FAILED(resource_result) || shared_resource == nullptr) {
            SafeRelease(attempt.texture);
            SetDiagnostic(
                diagnostic,
                NvDxInteropStatus::SharedResourceInterfaceUnavailable,
                "the texture does not expose IDXGIResource1", state_,
                resource_result);
            attempt.status =
                NvDxInteropStatus::SharedResourceInterfaceUnavailable;
            return attempt;
        }

        const HRESULT handle_result = shared_resource->CreateSharedHandle(
            nullptr,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            shared_name, &attempt.shared_handle);
        SafeRelease(shared_resource);
        if (FAILED(handle_result) || attempt.shared_handle == nullptr) {
            SafeRelease(attempt.texture);
            SetDiagnostic(diagnostic,
                          NvDxInteropStatus::SharedHandleCreationFailure,
                          "IDXGIResource1::CreateSharedHandle failed", state_,
                          handle_result);
            attempt.status = NvDxInteropStatus::SharedHandleCreationFailure;
            return attempt;
        }

        DrainGlErrors();
        glGenTextures(1, &attempt.gl_texture);
        const GLenum gl_result = glGetError();
        if (attempt.gl_texture == 0U || gl_result != GL_NO_ERROR) {
            release_attempt(attempt.texture, attempt.shared_handle,
                            attempt.gl_texture);
            SetDiagnostic(diagnostic,
                          NvDxInteropStatus::GlTextureCreationFailure,
                          "glGenTextures failed on the owning context",
                          state_);
            attempt.status = NvDxInteropStatus::GlTextureCreationFailure;
        }
        return attempt;
    };

    // WGL_NV_DX_interop2 explicitly exempts D3D10/11 resources from
    // wglDXSetResourceShareHandleNV. Some current NVIDIA drivers return FALSE
    // for an NT shared handle here even though direct D3D11 registration is
    // supported. Register the D3D11 texture directly; treating that optional
    // legacy association call as fatal prevented the producer from publishing
    // any named object to the x64 host.

    const GLenum access = static_cast<GLenum>(description.access);
    auto attempt = create_attempt();
    if (attempt.status != NvDxInteropStatus::Ok) {
        return attempt.status;
    }
    HANDLE interop_object = state_->register_object(
        state_->interop_device, attempt.texture, attempt.gl_texture,
        GL_TEXTURE_2D, access);
    bool used_compatibility_retry = false;
    if (interop_object == nullptr) {
        const DWORD first_error = GetLastError();
        SetDiagnostic(diagnostic,
                      NvDxInteropStatus::TextureRegistrationFailure,
                      "direct D3D11 registration failed", state_, S_OK,
                      first_error);
        const NvDxInteropDiagnostic first_failure = diagnostic;
        release_attempt(attempt.texture, attempt.shared_handle,
                        attempt.gl_texture);

        // WGL_NV_DX_interop2 says D3D11 resources do not require an explicit
        // share-handle association. NVIDIA 610.88 nevertheless rejects the
        // first registration made on a fresh WGL interop device, then accepts
        // a fresh texture after this compatibility association. Keep the
        // spec-correct direct path first and bound the workaround to one retry.
        attempt = create_attempt();
        if (attempt.status != NvDxInteropStatus::Ok) {
            diagnostic = first_failure;
            CopyDetail(
                diagnostic.detail,
                "direct registration failed; fresh-texture retry setup also failed");
            return NvDxInteropStatus::TextureRegistrationFailure;
        }
        if (state_->set_resource_share_handle(
                attempt.texture, attempt.shared_handle) == FALSE) {
            release_attempt(attempt.texture, attempt.shared_handle,
                            attempt.gl_texture);
            diagnostic = first_failure;
            CopyDetail(
                diagnostic.detail,
                "direct registration failed; fresh-texture retry association failed");
            return NvDxInteropStatus::TextureRegistrationFailure;
        }
        interop_object = state_->register_object(
            state_->interop_device, attempt.texture, attempt.gl_texture,
            GL_TEXTURE_2D, access);
        if (interop_object == nullptr) {
            release_attempt(attempt.texture, attempt.shared_handle,
                            attempt.gl_texture);
            diagnostic = first_failure;
            CopyDetail(
                diagnostic.detail,
                "direct registration failed; fresh-texture retry also failed");
            return NvDxInteropStatus::TextureRegistrationFailure;
        }
        used_compatibility_retry = true;
    }

    state_->texture = attempt.texture;
    state_->shared_handle = attempt.shared_handle;
    state_->gl_texture = attempt.gl_texture;
    state_->interop_object = interop_object;
    state_->object_locked = false;
    SetDiagnostic(
        diagnostic, NvDxInteropStatus::Ok,
        used_compatibility_retry
            ? "fresh-texture compatibility retry registered GL_TEXTURE_2D"
            : "shared D3D11 texture is registered as GL_TEXTURE_2D",
        state_);
    return NvDxInteropStatus::Ok;
}

NvDxInteropStatus NvDxInteropBridge::Lock(
    NvDxInteropDiagnostic& diagnostic) noexcept {
    const NvDxInteropStatus decision = DecideNvDxLock(
        state_ != nullptr,
        state_ != nullptr && state_->interop_object != nullptr,
        state_ != nullptr && ContextMatches(*state_),
        state_ != nullptr && state_->object_locked);
    if (decision != NvDxInteropStatus::Ok) {
        SetDiagnostic(diagnostic, decision, ToString(decision).data(), state_);
        return decision;
    }
    HANDLE object = state_->interop_object;
    if (state_->lock_objects(state_->interop_device, 1, &object) == FALSE) {
        const DWORD error = GetLastError();
        SetDiagnostic(diagnostic, NvDxInteropStatus::LockFailure,
                      "wglDXLockObjectsNV failed", state_, S_OK, error);
        return NvDxInteropStatus::LockFailure;
    }
    state_->object_locked = true;
    SetDiagnostic(diagnostic, NvDxInteropStatus::Ok,
                  "shared texture locked for OpenGL access", state_);
    return NvDxInteropStatus::Ok;
}

NvDxInteropStatus NvDxInteropBridge::Unlock(
    NvDxInteropDiagnostic& diagnostic) noexcept {
    const NvDxInteropStatus decision = DecideNvDxUnlock(
        state_ != nullptr,
        state_ != nullptr && state_->interop_object != nullptr,
        state_ != nullptr && ContextMatches(*state_),
        state_ != nullptr && state_->object_locked);
    if (decision != NvDxInteropStatus::Ok) {
        SetDiagnostic(diagnostic, decision, ToString(decision).data(), state_);
        return decision;
    }
    HANDLE object = state_->interop_object;
    if (state_->unlock_objects(state_->interop_device, 1, &object) == FALSE) {
        const DWORD error = GetLastError();
        SetDiagnostic(diagnostic, NvDxInteropStatus::UnlockFailure,
                      "wglDXUnlockObjectsNV failed", state_, S_OK, error);
        return NvDxInteropStatus::UnlockFailure;
    }
    state_->object_locked = false;
    SetDiagnostic(diagnostic, NvDxInteropStatus::Ok,
                  "shared texture released from OpenGL access", state_);
    return NvDxInteropStatus::Ok;
}

NvDxInteropStatus NvDxInteropBridge::ReleaseTexture(
    NvDxInteropDiagnostic& diagnostic) noexcept {
    if (state_ == nullptr) {
        SetDiagnostic(diagnostic, NvDxInteropStatus::NotInitialized,
                      "the bridge is not initialized");
        return NvDxInteropStatus::NotInitialized;
    }
    if (state_->interop_object == nullptr && state_->texture == nullptr) {
        SetDiagnostic(diagnostic, NvDxInteropStatus::Ok,
                      "no shared texture is registered", state_);
        return NvDxInteropStatus::Ok;
    }
    if (!ContextMatches(*state_)) {
        SetDiagnostic(diagnostic, NvDxInteropStatus::ContextMismatch,
                      "texture release requires the owning WGL context",
                      state_);
        return NvDxInteropStatus::ContextMismatch;
    }
    if (state_->object_locked) {
        const NvDxInteropStatus unlock_status = Unlock(diagnostic);
        if (unlock_status != NvDxInteropStatus::Ok) {
            return unlock_status;
        }
    }
    if (state_->interop_object != nullptr &&
        state_->unregister_object(state_->interop_device,
                                  state_->interop_object) == FALSE) {
        const DWORD error = GetLastError();
        SetDiagnostic(diagnostic, NvDxInteropStatus::CleanupFailure,
                      "wglDXUnregisterObjectNV failed", state_, S_OK, error);
        return NvDxInteropStatus::CleanupFailure;
    }
    state_->interop_object = nullptr;
    if (state_->gl_texture != 0U) {
        glDeleteTextures(1, &state_->gl_texture);
        state_->gl_texture = 0U;
    }
    if (state_->shared_handle != nullptr) {
        CloseHandle(state_->shared_handle);
        state_->shared_handle = nullptr;
    }
    SafeRelease(state_->texture);
    SetDiagnostic(diagnostic, NvDxInteropStatus::Ok,
                  "shared texture resources released", state_);
    return NvDxInteropStatus::Ok;
}

NvDxInteropStatus NvDxInteropBridge::Shutdown(
    NvDxInteropDiagnostic& diagnostic) noexcept {
    if (state_ == nullptr) {
        SetDiagnostic(diagnostic, NvDxInteropStatus::Ok,
                      "the bridge is already shut down");
        return NvDxInteropStatus::Ok;
    }
    if (!ContextMatches(*state_)) {
        SetDiagnostic(diagnostic, NvDxInteropStatus::ContextMismatch,
                      "Shutdown requires the owning WGL context", state_);
        return NvDxInteropStatus::ContextMismatch;
    }
    const NvDxInteropStatus texture_status = ReleaseTexture(diagnostic);
    if (texture_status != NvDxInteropStatus::Ok) {
        return texture_status;
    }
    if (state_->interop_device != nullptr &&
        state_->close_device(state_->interop_device) == FALSE) {
        const DWORD error = GetLastError();
        SetDiagnostic(diagnostic, NvDxInteropStatus::CleanupFailure,
                      "wglDXCloseDeviceNV failed", state_, S_OK, error);
        return NvDxInteropStatus::CleanupFailure;
    }
    state_->interop_device = nullptr;
    ReleaseComAndHandles(*state_);
    delete state_;
    state_ = nullptr;
    SetDiagnostic(diagnostic, NvDxInteropStatus::Ok,
                  "NV_DX_interop bridge shut down cleanly");
    return NvDxInteropStatus::Ok;
}

bool NvDxInteropBridge::initialized() const noexcept {
    return state_ != nullptr;
}

bool NvDxInteropBridge::texture_registered() const noexcept {
    return state_ != nullptr && state_->interop_object != nullptr;
}

bool NvDxInteropBridge::locked() const noexcept {
    return state_ != nullptr && state_->object_locked;
}

bool NvDxInteropBridge::owning_gl_context_is_current() const noexcept {
    return state_ != nullptr && ContextMatches(*state_);
}

NvDxAdapterLuid NvDxInteropBridge::adapter_luid() const noexcept {
    return state_ != nullptr ? state_->adapter_luid : NvDxAdapterLuid{};
}

std::uint32_t NvDxInteropBridge::gl_texture_name() const noexcept {
    return state_ != nullptr ? state_->gl_texture : 0U;
}

std::uintptr_t NvDxInteropBridge::shared_handle_value() const noexcept {
    return state_ != nullptr
               ? reinterpret_cast<std::uintptr_t>(state_->shared_handle)
               : 0U;
}

void* NvDxInteropBridge::d3d11_device5_native() const noexcept {
    return state_ != nullptr ? state_->device : nullptr;
}

void* NvDxInteropBridge::d3d11_context4_native() const noexcept {
    return state_ != nullptr ? state_->context : nullptr;
}

} // namespace k2vr::game32

#endif
