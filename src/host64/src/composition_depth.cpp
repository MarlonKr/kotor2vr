#include "kotorvr/host/composition_depth.hpp"
#include "kotorvr/host/composition_depth_contract.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <new>
#include <vector>

namespace kotorvr::host {
using Microsoft::WRL::ComPtr;
struct CompositionDepthUnpack::Impl {
    struct Binding {
        ComPtr<ID3D12Resource> atlas,destination;
        ComPtr<ID3D12DescriptorHeap> srv,dsv;
    };
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    depth_detail::Layout layout;
    std::vector<Binding> bindings;

    Binding* FindBinding(ID3D12Resource* atlas,ID3D12Resource* destination) {
        for (auto& b:bindings)
            if (b.atlas.Get()==atlas && b.destination.Get()==destination) return &b;
        Binding b;
        b.atlas=atlas; b.destination=destination;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors=1; heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&b.srv)))) return nullptr;
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV; heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&b.dsv)))) return nullptr;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(atlas,&srv,b.srv->GetCPUDescriptorHandleForHeapStart());
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format=DXGI_FORMAT_D32_FLOAT; dsv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(destination,&dsv,b.dsv->GetCPUDescriptorHandleForHeapStart());
        // Never rewrite descriptors already referenced by recorded/in-flight work.
        bindings.push_back(std::move(b));
        return &bindings.back();
    }
};
CompositionDepthUnpack::CompositionDepthUnpack()=default;
CompositionDepthUnpack::~CompositionDepthUnpack()=default;

bool CompositionDepthUnpack::Initialize(ID3D12Device* device,std::uint32_t w,
    std::uint32_t h,std::uint32_t depth_y) {
    if (!device || !depth_detail::ValidLayout({w,h,depth_y})) return false;
    auto candidate=std::make_unique<Impl>();
    candidate->device=device; candidate->layout={w,h,depth_y};
    ComPtr<ID3DBlob> vs,ps,error,signature;
    constexpr UINT flags=D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_ENABLE_STRICTNESS;
    if (FAILED(D3DCompile(depth_detail::kShader,sizeof(depth_detail::kShader)-1,"composition-depth",nullptr,nullptr,
        "vs_main","vs_5_0",flags,0,&vs,&error)) ||
        FAILED(D3DCompile(depth_detail::kShader,sizeof(depth_detail::kShader)-1,"composition-depth",nullptr,nullptr,
        "ps_main","ps_5_0",flags,0,&ps,&error))) return false;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors=1;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable={1,&range};
    parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants={0,0,1}; parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters=2; root.pParameters=parameters;
    if (FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&error)) ||
        FAILED(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),
            IID_PPV_ARGS(&candidate->root)))) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature=candidate->root.Get();
    pipeline.VS={vs->GetBufferPointer(),vs->GetBufferSize()};
    pipeline.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    for (auto& target:pipeline.BlendState.RenderTarget) {
        target.SrcBlend=D3D12_BLEND_ONE; target.DestBlend=D3D12_BLEND_ZERO;
        target.BlendOp=D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha=D3D12_BLEND_ONE; target.DestBlendAlpha=D3D12_BLEND_ZERO;
        target.BlendOpAlpha=D3D12_BLEND_OP_ADD; target.LogicOp=D3D12_LOGIC_OP_NOOP;
    }
    pipeline.SampleMask=UINT_MAX;
    pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable=TRUE;
    pipeline.DepthStencilState.DepthEnable=TRUE;
    pipeline.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    pipeline.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    auto& face=pipeline.DepthStencilState.FrontFace;
    face.StencilFailOp=face.StencilDepthFailOp=face.StencilPassOp=D3D12_STENCIL_OP_KEEP;
    face.StencilFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline.DepthStencilState.BackFace=face;
    pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.DSVFormat=DXGI_FORMAT_D32_FLOAT; pipeline.SampleDesc.Count=1;
    if (FAILED(device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&candidate->pipeline)))) return false;
    impl_=std::move(candidate);
    return true;
}

bool CompositionDepthUnpack::Record(ID3D12GraphicsCommandList* list,ID3D12Resource* atlas,
    ID3D12Resource* destination) {
    if (!impl_ || !list || !atlas || !destination || atlas==destination ||
        list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    const auto a=atlas->GetDesc(),d=destination->GetDesc();
    const auto plain=[](const D3D12_RESOURCE_DESC& t) {
        return t.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && t.DepthOrArraySize==1 &&
            t.MipLevels==1 && t.SampleDesc.Count==1 && t.SampleDesc.Quality==0;
    };
    if (!plain(a) || !plain(d) || !depth_detail::Fits(impl_->layout,a.Width,a.Height,d.Width,d.Height) ||
        (a.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && a.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS) ||
        (a.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
        (d.Format!=DXGI_FORMAT_D32_FLOAT && d.Format!=DXGI_FORMAT_R32_TYPELESS) ||
        !(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) return false;
    // Reject foreign-device inputs before creating descriptors or recording.
    ComPtr<ID3D12Device> source_device,destination_device,list_device;
    if (FAILED(atlas->GetDevice(IID_PPV_ARGS(&source_device))) ||
        FAILED(destination->GetDevice(IID_PPV_ARGS(&destination_device))) ||
        FAILED(list->GetDevice(IID_PPV_ARGS(&list_device))) ||
        source_device.Get()!=impl_->device.Get() || destination_device.Get()!=impl_->device.Get() ||
        list_device.Get()!=impl_->device.Get()) return false;
    Impl::Binding* binding{};
    try { binding=impl_->FindBinding(atlas,destination); }
    catch (const std::bad_alloc&) { return false; }
    if (!binding) return false;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={atlas,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1,&barrier);
    list->SetGraphicsRootSignature(impl_->root.Get());
    list->SetPipelineState(impl_->pipeline.Get());
    ID3D12DescriptorHeap* heaps[]={binding->srv.Get()}; list->SetDescriptorHeaps(1,heaps);
    list->SetGraphicsRootDescriptorTable(0,binding->srv->GetGPUDescriptorHandleForHeapStart());
    list->SetGraphicsRoot32BitConstant(1,impl_->layout.packed_depth_y,0);
    const auto dsv=binding->dsv->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
    const D3D12_VIEWPORT viewport{0,0,float(impl_->layout.eye_width*2),float(impl_->layout.eye_height),0,1};
    const D3D12_RECT scissor{0,0,LONG(impl_->layout.eye_width*2),LONG(impl_->layout.eye_height)};
    list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3,1,0,0);
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1,&barrier);
    return true;
}
void CompositionDepthUnpack::ResetBindings() {
    if (impl_) impl_->bindings.clear();
}
}
