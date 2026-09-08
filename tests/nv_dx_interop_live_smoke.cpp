#include "nv_dx_interop_bridge.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <gl/GL.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

using k2vr::game32::NvDxInteropBridge;
using k2vr::game32::NvDxInteropDiagnostic;
using k2vr::game32::NvDxInteropStatus;
using k2vr::game32::NvDxInteropTextureDescription;

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

constexpr GLenum kWglAccessReadWriteNv = 0x0001U;
constexpr GLenum kWglAccessWriteDiscardNv = 0x0002U;

template <typename Interface>
void SafeRelease(Interface*& value) noexcept {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

struct RawInteropFunctions {
    WglDxOpenDeviceNv open_device{};
    WglDxCloseDeviceNv close_device{};
    WglDxSetResourceShareHandleNv set_share_handle{};
    WglDxRegisterObjectNv register_object{};
    WglDxUnregisterObjectNv unregister_object{};
    WglDxLockObjectsNv lock_objects{};
    WglDxUnlockObjectsNv unlock_objects{};

    [[nodiscard]] bool Load() noexcept {
#define K2VR_LOAD(member, type, symbol)                                      \
    member = reinterpret_cast<type>(wglGetProcAddress(symbol));              \
    if (member == nullptr) {                                                  \
        std::printf("matrix missing %s\n", symbol);                         \
        return false;                                                         \
    }
        K2VR_LOAD(open_device, WglDxOpenDeviceNv, "wglDXOpenDeviceNV")
        K2VR_LOAD(close_device, WglDxCloseDeviceNv, "wglDXCloseDeviceNV")
        K2VR_LOAD(set_share_handle, WglDxSetResourceShareHandleNv,
                  "wglDXSetResourceShareHandleNV")
        K2VR_LOAD(register_object, WglDxRegisterObjectNv,
                  "wglDXRegisterObjectNV")
        K2VR_LOAD(unregister_object, WglDxUnregisterObjectNv,
                  "wglDXUnregisterObjectNV")
        K2VR_LOAD(lock_objects, WglDxLockObjectsNv, "wglDXLockObjectsNV")
        K2VR_LOAD(unlock_objects, WglDxUnlockObjectsNv,
                  "wglDXUnlockObjectsNV")
#undef K2VR_LOAD
        return true;
    }
};

enum class ShareHandleKind {
    None,
    Legacy,
    NtUnnamed,
    NtNamed,
};

struct MatrixCase {
    const char* name;
    UINT misc_flags;
    UINT bind_flags;
    ShareHandleKind handle_kind;
    bool call_set_share_handle;
    GLenum access;
};

[[nodiscard]] HANDLE MakeShareHandle(ID3D11Texture2D* texture,
                                     ShareHandleKind kind,
                                     unsigned case_index,
                                     HRESULT& result) noexcept;

void ProbeCurrentRegistration(RawInteropFunctions& functions,
                              const char* const label,
                              ID3D11Device* const resource_device,
                              void* const open_device_pointer,
                              const unsigned name_index) noexcept {
    SetLastError(ERROR_SUCCESS);
    HANDLE const interop_device =
        functions.open_device(open_device_pointer);
    std::printf("isolate case=%s open=%p error=0x%08lX\n", label,
                interop_device,
                static_cast<unsigned long>(GetLastError()));
    if (interop_device == nullptr) {
        return;
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = 16;
    description.Height = 16;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags =
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    description.MiscFlags =
        D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D* texture = nullptr;
    HRESULT result = resource_device->CreateTexture2D(
        &description, nullptr, &texture);
    HANDLE share_handle = nullptr;
    if (SUCCEEDED(result) && texture != nullptr) {
        share_handle = MakeShareHandle(
            texture, ShareHandleKind::NtNamed, name_index, result);
    }
    std::printf("isolate case=%s create/share=0x%08X texture=%p handle=%p\n",
                label, static_cast<unsigned>(result), texture, share_handle);

    GLuint gl_texture = 0;
    HANDLE interop_object = nullptr;
    if (SUCCEEDED(result) && texture != nullptr && share_handle != nullptr) {
        SetLastError(ERROR_SUCCESS);
        const BOOL set = functions.set_share_handle(texture, share_handle);
        std::printf("isolate case=%s set=%d error=0x%08lX\n", label,
                    static_cast<int>(set),
                    static_cast<unsigned long>(GetLastError()));
        glGenTextures(1, &gl_texture);
        SetLastError(ERROR_SUCCESS);
        interop_object = functions.register_object(
            interop_device, texture, gl_texture, GL_TEXTURE_2D,
            kWglAccessWriteDiscardNv);
        std::printf("isolate case=%s register=%p error=0x%08lX\n", label,
                    interop_object,
                    static_cast<unsigned long>(GetLastError()));
    }
    if (interop_object != nullptr) {
        (void)functions.unregister_object(interop_device, interop_object);
    }
    if (gl_texture != 0) {
        glDeleteTextures(1, &gl_texture);
    }
    if (share_handle != nullptr) {
        CloseHandle(share_handle);
    }
    SafeRelease(texture);
    (void)functions.close_device(interop_device);
}

[[nodiscard]] HANDLE MakeShareHandle(ID3D11Texture2D* const texture,
                                     const ShareHandleKind kind,
                                     const unsigned case_index,
                                     HRESULT& result) noexcept {
    result = S_OK;
    if (kind == ShareHandleKind::None) {
        return nullptr;
    }
    if (kind == ShareHandleKind::Legacy) {
        IDXGIResource* resource = nullptr;
        result = texture->QueryInterface(
            __uuidof(IDXGIResource), reinterpret_cast<void**>(&resource));
        HANDLE handle = nullptr;
        if (SUCCEEDED(result)) {
            result = resource->GetSharedHandle(&handle);
        }
        SafeRelease(resource);
        return handle;
    }

    IDXGIResource1* resource = nullptr;
    result = texture->QueryInterface(
        __uuidof(IDXGIResource1), reinterpret_cast<void**>(&resource));
    HANDLE handle = nullptr;
    if (SUCCEEDED(result)) {
        std::array<wchar_t, 128> name{};
        const wchar_t* optional_name = nullptr;
        if (kind == ShareHandleKind::NtNamed) {
            std::swprintf(name.data(), name.size(),
                          L"Local\\Kotor2VR-nv-dx-matrix-%lu-%u",
                          static_cast<unsigned long>(GetCurrentProcessId()),
                          case_index);
            optional_name = name.data();
        }
        result = resource->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            optional_name, &handle);
    }
    SafeRelease(resource);
    return handle;
}

void RunRegistrationMatrix() noexcept {
    RawInteropFunctions functions{};
    if (!functions.Load()) {
        return;
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* immediate_context = nullptr;
    D3D_FEATURE_LEVEL feature_level{};
    const HRESULT device_result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &device, &feature_level, &immediate_context);
    std::printf("matrix D3D11CreateDevice hr=0x%08X feature=0x%X\n",
                static_cast<unsigned>(device_result),
                static_cast<unsigned>(feature_level));
    if (FAILED(device_result) || device == nullptr) {
        SafeRelease(immediate_context);
        SafeRelease(device);
        return;
    }

    SetLastError(ERROR_SUCCESS);
    HANDLE const interop_device = functions.open_device(device);
    const DWORD open_error = GetLastError();
    std::printf("matrix wglDXOpenDeviceNV handle=%p error=0x%08lX\n",
                interop_device, static_cast<unsigned long>(open_error));
    if (interop_device == nullptr) {
        SafeRelease(immediate_context);
        SafeRelease(device);
        return;
    }

    constexpr std::array<MatrixCase, 9> cases{{
        {"current-named-set", D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                                  D3D11_RESOURCE_MISC_SHARED,
         D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
         ShareHandleKind::NtNamed, true, kWglAccessWriteDiscardNv},
        {"named-no-set", D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                             D3D11_RESOURCE_MISC_SHARED,
         D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
         ShareHandleKind::NtNamed, false, kWglAccessWriteDiscardNv},
        {"unnamed-no-set", D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                               D3D11_RESOURCE_MISC_SHARED,
         D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
         ShareHandleKind::NtUnnamed, false, kWglAccessWriteDiscardNv},
        {"tcs-like-srv-write-discard",
         D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED,
         D3D11_BIND_SHADER_RESOURCE, ShareHandleKind::NtUnnamed, false,
         kWglAccessWriteDiscardNv},
        {"tcs-like-srv-read-write",
         D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED,
         D3D11_BIND_SHADER_RESOURCE, ShareHandleKind::NtUnnamed, false,
         kWglAccessReadWriteNv},
        {"official-nt-keyed",
         D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
             D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX,
         D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
         ShareHandleKind::NtNamed, false, kWglAccessWriteDiscardNv},
        {"legacy-set", D3D11_RESOURCE_MISC_SHARED,
         D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
         ShareHandleKind::Legacy, true, kWglAccessWriteDiscardNv},
        {"legacy-no-set", D3D11_RESOURCE_MISC_SHARED,
         D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
         ShareHandleKind::Legacy, false, kWglAccessWriteDiscardNv},
        {"private-unshared", 0U,
         D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
         ShareHandleKind::None, false, kWglAccessWriteDiscardNv},
    }};

    for (unsigned index = 0; index < cases.size(); ++index) {
        const MatrixCase& test_case = cases[index];
        D3D11_TEXTURE2D_DESC description{};
        description.Width = 16;
        description.Height = 16;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = test_case.bind_flags;
        description.MiscFlags = test_case.misc_flags;

        ID3D11Texture2D* texture = nullptr;
        HRESULT result = device->CreateTexture2D(
            &description, nullptr, &texture);
        HANDLE share_handle = nullptr;
        if (SUCCEEDED(result) && texture != nullptr) {
            share_handle = MakeShareHandle(
                texture, test_case.handle_kind, index, result);
        }
        std::printf(
            "matrix case=%s create/share hr=0x%08X texture=%p handle=%p\n",
            test_case.name, static_cast<unsigned>(result), texture,
            share_handle);
        if (FAILED(result) || texture == nullptr) {
            SafeRelease(texture);
            continue;
        }

        if (test_case.call_set_share_handle) {
            SetLastError(ERROR_SUCCESS);
            const BOOL set =
                functions.set_share_handle(texture, share_handle);
            std::printf("matrix case=%s set=%d error=0x%08lX\n",
                        test_case.name, static_cast<int>(set),
                        static_cast<unsigned long>(GetLastError()));
        }

        GLuint gl_texture = 0;
        glGenTextures(1, &gl_texture);
        SetLastError(ERROR_SUCCESS);
        HANDLE interop_object = functions.register_object(
            interop_device, texture, gl_texture, GL_TEXTURE_2D,
            test_case.access);
        const DWORD register_error = GetLastError();
        std::printf(
            "matrix case=%s register=%p error=0x%08lX gl_error=0x%04X\n",
            test_case.name, interop_object,
            static_cast<unsigned long>(register_error),
            static_cast<unsigned>(glGetError()));

        if (interop_object != nullptr) {
            HANDLE object = interop_object;
            SetLastError(ERROR_SUCCESS);
            const BOOL locked =
                functions.lock_objects(interop_device, 1, &object);
            const DWORD lock_error = GetLastError();
            BOOL unlocked = FALSE;
            DWORD unlock_error = ERROR_SUCCESS;
            if (locked != FALSE) {
                glFlush();
                SetLastError(ERROR_SUCCESS);
                unlocked =
                    functions.unlock_objects(interop_device, 1, &object);
                unlock_error = GetLastError();
            }
            std::printf(
                "matrix case=%s lock=%d/0x%08lX unlock=%d/0x%08lX\n",
                test_case.name, static_cast<int>(locked),
                static_cast<unsigned long>(lock_error),
                static_cast<int>(unlocked),
                static_cast<unsigned long>(unlock_error));
            (void)functions.unregister_object(interop_device, interop_object);
        }
        glDeleteTextures(1, &gl_texture);
        if (share_handle != nullptr &&
            test_case.handle_kind != ShareHandleKind::Legacy) {
            CloseHandle(share_handle);
        }
        SafeRelease(texture);
    }

    ID3D11Device5* device5 = nullptr;
    const HRESULT device5_result = device->QueryInterface(
        __uuidof(ID3D11Device5), reinterpret_cast<void**>(&device5));
    std::printf("isolate FL11.0 base=%p device5=%p qi=0x%08X\n", device,
                device5, static_cast<unsigned>(device5_result));
    if (SUCCEEDED(device5_result) && device5 != nullptr) {
        ProbeCurrentRegistration(functions, "fl11.0-device5-pointer", device,
                                 device5, 100U);
    }
    SafeRelease(device5);

    constexpr D3D_FEATURE_LEVEL requested_levels[]{
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ID3D11Device* feature_device = nullptr;
    ID3D11DeviceContext* feature_context = nullptr;
    D3D_FEATURE_LEVEL selected_feature{};
    const HRESULT feature_result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested_levels,
        static_cast<UINT>(std::size(requested_levels)), D3D11_SDK_VERSION,
        &feature_device, &selected_feature, &feature_context);
    std::printf("isolate requested-11.1 create=0x%08X feature=0x%X base=%p\n",
                static_cast<unsigned>(feature_result),
                static_cast<unsigned>(selected_feature), feature_device);
    if (SUCCEEDED(feature_result) && feature_device != nullptr) {
        ProbeCurrentRegistration(functions, "fl11.1-base-pointer",
                                 feature_device, feature_device, 101U);
        ID3D11Device5* feature_device5 = nullptr;
        const HRESULT feature_qi = feature_device->QueryInterface(
            __uuidof(ID3D11Device5),
            reinterpret_cast<void**>(&feature_device5));
        std::printf("isolate FL11.1 base=%p device5=%p qi=0x%08X\n",
                    feature_device, feature_device5,
                    static_cast<unsigned>(feature_qi));
        if (SUCCEEDED(feature_qi) && feature_device5 != nullptr) {
            ProbeCurrentRegistration(functions, "fl11.1-device5-pointer",
                                     feature_device, feature_device5, 102U);
        }
        SafeRelease(feature_device5);
    }
    SafeRelease(feature_context);
    SafeRelease(feature_device);

    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* explicit_adapter = nullptr;
    HRESULT explicit_result = CreateDXGIFactory1(
        __uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (SUCCEEDED(explicit_result) && factory != nullptr) {
        for (UINT index = 0;; ++index) {
            IDXGIAdapter1* candidate = nullptr;
            const HRESULT enumerate = factory->EnumAdapters1(index, &candidate);
            if (enumerate == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 adapter_description{};
            if (SUCCEEDED(enumerate) && candidate != nullptr &&
                SUCCEEDED(candidate->GetDesc1(&adapter_description)) &&
                adapter_description.VendorId == 0x10DEU) {
                explicit_adapter = candidate;
                candidate = nullptr;
                std::printf(
                    "isolate explicit adapter index=%u luid=%08X:%08X\n",
                    index,
                    static_cast<unsigned>(adapter_description.AdapterLuid.HighPart),
                    static_cast<unsigned>(adapter_description.AdapterLuid.LowPart));
            }
            SafeRelease(candidate);
            if (explicit_adapter != nullptr) {
                break;
            }
        }
    }
    SafeRelease(factory);
    ID3D11Device* explicit_device = nullptr;
    ID3D11DeviceContext* explicit_context = nullptr;
    D3D_FEATURE_LEVEL explicit_feature{};
    if (explicit_adapter != nullptr) {
        explicit_result = D3D11CreateDevice(
            explicit_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested_levels,
            static_cast<UINT>(std::size(requested_levels)), D3D11_SDK_VERSION,
            &explicit_device, &explicit_feature, &explicit_context);
    }
    std::printf(
        "isolate explicit-adapter create=0x%08X feature=0x%X base=%p\n",
        static_cast<unsigned>(explicit_result),
        static_cast<unsigned>(explicit_feature), explicit_device);
    if (SUCCEEDED(explicit_result) && explicit_device != nullptr) {
        ProbeCurrentRegistration(functions, "explicit-adapter",
                                 explicit_device, explicit_device, 103U);
    }
    SafeRelease(explicit_context);
    SafeRelease(explicit_device);
    SafeRelease(explicit_adapter);

    (void)functions.close_device(interop_device);
    SafeRelease(immediate_context);
    SafeRelease(device);
}

void PrintDiagnostic(const char* const stage,
                     const NvDxInteropStatus status,
                     const NvDxInteropDiagnostic& diagnostic) noexcept {
    std::printf(
        "%s status=%.*s(%u) hr=0x%08X win32=%lu luid=%08X:%08X "
        "extension_advertised=%u detail=%s\n",
        stage, static_cast<int>(k2vr::game32::ToString(status).size()),
        k2vr::game32::ToString(status).data(),
        static_cast<unsigned>(status),
        static_cast<unsigned>(diagnostic.hresult),
        static_cast<unsigned long>(diagnostic.win32_error),
        static_cast<unsigned>(diagnostic.adapter_luid.high_part),
        static_cast<unsigned>(diagnostic.adapter_luid.low_part),
        static_cast<unsigned>(diagnostic.extension_advertised),
        diagnostic.detail);
}

LRESULT CALLBACK SmokeWindowProcedure(HWND const window,
                                      const UINT message,
                                      const WPARAM word_parameter,
                                      const LPARAM long_parameter) noexcept {
    return DefWindowProcW(window, message, word_parameter, long_parameter);
}

class HiddenGlContext final {
public:
    HiddenGlContext() noexcept = default;
    ~HiddenGlContext() { Reset(); }

    HiddenGlContext(const HiddenGlContext&) = delete;
    HiddenGlContext& operator=(const HiddenGlContext&) = delete;

    [[nodiscard]] bool Create() noexcept {
        module_ = GetModuleHandleW(nullptr);
        if (module_ == nullptr) {
            std::printf("GetModuleHandleW failed win32=%lu\n",
                        static_cast<unsigned long>(GetLastError()));
            return false;
        }

        std::swprintf(class_name_.data(), class_name_.size(),
                      L"Kotor2VrNvDxSmoke-%lu",
                      static_cast<unsigned long>(GetCurrentProcessId()));
        WNDCLASSW window_class{};
        window_class.style = CS_OWNDC;
        window_class.lpfnWndProc = SmokeWindowProcedure;
        window_class.hInstance = module_;
        window_class.lpszClassName = class_name_.data();
        if (RegisterClassW(&window_class) == 0) {
            std::printf("RegisterClassW failed win32=%lu\n",
                        static_cast<unsigned long>(GetLastError()));
            return false;
        }
        class_registered_ = true;

        window_ = CreateWindowExW(
            0, class_name_.data(), L"KOTOR2VR NV_DX smoke",
            WS_OVERLAPPED, 0, 0, 1, 1, nullptr, nullptr, module_, nullptr);
        if (window_ == nullptr) {
            std::printf("CreateWindowExW failed win32=%lu\n",
                        static_cast<unsigned long>(GetLastError()));
            return false;
        }
        device_context_ = GetDC(window_);
        if (device_context_ == nullptr) {
            std::printf("GetDC failed win32=%lu\n",
                        static_cast<unsigned long>(GetLastError()));
            return false;
        }

        PIXELFORMATDESCRIPTOR descriptor{};
        descriptor.nSize = sizeof(descriptor);
        descriptor.nVersion = 1;
        descriptor.dwFlags =
            PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        descriptor.iPixelType = PFD_TYPE_RGBA;
        descriptor.cColorBits = 32;
        descriptor.cDepthBits = 24;
        descriptor.cStencilBits = 8;
        descriptor.iLayerType = PFD_MAIN_PLANE;
        const int pixel_format = ChoosePixelFormat(device_context_, &descriptor);
        if (pixel_format == 0 ||
            SetPixelFormat(device_context_, pixel_format, &descriptor) == FALSE) {
            std::printf("pixel format setup failed format=%d win32=%lu\n",
                        pixel_format,
                        static_cast<unsigned long>(GetLastError()));
            return false;
        }

        rendering_context_ = wglCreateContext(device_context_);
        if (rendering_context_ == nullptr ||
            wglMakeCurrent(device_context_, rendering_context_) == FALSE) {
            std::printf("WGL context setup failed win32=%lu\n",
                        static_cast<unsigned long>(GetLastError()));
            return false;
        }
        return true;
    }

    void Reset() noexcept {
        if (wglGetCurrentContext() == rendering_context_) {
            (void)wglMakeCurrent(nullptr, nullptr);
        }
        if (rendering_context_ != nullptr) {
            (void)wglDeleteContext(rendering_context_);
            rendering_context_ = nullptr;
        }
        if (device_context_ != nullptr && window_ != nullptr) {
            (void)ReleaseDC(window_, device_context_);
            device_context_ = nullptr;
        }
        if (window_ != nullptr) {
            (void)DestroyWindow(window_);
            window_ = nullptr;
        }
        if (class_registered_) {
            (void)UnregisterClassW(class_name_.data(), module_);
            class_registered_ = false;
        }
    }

private:
    std::array<wchar_t, 64> class_name_{};
    HINSTANCE module_{};
    HWND window_{};
    HDC device_context_{};
    HGLRC rendering_context_{};
    bool class_registered_{};
};

} // namespace

int main() {
    static_assert(sizeof(void*) == 4,
                  "the live smoke executable must be built as x86");

    HiddenGlContext context;
    if (!context.Create()) {
        return 10;
    }

    const auto* const vendor =
        reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const auto* const renderer =
        reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto* const version =
        reinterpret_cast<const char*>(glGetString(GL_VERSION));
    std::printf("OpenGL vendor=%s renderer=%s version=%s context=%p tid=%lu\n",
                vendor != nullptr ? vendor : "<null>",
                renderer != nullptr ? renderer : "<null>",
                version != nullptr ? version : "<null>",
                wglGetCurrentContext(),
                static_cast<unsigned long>(GetCurrentThreadId()));

    NvDxInteropBridge bridge;
    NvDxInteropDiagnostic diagnostic{};
    NvDxInteropStatus status = bridge.Initialize({}, diagnostic);
    PrintDiagnostic("Initialize", status, diagnostic);
    if (status != NvDxInteropStatus::Ok) {
        return 20 + static_cast<int>(status);
    }

    std::array<wchar_t, 128> resource_name{};
    std::swprintf(resource_name.data(), resource_name.size(),
                  L"Local\\Kotor2VR-nv-dx-live-smoke-%lu",
                  static_cast<unsigned long>(GetCurrentProcessId()));
    const NvDxInteropTextureDescription texture{16U, 16U};
    status = bridge.RegisterSharedTexture(texture, resource_name.data(),
                                          diagnostic);
    PrintDiagnostic("RegisterSharedTexture", status, diagnostic);
    if (status != NvDxInteropStatus::Ok) {
        const NvDxInteropStatus shutdown_status =
            bridge.Shutdown(diagnostic);
        PrintDiagnostic("Shutdown-after-register-failure", shutdown_status,
                        diagnostic);
        RunRegistrationMatrix();
        return 50 + static_cast<int>(status);
    }

    status = bridge.Lock(diagnostic);
    PrintDiagnostic("Lock", status, diagnostic);
    if (status != NvDxInteropStatus::Ok) {
        return 80 + static_cast<int>(status);
    }

    glBindTexture(GL_TEXTURE_2D, bridge.gl_texture_name());
    const GLenum gl_error = glGetError();
    std::printf("GL alias bind texture=%u error=0x%04X\n",
                static_cast<unsigned>(bridge.gl_texture_name()),
                static_cast<unsigned>(gl_error));
    glBindTexture(GL_TEXTURE_2D, 0U);
    glFlush();

    status = bridge.Unlock(diagnostic);
    PrintDiagnostic("Unlock", status, diagnostic);
    if (status != NvDxInteropStatus::Ok) {
        return 110 + static_cast<int>(status);
    }

    status = bridge.Shutdown(diagnostic);
    PrintDiagnostic("Shutdown", status, diagnostic);
    return status == NvDxInteropStatus::Ok ? 0
                                           : 140 + static_cast<int>(status);
}
