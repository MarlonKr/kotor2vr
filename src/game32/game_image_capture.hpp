#pragma once

#include "../common/ipc_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#if defined(_WIN32)
#if defined(K2VR_GAME32_BUILD)
#define K2VR_GAME_IMAGE_EXPORT extern "C" __declspec(dllexport)
#else
#define K2VR_GAME_IMAGE_EXPORT extern "C" __declspec(dllimport)
#endif
#define K2VR_GAME_IMAGE_THREAD_CALL __stdcall
#else
#define K2VR_GAME_IMAGE_EXPORT extern "C"
#define K2VR_GAME_IMAGE_THREAD_CALL
#endif

namespace k2vr::game32 {

inline constexpr std::uint32_t kGameImageMagic = 0x4956324BU;
inline constexpr std::uint16_t kGameImageMajor = 1;
inline constexpr std::uint16_t kGameImageMinor = 0;
inline constexpr std::uint32_t kGameImagePixelFormatBgra8Unorm = 1;
inline constexpr std::uint32_t kGameImageGpuDiagnosticMagic = 0x31555047U;
inline constexpr std::uint32_t kGameImageMaximumDimension = 4096;
inline constexpr std::uint32_t kGameImageHeaderSize = 64;
inline constexpr int kGameImageCaptureVirtualKey = 0x76; // VK_F7

#pragma pack(push, 1)

struct GameImageSmokeBootstrapV1 {
    std::uint32_t structure_size;
    std::uint16_t version_major;
    std::uint16_t version_minor;
    ipc::SessionNonce session_nonce;
    std::uint64_t generation;
};

// The producer owns sequence. Zero means no frame, odd means an incomplete
// write, and a non-zero even value describes a stable header plus pixels.
struct GameImageSharedHeaderV1 {
    std::uint32_t magic;
    std::uint16_t version_major;
    std::uint16_t version_minor;
    std::uint32_t header_size;
    std::uint32_t mapping_size;
    volatile std::uint32_t sequence;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t stride;
    std::uint32_t pixel_format;
    std::uint64_t frame_id;
    std::uint32_t reserved[5];
};

struct GameImageGpuDiagnosticV1 {
    std::uint32_t magic;
    std::uint32_t producer_status;
    std::uint32_t interop_status;
    std::uint32_t hresult_bits;
    std::uint32_t win32_error;
};

#pragma pack(pop)

enum class GameImageCaptureResult : std::uint32_t {
    Ok = 0,
    InvalidArgument = 1,
    VersionMismatch = 2,
    InvalidSession = 3,
    Busy = 4,
    PersistentLogFailure = 5,
};

// Per-render-thread routing used while the camera hook performs more than one
// Scene pass. Default preserves the original F7/GPU-stream behavior,
// VrCapture marks the pass intended for the headset, and Suppressed makes the
// post-Scene callback a strict no-op.
enum class GameImageCapturePolicy : std::uint8_t {
    Default = 0,
    VrCapture = 1,
    Suppressed = 2,
};

// A window may be rendered by a different thread from the one that created
// it, and two DC handles may refer to that same window. GL resource ownership
// is checked separately by each producer against its original render thread.
[[nodiscard]] constexpr bool IsGameContextRecoverySurface(
    std::uintptr_t context, std::uintptr_t current_window,
    std::uintptr_t target_window, bool visible,
    std::uint32_t window_process, std::uint32_t current_process) noexcept {
    return context && current_window && current_window == target_window &&
        visible && window_process && window_process == current_process;
}

namespace detail {

inline thread_local GameImageCapturePolicy g_game_image_capture_policy =
    GameImageCapturePolicy::Default;

} // namespace detail

[[nodiscard]] inline GameImageCapturePolicy GetGameImageCapturePolicy()
    noexcept {
    return detail::g_game_image_capture_policy;
}

// Returns the previous policy so callers that cannot use an object scope can
// still restore nested routing exactly.
[[nodiscard]] inline GameImageCapturePolicy SetGameImageCapturePolicy(
    GameImageCapturePolicy policy) noexcept {
    const GameImageCapturePolicy previous =
        detail::g_game_image_capture_policy;
    detail::g_game_image_capture_policy = policy;
    return previous;
}

class ScopedGameImageCapturePolicy final {
public:
    explicit ScopedGameImageCapturePolicy(GameImageCapturePolicy policy)
        noexcept
        : previous_(SetGameImageCapturePolicy(policy)) {}

    ~ScopedGameImageCapturePolicy() noexcept {
        (void)SetGameImageCapturePolicy(previous_);
    }

    ScopedGameImageCapturePolicy(const ScopedGameImageCapturePolicy&) =
        delete;
    ScopedGameImageCapturePolicy& operator=(
        const ScopedGameImageCapturePolicy&) = delete;
    ScopedGameImageCapturePolicy(ScopedGameImageCapturePolicy&&) = delete;
    ScopedGameImageCapturePolicy& operator=(
        ScopedGameImageCapturePolicy&&) = delete;

private:
    GameImageCapturePolicy previous_;
};

enum class GameImageLayoutStatus : std::uint8_t {
    Ok = 0,
    InvalidDimensions = 1,
    SizeOverflow = 2,
};

struct GameImageLayout {
    GameImageLayoutStatus status{GameImageLayoutStatus::InvalidDimensions};
    std::uint32_t stride{0};
    std::uint32_t pixel_bytes{0};
    std::uint32_t mapping_bytes{0};

    [[nodiscard]] constexpr bool ok() const noexcept {
        return status == GameImageLayoutStatus::Ok;
    }
};

struct GameImageInputTransition {
    bool next_was_down{false};
    bool should_capture{false};
};

[[nodiscard]] constexpr GameImageCaptureResult
ValidateGameImageSmokeBootstrap(
    const GameImageSmokeBootstrapV1& bootstrap) noexcept {
    if (bootstrap.structure_size != sizeof(GameImageSmokeBootstrapV1)) {
        return GameImageCaptureResult::InvalidArgument;
    }
    if (bootstrap.version_major != kGameImageMajor ||
        bootstrap.version_minor != kGameImageMinor) {
        return GameImageCaptureResult::VersionMismatch;
    }
    if (!ipc::IsValid(bootstrap.session_nonce) || bootstrap.generation == 0) {
        return GameImageCaptureResult::InvalidSession;
    }
    return GameImageCaptureResult::Ok;
}

[[nodiscard]] constexpr GameImageInputTransition EvaluateGameImageInput(
    bool enabled, bool already_claimed, bool was_down, bool is_down) noexcept {
    return {is_down,
            enabled && !already_claimed && !was_down && is_down};
}

[[nodiscard]] constexpr GameImageLayout ComputeGameImageLayout(
    std::uint32_t width, std::uint32_t height) noexcept {
    if (width == 0 || height == 0 || width > kGameImageMaximumDimension ||
        height > kGameImageMaximumDimension) {
        return {};
    }
    const std::uint64_t stride = static_cast<std::uint64_t>(width) * 4U;
    const std::uint64_t pixel_bytes = stride * height;
    const std::uint64_t mapping_bytes = kGameImageHeaderSize + pixel_bytes;
    if (stride > (std::numeric_limits<std::uint32_t>::max)() ||
        pixel_bytes > (std::numeric_limits<std::uint32_t>::max)() ||
        mapping_bytes > (std::numeric_limits<std::uint32_t>::max)()) {
        return {GameImageLayoutStatus::SizeOverflow, 0, 0, 0};
    }
    return {GameImageLayoutStatus::Ok,
            static_cast<std::uint32_t>(stride),
            static_cast<std::uint32_t>(pixel_bytes),
            static_cast<std::uint32_t>(mapping_bytes)};
}

[[nodiscard]] constexpr std::uint32_t SourceRowForTopDown(
    std::uint32_t top_down_row, std::uint32_t height) noexcept {
    return height - 1U - top_down_row;
}

// Called by the combined render-trace bootstrap outside DllMain. The input is
// copied synchronously; the injector may release it after the export returns.
[[nodiscard]] GameImageCaptureResult ArmGameImageCapture(
    void* bootstrap_v1) noexcept;
void CancelGameImageCaptureArm() noexcept;

// Called exactly after the original Scene+0xB8 method returns. It never writes
// engine state and becomes a cheap no-op unless the F7 diagnostic is armed.
void GameImageCaptureAfterScenePass() noexcept;
[[nodiscard]] bool InstallGameImagePresentCapture() noexcept;
// The verified world-camera entry is another safe boundary after a graphics
// reset, even when the replacement renderer has not used our present import.
void RecoverGameImageContextsForScene() noexcept;

static_assert(sizeof(GameImageSmokeBootstrapV1) == 32);
static_assert(offsetof(GameImageSmokeBootstrapV1, session_nonce) == 8);
static_assert(offsetof(GameImageSmokeBootstrapV1, generation) == 24);
static_assert(sizeof(GameImageSharedHeaderV1) == kGameImageHeaderSize);
static_assert(sizeof(GameImageGpuDiagnosticV1) ==
              sizeof(GameImageSharedHeaderV1::reserved));
static_assert(offsetof(GameImageSharedHeaderV1, sequence) == 16);
static_assert(offsetof(GameImageSharedHeaderV1, frame_id) == 36);
static_assert(std::is_standard_layout_v<GameImageSharedHeaderV1>);
static_assert(std::is_trivially_copyable_v<GameImageSharedHeaderV1>);

} // namespace k2vr::game32

// Combined opt-in entry point implemented alongside the trace installer. A
// successful return means F7 capture is armed and the Scene+0xB8 hook is live.
K2VR_GAME_IMAGE_EXPORT std::uint32_t K2VR_GAME_IMAGE_THREAD_CALL
K2VR_GameImageSmokeBootstrap(void* bootstrap_v1) noexcept;
