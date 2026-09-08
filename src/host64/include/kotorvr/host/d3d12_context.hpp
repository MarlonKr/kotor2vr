#pragma once

#include "kotorvr/host/logger.hpp"

#include <cstdint>
#include <memory>

struct ID3D12CommandQueue;
struct ID3D12Device;

namespace kotorvr::host {

struct AdapterLuid {
    std::uint32_t low_part{};
    std::int32_t high_part{};

    [[nodiscard]] constexpr bool specified() const noexcept {
        return low_part != 0 || high_part != 0;
    }

    friend constexpr bool operator==(const AdapterLuid&, const AdapterLuid&) = default;
};

struct D3D12Requirements {
    AdapterLuid adapter_luid{};
    std::uint32_t minimum_feature_level{};
};

class D3D12Context final {
public:
    D3D12Context();
    ~D3D12Context();

    D3D12Context(const D3D12Context&) = delete;
    D3D12Context& operator=(const D3D12Context&) = delete;

    [[nodiscard]] bool initialize(const D3D12Requirements& requirements, Logger& logger);
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] ID3D12Device* device() const noexcept;
    [[nodiscard]] ID3D12CommandQueue* queue() const noexcept;
    [[nodiscard]] AdapterLuid adapter_luid() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kotorvr::host

