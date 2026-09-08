#include "kotorvr/host/direct_neural_eye.hpp"
#include "kotorvr/host/neural_worker_client.hpp"
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <bcrypt.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

namespace kotorvr::host {
namespace {
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
void ShutdownStage(const char* stage,const void* owner) noexcept {
    std::fprintf(stderr,"[neural-direct-stage] shutdown.%s owner=%p tid=%lu\n",
        stage,owner,static_cast<unsigned long>(GetCurrentThreadId()));
    std::fflush(stderr);
}
void Check(HRESULT hr,const char* where) {
    if (FAILED(hr)) throw std::runtime_error(std::string(where)+" HRESULT="+std::to_string(static_cast<unsigned>(hr)));
}
void Ngx(NVSDK_NGX_Result rc,const char* where) {
    if (rc!=NVSDK_NGX_Result_Success) throw std::runtime_error(std::string(where)+" NGX="+std::to_string(static_cast<unsigned>(rc)));
}
// Binary pins from the validated rev05 probe manifest. No runtime substitution.
void VerifyFile(const fs::path& path,const char* expected) {
    std::ifstream input(path,std::ios::binary);
    if (!input) throw std::runtime_error("direct runtime file missing: "+path.string());
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    auto checked=[](NTSTATUS status) { if (status<0) throw std::runtime_error("direct runtime SHA256 failed"); };
    try {
        checked(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0));
        checked(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0));
        std::array<char,65536> bytes{};
        while (input) {
            input.read(bytes.data(),bytes.size());
            if (input.gcount()) checked(BCryptHashData(hash,reinterpret_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(input.gcount()),0));
        }
        if (!input.eof()) throw std::runtime_error("direct runtime read failed");
        std::array<unsigned char,32> digest{};
        checked(BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0));
        std::string hex; constexpr char digits[]="0123456789ABCDEF";
        for (auto byte:digest) { hex+=digits[byte>>4]; hex+=digits[byte&15]; }
        if (hex!=expected) throw std::runtime_error("direct runtime hash mismatch: "+path.string());
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
        throw;
    }
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm,0);
}
void ResidentAt(const wchar_t* name,const fs::path& expected) {
    auto module=GetModuleHandleW(name);
    if (!module) return;
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(module,path,32768) || !fs::equivalent(path,expected))
        throw std::runtime_error("direct runtime already resident from another directory");
}
// Serialize the last NGX shutdown with the next initialization, including an
// initialization exception whose local shared_ptr is destroyed under this lock.
std::recursive_mutex runtimeMutex;
struct Runtime {
    using Create=void*(__cdecl*)(const wchar_t*,const wchar_t*,ID3D12Device*,ID3D12GraphicsCommandList*,void*,unsigned,unsigned,int,float,int,float,float,float,int,int);
    using Eval=int(__cdecl*)(ID3D12GraphicsCommandList*,void*,void*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,unsigned,unsigned,unsigned,unsigned,int,int,float,int,float,float,float,int,float,float);
    using Release=void(__cdecl*)(void*);
    ComPtr<ID3D12Device> device;
    fs::path root;
    std::wstring searchDirectory;
    const wchar_t* searchPaths[1]{};
    NVSDK_NGX_FeatureCommonInfo common{};
    HMODULE forwarder{};
    Create create{}; Eval eval{}; Release release{};
    bool initialized{},transportOnly{};
    std::atomic<bool> quarantined{};
    std::mutex calls;
    std::set<void*> parameters,handles;
    template<class T> T Export(const char* name) {
        auto p=GetProcAddress(forwarder,name);
        if (!p) throw std::runtime_error(std::string("direct forwarder export missing: ")+name);
        return reinterpret_cast<T>(p);
    }
    void Initialize(ID3D12Device* dev,const fs::path& directory,bool transport) {
        device=dev; root=fs::canonical(directory); transportOnly=transport;
        if (transportOnly) {
            std::clog<<"[neural-direct] DIRECT_TRANSPORT no NN; same direct Submit/fence ownership; root="<<root.string()<<'\n';
            return;
        }
        VerifyFile(root/L"nvngx_dlss.dll","C85F971CE023C9F3492FC7455F0B01A24BA18EA39636407A846902C4360B0B7E");
        VerifyFile(root/L"nvngx_dlssnr.dll","8270B350CD82DE5CE89806872CDD6B6A9249B80836B91BBEB3573470744CC206");
        VerifyFile(root/L"nvngx.dll_dlssnr.dll","98841052E81DEADF8C105B34191CBF5973401A626922EB1D0F9B64127E887A63");
        ResidentAt(L"nvngx_dlss.dll",root/L"nvngx_dlss.dll");
        ResidentAt(L"nvngx_dlssnr.dll",root/L"nvngx_dlssnr.dll");
        ResidentAt(L"nvngx.dll_dlssnr.dll",root/L"nvngx.dll_dlssnr.dll");
        forwarder=LoadLibraryExW((root/L"nvngx.dll_dlssnr.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!forwarder) throw std::runtime_error("direct typed forwarder load failed");
        create=Export<Create>("dlssnr_call_create"); eval=Export<Eval>("dlssnr_call_evaluate"); release=Export<Release>("dlssnr_call_release");
        if (Export<unsigned(__cdecl*)()>("dlssnr_typed_adapter_version")()!=1) throw std::runtime_error("direct typed adapter version mismatch");
        searchDirectory=root.wstring(); searchPaths[0]=searchDirectory.c_str();
        common.PathListInfo.Path=searchPaths; common.PathListInfo.Length=1;
        std::clog<<"[neural-direct-stage] shared_init.begin tid="<<GetCurrentThreadId()<<std::endl;
        Ngx(NVSDK_NGX_D3D12_Init(0x1000000ULL,root.c_str(),device.Get(),&common,NVSDK_NGX_Version_API),"direct shared NGX init");
        std::clog<<"[neural-direct-stage] shared_init.return tid="<<GetCurrentThreadId()<<std::endl;
        initialized=true;
        std::clog<<"[neural-direct] shared NGX runtime; pinned rev05 runtimes; root="<<root.string()<<'\n';
    }
    void Shutdown() {
        ShutdownStage("runtime.begin",this);
        ShutdownStage("runtime.lock.call",this);
        std::lock_guard lock(runtimeMutex);
        ShutdownStage("runtime.lock.return",this);
        if (initialized) {
            ShutdownStage("ngx_shutdown1.call",this);
            NVSDK_NGX_D3D12_Shutdown1(device.Get());
            ShutdownStage("ngx_shutdown1.return",this);
            initialized=false;
        }
        if (forwarder) {
            ShutdownStage("free_library.call",this);
            FreeLibrary(forwarder);
            ShutdownStage("free_library.return",this);
            forwarder=nullptr;
        }
        ShutdownStage("runtime.body_complete",this);
    }
    ~Runtime() { Shutdown(); }
};
// Keep the one host-device NGX session alive across eye/pipeline recreation.
// Eye handles/maps still retire individually. This also prevents a final-eye
// destructor racing a new Init on the same SDK-global device session. A lost
// device or uncertain retirement requires a fresh host process.
// Explicit scope shutdown owns clean teardown. The holder deliberately survives
// static destruction: uncertain GPU ownership must never unload the SDK, and a
// vendor shutdown after its own static destructors is invalid.
std::shared_ptr<Runtime>& SharedRuntime() {
    static auto* runtime=new std::shared_ptr<Runtime>();
    return *runtime;
}
std::shared_ptr<Runtime> Acquire(ID3D12Device* device,const fs::path& root,bool transportOnly) {
    std::lock_guard lock(runtimeMutex);
    if (auto runtime=SharedRuntime()) {
        if (runtime->quarantined || runtime->transportOnly!=transportOnly || runtime->device.Get()!=device || !fs::equivalent(runtime->root,root))
            throw std::runtime_error("direct runtime quarantined or conflicting device/root");
        return runtime;
    }
    auto runtime=std::make_shared<Runtime>(); runtime->Initialize(device,root,transportOnly); SharedRuntime()=runtime; return runtime;
}
void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&b);
}
}

DirectNeuralSessionScope::~DirectNeuralSessionScope() {
    std::lock_guard lock(runtimeMutex);
    auto& runtime=SharedRuntime();
    if (!runtime) return;
    if (runtime->quarantined || runtime.use_count()!=1 ||
        !runtime->parameters.empty() || !runtime->handles.empty()) {
        ShutdownStage("runtime_scope.quarantined",runtime.get());
        runtime->quarantined=true;
        return;
    }
    ShutdownStage("runtime_scope.reset",runtime.get());
    runtime.reset();
}

struct DirectNeuralEye::Impl {
    static constexpr UINT kGpuTraceQueryCount=3;
    static constexpr UINT64 kGpuTraceWarmup=30;
    static constexpr size_t kGpuTraceRows=360;
    struct GpuTraceRow {
        UINT64 sequence{};
        bool reset{};
        UINT width{},height{};
        UINT64 timestamps[kGpuTraceQueryCount]{};
        UINT64 frequency{};
        bool timestampsAvailable{},valid{};
        double srMilliseconds{},nrMilliseconds{};
    };
    struct GpuTraceSnapshot {
        fs::path output;
        std::array<GpuTraceRow,kGpuTraceRows> rows{};
        UINT count{};
        DWORD pid{};
        unsigned eye{};
        INT priority{};
        bool retired{};
        DirectNeuralChain chain{DirectNeuralChain::DlaaThenNr};
        UINT outputWidth{},outputHeight{};
    };
    std::shared_ptr<Runtime> runtime;
    std::array<ComPtr<ID3D12Resource>,4> textures;
    ComPtr<ID3D12Resource> intermediate;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12QueryHeap> gpuTraceQueries;
    ComPtr<ID3D12Resource> gpuTraceReadback;
    NVSDK_NGX_Parameter *nrParameters{},*srParameters{};
    NVSDK_NGX_Handle* srHandle{}; void* nrHandle{};
    HANDLE event{};
    UINT width{},height{};
    UINT outputWidth{},outputHeight{};
    DirectNeuralChain chain{DirectNeuralChain::DlaaThenNr};
    UINT64 fenceValue{},sequence{},submittedAt{};
    UINT64 gpuTraceFrequency{};
    std::array<GpuTraceRow,kGpuTraceRows> gpuTraceRows{};
    UINT gpuTraceRowCount{};
    INT gpuTraceQueuePriority{};
    fs::path gpuTraceOutput;
    DWORD startThread{};
    unsigned eyeOrdinal{};
    bool pending{},failed{},uncertain{},created{};
    bool gpuTraceRequested{},gpuTraceReady{},gpuTraceFrequencyReady{};
    bool gpuTraceCollected{},gpuTraceBackgroundAttempted{},gpuTraceExportScheduled{};
    void Stage(const char* stage) const {
        // Startup/first-frame diagnostics only; flush before entering vendor code
        // so the last completed boundary survives an access violation.
        std::clog<<"[neural-direct-stage] "<<stage<<" eye="<<this
            <<" tid="<<GetCurrentThreadId()<<" start_tid="<<startThread
            <<" list="<<list.Get()<<" queue="<<queue.Get()
            <<" sr="<<srHandle<<" nr="<<nrHandle<<" fence="<<fenceValue<<std::endl;
    }
    void Begin() { Check(allocator->Reset(),"direct allocator reset"); Check(list->Reset(allocator.Get(),nullptr),"direct list reset"); }
    void Execute() {
        Check(list->Close(),"direct list close");
        uncertain=true; // Execute has no HRESULT; retain everything if its fence cannot be established.
        ID3D12CommandList* commands[]={list.Get()}; queue->ExecuteCommandLists(1,commands);
        Check(queue->Signal(fence.Get(),++fenceValue),"direct signal");
        ResetEvent(event); Check(fence->SetEventOnCompletion(fenceValue,event),"direct completion event");
    }
    bool Wait(DWORD milliseconds) {
        if (FAILED(runtime->device->GetDeviceRemovedReason())) return false;
        auto done=fence->GetCompletedValue();
        if (done==UINT64_MAX) return false;
        if (done<fenceValue) {
            if (WaitForSingleObject(event,milliseconds)!=WAIT_OBJECT_0) return false;
            done=fence->GetCompletedValue();
        }
        if (done==UINT64_MAX || done<fenceValue) return false;
        uncertain=false; return true;
    }
    void DisableGpuTrace(const char* reason,HRESULT hr=S_OK) noexcept {
        gpuTraceReady=false;
        // Allocations stay with Impl until the existing retirement/quarantine
        // decision. Disabling diagnostics must never release submitted work.
        std::fprintf(stderr,"[neural-direct-gpu-trace] disabled eye=%u reason=%s HRESULT=%08x normal_rendering_continues=true\n",
            eyeOrdinal,reason,static_cast<unsigned>(hr));
        std::fflush(stderr);
    }
    void InitializeGpuTrace(ID3D12Device* device,bool transportOnly) noexcept {
        try {
            const DWORD length=GetEnvironmentVariableW(L"KOTOR2VR_DIRECT_GPU_TRACE",nullptr,0);
            if (!length) return;
            gpuTraceRequested=true;
            if (transportOnly) {
                DisableGpuTrace("transport_only_backend");
                return;
            }
            std::vector<wchar_t> directory(length);
            if (GetEnvironmentVariableW(L"KOTOR2VR_DIRECT_GPU_TRACE",directory.data(),length)!=length-1 || !directory[0]) {
                DisableGpuTrace("invalid_output_directory");
                return;
            }
            static std::atomic<UINT64> nextTraceSession{};
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            const auto session=++nextTraceSession;
            const auto name=L"direct-gpu-pid"+std::to_wstring(GetCurrentProcessId())+
                L"-eye"+std::to_wstring(eyeOrdinal)+L"-session"+std::to_wstring(session)+
                L"-qpc"+std::to_wstring(qpc.QuadPart)+L".csv";
            gpuTraceOutput=fs::absolute(fs::path(directory.data()))/name;

            D3D12_QUERY_HEAP_DESC queryDesc{};
            queryDesc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            queryDesc.Count=kGpuTraceQueryCount*static_cast<UINT>(kGpuTraceRows);
            auto hr=device->CreateQueryHeap(&queryDesc,IID_PPV_ARGS(&gpuTraceQueries));
            if (FAILED(hr)) {
                DisableGpuTrace("create_query_heap_failed",hr);
                return;
            }
            D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width=sizeof(UINT64)*kGpuTraceQueryCount*kGpuTraceRows;
            desc.Height=1; desc.DepthOrArraySize=1; desc.MipLevels=1;
            desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
                D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&gpuTraceReadback));
            if (FAILED(hr)) {
                DisableGpuTrace("create_readback_failed",hr);
                return;
            }
            gpuTraceReady=true;
            std::clog<<"[neural-direct-gpu-trace] enabled eye="<<eyeOrdinal
                <<" warmup="<<kGpuTraceWarmup<<" rows="<<kGpuTraceRows
                <<" output="<<gpuTraceOutput.string()<<'\n';
        } catch (const std::exception& e) {
            std::fprintf(stderr,"[neural-direct-gpu-trace] disabled eye=%u reason=initialization_exception detail=%s normal_rendering_continues=true\n",
                eyeOrdinal,e.what());
            std::fflush(stderr);
            gpuTraceReady=false;
        } catch (...) {
            DisableGpuTrace("initialization_exception_unknown");
        }
    }
    bool PrepareGpuTrace(ID3D12CommandQueue* directQueue) noexcept {
        if (!gpuTraceReady || sequence<kGpuTraceWarmup || gpuTraceRowCount>=kGpuTraceRows)
            return false;
        if (!gpuTraceFrequencyReady) {
            UINT64 frequency{};
            const auto hr=directQueue->GetTimestampFrequency(&frequency);
            if (FAILED(hr) || !frequency) {
                DisableGpuTrace(FAILED(hr) ? "timestamp_frequency_failed":"timestamp_frequency_zero",hr);
                return false;
            }
            gpuTraceFrequency=frequency;
            gpuTraceQueuePriority=directQueue->GetDesc().Priority;
            gpuTraceFrequencyReady=true;
        }
        return true;
    }
    void CollectGpuTrace() noexcept {
        // One bounded readback, after the last sampled evaluation's fence (or
        // Close's existing retirement). No query slot is ever reused.
        if (gpuTraceCollected || !gpuTraceRowCount || !gpuTraceReadback) return;
        gpuTraceCollected=true;
        const D3D12_RANGE readRange{0,sizeof(UINT64)*kGpuTraceQueryCount*gpuTraceRowCount};
        void* mapped{};
        const auto hr=gpuTraceReadback->Map(0,&readRange,&mapped);
        if (SUCCEEDED(hr) && mapped) {
            for (UINT index=0;index<gpuTraceRowCount;++index) {
                auto& row=gpuTraceRows[index];
                std::memcpy(row.timestamps,static_cast<const UINT64*>(mapped)+index*kGpuTraceQueryCount,sizeof(row.timestamps));
                row.timestampsAvailable=true;
                row.valid=row.frequency && row.timestamps[0] && row.timestamps[0]<row.timestamps[1] && row.timestamps[1]<row.timestamps[2];
                if (row.valid) {
                    const double first=static_cast<double>(row.timestamps[1]-row.timestamps[0])*1000.0/static_cast<double>(row.frequency);
                    const double second=static_cast<double>(row.timestamps[2]-row.timestamps[1])*1000.0/static_cast<double>(row.frequency);
                    row.srMilliseconds=chain==DirectNeuralChain::NrThenSr ? second:first;
                    row.nrMilliseconds=chain==DirectNeuralChain::NrThenSr ? first:second;
                }
            }
            const D3D12_RANGE writtenRange{0,0};
            gpuTraceReadback->Unmap(0,&writtenRange);
        }
        if (FAILED(hr) || !mapped) DisableGpuTrace("readback_map_failed",hr);
    }
    static void WriteGpuTrace(const GpuTraceSnapshot& snapshot) noexcept {
        try {
            std::error_code directoryError;
            fs::create_directories(snapshot.output.parent_path(),directoryError);
            if (directoryError) throw std::runtime_error("create_directory="+directoryError.message());
            std::ofstream output(snapshot.output,std::ios::binary|std::ios::trunc);
            if (!output) throw std::runtime_error("open_failed");
            output.imbue(std::locale::classic());
            output<<"pid,eye,queue_priority,retired,sequence,reset,width,height,timestamp_start,timestamp_between_sr_nr,timestamp_end,frequency,sr_ms,nr_ms,valid,chain,output_width,output_height\n";
            output<<std::setprecision(17);
            for (UINT index=0;index<snapshot.count;++index) {
                const auto& row=snapshot.rows[index];
                output<<snapshot.pid<<','<<snapshot.eye<<','<<snapshot.priority<<','<<(snapshot.retired ? "true":"false")<<',';
                output<<row.sequence<<','<<(row.reset ? "true":"false")<<','<<row.width<<','<<row.height<<',';
                if (row.timestampsAvailable) output<<row.timestamps[0]<<','<<row.timestamps[1]<<','<<row.timestamps[2]<<',';
                else output<<",,,";
                output<<row.frequency<<',';
                if (row.valid) output<<row.srMilliseconds<<','<<row.nrMilliseconds;
                else output<<',';
                output<<','<<(row.valid ? "true":"false")<<','
                    <<(snapshot.chain==DirectNeuralChain::NrThenSr ? "nr_then_sr":"dlaa_then_nr")<<','
                    <<snapshot.outputWidth<<','<<snapshot.outputHeight<<'\n';
            }
            output.flush();
            if (!output) throw std::runtime_error("write_failed");
            std::clog<<"[neural-direct-gpu-trace] exported eye="<<snapshot.eye
                <<" rows="<<snapshot.count<<" output="<<snapshot.output.string()<<'\n';
        } catch (const std::exception& e) {
            std::fprintf(stderr,"[neural-direct-gpu-trace] export_failed eye=%u detail=%s\n",snapshot.eye,e.what());
            std::fflush(stderr);
        } catch (...) {
            std::fprintf(stderr,"[neural-direct-gpu-trace] export_failed eye=%u detail=unknown\n",snapshot.eye);
            std::fflush(stderr);
        }
    }
    void ExportGpuTrace(bool retired,bool background=false) noexcept {
        if (!gpuTraceRequested || gpuTraceOutput.empty() || gpuTraceExportScheduled) return;
        if (background && gpuTraceBackgroundAttempted) return;
        if (background) gpuTraceBackgroundAttempted=true;
        if (retired) CollectGpuTrace();
        try {
            auto snapshot=std::make_unique<GpuTraceSnapshot>();
            snapshot->output=gpuTraceOutput; snapshot->rows=gpuTraceRows;
            snapshot->count=gpuTraceRowCount; snapshot->pid=GetCurrentProcessId();
            snapshot->eye=eyeOrdinal; snapshot->priority=gpuTraceQueuePriority;
            snapshot->retired=retired;
            snapshot->chain=chain; snapshot->outputWidth=outputWidth; snapshot->outputHeight=outputHeight;
            if (background) {
                // The callback owns plain CPU values only: no Impl, device,
                // query/readback resource, fence or rendering-thread file I/O.
                const auto callback=[](PTP_CALLBACK_INSTANCE,void* context) {
                    std::unique_ptr<GpuTraceSnapshot> owned(static_cast<GpuTraceSnapshot*>(context));
                    WriteGpuTrace(*owned);
                };
                if (!TrySubmitThreadpoolCallback(callback,snapshot.get(),nullptr))
                    throw std::runtime_error("export_callback_submit_failed");
                (void)snapshot.release();
            } else {
                WriteGpuTrace(*snapshot);
            }
            gpuTraceExportScheduled=true;
        } catch (const std::exception& e) {
            std::fprintf(stderr,"[neural-direct-gpu-trace] export_schedule_failed eye=%u detail=%s\n",eyeOrdinal,e.what());
            std::fflush(stderr);
        }
    }
    void CreateFeatures() {
        Stage("lazy_create.begin");
        Begin();
        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth=width; cp.Feature.InHeight=height;
        cp.Feature.InTargetWidth=outputWidth; cp.Feature.InTargetHeight=outputHeight;
        const bool upscale=outputWidth!=width || outputHeight!=height;
        cp.Feature.InPerfQualityValue=upscale ? NVSDK_NGX_PerfQuality_Value_MaxQuality:NVSDK_NGX_PerfQuality_Value_DLAA;
        cp.InFeatureCreateFlags=NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        static_assert(NVSDK_NGX_Feature_SuperSampling==1 && NVSDK_NGX_PerfQuality_Value_DLAA==5);
        Stage("sr_create.call");
        const auto srResult=NGX_D3D12_CREATE_DLSS_EXT(list.Get(),1,1,&srHandle,srParameters,&cp);
        Stage("sr_create.return");
        Ngx(srResult,"direct SR/DLAA create");
        if (!srHandle || !runtime->handles.insert(srHandle).second) { srHandle=nullptr; throw std::runtime_error("direct SR/DLAA handle missing/aliased"); }
        Stage("sr_create.submit");
        Execute(); Stage("sr_create.wait");
        if (!Wait(15000)) throw std::runtime_error("direct SR/DLAA create retirement timeout/device loss");
        Stage("sr_create.retired");
        if (!GetModuleHandleW(L"nvngx_dlss.dll")) throw std::runtime_error("direct SR provenance unavailable");
        ResidentAt(L"nvngx_dlss.dll",runtime->root/L"nvngx_dlss.dll");
        Begin();
        Stage("nr_create.call");
        nrHandle=runtime->create((runtime->root/L"nvngx_dlssnr.dll").c_str(),runtime->root.c_str(),runtime->device.Get(),list.Get(),nrParameters,width,height,0,1.06f,0,1.f,1.f,-1.f,1,1);
        Stage("nr_create.return");
        if (!nrHandle || !runtime->handles.insert(nrHandle).second) { nrHandle=nullptr; throw std::runtime_error("direct NR handle missing/aliased"); }
        Stage("nr_create.submit");
        Execute(); Stage("nr_create.wait");
        if (!Wait(15000)) throw std::runtime_error("direct NR create retirement timeout/device loss");
        Stage("nr_create.retired");
        ResidentAt(L"nvngx_dlssnr.dll",runtime->root/L"nvngx_dlssnr.dll");
        created=true;
        Stage("lazy_create.complete");
    }
    ~Impl() {
        // Only called after the complete caller queue has retired (including output reads).
        ShutdownStage("impl.begin",this);
        if (runtime) {
            ShutdownStage("impl.lock.call",this);
            std::lock_guard lock(runtime->calls);
            ShutdownStage("impl.lock.return",this);
            if (nrHandle) {
                ShutdownStage("nr_release.call",this);
                runtime->release(nrHandle);
                ShutdownStage("nr_release.return",this);
                runtime->handles.erase(nrHandle);
            }
            if (srHandle) {
                ShutdownStage("sr_release.call",this);
                const auto rc=NVSDK_NGX_D3D12_ReleaseFeature(srHandle);
                std::fprintf(stderr,"[neural-direct-cleanup] sr_release=%08x\n",static_cast<unsigned>(rc));
                ShutdownStage("sr_release.return",this);
                runtime->handles.erase(srHandle);
            }
            if (srParameters) {
                ShutdownStage("sr_destroy_parameters.call",this);
                const auto rc=NVSDK_NGX_D3D12_DestroyParameters(srParameters);
                std::fprintf(stderr,"[neural-direct-cleanup] sr_destroy_parameters=%08x\n",static_cast<unsigned>(rc));
                ShutdownStage("sr_destroy_parameters.return",this);
                runtime->parameters.erase(srParameters);
            }
            if (nrParameters) {
                ShutdownStage("nr_destroy_parameters.call",this);
                const auto rc=NVSDK_NGX_D3D12_DestroyParameters(nrParameters);
                std::fprintf(stderr,"[neural-direct-cleanup] nr_destroy_parameters=%08x\n",static_cast<unsigned>(rc));
                ShutdownStage("nr_destroy_parameters.return",this);
                runtime->parameters.erase(nrParameters);
            }
        }
        if (event) {
            ShutdownStage("close_event.call",this);
            CloseHandle(event);
            ShutdownStage("close_event.return",this);
        }
        ShutdownStage("impl.body_complete",this);
    }
};
DirectNeuralEye::DirectNeuralEye()=default;
DirectNeuralEye::~DirectNeuralEye() { Close(); }
bool DirectNeuralEye::Start(ID3D12Device* device,const fs::path& root,UINT width,UINT height,bool transportOnly,
    UINT outputWidth,UINT outputHeight,DirectNeuralChain chain) {
    Close(); error_.clear();
    try {
        if (!device || !width || !height || width>4096 || height>4096) throw std::runtime_error("invalid direct eye dimensions/device");
        if (!outputWidth && !outputHeight) { outputWidth=width; outputHeight=height; }
        if (!outputWidth || !outputHeight || outputWidth>4096 || outputHeight>4096 || outputWidth<width || outputHeight<height)
            throw std::runtime_error("invalid direct eye output dimensions");
        if (chain!=DirectNeuralChain::DlaaThenNr && chain!=DirectNeuralChain::NrThenSr)
            throw std::runtime_error("invalid direct neural chain");
        if (chain==DirectNeuralChain::DlaaThenNr && (outputWidth!=width || outputHeight!=height))
            throw std::runtime_error("direct DLAA then NR requires native output dimensions");
        if (transportOnly && chain==DirectNeuralChain::NrThenSr)
            throw std::runtime_error("direct transport does not support NR then SR");
        impl_=std::make_unique<Impl>(); auto& p=*impl_; p.runtime=Acquire(device,root,transportOnly); p.width=width; p.height=height;
        p.outputWidth=outputWidth; p.outputHeight=outputHeight; p.chain=chain;
        p.startThread=GetCurrentThreadId();
        std::lock_guard lock(p.runtime->calls);
        auto allocate=[&](NVSDK_NGX_Parameter*& params,bool capability) {
            Ngx(capability ? NVSDK_NGX_D3D12_GetCapabilityParameters(&params):NVSDK_NGX_D3D12_AllocateParameters(&params),"direct parameter allocation");
            if (!params || !p.runtime->parameters.insert(params).second) { params=nullptr; throw std::runtime_error("direct parameter map missing/aliased"); }
        };
        if (!transportOnly) {
            p.Stage("nr_parameters.call"); allocate(p.nrParameters,true); p.Stage("nr_parameters.return");
            p.Stage("sr_parameters.call"); allocate(p.srParameters,false); p.Stage("sr_parameters.return");
        }
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=width; desc.Height=height;
        desc.DepthOrArraySize=desc.MipLevels=1; desc.SampleDesc.Count=1; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT};
        for (unsigned i=0;i<4;++i) {
            desc.Width=i==1 ? outputWidth:width; desc.Height=i==1 ? outputHeight:height;
            desc.Format=formats[i]; Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&p.textures[i])),"direct texture");
        }
        desc.Width=width; desc.Height=height;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        if (!transportOnly) Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&p.intermediate)),"direct neural intermediate");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&p.allocator)),"direct allocator");
        Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,p.allocator.Get(),nullptr,IID_PPV_ARGS(&p.list)),"direct list");
        Check(p.list->Close(),"direct initial close");
        Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&p.fence)),"direct fence");
        p.event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if (!p.event) throw std::runtime_error("direct event creation failed");
        static std::atomic<unsigned> nextEye{};
        p.eyeOrdinal=++nextEye;
        p.InitializeGpuTrace(device,transportOnly);
        std::clog<<"[neural-direct] eye="<<p.eyeOrdinal<<" host_pid="<<GetCurrentProcessId()
            <<" backend="<<(transportOnly ? "DIRECT_TRANSPORT":chain==DirectNeuralChain::NrThenSr ? "EXPERIMENTAL_NR_THEN_SR_NOT_PARITY":"ALTERNATE_DLAA_DIRECT_NR_NOT_PARITY")
            <<" chain="<<(chain==DirectNeuralChain::NrThenSr ? "nr_then_sr":"dlaa_then_nr")
            <<" dimensions="<<width<<'x'<<height<<" output_dimensions="<<outputWidth<<'x'<<outputHeight
            <<" quality_confirmed=false one_outstanding=true COMMON_io=true\n";
        return true;
    } catch (const std::exception& e) { error_=e.what(); if (impl_) impl_->failed=true; return false; }
}
bool DirectNeuralEye::Submit(ID3D12CommandQueue* queue,bool reset,float jitterX,float jitterY) {
    if (!impl_ || impl_->failed || impl_->pending) return false;
    auto& p=*impl_;
    try {
        if (!queue || queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT || jitterX!=0 || jitterY!=0)
            throw std::runtime_error("direct requires DIRECT queue and zero jitter");
        ComPtr<ID3D12Device> device; Check(queue->GetDevice(IID_PPV_ARGS(&device)),"direct queue device");
        if (device.Get()!=p.runtime->device.Get() || (p.queue && p.queue.Get()!=queue)) throw std::runtime_error("direct queue/device changed");
        p.queue=queue;
        std::lock_guard lock(p.runtime->calls);
        if (p.runtime->quarantined) throw std::runtime_error("direct shared runtime quarantined");
        if (p.runtime->transportOnly) {
            p.Begin();
            Barrier(p.list.Get(),p.textures[0].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(p.list.Get(),p.textures[1].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
            p.list->CopyResource(p.textures[1].Get(),p.textures[0].Get());
            Barrier(p.list.Get(),p.textures[0].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
            Barrier(p.list.Get(),p.textures[1].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
            p.Execute(); ++p.sequence; p.pending=true; p.submittedAt=GetTickCount64(); return true;
        }
        if (!p.created) p.CreateFeatures();
        if (!p.sequence) p.Stage("first_evaluate.begin");
        p.Begin();
        for (unsigned i:{0U,2U,3U}) Barrier(p.list.Get(),p.textures[i].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(p.list.Get(),p.intermediate.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(p.list.Get(),p.textures[1].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool nrFirst=p.chain==DirectNeuralChain::NrThenSr;
        NVSDK_NGX_D3D12_DLSS_Eval_Params ep{};
        ep.Feature.pInColor=nrFirst ? p.intermediate.Get():p.textures[0].Get();
        ep.Feature.pInOutput=nrFirst ? p.textures[1].Get():p.intermediate.Get();
        ep.pInDepth=p.textures[2].Get(); ep.pInMotionVectors=p.textures[3].Get();
        ep.InRenderSubrectDimensions.Width=p.width; ep.InRenderSubrectDimensions.Height=p.height;
        // Both features consume work-pixel motion and the identical reset.
        // Zero jitter/exposure defaults and the typed NR contract are unchanged.
        ep.InReset=reset || !p.sequence; ep.InMVScaleX=ep.InMVScaleY=1; ep.InPreExposure=ep.InExposureScale=1;
        const bool gpuTrace=p.PrepareGpuTrace(queue);
        const UINT queryBase=p.gpuTraceRowCount*Impl::kGpuTraceQueryCount;
        if (gpuTrace) p.list->EndQuery(p.gpuTraceQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,queryBase);
        const auto evaluateSr=[&] {
            if (!p.sequence) p.Stage("first_sr_evaluate.call");
            const auto srResult=NGX_D3D12_EVALUATE_DLSS_EXT(p.list.Get(),p.srHandle,p.srParameters,&ep);
            if (!p.sequence) p.Stage("first_sr_evaluate.return");
            Ngx(srResult,"direct SR/DLAA evaluate");
        };
        const auto evaluateNr=[&] {
            if (!p.sequence) p.Stage("first_nr_evaluate.call");
            const auto nrResult=p.runtime->eval(p.list.Get(),p.nrHandle,p.nrParameters,
                nrFirst ? p.textures[0].Get():p.intermediate.Get(),p.textures[2].Get(),p.textures[3].Get(),
                nrFirst ? p.intermediate.Get():p.textures[1].Get(),
                p.width,p.height,p.width,p.height,0,ep.InReset,1.06f,0,1.f,1.f,-1.f,1,1.f,1.f);
            if (!p.sequence) p.Stage("first_nr_evaluate.return");
            if (nrResult!=1) throw std::runtime_error("direct NR evaluate failed");
        };
        if (nrFirst) evaluateNr(); else evaluateSr();
        Barrier(p.list.Get(),p.intermediate.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // First span includes the intermediate transition; second ends before cleanup.
        // CSV retains the historical timestamp_between_sr_nr name for either order.
        // These elapsed queue spans include possible contention/preemption.
        if (gpuTrace) p.list->EndQuery(p.gpuTraceQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,queryBase+1);
        if (nrFirst) evaluateSr(); else evaluateNr();
        if (gpuTrace) {
            p.list->EndQuery(p.gpuTraceQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,queryBase+2);
            p.list->ResolveQueryData(p.gpuTraceQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,
                queryBase,Impl::kGpuTraceQueryCount,p.gpuTraceReadback.Get(),UINT64(queryBase)*sizeof(UINT64));
        }
        for (unsigned i:{0U,2U,3U}) Barrier(p.list.Get(),p.textures[i].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        Barrier(p.list.Get(),p.intermediate.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        Barrier(p.list.Get(),p.textures[1].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        if (!p.sequence) p.Stage("first_evaluate.submit");
        p.Execute();
        if (gpuTrace) {
            auto& row=p.gpuTraceRows[p.gpuTraceRowCount++];
            row.sequence=p.sequence+1; row.reset=ep.InReset!=0;
            row.width=p.width; row.height=p.height; row.frequency=p.gpuTraceFrequency;
        }
        if (!p.sequence) p.Stage("first_evaluate.submitted");
        ++p.sequence; p.pending=true; p.submittedAt=GetTickCount64(); return true;
    } catch (const std::exception& e) { error_=e.what(); p.failed=true; return false; }
}
NeuralPoll DirectNeuralEye::Poll() {
    if (!impl_ || impl_->failed) return NeuralPoll::Failed;
    auto& p=*impl_;
    if (!p.pending) return NeuralPoll::Idle;
    if (p.Wait(0)) {
        if (p.gpuTraceRowCount==Impl::kGpuTraceRows) p.ExportGpuTrace(true,true);
        if (p.sequence==1 && !p.runtime->transportOnly) p.Stage("first_evaluate.retired");
        p.pending=false; return NeuralPoll::Complete;
    }
    if (FAILED(p.runtime->device->GetDeviceRemovedReason()) || p.fence->GetCompletedValue()==UINT64_MAX || GetTickCount64()-p.submittedAt>5000) {
        p.failed=true; error_="direct completion timeout/device loss"; return NeuralPoll::Failed;
    }
    return NeuralPoll::Pending;
}
bool DirectNeuralEye::QueueWaitCompletedOutput(ID3D12CommandQueue* queue) {
    if (!impl_ || impl_->failed || impl_->pending || !impl_->sequence || queue!=impl_->queue.Get()) return false;
    return impl_->Wait(0) && SUCCEEDED(queue->Wait(impl_->fence.Get(),impl_->fenceValue));
}
void DirectNeuralEye::Close() noexcept {
    if (!impl_) return;
    auto& p=*impl_;
    ShutdownStage("close.begin",this);
    bool safe=!p.textures[0];
    // Also retire the caller's output copy, which follows our evaluation fence.
    ShutdownStage("retirement.check.call",this);
    if (p.queue && p.fence && p.event && SUCCEEDED(p.runtime->device->GetDeviceRemovedReason())) {
        ShutdownStage("retirement.check.ready",this);
        ShutdownStage("retirement.signal.call",this);
        if (SUCCEEDED(p.queue->Signal(p.fence.Get(),++p.fenceValue))) {
            ShutdownStage("retirement.signal.return_success",this);
            ShutdownStage("retirement.reset_event.call",this);
            ResetEvent(p.event);
            ShutdownStage("retirement.reset_event.return",this);
            ShutdownStage("retirement.set_event.call",this);
            if (SUCCEEDED(p.fence->SetEventOnCompletion(p.fenceValue,p.event))) {
                ShutdownStage("retirement.set_event.return_success",this);
                ShutdownStage("retirement.wait.call",this);
                safe=p.Wait(5000);
                ShutdownStage(safe ? "retirement.wait.return_safe":"retirement.wait.return_unsafe",this);
            } else ShutdownStage("retirement.set_event.return_failure",this);
        } else ShutdownStage("retirement.signal.return_failure",this);
    } else ShutdownStage("retirement.check.unavailable",this);
    ShutdownStage(safe ? "retirement.safe":"retirement.unsafe",this);
    p.ExportGpuTrace(safe);
    if (!safe) {
        // Process-lifetime quarantine owns device, features, allocator, textures,
        // fence and registered event. Do not unload NGX beneath unretired work.
        ShutdownStage("quarantine.call",this);
        if (p.runtime) { std::lock_guard lock(runtimeMutex); p.runtime->quarantined=true; }
        OutputDebugStringA("[neural-direct] retirement unproven: quarantined until process exit\n");
        std::fputs("[neural-direct] retirement unproven: quarantined until process exit\n",stderr);
        std::fflush(stderr);
        (void)impl_.release();
        ShutdownStage("quarantine.return",this);
    } else {
        ShutdownStage("impl_reset.call",this);
        impl_.reset();
        ShutdownStage("impl_reset.return",this);
    }
    ShutdownStage("close.complete",this);
}
ID3D12Resource* DirectNeuralEye::texture(unsigned i) const noexcept { return impl_ && i<4 ? impl_->textures[i].Get():nullptr; }
std::uint64_t DirectNeuralEye::sequence() const noexcept { return impl_ ? impl_->sequence:0; }
HANDLE DirectNeuralEye::completion_event() const noexcept { return impl_ ? impl_->event:nullptr; }
const std::string& DirectNeuralEye::error() const noexcept { return error_; }
}
