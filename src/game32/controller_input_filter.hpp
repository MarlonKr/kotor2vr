#pragma once

#include <cstdint>

namespace k2vr::game32::controller {

// Xbox-compatible DirectInput button indices, NOT XInput wButtons values.
inline constexpr std::uint32_t kA = 1U << 0;
inline constexpr std::uint32_t kB = 1U << 1;
inline constexpr std::uint32_t kX = 1U << 2;
inline constexpr std::uint32_t kY = 1U << 3;
inline constexpr std::uint32_t kView = 1U << 6;
inline constexpr std::uint32_t kMenu = 1U << 7;
inline constexpr std::uint32_t kDown = 1U << 16; // virtual bit for POV[0]
inline constexpr std::uint32_t kPartners = kA | kB | kX | kY | kMenu | kDown;
inline constexpr std::uint32_t kIntercept = kView | kPartners;

[[nodiscard]] constexpr bool PovHasDown(std::uint32_t pov) noexcept {
    return (pov & 0xFFFFU) != 0xFFFFU && pov >= 13500U && pov <= 22500U;
}

struct FilterResult {
    std::uint32_t buttons{};
    bool standalone_view_tap{};
};

// One instance per physical DirectInput device. A successful poll is one step;
// the game consumes the returned state immediately, before its edge generator.
class ViewChordFilter {
public:
    constexpr void Disconnect() noexcept { *this = {}; }

    [[nodiscard]] constexpr FilterResult Apply(std::uint32_t raw) noexcept {
        if (!primed_) {
            primed_ = true;
            // Never manufacture a tap/press from a button held across install,
            // input loss or reconnect. Neutral first polls arm immediately.
            blocked_ = raw & kIntercept;
            view_was_down_ = (raw & kView) != 0;
            chord_used_ = view_was_down_;
            return {raw & ~blocked_, false};
        }
        blocked_ &= raw; // each consumed button must physically release
        const bool view_down = (raw & kView) != 0;
        bool tap = false;
        if (view_down) {
            if (!view_was_down_) chord_used_ = false;
            const auto partners = raw & kPartners;
            if (partners) {
                chord_used_ = true;
                blocked_ |= partners;
            }
        } else if (view_was_down_ && !chord_used_) {
            tap = true;
        }
        view_was_down_ = view_down;
        auto filtered = raw & ~(blocked_ | kView);
        if (tap) filtered |= kView;
        return {filtered, tap};
    }

private:
    std::uint32_t blocked_{};
    bool primed_{};
    bool view_was_down_{};
    bool chord_used_{};
};

} // namespace k2vr::game32::controller
