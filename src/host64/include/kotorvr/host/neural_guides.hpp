#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <wrl/client.h>
#include "stereo_stream.hpp"

namespace kotorvr::host {
// Camera/rigid-world motion from exact per-eye matrices and hardware depth.
// Animated/deforming objects still need residual optical flow or object motion.
class NeuralGuides final {
public:
    bool Initialize(ID3D12Device* device,ID3D12Resource* atlas,UINT width,UINT height,UINT work_width=0,UINT work_height=0,bool color_output=true);
    bool Record(ID3D12GraphicsCommandList* list,ID3D12Resource* atlas,UINT eye,
        const k2vr::math::Matrix4x4& current,const k2vr::math::Matrix4x4& previous,bool reset,
        ID3D12Resource* depth,ID3D12Resource* motion,ID3D12Resource* color=nullptr);
private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_,motion_,color_;
    UINT width_{},height_{},source_width_{},source_height_{};
    bool color_output_{};
};
}
