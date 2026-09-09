#include "kotorvr/host/d3d12_bgra_upload.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace kotorvr::host {
namespace {

[[nodiscard]] constexpr std::uint32_t align_up(
    const std::uint32_t value, const std::uint32_t alignment) noexcept {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

} // namespace

BgraUploadLayout ComputeBgraUploadLayout(const Extent2D extent) noexcept {
    if (!extent.valid() ||
        extent.width > (std::numeric_limits<std::uint32_t>::max)() / 4U) {
        return {};
    }
    const std::uint32_t tight_pitch = extent.width * 4U;
    if (tight_pitch >
        (std::numeric_limits<std::uint32_t>::max)() -
            (d3d12_texture_row_pitch_alignment - 1U)) {
        return {};
    }
    const std::uint32_t row_pitch =
        align_up(tight_pitch, d3d12_texture_row_pitch_alignment);
    const std::uint64_t byte_size =
        static_cast<std::uint64_t>(row_pitch) * extent.height;
    if (byte_size == 0 ||
        byte_size > static_cast<std::uint64_t>((std::numeric_limits<SIZE_T>::max)())) {
        return {};
    }
    return {row_pitch, byte_size};
}

BgraUploadTarget ClassifyBgraUploadTarget(
    const std::int64_t dxgi_format) noexcept {
    switch (static_cast<DXGI_FORMAT>(dxgi_format)) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return BgraUploadTarget::Bgra8;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return BgraUploadTarget::Rgba8;
    default:
        return BgraUploadTarget::Unsupported;
    }
}

bool ResampleBgra8(const GameImageFrame& source,
                   const Extent2D destination_extent,
                   const std::span<std::uint8_t> destination,
                   const std::uint32_t destination_stride,
                   const BgraUploadTarget target, const bool preserve_aspect) noexcept {
    if (!source.valid() || !destination_extent.valid() ||
        target == BgraUploadTarget::Unsupported ||
        destination_extent.width >
            (std::numeric_limits<std::uint32_t>::max)() / 4U ||
        destination_stride < destination_extent.width * 4U) {
        return false;
    }
    const std::uint64_t required =
        static_cast<std::uint64_t>(destination_stride) * destination_extent.height;
    if (required > destination.size()) {
        return false;
    }

    std::uint32_t fit_width=destination_extent.width,fit_height=destination_extent.height;
    if(preserve_aspect){
        if(std::uint64_t(source.width)*fit_height>std::uint64_t(source.height)*fit_width)
            fit_height=std::max(1U,static_cast<std::uint32_t>(std::uint64_t(fit_width)*source.height/source.width));
        else fit_width=std::max(1U,static_cast<std::uint32_t>(std::uint64_t(fit_height)*source.width/source.height));
    }
    const auto left=(destination_extent.width-fit_width)/2,top=(destination_extent.height-fit_height)/2;
    const std::size_t source_stride = static_cast<std::size_t>(source.width) * 4U;
    for (std::uint32_t y = 0; y < destination_extent.height; ++y) {
        const std::uint32_t source_y = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(y>=top ? y-top:0) * source.height / fit_height);
        const std::uint8_t* const source_row =
            source.pixels.data() + static_cast<std::size_t>(std::min(source_y,source.height-1)) * source_stride;
        std::uint8_t* const destination_row =
            destination.data() + static_cast<std::size_t>(y) * destination_stride;
        for (std::uint32_t x = 0; x < destination_extent.width; ++x) {
            if(x<left || x-left>=fit_width || y<top || y-top>=fit_height){
                auto* output=destination_row+x*4U;output[0]=output[1]=output[2]=0;output[3]=255;continue;
            }
            const std::uint32_t source_x = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(x-left) * source.width / fit_width);
            const std::uint8_t* const input = source_row + source_x * 4U;
            std::uint8_t* const output = destination_row + x * 4U;
            if (target == BgraUploadTarget::Bgra8) {
                std::memcpy(output, input, 4U);
            } else {
                output[0] = input[2];
                output[1] = input[1];
                output[2] = input[0];
                output[3] = input[3];
            }
        }
    }
    return true;
}

struct D3D12BgraUpload::Impl {
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    std::uint8_t* mapped{};
    Extent2D extent{};
    BgraUploadLayout layout{};
    BgraUploadTarget target{BgraUploadTarget::Unsupported};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
};

D3D12BgraUpload::D3D12BgraUpload() : impl_(std::make_unique<Impl>()) {}
D3D12BgraUpload::~D3D12BgraUpload() { Shutdown(); }

bool D3D12BgraUpload::Initialize(ID3D12Device* const device,
                                 const Extent2D extent,
                                 const std::int64_t dxgi_format) noexcept {
    Shutdown();
    if (device == nullptr) {
        return false;
    }
    const BgraUploadLayout upload_layout = ComputeBgraUploadLayout(extent);
    const BgraUploadTarget target = ClassifyBgraUploadTarget(dxgi_format);
    if (!upload_layout.valid() || target == BgraUploadTarget::Unsupported) {
        return false;
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Alignment = 0;
    description.Width = upload_layout.byte_size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = D3D12_RESOURCE_FLAG_NONE;

    const HRESULT created = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&impl_->upload));
    if (FAILED(created)) {
        return false;
    }
    void* mapped{};
    const D3D12_RANGE no_read{0, 0};
    if (FAILED(impl_->upload->Map(0, &no_read, &mapped)) || mapped == nullptr) {
        impl_->upload.Reset();
        return false;
    }
    impl_->mapped = static_cast<std::uint8_t*>(mapped);
    impl_->extent = extent;
    impl_->layout = upload_layout;
    impl_->target = target;
    impl_->format = static_cast<DXGI_FORMAT>(dxgi_format);
    return true;
}

bool D3D12BgraUpload::Record(ID3D12GraphicsCommandList* const command_list,
                             ID3D12Resource* const destination,
                             const GameImageFrame& frame,const bool preserve_aspect) noexcept {
    if (!ready() || command_list == nullptr || destination == nullptr ||
        !ResampleBgra8(frame, impl_->extent,
                       {impl_->mapped, static_cast<std::size_t>(impl_->layout.byte_size)},
                       impl_->layout.row_pitch, impl_->target,preserve_aspect)) {
        return false;
    }

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = impl_->upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = 0;
    source.PlacedFootprint.Footprint.Format = impl_->format;
    source.PlacedFootprint.Footprint.Width = impl_->extent.width;
    source.PlacedFootprint.Footprint.Height = impl_->extent.height;
    source.PlacedFootprint.Footprint.Depth = 1;
    source.PlacedFootprint.Footprint.RowPitch = impl_->layout.row_pitch;

    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = destination;
    target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.SubresourceIndex = 0;
    command_list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    return true;
}

void D3D12BgraUpload::Shutdown() noexcept {
    if (!impl_) {
        return;
    }
    if (impl_->upload && impl_->mapped != nullptr) {
        impl_->upload->Unmap(0, nullptr);
    }
    impl_->mapped = nullptr;
    impl_->upload.Reset();
    impl_->extent = {};
    impl_->layout = {};
    impl_->target = BgraUploadTarget::Unsupported;
    impl_->format = DXGI_FORMAT_UNKNOWN;
}

bool D3D12BgraUpload::ready() const noexcept {
    return impl_ && impl_->upload && impl_->mapped != nullptr &&
           impl_->extent.valid() && impl_->layout.valid() &&
           impl_->target != BgraUploadTarget::Unsupported;
}

BgraUploadLayout D3D12BgraUpload::layout() const noexcept {
    return impl_ ? impl_->layout : BgraUploadLayout{};
}

} // namespace kotorvr::host
