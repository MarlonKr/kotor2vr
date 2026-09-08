#include "kotorvr/host/visible_smoke.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

bool close_enough(const float left, const float right) {
    return std::fabs(left - right) < 0.0001F;
}

} // namespace

int main() {
    using namespace kotorvr::host;

    const std::array<std::int64_t, 5> unordered_formats{
        10, smoke_format_bgra8_unorm, smoke_format_rgba8_srgb, 24, 2};
    const auto preferred = select_visible_smoke_format(unordered_formats);
    if (!preferred || *preferred != smoke_format_rgba8_srgb) {
        std::cerr << "sRGB format preference failed\n";
        return 1;
    }

    const std::array<std::int64_t, 2> fallback_formats{
        smoke_format_bgra8_unorm, smoke_format_rgba8_unorm};
    const auto fallback = select_visible_smoke_format(fallback_formats);
    if (!fallback || *fallback != smoke_format_rgba8_unorm) {
        std::cerr << "linear format fallback order failed\n";
        return 2;
    }

    const std::array<std::int64_t, 2> unsupported_formats{10, 24};
    if (select_visible_smoke_format(unsupported_formats).has_value()) {
        std::cerr << "unsupported format was accepted\n";
        return 3;
    }

    const auto full = select_visible_smoke_layout({4096, 4096});
    if (full.pixel_extent != Extent2D{1024, 576} ||
        !close_enough(full.width_m, 1.2F) ||
        !close_enough(full.height_m, 0.675F) ||
        !close_enough(full.distance_m, 1.5F)) {
        std::cerr << "default smoke layout failed\n";
        return 4;
    }

    const auto width_limited = select_visible_smoke_layout({800, 600});
    if (width_limited.pixel_extent != Extent2D{800, 450}) {
        std::cerr << "width-limited aspect fit failed\n";
        return 5;
    }

    const auto height_limited = select_visible_smoke_layout({1920, 400});
    if (height_limited.pixel_extent != Extent2D{711, 400}) {
        std::cerr << "height-limited aspect fit failed\n";
        return 6;
    }

    const auto tiny = select_visible_smoke_layout({1, 1});
    if (tiny.pixel_extent != Extent2D{1, 1} || !tiny.valid()) {
        std::cerr << "tiny valid runtime limit failed\n";
        return 7;
    }

    if (select_visible_smoke_layout({0, 400}).valid()) {
        std::cerr << "invalid runtime limit was accepted\n";
        return 8;
    }

    std::cout << "visible smoke format/layout tests passed\n";
    return 0;
}
