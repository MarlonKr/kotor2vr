#include "kotorvr/host/visible_smoke.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace kotorvr::host {

std::optional<std::int64_t>
select_visible_smoke_format(
    const std::span<const std::int64_t> runtime_formats) noexcept {
    constexpr std::array preferred_formats{
        smoke_format_rgba8_srgb,
        smoke_format_bgra8_srgb,
        smoke_format_rgba8_unorm,
        smoke_format_bgra8_unorm,
    };

    for (const std::int64_t preferred : preferred_formats) {
        if (std::find(runtime_formats.begin(), runtime_formats.end(), preferred) !=
            runtime_formats.end()) {
            return preferred;
        }
    }
    return std::nullopt;
}

VisibleSmokeLayout
select_visible_smoke_layout(const Extent2D maximum_swapchain_extent) noexcept {
    constexpr std::uint32_t target_width = 1024;
    constexpr std::uint32_t target_height = 576;

    VisibleSmokeLayout layout{};
    if (!maximum_swapchain_extent.valid()) {
        layout.pixel_extent = {};
        return layout;
    }

    std::uint32_t width = std::min(target_width, maximum_swapchain_extent.width);
    std::uint32_t height = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(width) * target_height) / target_width);
    height = std::max<std::uint32_t>(height, 1);

    if (height > maximum_swapchain_extent.height) {
        height = std::min(target_height, maximum_swapchain_extent.height);
        width = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(height) * target_width) / target_height);
        width = std::max<std::uint32_t>(width, 1);
    }

    layout.pixel_extent = {width, height};
    layout.height_m = layout.width_m * static_cast<float>(height) /
                      static_cast<float>(width);
    return layout;
}

} // namespace kotorvr::host
