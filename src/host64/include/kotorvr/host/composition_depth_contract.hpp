#pragma once
#include <cmath>
#include <bit>
#include <cstdint>

// One function body for the CPU oracle checks and embedded HLSL. Inputs are
// byte channels read through a linear UNORM view; no filtering or sRGB decode.
#define KOTORVR_DEPTH24_FUNCTION \
float DecodePackedDepth24(float r,float g,float b) { \
    const uint red=uint(round(r*255.0F)); \
    const uint green=uint(round(g*255.0F)); \
    const uint blue=uint(round(b*255.0F)); \
    const uint packed=(red<<16)|(green<<8)|blue; \
    const float scaled=float(packed)*0.000000059604644775390625F; \
    return packed ? asfloat(asuint(scaled)+1U):0.0F; \
}
#define KOTORVR_DEPTH_STRING_INNER(...) #__VA_ARGS__
#define KOTORVR_DEPTH_STRING(...) KOTORVR_DEPTH_STRING_INNER(__VA_ARGS__)

namespace kotorvr::host::depth_detail {
using uint=std::uint32_t;
using std::round;
inline uint asuint(float value) { return std::bit_cast<uint>(value); }
inline float asfloat(uint value) { return std::bit_cast<float>(value); }
// packed/2^24 is exact. For each nonzero 24-bit integer, division by
// (2^24-1) rounds to its next positive float. Using that representation
// avoids HLSL reciprocal error/reassociation and preserves depth 1 exactly.
// Header-local to avoid exporting this implementation helper.
inline KOTORVR_DEPTH24_FUNCTION

struct Layout {
    std::uint32_t eye_width{},eye_height{},packed_depth_y{};
};
[[nodiscard]] constexpr bool ValidLayout(Layout l) noexcept {
    // Matches the supported native-eye request bounds. Arithmetic stays bounded.
    return l.eye_width>0 && l.eye_width<=4096 && l.eye_height>0 && l.eye_height<=4096 &&
        l.packed_depth_y>=l.eye_height &&
        std::uint64_t(l.packed_depth_y)+l.eye_height<=16384;
}
[[nodiscard]] constexpr bool Fits(Layout l,std::uint64_t source_width,
    std::uint32_t source_height,std::uint64_t destination_width,
    std::uint32_t destination_height) noexcept {
    return ValidLayout(l) && source_width==std::uint64_t(l.eye_width)*2 &&
        source_height>=std::uint64_t(l.packed_depth_y)+l.eye_height &&
        destination_width==std::uint64_t(l.eye_width)*2 && destination_height==l.eye_height;
}

inline constexpr char kShader[]=R"(
Texture2D<float4> packedAtlas:register(t0);
cbuffer DepthRect:register(b0) { uint depthY; };
)" KOTORVR_DEPTH_STRING(KOTORVR_DEPTH24_FUNCTION) R"(
float4 vs_main(uint id:SV_VertexID):SV_Position {
    float2 p=float2((id<<1)&2,id&2);
    return float4(p*float2(2,-2)+float2(-1,1),0,1);
}
float ps_main(float4 position:SV_Position):SV_Depth {
    uint2 pixel=uint2(position.xy);
    float3 rgb=packedAtlas.Load(int3(pixel.x,pixel.y+depthY,0)).rgb;
    return DecodePackedDepth24(rgb.r,rgb.g,rgb.b);
}
)";
}
#undef KOTORVR_DEPTH24_FUNCTION
#undef KOTORVR_DEPTH_STRING
#undef KOTORVR_DEPTH_STRING_INNER
