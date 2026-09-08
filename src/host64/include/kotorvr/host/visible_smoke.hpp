#pragma once

#include "kotorvr/host/types.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace kotorvr::host {

// DXGI_FORMAT values are part of the OpenXR swapchain ABI. Keeping the pure
// selection helper independent of Windows headers makes it cheap to test.
inline constexpr std::int64_t smoke_format_rgba8_srgb = 29;
inline constexpr std::int64_t smoke_format_bgra8_srgb = 91;
inline constexpr std::int64_t smoke_format_rgba8_unorm = 28;
inline constexpr std::int64_t smoke_format_bgra8_unorm = 87;

struct VisibleSmokeLayout {
    Extent2D pixel_extent{};
    float width_m{1.2F};
    float height_m{0.675F};
    float distance_m{1.5F};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return pixel_extent.valid() && width_m > 0.0F && height_m > 0.0F &&
               distance_m > 0.0F;
    }
};

// Prefer explicitly sRGB 8-bit formats so the diagnostic colors are stable,
// then accept their linear equivalents. Other runtime formats are rejected
// because the smoke renderer only creates matching 8-bit RTVs.
[[nodiscard]] std::optional<std::int64_t>
select_visible_smoke_format(std::span<const std::int64_t> runtime_formats) noexcept;

// Produces a 16:9 diagnostic panel no larger than 1024x576 and never larger
// than the runtime's system-wide swapchain limit.
[[nodiscard]] VisibleSmokeLayout
select_visible_smoke_layout(Extent2D maximum_swapchain_extent) noexcept;

} // namespace kotorvr::host
