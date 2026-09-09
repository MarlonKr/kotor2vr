#pragma once
#include "math.hpp"

namespace kotorvr::host {
struct UprightUiHead {
    k2vr::math::Pose pose{};
    float yaw{};
    bool valid{};
};

// OpenXR uses +Y for gravity-up and -Z for forward. Follow gaze yaw/pitch,
// but keep the panel's right axis horizontal, independent of head roll.
[[nodiscard]] inline UprightUiHead MakeUprightUiHead(
    const k2vr::math::Pose& head, float previous_yaw=0.F) noexcept {
    using namespace k2vr::math;
    if (!IsFinite(head) || Dot(head.orientation,head.orientation)<kEpsilon*kEpsilon)
        return {};
    const auto forward=Rotate(head.orientation,{0,0,-1});
    const float horizontal=std::sqrt(forward.x*forward.x+forward.z*forward.z);
    // Yaw is undefined at a vertical gaze. Retain its last stable value instead
    // of letting tiny tracking noise spin the panel around the gaze axis.
    const float yaw=horizontal>0.01F ? std::atan2(-forward.x,-forward.z) :
        (std::isfinite(previous_yaw) ? previous_yaw:0.F);
    const float pitch=std::atan2(forward.y,horizontal);
    const auto rotation=Multiply(FromAxisAngle({0,1,0},yaw),FromAxisAngle({1,0,0},pitch));
    return {{head.position,rotation},yaw,true};
}

[[nodiscard]] inline k2vr::math::Pose PlaceUiPanel(
    const k2vr::math::Pose& upright_head,float distance,float vertical_offset=0.F) noexcept {
    return {upright_head.position+k2vr::math::Rotate(upright_head.orientation,
        {0,vertical_offset,-distance}),upright_head.orientation};
}
}
