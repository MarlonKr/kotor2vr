#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <filesystem>
#include <string>
#include "kotorvr/host/direct_neural_eye.hpp"

namespace kotorvr::host {
enum class NeuralPoll { Idle, Pending, Complete, Failed };
// Default: one isolated process/feature per eye. Nonempty
// KOTOR2VR_NEURAL_DIRECT_ROOT selects the native-size experimental direct backend
// (also for transport tests); invalid opt-in never falls back to a worker.
// Direct quality is alternate/unconfirmed. All resources rest in COMMON across
// processes. The caller copies inputs on its queue before Submit, polls output
// without queueing a wait on an untrusted worker, then copies Output BEFORE
// submitting that eye's next input. Exactly one outstanding frame is allowed.
class NeuralWorkerClient final {
public:
    NeuralWorkerClient()=default;
    ~NeuralWorkerClient();
    NeuralWorkerClient(const NeuralWorkerClient&)=delete;
    NeuralWorkerClient& operator=(const NeuralWorkerClient&)=delete;
    bool Start(ID3D12Device* device,const std::filesystem::path& worker,
        UINT width,UINT height,bool transport_only=false,UINT output_width=0,UINT output_height=0);
    bool Submit(ID3D12CommandQueue* queue,bool reset,float jitter_x=0,float jitter_y=0);
    NeuralPoll Poll();
    bool QueueWaitCompletedOutput(ID3D12CommandQueue* queue);
    void Close() noexcept;
    ID3D12Resource* color() const noexcept { return direct_ ? direct_->texture(0):textures_[0].Get(); }
    ID3D12Resource* output() const noexcept { return direct_ ? direct_->texture(1):textures_[1].Get(); }
    ID3D12Resource* depth() const noexcept { return direct_ ? direct_->texture(2):textures_[2].Get(); }
    ID3D12Resource* motion() const noexcept { return direct_ ? direct_->texture(3):textures_[3].Get(); }
    std::uint64_t sequence() const noexcept { return direct_ ? direct_->sequence():sequence_; }
    DWORD pid() const noexcept { return direct_ ? GetCurrentProcessId():pid_; }
    HANDLE completion_event() const noexcept { return direct_ ? direct_->completion_event():completion_event_; }
    const std::string& error() const noexcept { return direct_ ? direct_->error():error_; }
private:
    std::unique_ptr<DirectNeuralEye> direct_;
    bool Transfer(bool write,void* data,DWORD size,DWORD timeout_ms=5000);
    bool Fail(const char* operation,DWORD code=GetLastError());
    HANDLE process_{},pipe_{INVALID_HANDLE_VALUE};
    HANDLE completion_event_{};
    DWORD pid_{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,4> textures_;
    Microsoft::WRL::ComPtr<ID3D12Fence> input_fence_,output_fence_;
    std::uint64_t sequence_{};
    bool pending_{},ack_received_{},failed_{};
    std::string error_;
};
}
