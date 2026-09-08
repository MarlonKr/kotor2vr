#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace k2vr::ui {

struct RectF {
    float x;
    float y;
    float width;
    float height;
};

struct PointF {
    float x;
    float y;
};

// Maps a pointer on the actual quad into KOTOR's fixed logical surface. A
// point in letterbox/pillarbox padding returns nullopt instead of activating a
// control that is not visually under the cursor.
[[nodiscard]] inline std::optional<PointF> MapPointerToLogical(
    PointF pointer, RectF content_rect, std::uint32_t logical_width,
    std::uint32_t logical_height) noexcept {
    if (!std::isfinite(pointer.x) || !std::isfinite(pointer.y) ||
        !std::isfinite(content_rect.x) || !std::isfinite(content_rect.y) ||
        !std::isfinite(content_rect.width) ||
        !std::isfinite(content_rect.height) || content_rect.width <= 0.0F ||
        content_rect.height <= 0.0F || logical_width == 0 || logical_height == 0) {
        return std::nullopt;
    }
    const float normalized_x =
        (pointer.x - content_rect.x) / content_rect.width;
    const float normalized_y =
        (pointer.y - content_rect.y) / content_rect.height;
    if (normalized_x < 0.0F || normalized_x > 1.0F ||
        normalized_y < 0.0F || normalized_y > 1.0F) {
        return std::nullopt;
    }
    return PointF{
        std::clamp(normalized_x * static_cast<float>(logical_width), 0.0F,
                   static_cast<float>(logical_width - 1)),
        std::clamp(normalized_y * static_cast<float>(logical_height), 0.0F,
                   static_cast<float>(logical_height - 1)),
    };
}

// Largest centered rectangle preserving the logical UI aspect ratio.
[[nodiscard]] inline std::optional<RectF> FitLogicalSurface(
    RectF available, std::uint32_t logical_width,
    std::uint32_t logical_height) noexcept {
    if (available.width <= 0.0F || available.height <= 0.0F ||
        logical_width == 0 || logical_height == 0) {
        return std::nullopt;
    }
    const float logical_aspect = static_cast<float>(logical_width) /
                                 static_cast<float>(logical_height);
    const float available_aspect = available.width / available.height;
    RectF fitted = available;
    if (available_aspect > logical_aspect) {
        fitted.width = available.height * logical_aspect;
        fitted.x += (available.width - fitted.width) * 0.5F;
    } else {
        fitted.height = available.width / logical_aspect;
        fitted.y += (available.height - fitted.height) * 0.5F;
    }
    return fitted;
}

} // namespace k2vr::ui
