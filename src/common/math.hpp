#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace k2vr::math {

inline constexpr float kPi = 3.14159265358979323846F;
inline constexpr float kEpsilon = 1.0e-6F;

struct Vec3 {
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

// Quaternion storage is explicitly x, y, z, w.
struct Quat {
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
    float w{1.0F};
};

struct Pose {
    Vec3 position{};
    Quat orientation{};
};

struct PoseClampLimits {
    // Radius of the permitted local translation. Zero disables translation.
    float max_translation{0.0F};
    // Shortest-arc angular distance from identity, in radians.
    float max_rotation{kPi};
};

struct SeatedLeanLimits {
    // x/z form the seated horizontal plane; y is vertical in OpenXR space.
    float max_horizontal{0.20F};
    float max_vertical{0.10F};
};

struct FovAngles {
    float left;
    float right;
    float up;
    float down;
};

struct Matrix4x4 {
    // Column-major, matching OpenGL's conventional memory layout.
    std::array<float, 16> value{};
};

[[nodiscard]] constexpr Vec3 operator+(Vec3 lhs, Vec3 rhs) noexcept {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

[[nodiscard]] constexpr Vec3 operator-(Vec3 lhs, Vec3 rhs) noexcept {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

[[nodiscard]] constexpr Vec3 operator*(Vec3 value, float scale) noexcept {
    return {value.x * scale, value.y * scale, value.z * scale};
}

[[nodiscard]] constexpr Vec3 operator/(Vec3 value, float scale) noexcept {
    return {value.x / scale, value.y / scale, value.z / scale};
}

[[nodiscard]] constexpr float Dot(Vec3 lhs, Vec3 rhs) noexcept {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

[[nodiscard]] constexpr Vec3 Cross(Vec3 lhs, Vec3 rhs) noexcept {
    return {lhs.y * rhs.z - lhs.z * rhs.y,
            lhs.z * rhs.x - lhs.x * rhs.z,
            lhs.x * rhs.y - lhs.y * rhs.x};
}

[[nodiscard]] inline float Length(Vec3 value) noexcept {
    return std::sqrt(Dot(value, value));
}

[[nodiscard]] inline bool IsFinite(Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

[[nodiscard]] inline bool IsFinite(Quat value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

[[nodiscard]] inline bool IsFinite(const Pose& value) noexcept {
    return IsFinite(value.position) && IsFinite(value.orientation);
}

[[nodiscard]] constexpr float Dot(Quat lhs, Quat rhs) noexcept {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z + lhs.w * rhs.w;
}

[[nodiscard]] constexpr Quat operator-(Quat value) noexcept {
    return {-value.x, -value.y, -value.z, -value.w};
}

[[nodiscard]] constexpr Quat Conjugate(Quat value) noexcept {
    return {-value.x, -value.y, -value.z, value.w};
}

[[nodiscard]] constexpr Quat Multiply(Quat lhs, Quat rhs) noexcept {
    return {
        lhs.w * rhs.x + lhs.x * rhs.w + lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.w * rhs.y - lhs.x * rhs.z + lhs.y * rhs.w + lhs.z * rhs.x,
        lhs.w * rhs.z + lhs.x * rhs.y - lhs.y * rhs.x + lhs.z * rhs.w,
        lhs.w * rhs.w - lhs.x * rhs.x - lhs.y * rhs.y - lhs.z * rhs.z,
    };
}

[[nodiscard]] inline Quat NormalizeOrIdentity(Quat value) noexcept {
    if (!IsFinite(value)) {
        return {};
    }
    const float length_squared = Dot(value, value);
    if (!(length_squared > kEpsilon * kEpsilon)) {
        return {};
    }
    const float inverse_length = 1.0F / std::sqrt(length_squared);
    return {value.x * inverse_length,
            value.y * inverse_length,
            value.z * inverse_length,
            value.w * inverse_length};
}

[[nodiscard]] inline Vec3 Rotate(Quat rotation, Vec3 value) noexcept {
    rotation = NormalizeOrIdentity(rotation);
    const Vec3 imaginary{rotation.x, rotation.y, rotation.z};
    const Vec3 twice_cross = Cross(imaginary, value) * 2.0F;
    return value + twice_cross * rotation.w + Cross(imaginary, twice_cross);
}

[[nodiscard]] inline Quat FromAxisAngle(Vec3 axis, float radians) noexcept {
    if (!IsFinite(axis) || !std::isfinite(radians)) {
        return {};
    }
    const float axis_length = Length(axis);
    if (!(axis_length > kEpsilon)) {
        return {};
    }
    axis = axis / axis_length;
    const float half_angle = radians * 0.5F;
    const float sine = std::sin(half_angle);
    return NormalizeOrIdentity(
        {axis.x * sine, axis.y * sine, axis.z * sine, std::cos(half_angle)});
}

[[nodiscard]] inline float AngularDistanceFromIdentity(Quat value) noexcept {
    value = NormalizeOrIdentity(value);
    if (value.w < 0.0F) {
        value = -value;
    }
    return 2.0F * std::acos(std::clamp(value.w, -1.0F, 1.0F));
}

[[nodiscard]] inline Vec3 ClampLength(Vec3 value, float maximum) noexcept {
    if (!IsFinite(value) || !std::isfinite(maximum) || maximum <= 0.0F) {
        return {};
    }
    const float length = Length(value);
    if (!(length > maximum) || !(length > kEpsilon)) {
        return value;
    }
    return value * (maximum / length);
}

[[nodiscard]] inline Vec3 ClampSeatedLean(
    Vec3 value, SeatedLeanLimits limits) noexcept {
    if (!IsFinite(value) || !std::isfinite(limits.max_horizontal) ||
        !std::isfinite(limits.max_vertical) || limits.max_horizontal < 0.0F ||
        limits.max_vertical < 0.0F) {
        return {};
    }
    const Vec3 horizontal = ClampLength(
        {value.x, 0.0F, value.z}, limits.max_horizontal);
    return {horizontal.x,
            std::clamp(value.y, -limits.max_vertical, limits.max_vertical),
            horizontal.z};
}

[[nodiscard]] inline Quat ClampAngularDistance(Quat value,
                                               float maximum_radians) noexcept {
    if (!std::isfinite(maximum_radians) || maximum_radians <= 0.0F) {
        return {};
    }

    value = NormalizeOrIdentity(value);
    if (value.w < 0.0F) {
        value = -value;
    }

    maximum_radians = std::min(maximum_radians, kPi);
    const float angle = AngularDistanceFromIdentity(value);
    if (!(angle > maximum_radians)) {
        return value;
    }

    const Vec3 imaginary{value.x, value.y, value.z};
    const float imaginary_length = Length(imaginary);
    if (!(imaginary_length > kEpsilon)) {
        return {};
    }
    return FromAxisAngle(imaginary / imaginary_length, maximum_radians);
}

[[nodiscard]] inline Pose Compose(const Pose& parent, const Pose& local) noexcept {
    const Quat parent_rotation = NormalizeOrIdentity(parent.orientation);
    const Quat local_rotation = NormalizeOrIdentity(local.orientation);
    return {
        parent.position + Rotate(parent_rotation, local.position),
        NormalizeOrIdentity(Multiply(parent_rotation, local_rotation)),
    };
}

[[nodiscard]] inline Pose Inverse(const Pose& value) noexcept {
    const Quat inverse_rotation = Conjugate(NormalizeOrIdentity(value.orientation));
    return {Rotate(inverse_rotation, value.position * -1.0F), inverse_rotation};
}

[[nodiscard]] inline Pose RelativeTo(const Pose& origin,
                                     const Pose& world_pose) noexcept {
    return Compose(Inverse(origin), world_pose);
}

[[nodiscard]] inline Pose ClampPoseDelta(const Pose& local_delta,
                                         PoseClampLimits limits) noexcept {
    if (!IsFinite(local_delta)) {
        return {};
    }
    return {
        ClampLength(local_delta.position, limits.max_translation),
        ClampAngularDistance(local_delta.orientation, limits.max_rotation),
    };
}

// The order is intentional: the engine-provided base camera remains the
// cinematic/navigation anchor, then bounded HMD motion, then per-eye offset.
[[nodiscard]] inline Pose ComposeTrackedEye(const Pose& base_camera,
                                            const Pose& local_head_delta,
                                            const Pose& local_eye_offset,
                                            PoseClampLimits limits) noexcept {
    if (!IsFinite(base_camera) || !IsFinite(local_eye_offset)) {
        return {};
    }
    return Compose(Compose(base_camera, ClampPoseDelta(local_head_delta, limits)),
                   local_eye_offset);
}

// Right-handed OpenGL projection from OpenXR's asymmetric angular FOV. The
// resulting clip-depth range is [-1,+1], as expected by the game's GL path.
[[nodiscard]] inline std::optional<Matrix4x4> OpenGlProjection(
    FovAngles fov, float near_z, float far_z) noexcept {
    if (!std::isfinite(fov.left) || !std::isfinite(fov.right) ||
        !std::isfinite(fov.up) || !std::isfinite(fov.down) ||
        !std::isfinite(near_z) || !std::isfinite(far_z) || near_z <= 0.0F ||
        far_z <= near_z) {
        return std::nullopt;
    }
    const float tangent_left = std::tan(fov.left);
    const float tangent_right = std::tan(fov.right);
    const float tangent_up = std::tan(fov.up);
    const float tangent_down = std::tan(fov.down);
    const float width = tangent_right - tangent_left;
    const float height = tangent_up - tangent_down;
    if (!std::isfinite(width) || !std::isfinite(height) ||
        width <= kEpsilon || height <= kEpsilon) {
        return std::nullopt;
    }

    Matrix4x4 result{};
    result.value[0] = 2.0F / width;
    result.value[5] = 2.0F / height;
    result.value[8] = (tangent_right + tangent_left) / width;
    result.value[9] = (tangent_up + tangent_down) / height;
    result.value[10] = -(far_z + near_z) / (far_z - near_z);
    result.value[11] = -1.0F;
    result.value[14] = -(2.0F * far_z * near_z) / (far_z - near_z);
    return result;
}

static_assert(sizeof(Vec3) == 12);
static_assert(sizeof(Quat) == 16);
static_assert(sizeof(Pose) == 28);
static_assert(sizeof(Matrix4x4) == 64);

} // namespace k2vr::math
