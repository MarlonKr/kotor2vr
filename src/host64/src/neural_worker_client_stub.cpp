#include "kotorvr/host/neural_worker_client.hpp"

// V1 never creates a neural worker, IPC pipe, vendor session, or GPU resource.
namespace kotorvr::host {
NeuralWorkerClient::~NeuralWorkerClient() = default;
bool NeuralWorkerClient::Start(ID3D12Device*, const std::filesystem::path&,
    UINT, UINT, bool, UINT, UINT) {
    failed_ = true;
    error_ = "Neural/DLSS workers are not included in KOTOR2VR V1. Use native stereo.";
    return false;
}
bool NeuralWorkerClient::Submit(ID3D12CommandQueue*, bool, float, float) { return false; }
NeuralPoll NeuralWorkerClient::Poll() { return NeuralPoll::Failed; }
bool NeuralWorkerClient::QueueWaitCompletedOutput(ID3D12CommandQueue*) { return false; }
void NeuralWorkerClient::Close() noexcept {}
bool NeuralWorkerClient::Transfer(bool, void*, DWORD, DWORD) { return false; }
bool NeuralWorkerClient::Fail(const char*, DWORD) {
    failed_ = true;
    error_ = "Neural/DLSS workers are unavailable in KOTOR2VR V1.";
    return false;
}
} // namespace kotorvr::host
