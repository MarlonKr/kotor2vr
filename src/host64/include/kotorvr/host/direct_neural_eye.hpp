#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace kotorvr::host {
enum class NeuralPoll;
enum class DirectNeuralChain { DlaaThenNr, NrThenSr };
// Declare first in main: retire pipelines before unloading the shared vendor
// session, and do so before SDK/CRT static destructors begin running.
class DirectNeuralSessionScope final {
public:
    DirectNeuralSessionScope()=default;
    ~DirectNeuralSessionScope();
    DirectNeuralSessionScope(const DirectNeuralSessionScope&)=delete;
    DirectNeuralSessionScope& operator=(const DirectNeuralSessionScope&)=delete;
};
// Experimental DLAA -> typed NR or opt-in work-size NR -> DLSS-SR,
// without RenoDX's color codec. NR-before-SR quality is unconfirmed.
// Alternate quality, NOT parity. Inputs/output rest in COMMON. Submit uses the
// caller's DIRECT queue; exactly one frame per eye may be outstanding. Feature
// creation is fenced on first Submit. Never load this backend unless opted in.
class DirectNeuralEye final {
public:
    DirectNeuralEye();
    ~DirectNeuralEye();
    DirectNeuralEye(const DirectNeuralEye&)=delete;
    DirectNeuralEye& operator=(const DirectNeuralEye&)=delete;
    // Zero/zero output defaults to input; only NrThenSr permits larger output.
    // Color/Depth/Motion and intermediate use input dimensions; texture(1) uses output.
    bool Start(ID3D12Device*,const std::filesystem::path&,UINT,UINT,bool transportOnly=false,
        UINT outputWidth=0,UINT outputHeight=0,DirectNeuralChain chain=DirectNeuralChain::DlaaThenNr);
    bool Submit(ID3D12CommandQueue*,bool,float,float);
    NeuralPoll Poll();
    bool QueueWaitCompletedOutput(ID3D12CommandQueue*);
    void Close() noexcept;
    ID3D12Resource* texture(unsigned) const noexcept;
    std::uint64_t sequence() const noexcept;
    HANDLE completion_event() const noexcept;
    const std::string& error() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
};
}
