#pragma once

#include "gpu_stream_contract.hpp"
#include "kotorvr/host/types.hpp"

#include <cstdint>
#include <cstddef>
#include <limits>
#include <memory>
#include <string_view>

struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace kotorvr::host {

inline constexpr std::uint32_t gpu_stream_texture2d_dimension = 3;
inline constexpr std::int64_t gpu_stream_dxgi_format_rgba8_typeless = 27;
inline constexpr std::int64_t gpu_stream_dxgi_format_rgba8_unorm_srgb = 29;

struct GpuStreamTextureDescription {
    std::uint32_t dimension{};
    std::uint64_t width{};
    std::uint32_t height{};
    std::uint16_t depth_or_array_size{};
    std::uint16_t mip_levels{};
    std::int64_t format{};
    std::uint32_t sample_count{};
    std::uint32_t sample_quality{};
};

enum class GpuStreamConsumerStatus : std::uint32_t {
    Ok = 0,
    InvalidArgument,
    ObjectUnavailable,
    ObjectOpenFailed,
    InvalidTexture,
    InvalidFence,
    CacheCreationFailed,
    NotOpen,
    NoNewFrame,
    FenceQueryFailed,
    InvalidFrameToken,
    IncompatibleDestination,
    QueueWaitFailed,
    QueueSignalFailed,
};

struct GpuStreamFrameToken {
    std::uint64_t ready_value{};
    std::size_t slot{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return k2vr::ipc::IsUsableSharedFenceValue(ready_value) &&
               slot < k2vr::ipc::kGpuStreamSlotCount;
    }
};

[[nodiscard]] constexpr bool IsGpuStreamFrameTokenCompatible(
    const GpuStreamFrameToken frame,
    const std::size_t slot_count) noexcept {
    if (!frame.valid()) {
        return false;
    }
    if (slot_count == 1U) {
        return frame.slot == 0U;
    }
    return slot_count == k2vr::ipc::kGpuStreamSlotCount &&
           frame.slot ==
               k2vr::ipc::GpuStreamSlotForSequence(frame.ready_value);
}

struct GpuStreamPollResult {
    GpuStreamConsumerStatus status{GpuStreamConsumerStatus::NotOpen};
    GpuStreamFrameToken frame{};

    [[nodiscard]] constexpr bool has_frame() const noexcept {
        return status == GpuStreamConsumerStatus::Ok && frame.valid();
    }
};

[[nodiscard]] constexpr bool ValidateGpuStreamTextureDescription(
    const GpuStreamTextureDescription& description,
    const Extent2D expected = {k2vr::ipc::kGpuStreamWidth,k2vr::ipc::kGpuStreamHeight}) noexcept {
    return description.dimension == gpu_stream_texture2d_dimension &&
           expected.width > 0 && expected.height > 0 &&
           expected.width <= 8192 && expected.height <= 8192 &&
           description.width == expected.width && description.height == expected.height &&
           description.depth_or_array_size == 1 &&
           description.mip_levels == 1 &&
           description.format ==
               k2vr::ipc::kGpuStreamDxgiFormatRgba8Unorm &&
           description.sample_count == 1 && description.sample_quality == 0;
}

[[nodiscard]] constexpr bool IsGpuStreamDestinationCompatible(
    const Extent2D extent, const std::int64_t dxgi_format,
    const Extent2D expected = {k2vr::ipc::kGpuStreamWidth,k2vr::ipc::kGpuStreamHeight},
    const bool allow_vertical_crop = false) noexcept {
    const bool rgba8_family =
        dxgi_format == gpu_stream_dxgi_format_rgba8_typeless ||
        dxgi_format == k2vr::ipc::kGpuStreamDxgiFormatRgba8Unorm ||
        dxgi_format == gpu_stream_dxgi_format_rgba8_unorm_srgb;
    const bool matching = extent == expected || (allow_vertical_crop && extent.width == expected.width &&
        extent.height > 0 && extent.height < expected.height);
    return matching && expected.width > 0 && expected.height > 0 &&
           expected.width <= 8192 && expected.height <= 8192 &&
           rgba8_family;
}

[[nodiscard]] constexpr GpuStreamPollResult SelectGpuStreamFrame(
    const std::uint64_t ready_value,
    const std::uint64_t last_consumed_value,
    const std::size_t slot_count = k2vr::ipc::kGpuStreamSlotCount) noexcept {
    if (slot_count != 1U && slot_count != k2vr::ipc::kGpuStreamSlotCount) {
        return {GpuStreamConsumerStatus::InvalidArgument, {}};
    }
    if (ready_value == (std::numeric_limits<std::uint64_t>::max)()) {
        return {GpuStreamConsumerStatus::FenceQueryFailed, {}};
    }
    if (!k2vr::ipc::HasNewGpuStreamFrame(ready_value,
                                         last_consumed_value)) {
        return {GpuStreamConsumerStatus::NoNewFrame, {}};
    }
    return {GpuStreamConsumerStatus::Ok,
            {ready_value,
             slot_count == 1U
                 ? 0U
                 : k2vr::ipc::GpuStreamSlotForSequence(ready_value)}};
}

[[nodiscard]] std::string_view ToString(
    GpuStreamConsumerStatus status) noexcept;

class GpuStreamConsumer final {
public:
    GpuStreamConsumer();
    ~GpuStreamConsumer();

    GpuStreamConsumer(const GpuStreamConsumer&) = delete;
    GpuStreamConsumer& operator=(const GpuStreamConsumer&) = delete;

    [[nodiscard]] GpuStreamConsumerStatus TryOpen(
        ID3D12Device* device, k2vr::ipc::SessionNonce nonce,
        Extent2D expected = {k2vr::ipc::kGpuStreamWidth,k2vr::ipc::kGpuStreamHeight}) noexcept;
    [[nodiscard]] GpuStreamPollResult PollLatest() noexcept;
    [[nodiscard]] GpuStreamConsumerStatus QueueWait(
        ID3D12CommandQueue* queue, GpuStreamFrameToken frame) noexcept;
    [[nodiscard]] GpuStreamConsumerStatus RecordCopy(
        ID3D12GraphicsCommandList* command_list,
        ID3D12Resource* destination,
        GpuStreamFrameToken frame, bool write_destination = true) noexcept;
    // Replays the most recently accepted producer image into a newly acquired
    // OpenXR swapchain image. This prevents the lower-rate game stream from
    // alternating with the CPU fallback while OpenXR continues at 72 Hz.
    [[nodiscard]] GpuStreamConsumerStatus RecordCachedCopy(
        ID3D12GraphicsCommandList* command_list,
        ID3D12Resource* destination) noexcept;
    [[nodiscard]] GpuStreamConsumerStatus SignalConsumed(
        ID3D12CommandQueue* queue, GpuStreamFrameToken frame) noexcept;
    void Close() noexcept;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool has_cached_frame() const noexcept;
    [[nodiscard]] ID3D12Resource* cached_texture() const noexcept;
    [[nodiscard]] std::size_t slot_count() const noexcept;
    [[nodiscard]] std::uint64_t last_consumed_value() const noexcept;
    [[nodiscard]] std::int32_t last_native_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kotorvr::host
