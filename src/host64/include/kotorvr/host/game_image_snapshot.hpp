#pragma once

#include "ipc_protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace kotorvr::host {

inline constexpr std::uint32_t game_image_magic = 0x4956324BU; // "K2VI"
inline constexpr std::uint16_t game_image_version_major = 1;
inline constexpr std::uint16_t game_image_version_minor = 0;
inline constexpr std::uint32_t game_image_pixel_format_bgra8_unorm = 1;
inline constexpr std::uint32_t game_image_gpu_diagnostic_magic = 0x31555047U;
inline constexpr std::uint32_t game_image_interop_status_unavailable =
    0xFFFFFFFFU;
inline constexpr std::uint32_t game_image_max_dimension = 4096;
inline constexpr std::size_t game_image_name_capacity = 112;

#pragma pack(push, 1)
// Version-1 producer/consumer wire header. sequence is a 32-bit seqlock:
// zero means no image, odd means a writer owns the buffer, and a stable equal
// even value before/after the pixel copy commits one complete snapshot.
struct GameImageSnapshotHeader {
    std::uint32_t magic;                 // 0
    std::uint16_t version_major;         // 4
    std::uint16_t version_minor;         // 6
    std::uint32_t header_size;           // 8
    std::uint32_t mapping_size;          // 12
    std::uint32_t sequence;              // 16 (naturally aligned)
    std::uint32_t width;                 // 20
    std::uint32_t height;                // 24
    std::uint32_t stride;                // 28
    std::uint32_t pixel_format;          // 32
    std::uint64_t frame_id;              // 36
    std::array<std::uint32_t, 5> reserved; // 44
};
#pragma pack(pop)

static_assert(sizeof(GameImageSnapshotHeader) == 64);
static_assert(offsetof(GameImageSnapshotHeader, sequence) == 16);
static_assert(offsetof(GameImageSnapshotHeader, frame_id) == 36);
static_assert(offsetof(GameImageSnapshotHeader, reserved) == 44);

struct GameImageGpuDiagnostic {
    std::uint32_t magic{};
    std::uint32_t producer_status{};
    std::uint32_t interop_status{game_image_interop_status_unavailable};
    std::uint32_t hresult_bits{};
    std::uint32_t win32_error{};

    [[nodiscard]] constexpr bool present() const noexcept {
        return magic == game_image_gpu_diagnostic_magic;
    }
};

[[nodiscard]] constexpr GameImageGpuDiagnostic DecodeGameImageGpuDiagnostic(
    const GameImageSnapshotHeader& header) noexcept {
    return {header.reserved[0], header.reserved[1], header.reserved[2],
            header.reserved[3], header.reserved[4]};
}

[[nodiscard]] std::string_view GpuProducerStatusName(
    std::uint32_t status) noexcept;
[[nodiscard]] std::string_view GpuInteropStatusName(
    std::uint32_t status) noexcept;

struct GameImageMappingName {
    std::array<wchar_t, game_image_name_capacity> characters{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return characters[0] != L'\0';
    }
    [[nodiscard]] constexpr const wchar_t* c_str() const noexcept {
        return characters.data();
    }
};

enum class GameImageSnapshotStatus : std::uint32_t {
    Ok = 0,
    Unchanged,
    MappingUnavailable,
    MappingViewFailure,
    NoFrame,
    SnapshotContended,
    InvalidHeader,
    InvalidDimensions,
    UnsupportedPixelFormat,
    AllocationFailure,
};

struct GameImageFrame {
    std::uint64_t frame_id{};
    std::uint32_t sequence{};
    std::uint32_t width{};
    std::uint32_t height{};
    GameImageGpuDiagnostic gpu_diagnostic{};
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] bool valid() const noexcept {
        return frame_id != 0 && sequence != 0 && width != 0 && height != 0 &&
               pixels.size() == static_cast<std::size_t>(width) * height * 4U;
    }
};

[[nodiscard]] GameImageMappingName MakeGameImageMappingName(
    k2vr::ipc::SessionNonce nonce) noexcept;
[[nodiscard]] GameImageSnapshotStatus ValidateGameImageSnapshotHeader(
    const GameImageSnapshotHeader& header, std::size_t mapped_size) noexcept;
[[nodiscard]] std::string_view ToString(GameImageSnapshotStatus status) noexcept;

class GameImageSnapshotReader final {
public:
    GameImageSnapshotReader() noexcept = default;
    ~GameImageSnapshotReader();

    GameImageSnapshotReader(const GameImageSnapshotReader&) = delete;
    GameImageSnapshotReader& operator=(const GameImageSnapshotReader&) = delete;

    [[nodiscard]] GameImageSnapshotStatus TryOpen(
        k2vr::ipc::SessionNonce nonce) noexcept;
    [[nodiscard]] GameImageSnapshotStatus ReadLatest() noexcept;
    void Close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return mapped_view_ != nullptr; }
    [[nodiscard]] bool has_frame() const noexcept { return latest_.valid(); }
    [[nodiscard]] const GameImageFrame& latest() const noexcept { return latest_; }

private:
    void* mapping_handle_{};
    void* mapped_view_{};
    std::size_t mapped_size_{};
    std::uint32_t last_sequence_{};
    GameImageFrame latest_;
    std::vector<std::uint8_t> scratch_;
};

} // namespace kotorvr::host
