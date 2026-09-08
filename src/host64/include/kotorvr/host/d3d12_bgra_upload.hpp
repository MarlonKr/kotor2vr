#pragma once

#include "kotorvr/host/game_image_snapshot.hpp"
#include "kotorvr/host/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

struct ID3D12GraphicsCommandList;
struct ID3D12Device;
struct ID3D12Resource;

namespace kotorvr::host {

inline constexpr std::uint32_t d3d12_texture_row_pitch_alignment = 256;

struct BgraUploadLayout {
    std::uint32_t row_pitch{};
    std::uint64_t byte_size{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return row_pitch != 0 && byte_size != 0;
    }
};

enum class BgraUploadTarget : std::uint8_t {
    Unsupported = 0,
    Bgra8,
    Rgba8,
};

[[nodiscard]] BgraUploadLayout ComputeBgraUploadLayout(
    Extent2D extent) noexcept;
[[nodiscard]] BgraUploadTarget ClassifyBgraUploadTarget(
    std::int64_t dxgi_format) noexcept;

// Nearest-neighbour scaling from the producer's tightly packed, top-down
// BGRA8 snapshot into a D3D12 upload footprint. RGBA targets swap B and R.
[[nodiscard]] bool ResampleBgra8(
    const GameImageFrame& source, Extent2D destination_extent,
    std::span<std::uint8_t> destination, std::uint32_t destination_stride,
    BgraUploadTarget target) noexcept;

class D3D12BgraUpload final {
public:
    D3D12BgraUpload();
    ~D3D12BgraUpload();

    D3D12BgraUpload(const D3D12BgraUpload&) = delete;
    D3D12BgraUpload& operator=(const D3D12BgraUpload&) = delete;

    [[nodiscard]] bool Initialize(ID3D12Device* device, Extent2D extent,
                                  std::int64_t dxgi_format) noexcept;
    [[nodiscard]] bool Record(ID3D12GraphicsCommandList* command_list,
                              ID3D12Resource* destination,
                              const GameImageFrame& frame) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] BgraUploadLayout layout() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kotorvr::host
