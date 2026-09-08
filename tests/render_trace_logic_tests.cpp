#include "render_trace.hpp"

#include <cmath>
#include <iostream>

int main() {
    using namespace k2vr::game32;

    constexpr std::uintptr_t original = 0x0046C5C0U;
    constexpr std::uintptr_t wrapper = 0x12345678U;
    constexpr std::uintptr_t foreign = 0x76543210U;

    static_assert(DecideInstallCell(original, original, wrapper) ==
                  InstallCellDecision::SwapExpected);
    static_assert(DecideInstallCell(wrapper, original, wrapper) ==
                  InstallCellDecision::AlreadyOwned);
    static_assert(DecideInstallCell(foreign, original, wrapper) ==
                  InstallCellDecision::RejectForeign);

    static_assert(DecideRestoreCell(wrapper, original, wrapper) ==
                  RestoreCellDecision::RestoreOwned);
    static_assert(DecideRestoreCell(original, original, wrapper) ==
                  RestoreCellDecision::AlreadyOriginal);
    static_assert(DecideRestoreCell(foreign, original, wrapper) ==
                  RestoreCellDecision::PreserveForeign);

    static_assert(!EvaluateDoublePassInput(false, false, false, true)
                       .should_attempt);
    static_assert(!EvaluateDoublePassInput(true, true, false, true)
                       .should_attempt);
    static_assert(!EvaluateDoublePassInput(true, false, true, true)
                       .should_attempt);
    static_assert(!EvaluateDoublePassInput(true, false, true, false)
                       .should_attempt);
    static_assert(EvaluateDoublePassInput(true, false, false, true)
                      .should_attempt);
    static_assert(EvaluateDoublePassInput(true, false, true, false)
                      .next_was_down == false);
    static_assert(EvaluateDoublePassInput(true, false, false, true)
                      .next_was_down == true);

    const EngineCameraPoseWxyz authored{};
    k2vr::ipc::PoseF32 baseline{};
    baseline.orientation_w = 1.0F;
    const HmdCameraPoseResult identity =
        ComposeHmdCameraPose(authored, baseline, baseline);
    if (!identity.valid ||
        std::abs(identity.pose.orientation_w - 1.0F) > 0.0001F) {
        return 1;
    }

    k2vr::ipc::PoseF32 xr_yaw = baseline;
    const float half_sine = std::sqrt(0.5F);
    xr_yaw.orientation_y = half_sine;
    xr_yaw.orientation_w = half_sine;
    const HmdCameraPoseResult engine_yaw =
        ComposeHmdCameraPose(authored, baseline, xr_yaw);
    if (!engine_yaw.valid ||
        std::abs(engine_yaw.pose.orientation_y - half_sine) > 0.0001F ||
        std::abs(engine_yaw.pose.orientation_z) > 0.0001F) {
        return 1;
    }

    k2vr::ipc::PoseF32 xr_roll = baseline;
    xr_roll.orientation_z = half_sine;
    xr_roll.orientation_w = half_sine;
    const HmdCameraPoseResult engine_roll =
        ComposeHmdCameraPose(authored, baseline, xr_roll);
    if (!engine_roll.valid ||
        std::abs(engine_roll.pose.orientation_z - half_sine) > 0.0001F) {
        return 1;
    }

    std::cout << "Render-trace cell and F8 edge decisions passed.\n";
    return 0;
}
