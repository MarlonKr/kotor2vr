#pragma once
#include <cstdint>
#include <memory>
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace kotorvr::host {
// Standalone GPU unpacker; does not submit XR layers or select temporal sources.
// Depth is projected 0..1, with no metric conversion or linearization here.
// The caller must pair it with the SAME color's pose/FOV/metric depth metadata.
class CompositionDepthUnpack final {
public:
    CompositionDepthUnpack();
    ~CompositionDepthUnpack();
    // No dependency on StereoFrameMetadata's ABI. Pass StereoDepthOffset(H)
    // explicitly. Reinitialization/destruction require all prior GPU work idle.
    bool Initialize(ID3D12Device*,std::uint32_t eye_width,
        std::uint32_t eye_height,std::uint32_t packed_depth_y);
    // Atlas: R8G8B8A8_UNORM/TYPELESS, two eyes, COMMON in/out.
    // Destination: D32_FLOAT/R32_TYPELESS with ALLOW_DEPTH_STENCIL, exactly
    // 2W x H, one slice/mip/sample, DEPTH_WRITE in/out (XR acquire/release state).
    // On false: no commands recorded. On true: every depth pixel overwritten.
    // Uses caller's direct list/ordered host queue; overwrites graphics bindings.
    // Immutable descriptors retain source/destination COM refs across batches.
    bool Record(ID3D12GraphicsCommandList*,ID3D12Resource* atlas,
        ID3D12Resource* depth_destination);
    // Only after the caller's completion fence. Call before destroying/replacing
    // an XR depth swapchain, to release the retained runtime image references.
    void ResetBindings();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
