#pragma once
#include "stereo_stream.hpp"
#include <cstdint>
#include <memory>
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
namespace kotorvr::host {
// Reuses neural appearance at the newest raw camera/depth. This is camera
// reprojection, not animated-object optical flow or alternate-eye rendering.
// COMMON in/out; Record calls must use one ordered host queue. Source resources
// stay alive until GPU completion; immutable descriptor bindings retain them.
class NeuralReprojection final {
public:
    NeuralReprojection();
    ~NeuralReprojection();
    bool Initialize(ID3D12Device*,std::uint32_t eye_width,std::uint32_t eye_height);
    // Both source atlases include packed depth at StereoDepthOffset(height).
    // On true, destination contains both eyes and the CURRENT raw HUD; its
    // caller must submit current metadata/pose. On false, no commands recorded.
    bool Record(ID3D12GraphicsCommandList*,ID3D12Resource* neural,
        const k2vr::ipc::StereoFrameMetadata& neural_frame,ID3D12Resource* current,
        const k2vr::ipc::StereoFrameMetadata& current_frame,ID3D12Resource* destination);
    // Call only after the host fence covering the last Record has completed.
    double CompletedGpuMilliseconds(std::uint64_t timestamp_frequency) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
