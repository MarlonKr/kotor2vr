#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace k2vr::game32 {

inline constexpr std::uint32_t kGlExtD3D12DxgiFormatR8G8B8A8Unorm = 28U;
inline constexpr std::uint32_t kGlExtD3D12MaximumDimension = 8192U;
inline constexpr std::uint32_t kGlExtD3D12GenericAllAccess = 0x10000000U;
inline constexpr std::size_t kGlExtD3D12StreamSlotCount = 3U;

enum GlExtD3D12ExtensionBit : std::uint32_t {
    GlExtMemoryObject = 1U << 0U,
    GlExtMemoryObjectWin32 = 1U << 1U,
    GlExtSemaphore = 1U << 2U,
    GlExtSemaphoreWin32 = 1U << 3U,
};

inline constexpr std::uint32_t kGlExtD3D12RequiredExtensionMask =
    GlExtMemoryObject | GlExtMemoryObjectWin32 | GlExtSemaphore |
    GlExtSemaphoreWin32;

struct GlExtD3D12AdapterLuid {
    std::uint32_t low_part{};
    std::int32_t high_part{};
};

struct GlExtD3D12TextureDescription {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t dxgi_format{kGlExtD3D12DxgiFormatR8G8B8A8Unorm};
};

struct GlExtD3D12ObjectNames {
    std::wstring_view color;
    std::wstring_view ready_fence;
    std::wstring_view consumed_fence;
};

// The three color resources are independently owned by GL/D3D12 while the
// ready and consumed timeline fences are shared by the entire ring.
struct GlExtD3D12RingObjectNames {
    std::array<std::wstring_view, kGlExtD3D12StreamSlotCount> colors;
    std::wstring_view ready_fence;
    std::wstring_view consumed_fence;
};

enum class GlExtD3D12Status : std::uint32_t {
    Ok = 0,
    AlreadyInitialized,
    NotInitialized,
    StreamAlreadyCreated,
    StreamNotCreated,
    InvalidTextureDescription,
    InvalidSharedObjectName,
    DuplicateSharedObjectName,
    InvalidFenceValue,
    NoCurrentGlContext,
    ContextMismatch,
    OpenGlRuntimeUnavailable,
    MissingGlEntryPoint,
    InteropProbeFailure,
    GlDeviceLuidQueryFailure,
    DxgiFactoryFailure,
    AdapterUnavailable,
    D3d12DeviceCreationFailure,
    TextureCreationFailure,
    FenceCreationFailure,
    SharedHandleCreationFailure,
    GlMemoryObjectCreationFailure,
    GlMemoryImportFailure,
    GlTextureCreationFailure,
    GlTextureStorageFailure,
    GlSemaphoreCreationFailure,
    GlSemaphoreImportFailure,
    GlSemaphoreSignalFailure,
    GlSemaphoreWaitFailure,
    CleanupFailure,
    OutOfMemory,
    InvalidStreamSlot,
    StreamSlotNotConsumed,
    AdapterMismatch,
};

struct GlExtD3D12Diagnostic {
    GlExtD3D12Status status{GlExtD3D12Status::Ok};
    std::int32_t hresult{};
    std::uint32_t win32_error{};
    std::uint32_t gl_error{};
    GlExtD3D12AdapterLuid adapter_luid{};
    std::uint32_t advertised_extension_mask{};
    std::uint32_t live_probe_used{};
    char detail[192]{};
};

[[nodiscard]] constexpr bool IsValidGlExtD3D12AdapterLuid(
    const GlExtD3D12AdapterLuid luid) noexcept {
    return luid.low_part != 0U || luid.high_part != 0;
}

[[nodiscard]] constexpr bool SameGlExtD3D12AdapterLuid(
    const GlExtD3D12AdapterLuid left,
    const GlExtD3D12AdapterLuid right) noexcept {
    return left.low_part == right.low_part &&
           left.high_part == right.high_part;
}

// wglGetProcAddress additionally uses 1, 2, 3 and -1 as failure sentinels.
[[nodiscard]] constexpr bool IsUsableGlExtD3D12ProcAddressValue(
    const std::uintptr_t value) noexcept {
    return value > 3U && value != static_cast<std::uintptr_t>(-1);
}

[[nodiscard]] constexpr bool HasExactGlExtD3D12ExtensionToken(
    const std::string_view extensions,
    const std::string_view requested) noexcept {
    if (requested.empty() || requested.find(' ') != std::string_view::npos) {
        return false;
    }
    std::size_t offset = 0U;
    while (offset < extensions.size()) {
        while (offset < extensions.size() && extensions[offset] == ' ') {
            ++offset;
        }
        const std::size_t end = extensions.find(' ', offset);
        const std::size_t length =
            (end == std::string_view::npos ? extensions.size() : end) -
            offset;
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

[[nodiscard]] constexpr GlExtD3D12Status
ValidateGlExtD3D12TextureDescription(
    const GlExtD3D12TextureDescription description) noexcept {
    if (description.width == 0U || description.height == 0U ||
        description.width > kGlExtD3D12MaximumDimension ||
        description.height > kGlExtD3D12MaximumDimension ||
        description.dxgi_format != kGlExtD3D12DxgiFormatR8G8B8A8Unorm) {
        return GlExtD3D12Status::InvalidTextureDescription;
    }
    return GlExtD3D12Status::Ok;
}

[[nodiscard]] constexpr bool IsValidGlExtD3D12SharedObjectName(
    const std::wstring_view name) noexcept {
    return !name.empty() && name.size() < 128U &&
           name.find(L'\0') == std::wstring_view::npos;
}

[[nodiscard]] constexpr bool IsValidGlExtD3D12StreamSlot(
    const std::size_t slot, const std::size_t active_slot_count) noexcept {
    return active_slot_count > 0U &&
           active_slot_count <= kGlExtD3D12StreamSlotCount &&
           slot < active_slot_count;
}

[[nodiscard]] constexpr bool AreDistinctGlExtD3D12RingObjectNames(
    const GlExtD3D12RingObjectNames& names) noexcept {
    if (names.ready_fence == names.consumed_fence) {
        return false;
    }
    for (std::size_t slot = 0U; slot < names.colors.size(); ++slot) {
        if (names.colors[slot] == names.ready_fence ||
            names.colors[slot] == names.consumed_fence) {
            return false;
        }
        for (std::size_t other = slot + 1U; other < names.colors.size();
             ++other) {
            if (names.colors[slot] == names.colors[other]) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] constexpr bool CanSignalGlExtD3D12StreamSlot(
    const std::uint64_t slot_last_ready,
    const std::uint64_t slot_last_consumed) noexcept {
    return slot_last_ready == 0U || slot_last_consumed >= slot_last_ready;
}

[[nodiscard]] constexpr GlExtD3D12Status DecideGlExtD3D12CreateStream(
    const bool initialized, const bool stream_created,
    const bool context_matches) noexcept {
    if (!initialized) {
        return GlExtD3D12Status::NotInitialized;
    }
    if (stream_created) {
        return GlExtD3D12Status::StreamAlreadyCreated;
    }
    if (!context_matches) {
        return GlExtD3D12Status::ContextMismatch;
    }
    return GlExtD3D12Status::Ok;
}

[[nodiscard]] constexpr GlExtD3D12Status
DecideGlExtD3D12SemaphoreOperation(
    const bool initialized, const bool stream_created,
    const bool context_matches, const std::uint64_t value,
    const std::uint64_t previous_value) noexcept {
    if (!initialized) {
        return GlExtD3D12Status::NotInitialized;
    }
    if (!stream_created) {
        return GlExtD3D12Status::StreamNotCreated;
    }
    if (!context_matches) {
        return GlExtD3D12Status::ContextMismatch;
    }
    if (value == 0U || value <= previous_value ||
        value == (std::numeric_limits<std::uint64_t>::max)()) {
        return GlExtD3D12Status::InvalidFenceValue;
    }
    return GlExtD3D12Status::Ok;
}

[[nodiscard]] std::string_view ToString(GlExtD3D12Status status) noexcept;

struct GlExtD3D12BridgeNativeState;

// Render-thread-only zero-copy bridge. D3D12 creates named committed resources;
// the current OpenGL context imports those handles directly. The names match the
// host's existing protocol, but D3D12 requires GENERIC_ALL for every named handle.
class GlExtD3D12Bridge final {
public:
    GlExtD3D12Bridge() noexcept = default;
    ~GlExtD3D12Bridge();

    GlExtD3D12Bridge(const GlExtD3D12Bridge&) = delete;
    GlExtD3D12Bridge& operator=(const GlExtD3D12Bridge&) = delete;
    GlExtD3D12Bridge(GlExtD3D12Bridge&&) = delete;
    GlExtD3D12Bridge& operator=(GlExtD3D12Bridge&&) = delete;

    [[nodiscard]] GlExtD3D12Status Initialize(
        GlExtD3D12Diagnostic& diagnostic) noexcept;
    [[nodiscard]] GlExtD3D12Status CreateNamedStream(
        const GlExtD3D12TextureDescription& description,
        const GlExtD3D12ObjectNames& names,
        GlExtD3D12Diagnostic& diagnostic) noexcept;
    [[nodiscard]] GlExtD3D12Status CreateNamedStreams(
        const GlExtD3D12TextureDescription& description,
        const GlExtD3D12RingObjectNames& names,
        GlExtD3D12Diagnostic& diagnostic) noexcept;

    // Render thread only, after the caller has confirmed that the old context
    // was destroyed and the current replacement belongs to the main window.
    // HGLRC values may be recycled: this is not an ordinary context switch.
    // Preserves D3D12 colors, named handles, fences and ready sequence. Old GL
    // names are forgotten, never deleted in the replacement context. On failure
    // stream_created() is false but D3D12 resources remain available for retry.
    // After success, recreate caller-owned FBOs with gl_texture_name(slot) and
    // WaitConsumed(slot, its previous ready value) before writing each used slot.
    [[nodiscard]] GlExtD3D12Status RebindAfterContextReplacement(
        GlExtD3D12Diagnostic& diagnostic) noexcept;

    // SignalReady releases the texture to D3D12. WaitConsumed reacquires it
    // for GL after the producer has nonblockingly observed consumed_value().
    [[nodiscard]] GlExtD3D12Status SignalReady(
        std::uint64_t value, GlExtD3D12Diagnostic& diagnostic) noexcept;
    [[nodiscard]] GlExtD3D12Status SignalReady(
        std::size_t slot, std::uint64_t value,
        GlExtD3D12Diagnostic& diagnostic) noexcept;
    [[nodiscard]] GlExtD3D12Status WaitConsumed(
        std::uint64_t value, GlExtD3D12Diagnostic& diagnostic) noexcept;
    [[nodiscard]] GlExtD3D12Status WaitConsumed(
        std::size_t slot, std::uint64_t value,
        GlExtD3D12Diagnostic& diagnostic) noexcept;

    [[nodiscard]] GlExtD3D12Status ReleaseStream(
        GlExtD3D12Diagnostic& diagnostic) noexcept;
    [[nodiscard]] GlExtD3D12Status Shutdown(
        GlExtD3D12Diagnostic& diagnostic) noexcept;

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] bool stream_created() const noexcept;
    [[nodiscard]] bool owning_gl_context_is_current() const noexcept;
    [[nodiscard]] GlExtD3D12AdapterLuid adapter_luid() const noexcept;
    [[nodiscard]] std::uint32_t advertised_extension_mask() const noexcept;
    [[nodiscard]] bool live_probe_used() const noexcept;
    [[nodiscard]] std::uint32_t gl_texture_name() const noexcept;
    [[nodiscard]] std::uint32_t gl_texture_name(
        std::size_t slot) const noexcept;
    [[nodiscard]] std::size_t active_slot_count() const noexcept;
    [[nodiscard]] std::uint64_t consumed_value() const noexcept;
    [[nodiscard]] std::uint64_t ready_value() const noexcept;

private:
    GlExtD3D12BridgeNativeState* state_{};
};

} // namespace k2vr::game32
