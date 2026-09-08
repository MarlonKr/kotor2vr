#include "kotorvr/host/neural_reprojection.hpp"
#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace kotorvr::host {
namespace {
void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* texture,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={texture,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&b);
}
bool Rgba(DXGI_FORMAT f) { return f==DXGI_FORMAT_R8G8B8A8_UNORM || f==DXGI_FORMAT_R8G8B8A8_TYPELESS || f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
bool TextureMatches(ID3D12Resource* r,UINT width,UINT height) {
    if (!r) return false;
    const auto d=r->GetDesc();
    return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width==width && d.Height==height &&
        d.DepthOrArraySize==1 && d.MipLevels==1 && d.SampleDesc.Count==1 && Rgba(d.Format);
}
constexpr char shader[]=R"(
Texture2D<float4> neural:register(t0);
Texture2D<float4> current:register(t1);
RWTexture2D<float4> result:register(u0);
cbuffer Camera:register(b0) {
    column_major float4x4 currentToPrevious;
    float4 oldInverseW;
    uint width; uint height; uint depthY; uint eyeX;
};
float unpack(float3 rgb) {
    uint3 b=uint3(round(rgb*255.0));
    return float((b.x<<16)|(b.y<<8)|b.z)/16777215.0;
}
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
    if(id.x>=width || id.y>=height) return;
    uint2 dst=id.xy+uint2(eyeX,0);
    float4 fallback=current.Load(int3(dst,0));
    result[dst]=fallback;
    float z=unpack(current.Load(int3(dst+uint2(0,depthY),0)).rgb);
    if(z>=0.9999999 || z<=0) return;
    float2 uv=(float2(id.xy)+0.5)/float2(width,height);
    float4 old=mul(currentToPrevious,float4(uv.x*2-1,1-uv.y*2,z*2-1,1));
    if(!all(isfinite(old)) || old.w<=0.000001) return;
    float3 ndc=old.xyz/old.w;
    if(any(abs(ndc)>1.0)) return;
    float2 oldUv=float2(ndc.x*0.5+0.5,0.5-ndc.y*0.5);
    float2 pixel=oldUv*float2(width,height)-0.5;
    int2 base=int2(floor(pixel)); float2 f=frac(pixel);
    float expectedW=dot(oldInverseW,float4(ndc,1));
    float4 sum=0; float weightSum=0;
    // Depth-aware bilinear taps prevent foreground/background neural colors
    // bleeding over disocclusions. Reject unmatched geometry to CURRENT raw.
    [unroll] for(int y=0;y<2;++y) [unroll] for(int x=0;x<2;++x) {
        int2 p=base+int2(x,y);
        if(any(p<0) || p.x>=int(width) || p.y>=int(height)) continue;
        float d=unpack(neural.Load(int3(p+int2(eyeX,depthY),0)).rgb);
        if(d<=0 || d>=0.9999999) continue;
        float2 tapUv=(float2(p)+0.5)/float2(width,height);
        float capturedW=dot(oldInverseW,float4(tapUv.x*2-1,1-tapUv.y*2,d*2-1,1));
        // Inverse homogeneous W encodes linear camera depth, avoiding a fixed
        // nonlinear-Z threshold that incorrectly accepts distant occluders.
        if(expectedW<=0 || capturedW<=0 || abs(capturedW-expectedW)>abs(expectedW)*0.015+0.000001) continue;
        float weight=(x ? f.x:1-f.x)*(y ? f.y:1-f.y);
        sum+=neural.Load(int3(p+int2(eyeX,0),0))*weight;
        weightSum+=weight;
    }
    if(weightSum>=0.5) result[dst]=float4((sum/weightSum).rgb,fallback.a);
})";
struct Constants { DirectX::XMFLOAT4X4 transform; float inverse_w[4]; UINT width,height,depth_y,eye_x; };
static_assert(sizeof(Constants)==24*4);
bool Prepare(const k2vr::ipc::StereoFrameMetadata& old,const k2vr::ipc::StereoFrameMetadata& now,UINT w,UINT h,std::array<Constants,2>& constants) {
    using namespace k2vr::ipc;
    if (old.magic!=kStereoMetadataMagic || now.magic!=kStereoMetadataMagic || old.eyes_complete!=3 || now.eyes_complete!=3 ||
        !old.ready_value || now.ready_value<old.ready_value || old.guide_mask!=3 || now.guide_mask!=3 ||
        !ValidStereoGuides(old) || !ValidStereoGuides(now) || !ValidStereoHud(now) ||
        old.request.header.generation!=now.request.header.generation ||
        old.request.presentation_state!=now.request.presentation_state || now.request.history_reset_reasons ||
        old.request.render_width!=w || now.request.render_width!=w || old.request.render_height!=h || now.request.render_height!=h ||
        old.request.predicted_display_time_ns<=0 || now.request.predicted_display_time_ns<old.request.predicted_display_time_ns ||
        now.request.predicted_display_time_ns-old.request.predicted_display_time_ns>150000000) return false;
    if (now.request.presentation_state!=PresentationState::WorldFirstPerson &&
        now.request.presentation_state!=PresentationState::WorldThirdPerson && now.request.presentation_state!=PresentationState::DialogueStereo) return false;
    using namespace DirectX;
    for (UINT eye=0;eye<2;++eye) {
        XMFLOAT4X4 a{},b{},inverse_old{};
        std::memcpy(&a,now.view_projection[eye].value.data(),64); std::memcpy(&b,old.view_projection[eye].value.data(),64);
        XMVECTOR determinant{}; const auto inv_current=XMMatrixInverse(&determinant,XMLoadFloat4x4(&a));
        const float det=XMVectorGetX(determinant);
        if (!std::isfinite(det) || std::abs(det)<1.0e-12F) return false;
        const auto inv_previous=XMMatrixInverse(&determinant,XMLoadFloat4x4(&b));
        const float old_det=XMVectorGetX(determinant);
        if (!std::isfinite(old_det) || std::abs(old_det)<1.0e-12F) return false;
        auto& c=constants[eye];
        // GL columns loaded as DX rows are transposed: multiply in reverse.
        XMStoreFloat4x4(&c.transform,XMMatrixMultiply(inv_current,XMLoadFloat4x4(&b)));
        XMStoreFloat4x4(&inverse_old,inv_previous);
        for (UINT i=0;i<4;++i) c.inverse_w[i]=inverse_old.m[i][3];
        c.width=w; c.height=h; c.depth_y=StereoDepthOffset(h); c.eye_x=eye*w;
        for (const auto& row:c.transform.m) for (float value:row) if (!std::isfinite(value)) return false;
    }
    return true;
}
}
struct NeuralReprojection::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12Resource> output,readback;
    ComPtr<ID3D12QueryHeap> timestamps;
    struct Binding { ComPtr<ID3D12Resource> neural,current; ComPtr<ID3D12DescriptorHeap> heap; };
    std::vector<Binding> bindings;
    UINT width{},height{};
    bool recorded{};
    ID3D12DescriptorHeap* Bind(ID3D12Resource* neural,ID3D12Resource* current) {
        for (auto& b:bindings) if (b.neural.Get()==neural && b.current.Get()==current) return b.heap.Get();
        if (bindings.size()>=8) return nullptr;
        Binding b; b.neural=neural; b.current=current;
        D3D12_DESCRIPTOR_HEAP_DESC d{}; d.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; d.NumDescriptors=3; d.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&d,IID_PPV_ARGS(&b.heap)))) return nullptr;
        auto cpu=b.heap->GetCPUDescriptorHandleForHeapStart(); const auto stride=device->GetDescriptorHandleIncrementSize(d.Type);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM; srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(neural,&srv,cpu); cpu.ptr+=stride;
        device->CreateShaderResourceView(current,&srv,cpu); cpu.ptr+=stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format=DXGI_FORMAT_R8G8B8A8_UNORM; uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output.Get(),nullptr,&uav,cpu);
        bindings.push_back(std::move(b)); return bindings.back().heap.Get();
    }
};
NeuralReprojection::NeuralReprojection():impl_(std::make_unique<Impl>()) {}
NeuralReprojection::~NeuralReprojection()=default;
bool NeuralReprojection::Initialize(ID3D12Device* device,UINT w,UINT h) {
    if (!device || !w || !h || w>4096 || k2vr::ipc::StereoAtlasHeight(h)>8192) return false;
    auto s=std::make_unique<Impl>(); s->device=device; s->width=w; s->height=h;
    ComPtr<ID3DBlob> code,error,signature;
    if (FAILED(D3DCompile(shader,sizeof(shader)-1,"native-neural-reprojection",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error))) return false;
    D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,2}};
    D3D12_ROOT_PARAMETER params[2]{}; params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable={2,ranges};
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[1].Constants={0,0,24};
    D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters=2; desc.pParameters=params;
    if (FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error)) ||
        FAILED(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&s->root)))) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature=s->root.Get(); pso.CS={code->GetBufferPointer(),code->GetBufferSize()};
    if (FAILED(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&s->pipeline)))) return false;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC tex{}; tex.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; tex.Width=w*2; tex.Height=h;
    tex.DepthOrArraySize=tex.MipLevels=1; tex.Format=DXGI_FORMAT_R8G8B8A8_UNORM; tex.SampleDesc.Count=1; tex.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&tex,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&s->output)))) return false;
    D3D12_QUERY_HEAP_DESC query{}; query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP; query.Count=2;
    if (FAILED(device->CreateQueryHeap(&query,IID_PPV_ARGS(&s->timestamps)))) return false;
    heap.Type=D3D12_HEAP_TYPE_READBACK; tex={}; tex.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; tex.Width=16;
    tex.Height=tex.DepthOrArraySize=tex.MipLevels=1; tex.SampleDesc.Count=1; tex.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&tex,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s->readback)))) return false;
    impl_=std::move(s); return true;
}
bool NeuralReprojection::Record(ID3D12GraphicsCommandList* list,ID3D12Resource* neural,const k2vr::ipc::StereoFrameMetadata& old,
    ID3D12Resource* current,const k2vr::ipc::StereoFrameMetadata& now,ID3D12Resource* destination) {
    auto& s=*impl_; std::array<Constants,2> constants{};
    if (!list || !s.pipeline || neural==current || neural==destination || current==destination ||
        !TextureMatches(neural,s.width*2,k2vr::ipc::StereoAtlasHeight(s.height)) || !TextureMatches(current,s.width*2,k2vr::ipc::StereoAtlasHeight(s.height)) ||
        !TextureMatches(destination,s.width*2,k2vr::ipc::StereoDepthOffset(s.height)) || !Prepare(old,now,s.width,s.height,constants)) return false;
    ID3D12DescriptorHeap* descriptors=s.Bind(neural,current); if (!descriptors) return false;
    list->EndQuery(s.timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
    Barrier(list,neural,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list,current,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list,s.output.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetComputeRootSignature(s.root.Get()); list->SetPipelineState(s.pipeline.Get()); list->SetDescriptorHeaps(1,&descriptors);
    list->SetComputeRootDescriptorTable(0,descriptors->GetGPUDescriptorHandleForHeapStart());
    for (const auto& c:constants) { list->SetComputeRoot32BitConstants(1,24,&c,0); list->Dispatch((s.width+7)/8,(s.height+7)/8,1); }
    Barrier(list,neural,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    Barrier(list,current,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list,s.output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list,destination,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION from{},to{}; from.pResource=s.output.Get(); to.pResource=destination;
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    // Fill the entire reserved HUD rectangle from current raw, including its
    // unused pixels, so no old XR swapchain contents can leak into later UI.
    from.pResource=current;
    const D3D12_BOX hud{0,s.height,0,s.width*2,k2vr::ipc::StereoDepthOffset(s.height),1};
    list->CopyTextureRegion(&to,0,s.height,0,&from,&hud);
    Barrier(list,destination,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    Barrier(list,s.output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    Barrier(list,current,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
    list->EndQuery(s.timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
    list->ResolveQueryData(s.timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,s.readback.Get(),0);
    s.recorded=true; return true;
}
double NeuralReprojection::CompletedGpuMilliseconds(std::uint64_t frequency) const {
    const auto& s=*impl_; if (!s.recorded || !frequency) return 0;
    void* data{}; D3D12_RANGE range{0,16}; if (FAILED(s.readback->Map(0,&range,&data))) return 0;
    std::uint64_t t[2]{}; std::memcpy(t,data,sizeof(t)); D3D12_RANGE none{0,0}; s.readback->Unmap(0,&none);
    return t[1]>=t[0] ? static_cast<double>(t[1]-t[0])*1000.0/static_cast<double>(frequency):0;
}
}
