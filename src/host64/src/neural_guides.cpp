#include "kotorvr/host/neural_guides.hpp"
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <cstring>
#include <cmath>
using Microsoft::WRL::ComPtr;
namespace kotorvr::host {
namespace {
void Transition(ID3D12GraphicsCommandList* list,ID3D12Resource* texture,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={texture,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&barrier);
}
constexpr char shader[]=R"(
Texture2D<float4> atlas:register(t0);
RWTexture2D<float> depthOut:register(u0);
RWTexture2D<float2> motionOut:register(u1);
RWTexture2D<float4> colorOut:register(u2);
cbuffer Camera:register(b0){column_major float4x4 currentToPrevious; uint width; uint height; uint eyeX; uint depthY; uint reset; uint sourceWidth; uint sourceHeight; uint writeColor;};
float4 colorAt(int2 p) { return atlas.Load(int3(clamp(p,int2(0,0),int2(sourceWidth-1,sourceHeight-1))+int2(eyeX,0),0)); }
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
    if(id.x>=width || id.y>=height) return;
    float2 uv=(float2(id.xy)+0.5)/float2(width,height);
    uint2 source=((id.xy*2+1)*uint2(sourceWidth,sourceHeight))/(uint2(width,height)*2);
    uint3 rgb=uint3(round(atlas.Load(int3(source+uint2(eyeX,depthY),0)).rgb*255.0));
    float depth=float((rgb.x<<16)|(rgb.y<<8)|rgb.z)/16777215.0;
    depthOut[id.xy]=depth;
    float4 old=mul(currentToPrevious,float4(uv.x*2-1,1-uv.y*2,depth*2-1,1));
    float2 motion=0;
    if(!reset && old.w>0.000001){
        float2 oldUv=float2(old.x/old.w*0.5+0.5,0.5-old.y/old.w*0.5);
        motion=(oldUv-uv)*float2(width,height);
        if(any(abs(motion)>float2(width,height))) motion=0;
    }
    motionOut[id.xy]=motion;
    if(writeColor) {
        float2 p=uv*float2(sourceWidth,sourceHeight)-0.5;
        int2 base=int2(floor(p)); float2 f=frac(p);
        colorOut[id.xy]=lerp(lerp(colorAt(base),colorAt(base+int2(1,0)),f.x),
            lerp(colorAt(base+int2(0,1)),colorAt(base+int2(1,1)),f.x),f.y);
    }
})";
}
bool NeuralGuides::Initialize(ID3D12Device* device,ID3D12Resource* atlas,UINT width,UINT height,UINT work_width,UINT work_height,bool color_output) {
    if (!device || !atlas || !width || !height) return false;
    if (!work_width && !work_height) { work_width=width; work_height=height; }
    if (!work_width || !work_height || work_width>width || work_height>height) return false;
    source_width_=width; source_height_=height;
    width_=work_width; height_=work_height;
    color_output_=color_output;
    ComPtr<ID3DBlob> code,error;
    if (FAILED(D3DCompile(shader,sizeof(shader)-1,"native-neural-guides",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error))) return false;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0}; ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0,0,1};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[0].DescriptorTable={2,ranges};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[1].Constants={0,0,24};
    D3D12_ROOT_SIGNATURE_DESC root_desc{}; root_desc.NumParameters=2; root_desc.pParameters=parameters;
    ComPtr<ID3DBlob> signature;
    if (FAILED(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error)) ||
        FAILED(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root_)))) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{}; pipeline.pRootSignature=root_.Get(); pipeline.CS={code->GetBufferPointer(),code->GetBufferSize()};
    if (FAILED(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&pipeline_)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{}; heap_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors=4; heap_desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(&descriptors_)))) return false;
    auto handle=descriptors_->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels=1; srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    device->CreateShaderResourceView(atlas,&srv,handle);
    D3D12_HEAP_PROPERTIES props{}; props.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture{}; texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; texture.Width=width_;
    texture.Height=height_; texture.DepthOrArraySize=texture.MipLevels=1; texture.SampleDesc.Count=1; texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const UINT stride=device->GetDescriptorHandleIncrementSize(heap_desc.Type);
    for (unsigned i=0;i<3;++i) {
        auto& target=i==2 ? color_:(i ? motion_:depth_);
        texture.Format=i==2 ? DXGI_FORMAT_R8G8B8A8_UNORM:(i ? DXGI_FORMAT_R16G16_FLOAT:DXGI_FORMAT_R32_FLOAT);
        // Native-resolution input is copied directly from the atlas. Keep a
        // valid dummy UAV for the unexecuted shader branch instead of another
        // full-eye color allocation (about 17 MiB per input slot at Quality).
        if (i==2 && !color_output_) { texture.Width=1; texture.Height=1; }
        if (FAILED(device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&target)))) return false;
        handle.ptr+=stride; D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format=texture.Format; uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(target.Get(),nullptr,&uav,handle);
    }
    return true;
}
bool NeuralGuides::Record(ID3D12GraphicsCommandList* list,ID3D12Resource* atlas,UINT eye,
    const k2vr::math::Matrix4x4& current,const k2vr::math::Matrix4x4& previous,bool reset,
    ID3D12Resource* depth,ID3D12Resource* motion,ID3D12Resource* color) {
    if (!pipeline_ || !list || !atlas || !depth || !motion || eye>1 || (color && !color_output_)) return false;
    using namespace DirectX;
    XMFLOAT4X4 current_storage{},previous_storage{}; std::memcpy(&current_storage,current.value.data(),64); std::memcpy(&previous_storage,previous.value.data(),64);
    XMVECTOR determinant{}; const XMMATRIX inverse=XMMatrixInverse(&determinant,XMLoadFloat4x4(&current_storage));
    if (!std::isfinite(XMVectorGetX(determinant)) || std::abs(XMVectorGetX(determinant))<1.0e-12F) return false;
    // GL columns are loaded as DirectX rows (transposed). inv(current)^T *
    // previous^T, stored as rows, yields the column-major previous*inv(current).
    struct Constants { XMFLOAT4X4 transform; UINT width,height,eye_x,depth_y,reset,source_width,source_height,write_color; } constants{};
    XMStoreFloat4x4(&constants.transform,XMMatrixMultiply(inverse,XMLoadFloat4x4(&previous_storage)));
    constants.width=width_; constants.height=height_; constants.eye_x=eye*source_width_;
    constants.depth_y=k2vr::ipc::StereoDepthOffset(source_height_); constants.reset=reset ? 1U:0U;
    constants.source_width=source_width_; constants.source_height=source_height_; constants.write_color=color ? 1U:0U;
    static_assert(sizeof(constants)==24*4);
    Transition(list,atlas,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(list,depth_.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list,motion_.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(list,color_.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(root_.Get()); list->SetPipelineState(pipeline_.Get());
    ID3D12DescriptorHeap* heaps[]={descriptors_.Get()}; list->SetDescriptorHeaps(1,heaps);
    list->SetComputeRootDescriptorTable(0,descriptors_->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRoot32BitConstants(1,24,&constants,0); list->Dispatch((width_+7)/8,(height_+7)/8,1);
    Transition(list,atlas,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* sources[]={depth_.Get(),motion_.Get(),color_.Get()}; ID3D12Resource* destinations[]={depth,motion,color};
    for (unsigned i=0;i<3;++i) {
        if (!destinations[i]) { Transition(list,sources[i],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON); continue; }
        Transition(list,sources[i],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(list,destinations[i],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(destinations[i],sources[i]);
        Transition(list,destinations[i],D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        Transition(list,sources[i],D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    }
    return true;
}
}
