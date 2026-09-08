#pragma once
#include "stereo_stream.hpp"
#include <cstdlib>

namespace kotorvr::host {
struct StereoEyeResolutionSetting {
    std::uint32_t percent{k2vr::ipc::kStereoDefaultEyePercent};
    bool valid{true};
    bool from_environment{};
    bool fixed{};
    std::uint32_t width{};
    std::uint32_t height{};
};

[[nodiscard]] inline StereoEyeResolutionSetting ParseStereoEyeResolutionSetting(const char* value, const char* fixed_value=nullptr) noexcept {
    if (fixed_value) {
        StereoEyeResolutionSetting result{};
        result.fixed=true;
        result.from_environment=true;
        result.valid=false;
        const char* cursor=fixed_value;
        const auto dimension=[&](std::uint32_t& output) {
            unsigned digits=0;
            while (*cursor>='0' && *cursor<='9') {
                if (++digits>4) return false;
                output=output*10U+static_cast<std::uint32_t>(*cursor++-'0');
            }
            return digits!=0 && output>=256 && output<=4096 && output%2U==0;
        };
        std::uint32_t width{},height{};
        if (!dimension(width) || *cursor++!='x' || !dimension(height) || *cursor!='\0') return result;
        // Transport includes HUD and raw depth rows; never silently reduce fixed eyes.
        if (width*2U>8192 || k2vr::ipc::StereoAtlasHeight(height)>8192) return result;
        result.width=width;
        result.height=height;
        result.valid=true;
        return result;
    }
    if (!value) return {};
    const auto percent=k2vr::ipc::ParseStereoEyePercent(value);
    return {percent.value_or(k2vr::ipc::kStereoDefaultEyePercent),percent.has_value(),true};
}

// One process-wide snapshot keeps IPC requests and XR/shared allocations equal.
// A changed setting is applied by the next host process, never mid-session.
[[nodiscard]] inline const StereoEyeResolutionSetting& NativeStereoEyeResolutionSetting() noexcept {
    static const StereoEyeResolutionSetting setting=[] {
#ifdef _MSC_VER
        char* fixed_value{};
        if (_dupenv_s(&fixed_value,nullptr,"KOTOR2VR_EYE_RESOLUTION")!=0)
            return StereoEyeResolutionSetting{k2vr::ipc::kStereoDefaultEyePercent,false,true,true};
        if (fixed_value) {
            const auto parsed=ParseStereoEyeResolutionSetting(nullptr,fixed_value);
            std::free(fixed_value);
            return parsed;
        }
        char* value{};
        if (_dupenv_s(&value,nullptr,"KOTOR2VR_EYE_PERCENT")!=0)
            return StereoEyeResolutionSetting{k2vr::ipc::kStereoDefaultEyePercent,false,true};
        const auto parsed=ParseStereoEyeResolutionSetting(value);
        std::free(value);
        return parsed;
#else
        return ParseStereoEyeResolutionSetting(std::getenv("KOTOR2VR_EYE_PERCENT"),std::getenv("KOTOR2VR_EYE_RESOLUTION"));
#endif
    }();
    return setting;
}

[[nodiscard]] inline std::uint32_t StereoEyeWidth(std::uint32_t recommended,
    const StereoEyeResolutionSetting& setting) noexcept {
    return setting.fixed ? (setting.valid ? setting.width:0U):
        k2vr::ipc::StereoEyeDimension(recommended,setting.percent);
}
[[nodiscard]] inline std::uint32_t StereoEyeHeight(std::uint32_t recommended,
    const StereoEyeResolutionSetting& setting) noexcept {
    return setting.fixed ? (setting.valid ? setting.height:0U):
        k2vr::ipc::StereoEyeDimension(recommended,setting.percent);
}
[[nodiscard]] inline std::uint32_t NativeStereoEyeWidth(std::uint32_t recommended) noexcept {
    return StereoEyeWidth(recommended,NativeStereoEyeResolutionSetting());
}
[[nodiscard]] inline std::uint32_t NativeStereoEyeHeight(std::uint32_t recommended) noexcept {
    return StereoEyeHeight(recommended,NativeStereoEyeResolutionSetting());
}
}
