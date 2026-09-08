#include "kotorvr/host/neural_worker_client.hpp"

// V1 deliberately has no NVIDIA NGX/DLSS dependency. Keep the optional
// backend's interface so native stereo host control flow remains unchanged.
namespace kotorvr::host {
struct DirectNeuralEye::Impl {};
DirectNeuralSessionScope::~DirectNeuralSessionScope() = default;
DirectNeuralEye::DirectNeuralEye() = default;
DirectNeuralEye::~DirectNeuralEye() = default;
bool DirectNeuralEye::Start(ID3D12Device*, const std::filesystem::path&,
    UINT, UINT, bool, UINT, UINT, DirectNeuralChain) {
    error_ = "Neural/DLSS processing is not included in KOTOR2VR V1. Use native stereo.";
    return false;
}
bool DirectNeuralEye::Submit(ID3D12CommandQueue*, bool, float, float) { return false; }
NeuralPoll DirectNeuralEye::Poll() { return NeuralPoll::Failed; }
bool DirectNeuralEye::QueueWaitCompletedOutput(ID3D12CommandQueue*) { return false; }
void DirectNeuralEye::Close() noexcept {}
ID3D12Resource* DirectNeuralEye::texture(unsigned) const noexcept { return nullptr; }
std::uint64_t DirectNeuralEye::sequence() const noexcept { return 0; }
HANDLE DirectNeuralEye::completion_event() const noexcept { return nullptr; }
const std::string& DirectNeuralEye::error() const noexcept { return error_; }
} // namespace kotorvr::host
