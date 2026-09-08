#include "kotorvr/host/neural_worker_client.hpp"
#include "../../../third_party/DLSS5-Feeder/src/feed_ipc.h"
#include <cmath>
#include <vector>

namespace kotorvr::host {
namespace {
void CloseRemoteHandles(const FeedBuildAck& ack) {
    for (auto handle:ack.tex) if (handle) CloseHandle(reinterpret_cast<HANDLE>(handle));
    if (ack.fence_in) CloseHandle(reinterpret_cast<HANDLE>(ack.fence_in));
    if (ack.fence_out) CloseHandle(reinterpret_cast<HANDLE>(ack.fence_out));
    if (ack.panel_tex) CloseHandle(reinterpret_cast<HANDLE>(ack.panel_tex));
}
}
NeuralWorkerClient::~NeuralWorkerClient() { Close(); }
bool NeuralWorkerClient::Fail(const char* operation,DWORD code) {
    failed_=true; error_=std::string(operation)+" error="+std::to_string(code); return false;
}
bool NeuralWorkerClient::Transfer(bool write,void* data,DWORD size,DWORD timeout_ms) {
    HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!event) return Fail("pipe event");
    const ULONGLONG deadline=GetTickCount64()+timeout_ms;
    auto* bytes=static_cast<BYTE*>(data);
    bool ok=true;
    while (size) {
        OVERLAPPED operation{}; operation.hEvent=event; ResetEvent(event);
        const BOOL started=write ? WriteFile(pipe_,bytes,size,nullptr,&operation) : ReadFile(pipe_,bytes,size,nullptr,&operation);
        if (!started && GetLastError()!=ERROR_IO_PENDING) { ok=Fail(write ? "pipe write":"pipe read"); break; }
        const auto now=GetTickCount64();
        if (WaitForSingleObject(event,now>=deadline ? 0:static_cast<DWORD>(deadline-now))!=WAIT_OBJECT_0) {
            CancelIoEx(pipe_,&operation); DWORD ignored{};
            // Cancellation must retire before the stack OVERLAPPED is released.
            GetOverlappedResult(pipe_,&operation,&ignored,TRUE);
            ok=Fail("pipe timeout",ERROR_TIMEOUT); break;
        }
        DWORD completed{};
        if (!GetOverlappedResult(pipe_,&operation,&completed,FALSE) || !completed) { ok=Fail("pipe completion"); break; }
        size-=completed; bytes+=completed;
    }
    CloseHandle(event); return ok;
}
bool NeuralWorkerClient::Start(ID3D12Device* device,const std::filesystem::path& worker,
    UINT width,UINT height,bool transport_only,UINT output_width,UINT output_height) {
    Close(); error_.clear(); failed_=false;
    if (!output_width && !output_height) { output_width=width; output_height=height; }
    wchar_t upstreamValue[2]{};
    const DWORD upstreamLength=GetEnvironmentVariableW(L"KOTOR2VR_NEURAL_DIRECT_UPSTREAM",upstreamValue,2);
    if (upstreamLength && (upstreamLength!=1 || (upstreamValue[0]!=L'0' && upstreamValue[0]!=L'1')))
        return Fail("direct upstream flag requires 0 or 1",ERROR_INVALID_PARAMETER);
    const bool upstream=upstreamLength==1 && upstreamValue[0]==L'1';
    const DWORD rootLength=GetEnvironmentVariableW(L"KOTOR2VR_NEURAL_DIRECT_ROOT",nullptr,0);
    if (upstream && rootLength<=1)
        return Fail("upstream experiment requires the pinned direct runtime",ERROR_INVALID_PARAMETER);
    if (rootLength>1) { // A present-but-empty variable reports only the trailing NUL.
        std::vector<wchar_t> root(rootLength);
        const DWORD copied=GetEnvironmentVariableW(L"KOTOR2VR_NEURAL_DIRECT_ROOT",root.data(),rootLength);
        if (!copied || copied>=rootLength) return Fail("direct root changed during startup",ERROR_INVALID_PARAMETER);
        if (!upstream && (width!=output_width || height!=output_height))
            return Fail("direct backend requires native work scale (Quality100)",ERROR_NOT_SUPPORTED);
        direct_=std::make_unique<DirectNeuralEye>();
        return direct_->Start(device,std::filesystem::path(root.data()),width,height,transport_only,
            output_width,output_height,upstream ? DirectNeuralChain::NrThenSr:DirectNeuralChain::DlaaThenNr);
    }
    if (!device || !width || !height || width>4096 || height>4096 || !std::filesystem::is_regular_file(worker))
        return Fail("invalid worker arguments",ERROR_INVALID_PARAMETER);
    if (output_width<width || output_height<height || output_width>4096 || output_height>4096 ||
        (transport_only && (output_width!=width || output_height!=height)))
        return Fail("invalid worker output dimensions",ERROR_INVALID_PARAMETER);
    std::wstring command=L"\""+worker.wstring()+L"\" --serve-self";
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESHOWWINDOW; startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(worker.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
        nullptr,worker.parent_path().c_str(),&startup,&child)) return Fail("worker launch");
    process_=child.hProcess; pid_=child.dwProcessId; CloseHandle(child.hThread);
    const std::wstring name=L"\\\\.\\pipe\\dlss5-feed."+std::to_wstring(pid_);
    const ULONGLONG deadline=GetTickCount64()+10000;
    while (GetTickCount64()<deadline && WaitForSingleObject(process_,0)==WAIT_TIMEOUT) {
        pipe_=CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        if (pipe_!=INVALID_HANDLE_VALUE) break;
        Sleep(10);
    }
    if (pipe_==INVALID_HANDLE_VALUE) return Fail("worker pipe unavailable");
    HANDLE self{};
    if (!DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),process_,&self,
        PROCESS_DUP_HANDLE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,0)) return Fail("client handle duplication");
    FeedHello hello{FEED_IPC_MAGIC,FEED_IPC_VERSION,GetCurrentProcessId(),FEED_CLIENT_KOTOR_D3D12,reinterpret_cast<uint64_t>(self)};
    FeedHelloAck greeting{};
    if (!Transfer(true,&hello,sizeof(hello)) || !Transfer(false,&greeting,sizeof(greeting))) return false;
    if (greeting.magic!=FEED_IPC_MAGIC || greeting.version!=FEED_IPC_VERSION) return Fail("worker protocol mismatch",ERROR_REVISION_MISMATCH);
    FeedBuild build{}; build.width=width; build.height=height;
    build.target_width=output_width; build.target_height=output_height;
    build.color_fmt=build.output_fmt=DXGI_FORMAT_R8G8B8A8_UNORM;
    build.flags_override=-1; build.transport=transport_only ? 1:0; build.mv_scale_x=build.mv_scale_y=1;
    build.client_flags=FEED_BUILD_HOST_CREATES;
    char tag='B'; FeedBuildAck ack{};
    if (!Transfer(true,&tag,1) || !Transfer(true,&build,sizeof(build)) || !Transfer(false,&ack,sizeof(ack),15000)) return false;
    bool ok=ack.ok==1 && ack.output_fmt==DXGI_FORMAT_R8G8B8A8_UNORM;
    const DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT};
    HRESULT result=S_OK;
    for (unsigned slot=0;slot<4 && ok;++slot) {
        result=device->OpenSharedHandle(reinterpret_cast<HANDLE>(ack.tex[slot]),IID_PPV_ARGS(&textures_[slot]));
        ok=SUCCEEDED(result);
        if (ok) {
            const auto description=textures_[slot]->GetDesc();
            ok=description.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && description.Width==(slot==1 ? output_width:width) && description.Height==(slot==1 ? output_height:height) &&
                description.Format==formats[slot] && description.MipLevels==1 && description.DepthOrArraySize==1 && description.SampleDesc.Count==1;
        }
    }
    if (ok) { result=device->OpenSharedHandle(reinterpret_cast<HANDLE>(ack.fence_in),IID_PPV_ARGS(&input_fence_)); ok=SUCCEEDED(result); }
    if (ok) { result=device->OpenSharedHandle(reinterpret_cast<HANDLE>(ack.fence_out),IID_PPV_ARGS(&output_fence_)); ok=SUCCEEDED(result); }
    CloseRemoteHandles(ack);
    if (!ok) return Fail("worker build or resource import",FAILED(result) ? static_cast<DWORD>(result):ack.ngx_result);
    completion_event_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!completion_event_) return Fail("worker completion event");
    return true;
}
bool NeuralWorkerClient::Submit(ID3D12CommandQueue* queue,bool reset,float jitter_x,float jitter_y) {
    if (direct_) return direct_->Submit(queue,reset,jitter_x,jitter_y);
    if (failed_ || pending_ || !queue || !input_fence_ || !std::isfinite(jitter_x) || !std::isfinite(jitter_y)) return false;
    const auto next=sequence_+1;
    if (!next || next==UINT64_MAX) return Fail("sequence exhausted",ERROR_ARITHMETIC_OVERFLOW);
    // The caller has already submitted input copies and the previous output
    // copy on THIS queue. No output resource can be overwritten before it retires.
    const HRESULT result=queue->Signal(input_fence_.Get(),next);
    if (FAILED(result)) return Fail("input fence signal",static_cast<DWORD>(result));
    ResetEvent(completion_event_);
    const HRESULT notify=output_fence_->SetEventOnCompletion(next,completion_event_);
    if (FAILED(notify)) return Fail("output completion event",static_cast<DWORD>(notify));
    FeedFrameMsg frame{next,reset ? 1U:0U,jitter_x,jitter_y}; char tag='F';
    if (!Transfer(true,&tag,1) || !Transfer(true,&frame,sizeof(frame))) return false;
    sequence_=next; pending_=true; ack_received_=false; return true;
}
NeuralPoll NeuralWorkerClient::Poll() {
    if (direct_) return direct_->Poll();
    if (failed_) return NeuralPoll::Failed;
    if (!pending_) return NeuralPoll::Idle;
    if (!ack_received_) {
        DWORD available{};
        if (!PeekNamedPipe(pipe_,nullptr,0,nullptr,&available,nullptr)) { Fail("worker reply pipe"); return NeuralPoll::Failed; }
        if (available>=sizeof(FeedFrameAck)) {
            FeedFrameAck ack{};
            if (!Transfer(false,&ack,sizeof(ack),100) || ack.n!=sequence_ || ack.ok!=1) {
                Fail("worker evaluate failed or sequence mismatch",ERROR_INVALID_DATA); return NeuralPoll::Failed;
            }
            ack_received_=true;
        }
    }
    const auto completed=output_fence_->GetCompletedValue();
    if (completed==UINT64_MAX) { Fail("worker fence invalid",ERROR_DEVICE_NOT_CONNECTED); return NeuralPoll::Failed; }
    if (ack_received_ && completed>=sequence_) { pending_=false; return NeuralPoll::Complete; }
    if (WaitForSingleObject(process_,0)!=WAIT_TIMEOUT) { Fail("worker exited",ERROR_PROCESS_ABORTED); return NeuralPoll::Failed; }
    return NeuralPoll::Pending;
}
bool NeuralWorkerClient::QueueWaitCompletedOutput(ID3D12CommandQueue* queue) {
    if (direct_) return direct_->QueueWaitCompletedOutput(queue);
    if (failed_ || pending_ || !sequence_ || !queue || !output_fence_) return false;
    const auto completed=output_fence_->GetCompletedValue();
    if (completed<sequence_ || completed==UINT64_MAX) return false;
    return SUCCEEDED(queue->Wait(output_fence_.Get(),sequence_));
}
void NeuralWorkerClient::Close() noexcept {
    direct_.reset();
    const bool connected=pipe_!=INVALID_HANDLE_VALUE;
    if (connected) { CancelIoEx(pipe_,nullptr); CloseHandle(pipe_); pipe_=INVALID_HANDLE_VALUE; }
    if (process_) {
        // A connected feeder exits on EOF after draining its own GPU commands.
        // An unconnected startup failure has no submitted feature work to drain.
        if (!connected && WaitForSingleObject(process_,0)==WAIT_TIMEOUT) TerminateProcess(process_,1);
        WaitForSingleObject(process_,1000); CloseHandle(process_); process_=nullptr;
    }
    for (auto& texture:textures_) texture.Reset();
    input_fence_.Reset(); output_fence_.Reset(); pid_=0; sequence_=0; pending_=false; ack_received_=false;
    if (completion_event_) { CloseHandle(completion_event_); completion_event_=nullptr; }
}
}
