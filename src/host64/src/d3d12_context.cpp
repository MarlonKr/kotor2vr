#include "kotorvr/host/d3d12_context.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <iomanip>
#include <sstream>

namespace kotorvr::host {
namespace {

using Microsoft::WRL::ComPtr;

std::string hresult_string(const HRESULT result) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase
           << static_cast<std::uint32_t>(result);
    return stream.str();
}

AdapterLuid portable_luid(const LUID luid) {
    return {luid.LowPart, luid.HighPart};
}

LUID native_luid(const AdapterLuid luid) {
    return {luid.low_part, luid.high_part};
}

} // namespace

struct D3D12Context::Impl {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    AdapterLuid luid{};
    Logger* logger{};
};

D3D12Context::D3D12Context() : impl_(std::make_unique<Impl>()) {}
D3D12Context::~D3D12Context() { shutdown(); }

bool D3D12Context::initialize(const D3D12Requirements& requirements, Logger& logger) {
    shutdown();
    impl_ = std::make_unique<Impl>();
    impl_->logger = &logger;

    const HRESULT factory_result = CreateDXGIFactory2(0, IID_PPV_ARGS(&impl_->factory));
    if (FAILED(factory_result)) {
        logger.write(LogLevel::error,
                     "d3d12_factory_failed",
                     "CreateDXGIFactory2 failed",
                     {{"hresult", hresult_string(factory_result)}});
        return false;
    }

    if (requirements.adapter_luid.specified()) {
        ComPtr<IDXGIAdapter4> adapter;
        const HRESULT adapter_result = impl_->factory->EnumAdapterByLuid(
            native_luid(requirements.adapter_luid), IID_PPV_ARGS(&adapter));
        if (FAILED(adapter_result) || FAILED(adapter.As(&impl_->adapter))) {
            logger.write(LogLevel::error,
                         "d3d12_adapter_not_found",
                         "The OpenXR-selected adapter LUID is unavailable",
                         {{"hresult", hresult_string(adapter_result)}});
            return false;
        }
    } else {
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT result = impl_->factory->EnumAdapterByGpuPreference(
                index,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&candidate));
            if (result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(result)) {
                continue;
            }
            DXGI_ADAPTER_DESC1 description{};
            candidate->GetDesc1(&description);
            if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                impl_->adapter = std::move(candidate);
                break;
            }
        }
    }

    if (!impl_->adapter) {
        logger.write(LogLevel::error,
                     "d3d12_adapter_not_found",
                     "No suitable hardware D3D12 adapter was found");
        return false;
    }

    DXGI_ADAPTER_DESC1 adapter_description{};
    impl_->adapter->GetDesc1(&adapter_description);
    impl_->luid = portable_luid(adapter_description.AdapterLuid);

    const auto requested_level = requirements.minimum_feature_level == 0
                                     ? D3D_FEATURE_LEVEL_11_0
                                     : static_cast<D3D_FEATURE_LEVEL>(
                                           requirements.minimum_feature_level);
    const HRESULT device_result = D3D12CreateDevice(
        impl_->adapter.Get(), requested_level, IID_PPV_ARGS(&impl_->device));
    if (FAILED(device_result)) {
        logger.write(LogLevel::error,
                     "d3d12_device_failed",
                     "D3D12CreateDevice failed on the OpenXR-selected adapter",
                     {{"hresult", hresult_string(device_result)},
                      {"feature_level", std::to_string(requirements.minimum_feature_level)}});
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    const HRESULT queue_result = impl_->device->CreateCommandQueue(
        &queue_description, IID_PPV_ARGS(&impl_->queue));
    if (FAILED(queue_result)) {
        logger.write(LogLevel::error,
                     "d3d12_queue_failed",
                     "Unable to create the OpenXR direct command queue",
                     {{"hresult", hresult_string(queue_result)}});
        return false;
    }

    logger.write(LogLevel::info,
                 "d3d12_ready",
                 "D3D12 device and direct queue initialized",
                 {{"adapter_luid_low", std::to_string(impl_->luid.low_part)},
                  {"adapter_luid_high", std::to_string(impl_->luid.high_part)}});
    return true;
}

void D3D12Context::shutdown() noexcept {
    if (!impl_) {
        return;
    }
    impl_->queue.Reset();
    impl_->device.Reset();
    impl_->adapter.Reset();
    impl_->factory.Reset();
    impl_->luid = {};
}

bool D3D12Context::ready() const noexcept {
    return impl_ && impl_->device && impl_->queue;
}

ID3D12Device* D3D12Context::device() const noexcept {
    return impl_ ? impl_->device.Get() : nullptr;
}

ID3D12CommandQueue* D3D12Context::queue() const noexcept {
    return impl_ ? impl_->queue.Get() : nullptr;
}

AdapterLuid D3D12Context::adapter_luid() const noexcept {
    return impl_ ? impl_->luid : AdapterLuid{};
}

} // namespace kotorvr::host

