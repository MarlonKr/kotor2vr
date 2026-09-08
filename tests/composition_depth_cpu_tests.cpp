#include "kotorvr/host/composition_depth.hpp"
#include "kotorvr/host/composition_depth_contract.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <iostream>
#include <limits>

int main() {
    using namespace kotorvr::host;
    using namespace depth_detail;
    int failures=0;
    const auto check=[&](bool ok,const char* what) {
        if (!ok) { ++failures; std::cerr<<"FAIL "<<what<<'\n'; }
    };
    constexpr Layout layout{2040,2232,3768};
    static_assert(Fits(layout,4080,6000,4080,2232));
    static_assert(!Fits(layout,4080,5999,4080,2232)); // HUD-only neural output
    static_assert(!Fits(layout,4079,6000,4080,2232));
    static_assert(!Fits(layout,4080,6000,4080,2231));
    static_assert(!ValidLayout({0,2232,3768}));
    static_assert(!ValidLayout({4097,2232,3768}));
    static_assert(!ValidLayout({2040,2232,2231})); // overlaps eye-color rows
    static_assert(!ValidLayout({2040,2232,UINT32_MAX}));
    static_assert(Fits({1,1,1},2,2,2,1));
    // Exhaustive integer oracle exercises the SAME function body embedded in
    // the pixel shader, including byte carries, near zero and the far sentinel.
    std::uint32_t bad=0;
    float previous=-1;
    for (std::uint32_t packed=0;packed<=0xffffffU;++packed) {
        const float value=DecodePackedDepth24(float(packed>>16)/255.F,
            float((packed>>8)&255)/255.F,float(packed&255)/255.F);
        const auto roundtrip=static_cast<std::uint32_t>(std::floor(double(value)*16777215.0+0.5));
        if (roundtrip!=packed || !std::isfinite(value) || value<0 || value>1 || value<=previous) ++bad;
        previous=value;
    }
    check(bad==0,"all 16,777,216 packed depths round-trip and increase strictly");
    check(DecodePackedDepth24(0,0,0)==0 && DecodePackedDepth24(1,1,1)==1,
        "near and clear/far endpoints remain exact");
    // Real shader compiler, CPU only: no factory, adapter, D3D device or queue.
    for (unsigned stage=0;stage<2;++stage) {
        Microsoft::WRL::ComPtr<ID3DBlob> code,error;
        const HRESULT hr=D3DCompile(kShader,sizeof(kShader)-1,"composition-depth-cpu",nullptr,nullptr,
            stage ? "ps_main":"vs_main",stage ? "ps_5_0":"vs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_WARNINGS_ARE_ERRORS|D3DCOMPILE_ENABLE_STRICTNESS,
            0,&code,&error);
        if (FAILED(hr) && error) std::cerr<<static_cast<const char*>(error->GetBufferPointer())<<'\n';
        check(SUCCEEDED(hr),stage ? "production depth pixel shader compiles":"production fullscreen vertex shader compiles");
    }
    CompositionDepthUnpack unpack;
    check(!unpack.Initialize(nullptr,2040,2232,3768),"null device cannot initialize");
    check(!unpack.Record(nullptr,nullptr,nullptr),"uninitialized recorder rejects without commands");
    unpack.ResetBindings();
    std::cout<<(failures ? "FAIL":"PASS")<<" depth-unpack CPU; codes=16777216 shaders=2 GPU_devices=0 failures="<<failures<<'\n';
    return failures ? 1:0;
}
