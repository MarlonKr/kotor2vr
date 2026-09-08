#include "kotorvr/host/gpu_stream_consumer.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>

namespace kotorvr::host {
namespace {

template <typename Interface>
[[nodiscard]] HRESULT OpenNamedD3D12Object(
    ID3D12Device* const device, const wchar_t* const name,
    const DWORD access,
    Microsoft::WRL::ComPtr<Interface>& output) noexcept {
    HANDLE shared_handle{};
    HRESULT result =
        device->OpenSharedHandleByName(name, access, &shared_handle);
    if (FAILED(result)) {
        return result;
    }
    result = device->OpenSharedHandle(
        shared_handle, IID_PPV_ARGS(output.ReleaseAndGetAddressOf()));
    CloseHandle(shared_handle);
    return result;
}

[[nodiscard]] GpuStreamTextureDescription PortableDescription(
    const D3D12_RESOURCE_DESC& source) noexcept {
    return {static_cast<std::uint32_t>(source.Dimension),
            source.Width,
            source.Height,
            source.DepthOrArraySize,
            source.MipLevels,
            static_cast<std::int64_t>(source.Format),
            source.SampleDesc.Count,
            source.SampleDesc.Quality};
}

} // namespace

std::string_view ToString(const GpuStreamConsumerStatus status) noexcept {
    switch (status) {
    case GpuStreamConsumerStatus::Ok: return "ok";
    case GpuStreamConsumerStatus::InvalidArgument: return "invalid-argument";
    case GpuStreamConsumerStatus::ObjectUnavailable: return "object-unavailable";
    case GpuStreamConsumerStatus::ObjectOpenFailed: return "object-open-failed";
    case GpuStreamConsumerStatus::InvalidTexture: return "invalid-texture";
    case GpuStreamConsumerStatus::InvalidFence: return "invalid-fence";
    case GpuStreamConsumerStatus::CacheCreationFailed:
        return "cache-creation-failed";
    case GpuStreamConsumerStatus::NotOpen: return "not-open";
    case GpuStreamConsumerStatus::NoNewFrame: return "no-new-frame";
    case GpuStreamConsumerStatus::FenceQueryFailed: return "fence-query-failed";
    case GpuStreamConsumerStatus::InvalidFrameToken: return "invalid-frame-token";
    case GpuStreamConsumerStatus::IncompatibleDestination:
        return "incompatible-destination";
    case GpuStreamConsumerStatus::QueueWaitFailed: return "queue-wait-failed";
    case GpuStreamConsumerStatus::QueueSignalFailed: return "queue-signal-failed";
    }
    return "unknown";
}

struct GpuStreamConsumer::Impl {
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,
               k2vr::ipc::kGpuStreamSlotCount>
        colors;
    Microsoft::WRL::ComPtr<ID3D12Resource> cached_color;
    Microsoft::WRL::ComPtr<ID3D12Fence> ready_fence;
    Microsoft::WRL::ComPtr<ID3D12Fence> consumed_fence;
    std::size_t slot_count{};
    std::uint64_t last_consumed{};
    bool has_cached_frame{};
    Extent2D expected{};
    HRESULT last_error{S_OK};
};

GpuStreamConsumer::GpuStreamConsumer() : impl_(std::make_unique<Impl>()) {}
GpuStreamConsumer::~GpuStreamConsumer() { Close(); }

GpuStreamConsumerStatus GpuStreamConsumer::TryOpen(
    ID3D12Device* const device,
    const k2vr::ipc::SessionNonce nonce, const Extent2D expected) noexcept {
    if (is_open()) {
        return GpuStreamConsumerStatus::Ok;
    }
    if (device == nullptr || !k2vr::ipc::IsValid(nonce)) {
        return GpuStreamConsumerStatus::InvalidArgument;
    }
    Close();
    impl_->expected = expected;

    std::array<k2vr::ipc::GpuStreamObjectName,
               k2vr::ipc::kGpuStreamSlotCount>
        color_names{};
    for (std::size_t slot = 0; slot < color_names.size(); ++slot) {
        color_names[slot] =
            k2vr::ipc::MakeGpuStreamColorObjectName(nonce, slot);
    }
    const auto legacy_color_name = k2vr::ipc::MakeGpuStreamObjectName(
        nonce, k2vr::ipc::GpuStreamObjectKind::Color);
    const auto ready_name = k2vr::ipc::MakeGpuStreamObjectName(
        nonce, k2vr::ipc::GpuStreamObjectKind::ReadyFence);
    const auto consumed_name = k2vr::ipc::MakeGpuStreamObjectName(
        nonce, k2vr::ipc::GpuStreamObjectKind::ConsumedFence);
    bool color_names_valid = legacy_color_name.valid();
    for (const auto& name : color_names) {
        color_names_valid = color_names_valid && name.valid();
    }
    if (!color_names_valid || !ready_name.valid() ||
        !consumed_name.valid()) {
        return GpuStreamConsumerStatus::InvalidArgument;
    }

    // The primary GL_EXT/D3D12 producer publishes all three suffixed colors
    // before its shared fences. If that ring is absent, retain compatibility
    // with the proven unsuffixed single-texture D3D11/WGL fallback.
    bool opened_ring = true;
    for (std::size_t slot = 0; slot < color_names.size(); ++slot) {
        impl_->last_error = OpenNamedD3D12Object(
            device, color_names[slot].c_str(), GENERIC_ALL,
            impl_->colors[slot]);
        if (FAILED(impl_->last_error)) {
            opened_ring = false;
            break;
        }
    }
    if (opened_ring) {
        impl_->slot_count = k2vr::ipc::kGpuStreamSlotCount;
    } else {
        for (auto& color : impl_->colors) {
            color.Reset();
        }
        impl_->last_error = OpenNamedD3D12Object(
            device, legacy_color_name.c_str(), GENERIC_ALL,
            impl_->colors[0]);
        if (FAILED(impl_->last_error)) {
            impl_->last_error = OpenNamedD3D12Object(
                device, legacy_color_name.c_str(),
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                impl_->colors[0]);
        }
        if (FAILED(impl_->last_error)) {
            const HRESULT saved = impl_->last_error;
            Close();
            impl_->last_error = saved;
            return GpuStreamConsumerStatus::ObjectUnavailable;
        }
        impl_->slot_count = 1U;
    }
    impl_->last_error = OpenNamedD3D12Object(
        device, ready_name.c_str(), GENERIC_ALL, impl_->ready_fence);
    if (FAILED(impl_->last_error)) {
        const HRESULT saved = impl_->last_error;
        Close();
        impl_->last_error = saved;
        return GpuStreamConsumerStatus::ObjectUnavailable;
    }
    impl_->last_error = OpenNamedD3D12Object(
        device, consumed_name.c_str(), GENERIC_ALL,
        impl_->consumed_fence);
    if (FAILED(impl_->last_error)) {
        const HRESULT saved = impl_->last_error;
        Close();
        impl_->last_error = saved;
        return GpuStreamConsumerStatus::ObjectUnavailable;
    }

    for (std::size_t slot = 0; slot < impl_->slot_count; ++slot) {
        if (!ValidateGpuStreamTextureDescription(
                PortableDescription(impl_->colors[slot]->GetDesc()),impl_->expected)) {
            Close();
            return GpuStreamConsumerStatus::InvalidTexture;
        }
    }

    D3D12_HEAP_PROPERTIES cache_heap{};
    cache_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    cache_heap.CreationNodeMask = 1;
    cache_heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC cache_description = impl_->colors[0]->GetDesc();
    cache_description.Flags = D3D12_RESOURCE_FLAG_NONE;
    impl_->last_error = device->CreateCommittedResource(
        &cache_heap, D3D12_HEAP_FLAG_NONE, &cache_description,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(impl_->cached_color.ReleaseAndGetAddressOf()));
    if (FAILED(impl_->last_error)) {
        const HRESULT saved = impl_->last_error;
        Close();
        impl_->last_error = saved;
        return GpuStreamConsumerStatus::CacheCreationFailed;
    }
    const std::uint64_t ready = impl_->ready_fence->GetCompletedValue();
    const std::uint64_t consumed = impl_->consumed_fence->GetCompletedValue();
    if (ready == (std::numeric_limits<std::uint64_t>::max)() ||
        consumed == (std::numeric_limits<std::uint64_t>::max)()) {
        Close();
        return GpuStreamConsumerStatus::InvalidFence;
    }
    impl_->last_consumed = consumed;
    impl_->has_cached_frame = false;
    impl_->last_error = S_OK;
    return GpuStreamConsumerStatus::Ok;
}

GpuStreamPollResult GpuStreamConsumer::PollLatest() noexcept {
    if (!is_open()) {
        return {GpuStreamConsumerStatus::NotOpen, {}};
    }
    return SelectGpuStreamFrame(impl_->ready_fence->GetCompletedValue(),
                                impl_->last_consumed,
                                impl_->slot_count);
}

GpuStreamConsumerStatus GpuStreamConsumer::QueueWait(
    ID3D12CommandQueue* const queue,
    const GpuStreamFrameToken frame) noexcept {
    if (!is_open() || queue == nullptr ||
        !IsGpuStreamFrameTokenCompatible(frame, impl_->slot_count) ||
        frame.ready_value <= impl_->last_consumed) {
        return GpuStreamConsumerStatus::InvalidFrameToken;
    }
    impl_->last_error = queue->Wait(impl_->ready_fence.Get(), frame.ready_value);
    return SUCCEEDED(impl_->last_error)
               ? GpuStreamConsumerStatus::Ok
               : GpuStreamConsumerStatus::QueueWaitFailed;
}

GpuStreamConsumerStatus GpuStreamConsumer::RecordCopy(
    ID3D12GraphicsCommandList* const command_list,
    ID3D12Resource* const destination,
    const GpuStreamFrameToken frame, const bool write_destination) noexcept {
    if (!is_open() || command_list == nullptr || destination == nullptr ||
        !IsGpuStreamFrameTokenCompatible(frame, impl_->slot_count)) {
        return GpuStreamConsumerStatus::InvalidArgument;
    }
    ID3D12Resource* const source = impl_->colors[frame.slot].Get();
    const D3D12_RESOURCE_DESC destination_description = destination->GetDesc();
    if (destination_description.Width >
            (std::numeric_limits<std::uint32_t>::max)() ||
        !IsGpuStreamDestinationCompatible(
            {static_cast<std::uint32_t>(destination_description.Width),
             destination_description.Height},
            static_cast<std::int64_t>(destination_description.Format),impl_->expected,true) ||
        destination_description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        destination_description.DepthOrArraySize != 1 ||
        destination_description.MipLevels != 1 ||
        destination_description.SampleDesc.Count != 1 ||
        destination_description.SampleDesc.Quality != 0) {
        return GpuStreamConsumerStatus::IncompatibleDestination;
    }

    std::array<D3D12_RESOURCE_BARRIER, 2> to_cache{};
    for (auto& barrier : to_cache) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    }
    to_cache[0].Transition.pResource = source;
    to_cache[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    to_cache[1].Transition.pResource = impl_->cached_color.Get();
    to_cache[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    command_list->ResourceBarrier(
        static_cast<UINT>(to_cache.size()), to_cache.data());
    command_list->CopyResource(impl_->cached_color.Get(), source);

    // Neural presentation replaces the visible image. Retain the complete raw
    // cache without first copying a soon-overwritten image into the XR buffer.
    if (!write_destination) {
        for (auto& barrier : to_cache) {
            barrier.Transition.StateBefore = barrier.Transition.StateAfter;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        }
        command_list->ResourceBarrier(static_cast<UINT>(to_cache.size()), to_cache.data());
        impl_->has_cached_frame = true;
        return GpuStreamConsumerStatus::Ok;
    }

    std::array<D3D12_RESOURCE_BARRIER, 3> cache_to_output{};
    cache_to_output[0] = to_cache[0];
    cache_to_output[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cache_to_output[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    cache_to_output[1] = to_cache[1];
    cache_to_output[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    cache_to_output[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cache_to_output[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    cache_to_output[2].Transition.pResource = destination;
    cache_to_output[2].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cache_to_output[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    cache_to_output[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    command_list->ResourceBarrier(
        static_cast<UINT>(cache_to_output.size()), cache_to_output.data());
    D3D12_TEXTURE_COPY_LOCATION visible_dst{}; visible_dst.pResource=destination;
    D3D12_TEXTURE_COPY_LOCATION visible_src{}; visible_src.pResource=impl_->cached_color.Get();
    const D3D12_BOX visible_box{0,0,0,static_cast<UINT>(destination_description.Width),destination_description.Height,1};
    command_list->CopyTextureRegion(&visible_dst,0,0,0,&visible_src,&visible_box);

    std::array<D3D12_RESOURCE_BARRIER, 2> to_common{
        cache_to_output[1], cache_to_output[2]};
    for (auto& barrier : to_common) {
        const D3D12_RESOURCE_STATES prior = barrier.Transition.StateAfter;
        barrier.Transition.StateBefore = prior;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    }
    command_list->ResourceBarrier(static_cast<UINT>(to_common.size()),
                                  to_common.data());
    impl_->has_cached_frame = true;
    return GpuStreamConsumerStatus::Ok;
}

GpuStreamConsumerStatus GpuStreamConsumer::RecordCachedCopy(
    ID3D12GraphicsCommandList* const command_list,
    ID3D12Resource* const destination) noexcept {
    if (!is_open() || command_list == nullptr || destination == nullptr) {
        return GpuStreamConsumerStatus::InvalidArgument;
    }
    if (!impl_->has_cached_frame) {
        return GpuStreamConsumerStatus::NoNewFrame;
    }
    const D3D12_RESOURCE_DESC destination_description = destination->GetDesc();
    if (destination_description.Width >
            (std::numeric_limits<std::uint32_t>::max)() ||
        !IsGpuStreamDestinationCompatible(
            {static_cast<std::uint32_t>(destination_description.Width),
             destination_description.Height},
            static_cast<std::int64_t>(destination_description.Format),impl_->expected,true) ||
        destination_description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        destination_description.DepthOrArraySize != 1 ||
        destination_description.MipLevels != 1 ||
        destination_description.SampleDesc.Count != 1 ||
        destination_description.SampleDesc.Quality != 0) {
        return GpuStreamConsumerStatus::IncompatibleDestination;
    }

    std::array<D3D12_RESOURCE_BARRIER, 2> to_copy{};
    for (auto& barrier : to_copy) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    }
    to_copy[0].Transition.pResource = impl_->cached_color.Get();
    to_copy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    to_copy[1].Transition.pResource = destination;
    to_copy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    command_list->ResourceBarrier(static_cast<UINT>(to_copy.size()),
                                  to_copy.data());
    D3D12_TEXTURE_COPY_LOCATION visible_dst{}; visible_dst.pResource=destination;
    D3D12_TEXTURE_COPY_LOCATION visible_src{}; visible_src.pResource=impl_->cached_color.Get();
    const D3D12_BOX visible_box{0,0,0,static_cast<UINT>(destination_description.Width),destination_description.Height,1};
    command_list->CopyTextureRegion(&visible_dst,0,0,0,&visible_src,&visible_box);
    for (auto& barrier : to_copy) {
        const D3D12_RESOURCE_STATES prior = barrier.Transition.StateAfter;
        barrier.Transition.StateBefore = prior;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    }
    command_list->ResourceBarrier(static_cast<UINT>(to_copy.size()),
                                  to_copy.data());
    return GpuStreamConsumerStatus::Ok;
}

GpuStreamConsumerStatus GpuStreamConsumer::SignalConsumed(
    ID3D12CommandQueue* const queue,
    const GpuStreamFrameToken frame) noexcept {
    if (!is_open() || queue == nullptr ||
        !IsGpuStreamFrameTokenCompatible(frame, impl_->slot_count) ||
        frame.ready_value <= impl_->last_consumed) {
        return GpuStreamConsumerStatus::InvalidFrameToken;
    }
    impl_->last_error =
        queue->Signal(impl_->consumed_fence.Get(), frame.ready_value);
    if (FAILED(impl_->last_error)) {
        return GpuStreamConsumerStatus::QueueSignalFailed;
    }
    impl_->last_consumed = frame.ready_value;
    return GpuStreamConsumerStatus::Ok;
}

void GpuStreamConsumer::Close() noexcept {
    if (!impl_) {
        return;
    }
    for (auto& color : impl_->colors) {
        color.Reset();
    }
    impl_->cached_color.Reset();
    impl_->ready_fence.Reset();
    impl_->consumed_fence.Reset();
    impl_->slot_count = 0U;
    impl_->last_consumed = 0;
    impl_->has_cached_frame = false;
    impl_->last_error = S_OK;
}

bool GpuStreamConsumer::is_open() const noexcept {
    if (!impl_ ||
        (impl_->slot_count != 1U &&
         impl_->slot_count != k2vr::ipc::kGpuStreamSlotCount) ||
        !impl_->cached_color || !impl_->ready_fence ||
        !impl_->consumed_fence) {
        return false;
    }
    for (std::size_t slot = 0; slot < impl_->slot_count; ++slot) {
        if (!impl_->colors[slot]) {
            return false;
        }
    }
    return true;
}

bool GpuStreamConsumer::has_cached_frame() const noexcept {
    return is_open() && impl_->has_cached_frame;
}
ID3D12Resource* GpuStreamConsumer::cached_texture() const noexcept {
    return impl_ && impl_->has_cached_frame ? impl_->cached_color.Get():nullptr;
}

std::size_t GpuStreamConsumer::slot_count() const noexcept {
    return is_open() ? impl_->slot_count : 0U;
}

std::uint64_t GpuStreamConsumer::last_consumed_value() const noexcept {
    return impl_ ? impl_->last_consumed : 0;
}

std::int32_t GpuStreamConsumer::last_native_error() const noexcept {
    return impl_ ? static_cast<std::int32_t>(impl_->last_error) : 0;
}

} // namespace kotorvr::host
