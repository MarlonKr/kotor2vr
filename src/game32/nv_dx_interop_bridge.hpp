#pragma once

#include <cstdint>
#include <string_view>

namespace k2vr::game32 {

inline constexpr std::uint32_t kNvidiaPciVendorId = 0x10DEU;
inline constexpr std::uint32_t kDxgiAdapterSoftwareFlag = 0x2U;
inline constexpr std::uint32_t kDxgiFormatR8G8B8A8Unorm = 28U;
inline constexpr std::uint32_t kNvDxInteropMaximumDimension = 8192U;

struct NvDxAdapterLuid {
    std::uint32_t low_part{};
    std::int32_t high_part{};
};

struct NvDxAdapterCandidate {
    std::uint32_t vendor_id{};
    std::uint32_t flags{};
    NvDxAdapterLuid luid{};
};

enum class NvDxInteropAccess : std::uint32_t {
    ReadWrite = 1,
    WriteDiscard = 2,
};

struct NvDxInteropTextureDescription {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t dxgi_format{kDxgiFormatR8G8B8A8Unorm};
    NvDxInteropAccess access{NvDxInteropAccess::WriteDiscard};
};

enum class NvDxInteropStatus : std::uint32_t {
    Ok = 0,
    AlreadyInitialized,
    NotInitialized,
    InvalidTextureDescription,
    NoCurrentGlContext,
    OpenGlRuntimeUnavailable,
    MissingWglEntryPoint,
    DxgiFactoryFailure,
    NvidiaAdapterUnavailable,
    RequestedAdapterUnavailable,
    D3d11DeviceCreationFailure,
    D3d11FenceInterfaceUnavailable,
    InteropDeviceOpenFailure,
    TextureAlreadyRegistered,
    InvalidSharedObjectName,
    UnsupportedTextureFormat,
    TextureCreationFailure,
    SharedResourceInterfaceUnavailable,
    SharedHandleCreationFailure,
    GlTextureCreationFailure,
    ShareHandleAssociationFailure,
    TextureRegistrationFailure,
    ContextMismatch,
    TextureNotRegistered,
    AlreadyLocked,
    NotLocked,
    LockFailure,
    UnlockFailure,
    CleanupFailure,
    OutOfMemory,
};

struct NvDxInteropDiagnostic {
    NvDxInteropStatus status{NvDxInteropStatus::Ok};
    std::int32_t hresult{};
    std::uint32_t win32_error{};
    NvDxAdapterLuid adapter_luid{};
    std::uint32_t extension_advertised{};
    char detail[192]{};
};

[[nodiscard]] constexpr bool IsValidAdapterLuid(
    const NvDxAdapterLuid luid) noexcept {
    return luid.low_part != 0U || luid.high_part != 0;
}

[[nodiscard]] constexpr bool SameAdapterLuid(
    const NvDxAdapterLuid left, const NvDxAdapterLuid right) noexcept {
    return left.low_part == right.low_part &&
           left.high_part == right.high_part;
}

// An empty requested LUID means "choose the best hardware NVIDIA adapter".
// A non-empty value is an exact adapter contract and never falls back.
[[nodiscard]] constexpr bool IsEligibleNvDxAdapter(
    const NvDxAdapterCandidate candidate,
    const NvDxAdapterLuid requested_luid = {}) noexcept {
    if (candidate.vendor_id != kNvidiaPciVendorId ||
        (candidate.flags & kDxgiAdapterSoftwareFlag) != 0U) {
        return false;
    }
    return !IsValidAdapterLuid(requested_luid) ||
           SameAdapterLuid(candidate.luid, requested_luid);
}

// wglGetProcAddress uses 1, 2, 3 and -1 as failure sentinels on Windows in
// addition to nullptr. Keeping this decision pure makes it independently
// testable without constructing a GL context.
[[nodiscard]] constexpr bool IsUsableWglProcAddressValue(
    const std::uintptr_t value) noexcept {
    return value > 3U && value != static_cast<std::uintptr_t>(-1);
}

[[nodiscard]] constexpr bool HasExactExtensionToken(
    const std::string_view extensions,
    const std::string_view requested) noexcept {
    if (requested.empty() || requested.find(' ') != std::string_view::npos) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < extensions.size()) {
        while (offset < extensions.size() && extensions[offset] == ' ') {
            ++offset;
        }
        const std::size_t end = extensions.find(' ', offset);
        const std::size_t length =
            (end == std::string_view::npos ? extensions.size() : end) - offset;
        if (extensions.substr(offset, length) == requested) {
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1U;
    }
    return false;
}

[[nodiscard]] constexpr NvDxInteropStatus ValidateNvDxTextureDescription(
    const NvDxInteropTextureDescription description) noexcept {
    if (description.width == 0U || description.height == 0U ||
        description.width > kNvDxInteropMaximumDimension ||
        description.height > kNvDxInteropMaximumDimension ||
        description.dxgi_format != kDxgiFormatR8G8B8A8Unorm ||
        (description.access != NvDxInteropAccess::WriteDiscard &&
         description.access != NvDxInteropAccess::ReadWrite)) {
        return NvDxInteropStatus::InvalidTextureDescription;
    }
    return NvDxInteropStatus::Ok;
}

[[nodiscard]] constexpr bool IsValidNvDxSharedObjectName(
    const std::wstring_view name) noexcept {
    return !name.empty() && name.size() < 128U &&
           name.find(L'\0') == std::wstring_view::npos;
}

[[nodiscard]] constexpr NvDxInteropStatus DecideNvDxLock(
    const bool initialized, const bool texture_registered,
    const bool context_matches, const bool locked) noexcept {
    if (!initialized) {
        return NvDxInteropStatus::NotInitialized;
    }
    if (!texture_registered) {
        return NvDxInteropStatus::TextureNotRegistered;
    }
    if (!context_matches) {
        return NvDxInteropStatus::ContextMismatch;
    }
    if (locked) {
        return NvDxInteropStatus::AlreadyLocked;
    }
    return NvDxInteropStatus::Ok;
}

[[nodiscard]] constexpr NvDxInteropStatus DecideNvDxUnlock(
    const bool initialized, const bool texture_registered,
    const bool context_matches, const bool locked) noexcept {
    if (!initialized) {
        return NvDxInteropStatus::NotInitialized;
    }
    if (!texture_registered) {
        return NvDxInteropStatus::TextureNotRegistered;
    }
    if (!context_matches) {
        return NvDxInteropStatus::ContextMismatch;
    }
    if (!locked) {
        return NvDxInteropStatus::NotLocked;
    }
    return NvDxInteropStatus::Ok;
}

[[nodiscard]] std::string_view ToString(NvDxInteropStatus status) noexcept;

struct NvDxInteropBridgeNativeState;

// Render-thread-only bridge. Initialize, texture registration, locking and
// Shutdown must all run while the same WGL context is current. The returned
// shared handle remains owned by this object; duplicate it before transport to
// another process and never close the borrowed value.
class NvDxInteropBridge final {
public:
    NvDxInteropBridge() noexcept = default;
    ~NvDxInteropBridge();

    NvDxInteropBridge(const NvDxInteropBridge&) = delete;
    NvDxInteropBridge& operator=(const NvDxInteropBridge&) = delete;
    NvDxInteropBridge(NvDxInteropBridge&&) = delete;
    NvDxInteropBridge& operator=(NvDxInteropBridge&&) = delete;

    [[nodiscard]] NvDxInteropStatus Initialize(
        NvDxAdapterLuid requested_luid,
        NvDxInteropDiagnostic& diagnostic) noexcept;
    [[nodiscard]] NvDxInteropStatus RegisterSharedTexture(
        const NvDxInteropTextureDescription& description,
        std::wstring_view shared_object_name,
        NvDxInteropDiagnostic& diagnostic) noexcept;
    [[nodiscard]] NvDxInteropStatus Lock(
        NvDxInteropDiagnostic& diagnostic) noexcept;
    [[nodiscard]] NvDxInteropStatus Unlock(
        NvDxInteropDiagnostic& diagnostic) noexcept;
    [[nodiscard]] NvDxInteropStatus ReleaseTexture(
        NvDxInteropDiagnostic& diagnostic) noexcept;
    [[nodiscard]] NvDxInteropStatus Shutdown(
        NvDxInteropDiagnostic& diagnostic) noexcept;

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] bool texture_registered() const noexcept;
    [[nodiscard]] bool locked() const noexcept;
    [[nodiscard]] bool owning_gl_context_is_current() const noexcept;
    [[nodiscard]] NvDxAdapterLuid adapter_luid() const noexcept;
    [[nodiscard]] std::uint32_t gl_texture_name() const noexcept;
    [[nodiscard]] std::uintptr_t shared_handle_value() const noexcept;

    // Borrowed COM interface pointers for the later shared-fence layer.
    // They remain owned by this bridge and are valid only until Shutdown.
    [[nodiscard]] void* d3d11_device5_native() const noexcept;
    [[nodiscard]] void* d3d11_context4_native() const noexcept;

private:
    NvDxInteropBridgeNativeState* state_{};
};

} // namespace k2vr::game32
