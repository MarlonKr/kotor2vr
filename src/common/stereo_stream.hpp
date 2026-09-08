#pragma once
#include "gpu_stream_contract.hpp"
#include "math.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

namespace k2vr::ipc {
// Both eyes occupy disjoint atlas rectangles. One fence commits the WHOLE
// same-tick pair; never publish or consume an individual eye's completion.
// v5 adds optional metric depth. v4 (336 bytes) is deliberately incompatible.
inline constexpr std::uint32_t kStereoMetadataMagic = 0x3553544BU;
inline constexpr std::uint32_t kStereoMetadataSize = 384;
inline constexpr std::uint32_t kStereoHudMaximumHeight = 1536;
[[nodiscard]] constexpr std::uint32_t StereoAtlasHeight(std::uint32_t eye_height) {
    return eye_height*2U+kStereoHudMaximumHeight;
}
[[nodiscard]] constexpr std::uint32_t StereoDepthOffset(std::uint32_t eye_height) {
    return eye_height+kStereoHudMaximumHeight;
}
inline constexpr std::uint32_t kStereoDefaultEyePercent = 75;
inline constexpr float kStereoRenderScale = 0.75F;
[[nodiscard]] constexpr std::optional<std::uint32_t> ParseStereoEyePercent(std::string_view text) noexcept {
    if (text.size()<2 || text.size()>3) return std::nullopt;
    std::uint32_t percent{};
    for (const char digit:text) {
        if (digit<'0' || digit>'9') return std::nullopt;
        percent=percent*10U+static_cast<std::uint32_t>(digit-'0');
    }
    if (percent<50 || percent>100) return std::nullopt;
    return percent;
}
[[nodiscard]] constexpr std::uint32_t StereoEyeDimension(std::uint32_t recommended,
    std::uint32_t percent=kStereoDefaultEyePercent) noexcept {
    if (percent<50 || percent>100) return 0;
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(recommended)*percent+99U)/100U);
}
[[nodiscard]] constexpr SessionNonce StereoStreamNonce(SessionNonce nonce) {
    // Isolate v5 GPU objects AND metadata from v4/mono. An old/new executable
    // pair must fail discovery, never combine old pixels with new metadata.
    return {nonce.low ^ 0x53544552454F3035ULL, nonce.high};
}
[[nodiscard]] inline bool ValidStereoRequest(const RenderRequest& r) noexcept {
    if (!HeaderMatches<RenderRequest>(r.header, MessageType::RenderRequest) ||
        !IsValid(r.header.session_nonce) || r.header.generation == 0 ||
        r.frame_id == 0 || r.predicted_display_time_ns <= 0 ||
        r.render_width == 0 || r.render_height == 0 ||
        r.render_width > 4096 || r.render_height > 4096) return false;
    for (const auto& v : r.views) {
        const math::Quat q{v.pose.orientation_x, v.pose.orientation_y,
                           v.pose.orientation_z, v.pose.orientation_w};
        if (!math::IsFinite(q) || math::Dot(q,q) < 0.25F || math::Dot(q,q) > 4.F ||
            !math::IsFinite(math::Vec3{v.pose.position_x,v.pose.position_y,v.pose.position_z}))
            return false;
        const auto& f = v.fov;
        if (!std::isfinite(f.angle_left) || !std::isfinite(f.angle_right) ||
            !std::isfinite(f.angle_up) || !std::isfinite(f.angle_down) ||
            !(f.angle_left < 0 && f.angle_right > 0 && f.angle_down < 0 && f.angle_up > 0) ||
            f.angle_left < -1.5F || f.angle_right > 1.5F ||
            f.angle_down < -1.5F || f.angle_up > 1.5F) return false;
    }
    return true;
}
// A symmetric engine culling frustum conservatively encloses the asymmetric
// eye projection. Each eye reruns the full camera method and its visibility.
[[nodiscard]] inline float StereoCullingFovDegrees(const RenderRequest& r) noexcept {
    float tan_y = 0;
    const float aspect = static_cast<float>(r.render_width) / static_cast<float>(r.render_height);
    for (const auto& v : r.views) {
        tan_y = (std::max)(tan_y, (std::max)(std::tan(v.fov.angle_up), -std::tan(v.fov.angle_down)));
        tan_y = (std::max)(tan_y, (std::max)(-std::tan(v.fov.angle_left), std::tan(v.fov.angle_right)) / aspect);
    }
    return 2.F * std::atan(tan_y * 1.02F) * 180.F / math::kPi;
}
struct StereoFrameMetadata {
    std::uint32_t magic{kStereoMetadataMagic};
    std::uint32_t eyes_complete{3};
    std::uint64_t ready_value{};
    std::uint64_t camera_frame_id{};
    RenderRequest request{};
    std::uint32_t hud_width{},hud_height{};
    float hud_source_aspect{};
    std::uint32_t guide_mask{};
    // Exact engine GL projection * view, before object-local transforms.
    // Column-major; OpenGL NDC z is -1..1; depth atlas is raw 0..1.
    math::Matrix4x4 view_projection[2]{};
    std::uint32_t structure_size{kStereoMetadataSize};
    // Optional, all-or-none: 0 means unavailable; 3 means BOTH eyes valid.
    std::uint32_t depth_mask{};
    float engine_units_per_metre{};
    std::uint32_t depth_reserved{};
    // XR-ready projected depth, NOT linear depth. near_z/far_z are metres
    // at min_depth/max_depth. Reversed GL depth swaps the plane distances so
    // min_depth < max_depth still holds. Original packed pixels are unchanged.
    DepthRangeF32 depth[2]{};
};
static_assert(sizeof(StereoFrameMetadata) == kStereoMetadataSize);
static_assert(offsetof(StereoFrameMetadata,request) == 24);
static_assert(offsetof(StereoFrameMetadata,structure_size) == 336);
static_assert(offsetof(StereoFrameMetadata,depth_mask) == 340);
static_assert(offsetof(StereoFrameMetadata,engine_units_per_metre) == 344);
static_assert(offsetof(StereoFrameMetadata,depth) == 352);
[[nodiscard]] inline bool ValidStereoMetadataHeader(const StereoFrameMetadata& frame) noexcept {
    return frame.magic==kStereoMetadataMagic && frame.structure_size==kStereoMetadataSize &&
        frame.depth_reserved==0;
}
[[nodiscard]] inline bool ValidStereoDepthRange(const DepthRangeF32& depth) noexcept {
    return std::isfinite(depth.near_z) && std::isfinite(depth.far_z) &&
        depth.near_z>0 && depth.far_z>0 && depth.near_z!=depth.far_z &&
        std::isfinite(depth.min_depth) && std::isfinite(depth.max_depth) &&
        depth.min_depth>=0 && depth.max_depth<=1 && depth.min_depth<depth.max_depth;
}
[[nodiscard]] inline std::optional<DepthRangeF32> StereoDepthRangeFromOpenGl(
    double near_engine,double far_engine,double units_per_metre,
    double window_near,double window_far) noexcept {
    if (!std::isfinite(near_engine) || !std::isfinite(far_engine) ||
        near_engine<=0 || far_engine<=near_engine ||
        !std::isfinite(units_per_metre) || units_per_metre<=0 ||
        !std::isfinite(window_near) || !std::isfinite(window_far) ||
        window_near<0 || window_near>1 || window_far<0 || window_far>1 ||
        window_near==window_far) return std::nullopt;
    DepthRangeF32 result{static_cast<float>(near_engine/units_per_metre),
        static_cast<float>(far_engine/units_per_metre),
        static_cast<float>(window_near),static_cast<float>(window_far)};
    if (window_near>window_far) {
        std::swap(result.near_z,result.far_z);
        std::swap(result.min_depth,result.max_depth);
    }
    // Also rejects overflow, underflow, or distinct doubles rounding equal.
    return ValidStereoDepthRange(result) ? std::optional{result}:std::nullopt;
}
[[nodiscard]] inline bool ValidStereoDepth(const StereoFrameMetadata& frame) noexcept {
    if (!ValidStereoMetadataHeader(frame)) return false;
    if (!frame.depth_mask) {
        // Canonical absence prevents stale per-eye parameters surviving reuse.
        if (frame.engine_units_per_metre!=0) return false;
        for (const auto& depth:frame.depth)
            if (depth.near_z!=0 || depth.far_z!=0 || depth.min_depth!=0 || depth.max_depth!=0) return false;
        return true;
    }
    if (frame.depth_mask!=3 || frame.guide_mask!=3 ||
        !std::isfinite(frame.engine_units_per_metre) || frame.engine_units_per_metre<=0) return false;
    return ValidStereoDepthRange(frame.depth[0]) && ValidStereoDepthRange(frame.depth[1]);
}
[[nodiscard]] inline bool ValidStereoHud(const StereoFrameMetadata& frame) noexcept {
    if (frame.hud_width==0 && frame.hud_height==0) return frame.hud_source_aspect==0;
    return frame.hud_width>0 && frame.hud_width<=frame.request.render_width*2U &&
        frame.hud_height>0 && frame.hud_height<=kStereoHudMaximumHeight &&
        std::isfinite(frame.hud_source_aspect) && frame.hud_source_aspect>0.2F && frame.hud_source_aspect<8.0F;
}
[[nodiscard]] inline bool ValidStereoGuides(const StereoFrameMetadata& frame) noexcept {
    // Existing consumer entry points inherit v5 and optional-depth validation.
    if (!ValidStereoDepth(frame)) return false;
    if (!frame.guide_mask) return true;
    if (frame.guide_mask!=3) return false;
    for (const auto& matrix:frame.view_projection) {
        bool nonzero=false;
        for (float value:matrix.value) {
            if (!std::isfinite(value)) return false;
            nonzero=nonzero || value!=0;
        }
        if (!nonzero) return false;
    }
    return true;
}
// Windows mapping implementation. Metadata is slot-owned by the same
// ready/consumed fences as its images; a bounded seqlock also rejects torn reads.
class StereoFrameMapping final {
public:
    StereoFrameMapping() = default;
    StereoFrameMapping(const StereoFrameMapping&) = delete;
    StereoFrameMapping& operator=(const StereoFrameMapping&) = delete;
    ~StereoFrameMapping();
    [[nodiscard]] bool Open(SessionNonce nonce, bool create) noexcept;
    void Close() noexcept;
    [[nodiscard]] bool Write(const StereoFrameMetadata& frame) noexcept;
    [[nodiscard]] bool Read(std::uint64_t ready, StereoFrameMetadata& frame) const noexcept;
    void MarkWorldRendered() noexcept;
    [[nodiscard]] bool WorldRecentlyRendered() const noexcept;
private:
    void* handle_{};
    void* view_{};
    SessionNonce nonce_{};
    bool writer_{};
};
}
