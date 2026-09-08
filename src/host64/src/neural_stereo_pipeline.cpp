#include "kotorvr/host/neural_stereo_pipeline.hpp"
#include "kotorvr/host/neural_worker_client.hpp"
#include "kotorvr/host/neural_guides.hpp"
#include <atomic>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <locale>
#include <mutex>
#include <cstdio>
#include <thread>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace kotorvr::host {
namespace {
// BEGIN serial-eye scheduling (CPU-tested from this production source).
bool ReadSerialEyesOption() noexcept {
    wchar_t value[3]{};
    return GetEnvironmentVariableW(L"KOTOR2VR_NEURAL_SERIAL_EYES",value,3)==1 && value[0]==L'1';
}
template<class SubmitEye,class WaitEyeMask>
void RunEyeSchedule(bool serial,SubmitEye&& submit,WaitEyeMask&& wait) {
    submit(0);
    if (serial) wait(1U); // Poll Complete means matching ACK AND output fence.
    submit(1);
    wait(serial ? 2U:3U);
}
// END serial-eye scheduling.
// BEGIN bounded-age prefetch policy (CPU-tested from the unapplied patch).
// Both flags are required. Invalid/empty limits preserve ordinary prefetch.
[[maybe_unused]] static double ParsePrefetchMaxAgeMs(bool prefetch_enabled,const wchar_t* value) noexcept {
    if (!prefetch_enabled || !value || !*value) return 0.0;
    char ascii[64]{}; std::size_t length=0;
    while (value[length]) {
        if (length==sizeof(ascii)-1 || static_cast<unsigned int>(value[length])>127U) return 0.0;
        ascii[length]=static_cast<char>(value[length]); ++length;
    }
    const char* first=ascii;
    if (*first=='+') ++first;
    double limit=0.0;
    const auto parsed=std::from_chars(first,ascii+length,limit,std::chars_format::general);
    return parsed.ec==std::errc{} && parsed.ptr==ascii+length && std::isfinite(limit) && limit>0.0 ? limit:0.0;
}
struct PrefetchAgePolicy {
    using Clock=std::chrono::steady_clock;
    double max_age_ms{};
    std::uint32_t pending_reset_reasons{};
    bool pending_reset{};
    std::uint64_t dropped_inputs{};
    bool enabled() const noexcept { return max_age_ms>0.0; }
    static std::uint32_t AdmissionResetReasons(bool replaced,std::uint32_t previous,std::uint32_t current) noexcept {
        return (replaced ? previous:0U) | current;
    }
    // Called under the ownership mutex, only for Queued inputs, immediately
    // before Working. A drop never consumes deferred resets or starts GPU reads.
    bool TryBegin(Clock::time_point admitted,Clock::time_point now,std::uint32_t& reset_reasons,bool& reset) noexcept {
        if (enabled() && std::chrono::duration<double,std::milli>(now-admitted).count()>max_age_ms) {
            pending_reset_reasons|=reset_reasons;
            pending_reset=pending_reset || reset;
            ++dropped_inputs;
            return false;
        }
        reset_reasons|=pending_reset_reasons;
        reset=reset || pending_reset || reset_reasons!=0;
        pending_reset_reasons=0; pending_reset=false;
        return true;
    }
};
// END bounded-age prefetch policy.
std::uint64_t TraceQpc() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) ? static_cast<std::uint64_t>(value.QuadPart):0;
}
bool ReadQueueHighOption() noexcept {
    wchar_t value[3]{};
    return GetEnvironmentVariableW(L"KOTOR2VR_NEURAL_QUEUE_HIGH",value,3)==1 && value[0]==L'1';
}
const char* QueuePriorityName(D3D12_COMMAND_QUEUE_PRIORITY priority) noexcept {
    switch (priority) {
    case D3D12_COMMAND_QUEUE_PRIORITY_HIGH: return "HIGH";
    case D3D12_COMMAND_QUEUE_PRIORITY_NORMAL: return "NORMAL";
    default: return "UNKNOWN";
    }
}
void LogQueuePriorityDecision(D3D12_COMMAND_QUEUE_PRIORITY requested,D3D12_COMMAND_QUEUE_PRIORITY effective,
    const char* reason,HRESULT hr=S_OK) noexcept {
    std::fprintf(stderr,
        "[neural-queue] requested=%s effective=%s reason=%s",
        QueuePriorityName(requested),QueuePriorityName(effective),reason);
    if (FAILED(hr)) std::fprintf(stderr," hresult=0x%08X",static_cast<unsigned>(hr));
    std::fputc('\n',stderr);
    std::fflush(stderr);
}
// Stable, CPU-only records. Input ownership publishes admission/submission to
// the worker; output ownership publishes worker fields to RecordDisplay. No
// reader examines worker fields until work_published is set under Impl::mutex,
// or destruction has joined the worker thread. A bounded automatic export
// copies only settled rows under that mutex; its writer owns the entire copy.
// A replaced input/output keeps its own record; slot reuse never reuses a row.
struct NeuralTracePair {
    std::uint64_t id{},ready_value{},xr_frame_id{},camera_frame_id{},generation{};
    std::int64_t predicted_display_time_ns{},admission_predicted_display_time_ns{};
    std::uint64_t request_qpc{},admission_qpc{},submitted_qpc{},host_fence{};
    std::uint64_t idle_begin_qpc{},work_start_qpc{},input_wait_enqueued_qpc{};
    std::uint64_t guide_submit_qpc{},guide_wait_begin_qpc{},guide_end_qpc{};
    std::uint64_t eye_submit_begin_qpc[2]{},eye_submit_end_qpc[2]{},eye_complete_observed_qpc[2]{};
    std::uint64_t eye_sequence[2]{},eye_wait_ticks{},eye_wait_calls{},eyes_complete_qpc{};
    DWORD eye_pid[2]{};
    std::uint64_t compose_begin_qpc{},compose_submit_qpc{},compose_wait_begin_qpc{},compose_end_qpc{};
    std::int64_t display_predicted_display_time_ns{};
    std::uint64_t display_accept_qpc{},display_submit_qpc{},display_host_fence{},display_reads{};
    std::uint64_t processed_pair{};
    bool prefetched{},reset{},replaced_input{},superseded_output{},work_published{},discarded_age{};
};
// Called only at RecordDisplay's successful selection point (also external reads).
void TraceDisplay(NeuralTracePair& pair,std::int64_t predicted_display_time_ns=0) noexcept {
    if (!pair.display_reads) pair.display_predicted_display_time_ns=predicted_display_time_ns;
    if (!pair.display_accept_qpc) pair.display_accept_qpc=TraceQpc();
    ++pair.display_reads;
}
struct NeuralTimingTrace {
    static constexpr std::size_t limit=360;
    std::array<NeuralTracePair,limit> pairs{};
    std::size_t size{};
    std::uint64_t start_qpc{},frequency{},request_calls{},admissions{},display_calls{};
    std::uint64_t snapshot_qpc{},age_dropped_inputs{};
    double prefetch_max_age_ms{};
    bool serial_eyes{};
    bool direct_backend{};
    D3D12_COMMAND_QUEUE_PRIORITY requested_queue_priority{D3D12_COMMAND_QUEUE_PRIORITY_NORMAL};
    D3D12_COMMAND_QUEUE_PRIORITY effective_queue_priority{D3D12_COMMAND_QUEUE_PRIORITY_NORMAL};
    const char* queue_priority_reason{"default-normal"};
    DWORD process_id{};
    std::filesystem::path path;
    static std::unique_ptr<NeuralTimingTrace> Create() noexcept {
        try {
            wchar_t target[32768]{};
            DWORD length=GetEnvironmentVariableW(L"KOTOR2VR_NEURAL_TIMING_TRACE",target,32768);
            if (!length || length>=32768 || (length==1 && target[0]==L'0')) return {};
            LARGE_INTEGER hz{};
            if (!QueryPerformanceFrequency(&hz) || hz.QuadPart<=0) return {};
            auto trace=std::make_unique<NeuralTimingTrace>();
            trace->start_qpc=TraceQpc(); trace->frequency=static_cast<std::uint64_t>(hz.QuadPart);
            trace->process_id=GetCurrentProcessId();
            if (length==1 && target[0]==L'1') {
                length=GetEnvironmentVariableW(L"KOTOR2VR_NEURAL_TIMING_TRACE_PATH",target,32768);
                if (length>=32768) return {};
            }
            if (length) {
                trace->path=target;
                if (!trace->path.is_absolute()) {
                    OutputDebugStringA("KOTOR2VR neural timing: use an absolute JSONL path\n"); return {};
                }
            } else {
                const DWORD temp_length=GetTempPathW(32768,target);
                if (!temp_length || temp_length>=32768) return {};
                trace->path=std::filesystem::path(target)/(L"kotor2vr-neural-timing-"+
                    std::to_wstring(trace->process_id)+L"-"+std::to_wstring(trace->start_qpc)+L".jsonl");
            }
            OutputDebugStringW((L"KOTOR2VR neural timing: export at settled limit or pipeline shutdown to "+trace->path.wstring()+L"\n").c_str());
            return trace;
        } catch (...) { OutputDebugStringA("KOTOR2VR neural timing: initialization unavailable\n"); return {}; }
    }
    NeuralTracePair* Admit(const k2vr::ipc::StereoFrameMetadata& frame,std::uint64_t request,bool prefetched,std::int64_t admission_predicted_display_time_ns=0) noexcept {
        ++admissions;
        if (size==limit) return nullptr;
        auto& pair=pairs[size++];
        pair.id=admissions; pair.ready_value=frame.ready_value; pair.xr_frame_id=frame.request.frame_id;
        pair.camera_frame_id=frame.camera_frame_id; pair.generation=frame.request.header.generation;
        pair.predicted_display_time_ns=frame.request.predicted_display_time_ns;
        pair.admission_predicted_display_time_ns=admission_predicted_display_time_ns;
        pair.request_qpc=request; pair.admission_qpc=TraceQpc(); pair.prefetched=prefetched;
        return &pair;
    }
    // Host-thread-only, while holding Impl::mutex. Never inspect a row's worker
    // fields before publication: its QPC stamps are written outside the mutex.
    bool ReadyForSnapshot() const noexcept {
        if (size!=limit) return false;
        for (const auto& p:pairs) {
            if (p.replaced_input || p.discarded_age) continue; // worker never acquired this input
            if (!p.work_published) return false;
            if (!p.display_submit_qpc && !p.superseded_output) return false;
        }
        return true;
    }
    void Write(UINT width,UINT height,UINT work_width,UINT work_height,bool prefetch,bool failed) const noexcept {
        // Append permits several pipeline lifetimes in one explicitly named
        // file. All rows carry the session QPC; no filesystem work in frame loops.
        try {
            std::ofstream out(path,std::ios::out|std::ios::app);
            out.imbue(std::locale::classic());
            out << "{\"type\":\"neural_timing_session\",\"schema\":1,\"trace_qpc\":" << start_qpc
                << ",\"pid\":" << process_id << ",\"qpc_frequency\":" << frequency
                << ",\"snapshot_qpc\":" << snapshot_qpc
                << ",\"export_reason\":\"" << (snapshot_qpc ? "settled_limit":"shutdown") << '"'
                << ",\"counters_scope\":\"" << (snapshot_qpc ? "through_snapshot":"pipeline_lifetime") << '"'
                << ",\"limit\":" << limit << ",\"records\":" << size
                << ",\"request_calls\":" << request_calls << ",\"admissions\":" << admissions
                << ",\"display_calls\":" << display_calls << ",\"failed\":" << (failed ? "true":"false")
                << ",\"width\":" << width << ",\"height\":" << height
                << ",\"work_width\":" << work_width << ",\"work_height\":" << work_height
                << ",\"prefetch\":" << (prefetch ? "true":"false")
                << ",\"serial_eyes\":" << (serial_eyes ? "true":"false")
                << ",\"direct_backend\":" << (direct_backend ? "true":"false")
                << ",\"requested_queue_priority\":\"" << QueuePriorityName(requested_queue_priority) << '"'
                << ",\"effective_queue_priority\":\"" << QueuePriorityName(effective_queue_priority) << '"'
                << ",\"queue_priority_reason\":\"" << queue_priority_reason << '"'
                << ",\"prefetch_max_age_ms\":" << prefetch_max_age_ms
                << ",\"age_dropped_inputs\":" << age_dropped_inputs
                << ",\"gpu_timestamps\":false,\"request_origin\":\"RecordInput_entry\",\"display_origin\":\"RecordDisplay_success_not_scanout\"}\n";
            for (std::size_t i=0;i<size;++i) {
                const auto& p=pairs[i];
                out << "{\"type\":\"neural_pair\",\"trace_qpc\":" << start_qpc << ",\"pid\":" << process_id;
                const auto value=[&](const char* key,auto v) { out << ",\"" << key << "\":" << v; };
                value("pair",p.id); value("ready_value",p.ready_value); value("xr_frame_id",p.xr_frame_id);
                value("camera_frame_id",p.camera_frame_id); value("generation",p.generation);
                value("predicted_display_time_ns",p.predicted_display_time_ns);
                value("admission_predicted_display_time_ns",p.admission_predicted_display_time_ns);
                value("request_qpc",p.request_qpc); value("admission_qpc",p.admission_qpc);
                value("submitted_qpc",p.submitted_qpc); value("host_fence",p.host_fence);
                value("idle_begin_qpc",p.idle_begin_qpc); value("work_start_qpc",p.work_start_qpc);
                value("input_wait_enqueued_qpc",p.input_wait_enqueued_qpc);
                value("guide_submit_qpc",p.guide_submit_qpc); value("guide_wait_begin_qpc",p.guide_wait_begin_qpc);
                value("guide_end_qpc",p.guide_end_qpc);
                for (unsigned eye=0;eye<2;++eye) {
                    out << ",\"" << (eye ? "right":"left") << "\":{\"submit_begin_qpc\":" << p.eye_submit_begin_qpc[eye]
                        << ",\"submit_end_qpc\":" << p.eye_submit_end_qpc[eye]
                        << ",\"complete_observed_qpc\":" << p.eye_complete_observed_qpc[eye]
                        << ",\"worker_pid\":" << p.eye_pid[eye]
                        << ",\"worker_sequence\":" << p.eye_sequence[eye] << '}';
                }
                value("eye_wait_ticks",p.eye_wait_ticks); value("eye_wait_calls",p.eye_wait_calls);
                value("eyes_complete_qpc",p.eyes_complete_qpc); value("compose_begin_qpc",p.compose_begin_qpc);
                value("compose_submit_qpc",p.compose_submit_qpc); value("compose_wait_begin_qpc",p.compose_wait_begin_qpc);
                value("compose_end_qpc",p.compose_end_qpc); value("processed_pair",p.processed_pair);
                value("display_predicted_display_time_ns",p.display_predicted_display_time_ns);
                value("display_accept_qpc",p.display_accept_qpc); value("display_submit_qpc",p.display_submit_qpc);
                value("display_host_fence",p.display_host_fence); value("display_reads",p.display_reads);
                value("prefetched",p.prefetched ? "true":"false"); value("reset",p.reset ? "true":"false");
                value("replaced_input",p.replaced_input ? "true":"false");
                value("discarded_age",p.discarded_age ? "true":"false");
                value("superseded_output",p.superseded_output ? "true":"false");
                out << "}\n";
            }
            out.flush();
            if (!out) OutputDebugStringA("KOTOR2VR neural timing: export failed\n");
        } catch (...) { OutputDebugStringA("KOTOR2VR neural timing: export failed\n"); }
    }
};
void Check(HRESULT result,const char* stage) {
    if (FAILED(result)) throw std::runtime_error(std::string(stage)+" HRESULT="+std::to_string(result));
}
void Transition(ID3D12GraphicsCommandList* list,ID3D12Resource* texture,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={texture,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&barrier);
}
void CopyRect(ID3D12GraphicsCommandList* list,ID3D12Resource* src,ID3D12Resource* dst,
    UINT source_x,UINT source_y,UINT width,UINT height,UINT target_x=0,UINT target_y=0) {
    Transition(list,src,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list,dst,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource=src;
    D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource=dst;
    const D3D12_BOX box{source_x,source_y,0,source_x+width,source_y+height,1};
    list->CopyTextureRegion(&to,target_x,target_y,0,&from,&box);
    Transition(list,dst,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    Transition(list,src,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
}
}
struct NeuralStereoPipeline::Impl {
    enum Phase { Starting,Running,Failed };
    std::atomic<Phase> phase{Starting}; std::atomic<bool> stopping{};
    std::thread thread; std::mutex mutex; std::condition_variable wake;
    std::string failure;
    ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> done,host_submitted;
    using Clock=std::chrono::steady_clock;
    enum class InputState { Empty,Recording,Queued,Working };
    enum class OutputState { Empty,Working,Complete,Displaying,Retiring };
    struct Input {
        ComPtr<ID3D12Resource> texture;
        // Each SRV permanently names its own input atlas; descriptors are never
        // rewritten while a GPU dispatch can still reference them.
        NeuralGuides guides;
        InputState state{InputState::Empty};
        k2vr::ipc::StereoFrameMetadata frame{};
        UINT64 ready{}; Clock::time_point submitted_at{},admitted_at{};
        std::uint32_t reset_reasons{};
        NeuralTracePair* trace{};
        bool reset{},prefetched{};
    } inputs[2];
    struct Output {
        ComPtr<ID3D12Resource> texture;
        OutputState state{OutputState::Empty};
        k2vr::ipc::StereoFrameMetadata frame{};
        NeuralPairTiming timing{};
        UINT64 retired{}; ULONGLONG completed_at{};
        NeuralTracePair* trace{};
    } outputs[2];
    UINT width{},height{},work_width{},work_height{}; UINT64 own_sequence{},host_sequence{}; HANDLE event{};
    k2vr::ipc::StereoFrameMetadata previous{};
    NeuralPairTiming processed_timing{},displayed_timing{};
    int recorded_input{-1},display_index{-1}; bool display_recorded{},transport_only{},prefetch_enabled{},retain_source_depth{};
    bool serial_eyes{};
    PrefetchAgePolicy prefetch_age; // Independent of optional trace storage/limits.
    std::uint64_t replaced_inputs{};
    std::uint64_t last_input_ready{};
    std::filesystem::path workers;
    NeuralWorkerClient eyes[2];
    std::unique_ptr<NeuralTimingTrace> trace;
    std::thread trace_writer;
    bool trace_export_attempted{},trace_export_started{},own_submission_attempted{};
    // Called by the owning destructor before deciding whether Impl may die.
    // The host must have stopped calling Record/Submit before destroying us.
    bool StopAndRetire() noexcept {
        stopping.store(true); wake.notify_all(); if (thread.joinable()) thread.join();
        // No CPU writer may remain active even when GPU ownership is quarantined.
        if (trace_writer.joinable()) trace_writer.join();
        if (trace && !trace_export_started) {
            trace_export_started=true;
            try { trace->Write(width,height,work_width,work_height,prefetch_enabled,phase.load()==Failed); }
            catch (...) { OutputDebugStringA("KOTOR2VR neural timing: shutdown export failed\n"); }
        }
        const auto removed=[&] { return device && FAILED(device->GetDeviceRemovedReason()); };
        if (removed()) return true;
        const ULONGLONG deadline=GetTickCount64()+5000;
        UINT64 final_own=0;
        if (queue && done) {
            if (own_sequence>=UINT64_MAX-1) return removed();
            final_own=++own_sequence;
            // Includes guide/evaluation/composition work, even if an earlier
            // Execute succeeded but its Signal or completion wait failed.
            if (FAILED(queue->Signal(done.Get(),final_own))) return removed();
        } else if (own_submission_attempted) return removed();
        // Poll values rather than reusing an event with an older outstanding
        // registration. Both queues share this one bounded retirement budget.
        for (;;) {
            if (removed()) return true;
            const auto own=final_own ? done->GetCompletedValue():0;
            const auto host=host_sequence && host_submitted ? host_submitted->GetCompletedValue():0;
            if (own==UINT64_MAX || host==UINT64_MAX) return removed();
            if ((!final_own || own>=final_own) &&
                (!host_sequence || (host_submitted && host>=host_sequence))) return true;
            const auto now=GetTickCount64();
            if (now>=deadline) return removed();
            Sleep(static_cast<DWORD>(deadline-now<5 ? deadline-now:5));
        }
    }
    ~Impl() {
        // StopAndRetire proved both queues retired (or device removal).
        // On uncertainty the owner releases the entire Impl instead.
        if (event) CloseHandle(event);
    }
    void TryExportTrace() noexcept {
        // Called ONLY by SubmitRecordedInput under mutex, on the host thread.
        // Each captured row is either never worked (replaced/age-dropped) or fully published
        // and first displayed/submitted or discarded. No worker can still be
        // writing its stamps. Host counters/display reuse and worker discard
        // flags are also safe here. The immutable COPY outlives slot reuse and
        // owns no D3D resource. No GPU fence, worker stop/join or file I/O here.
        if (!trace || trace_export_attempted || !trace->ReadyForSnapshot()) return;
        trace_export_attempted=true;
        try {
            auto snapshot=std::make_unique<NeuralTimingTrace>(*trace);
            snapshot->snapshot_qpc=TraceQpc();
            trace_writer=std::thread([snapshot=std::move(snapshot),w=width,h=height,
                ww=work_width,wh=work_height,prefetch=prefetch_enabled,failed=phase.load()==Failed] {
                snapshot->Write(w,h,ww,wh,prefetch,failed);
            });
            trace_export_started=true;
        } catch (...) {
            OutputDebugStringA("KOTOR2VR neural timing: automatic export unavailable; shutdown fallback retained\n");
        }
    }
    void Fail(const std::string& error) {
        std::lock_guard lock(mutex);
        if (phase.load()!=Failed) { failure=error; phase.store(Failed,std::memory_order_release); }
        wake.notify_all();
    }
    bool Ended() const { return stopping.load() || phase.load()==Failed; }
    ComPtr<ID3D12Resource> Texture(UINT w,UINT h) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=w; desc.Height=h; desc.DepthOrArraySize=desc.MipLevels=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1;
        ComPtr<ID3D12Resource> texture;
        Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&texture)),"neural atlas"); return texture;
    }
    void Begin() { Check(allocator->Reset(),"neural allocator"); Check(commands->Reset(allocator.Get(),nullptr),"neural commands"); }
    void Execute(NeuralTracePair* pair=nullptr,bool compose=false) {
        Check(commands->Close(),"neural list close"); ID3D12CommandList* lists[]={commands.Get()};
        if (pair) (compose ? pair->compose_submit_qpc:pair->guide_submit_qpc)=TraceQpc();
        own_submission_attempted=true;
        queue->ExecuteCommandLists(1,lists);
        Check(queue->Signal(done.Get(),++own_sequence),"neural own signal");
        Check(done->SetEventOnCompletion(own_sequence,event),"neural own completion");
        if (pair) (compose ? pair->compose_wait_begin_qpc:pair->guide_wait_begin_qpc)=TraceQpc();
        if (WaitForSingleObject(event,5000)!=WAIT_OBJECT_0) throw std::runtime_error("neural GPU timeout");
        if (pair) (compose ? pair->compose_end_qpc:pair->guide_end_qpc)=TraceQpc();
    }
    void Run() noexcept {
        try {
            D3D12_COMMAND_QUEUE_DESC qdesc{}; qdesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
            const bool request_high=ReadQueueHighOption();
            qdesc.Priority=static_cast<INT>(request_high ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
            D3D12_COMMAND_QUEUE_PRIORITY effective_priority=static_cast<D3D12_COMMAND_QUEUE_PRIORITY>(qdesc.Priority);
            const char* queue_reason=request_high ? "high-requested" : "default-normal";
            HRESULT fallback_hresult=S_OK;
            if (trace) {
                trace->requested_queue_priority=static_cast<D3D12_COMMAND_QUEUE_PRIORITY>(qdesc.Priority);
                trace->effective_queue_priority=static_cast<D3D12_COMMAND_QUEUE_PRIORITY>(qdesc.Priority);
                trace->queue_priority_reason=queue_reason;
            }
            if (request_high) {
                D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY feature{};
                feature.CommandListType=D3D12_COMMAND_LIST_TYPE_DIRECT;
                feature.Priority=D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
                const HRESULT feature_result=device->CheckFeatureSupport(
                    D3D12_FEATURE_COMMAND_QUEUE_PRIORITY,&feature,sizeof(feature));
                if (FAILED(feature_result) || !feature.PriorityForTypeIsSupported) {
                    qdesc.Priority=static_cast<INT>(D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
                    effective_priority=D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
                    queue_reason=FAILED(feature_result) ? "feature-check-failed" : "high-unsupported";
                    fallback_hresult=feature_result;
                }
            }
            HRESULT queue_result=device->CreateCommandQueue(&qdesc,IID_PPV_ARGS(&queue));
            if (FAILED(queue_result) && static_cast<D3D12_COMMAND_QUEUE_PRIORITY>(qdesc.Priority)==D3D12_COMMAND_QUEUE_PRIORITY_HIGH) {
                qdesc.Priority=static_cast<INT>(D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
                effective_priority=D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
                queue_reason="high-create-failed-fallback";
                fallback_hresult=queue_result;
                queue.Reset();
                queue_result=device->CreateCommandQueue(&qdesc,IID_PPV_ARGS(&queue));
            }
            if (FAILED(queue_result)) {
                LogQueuePriorityDecision(request_high ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH
                    : static_cast<D3D12_COMMAND_QUEUE_PRIORITY>(qdesc.Priority),
                    effective_priority,queue_reason,queue_result);
            }
            Check(queue_result,"neural queue");
            if (trace) {
                trace->effective_queue_priority=effective_priority;
                trace->queue_priority_reason=queue_reason;
            }
            LogQueuePriorityDecision(request_high ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH
                : static_cast<D3D12_COMMAND_QUEUE_PRIORITY>(qdesc.Priority),
                effective_priority,queue_reason,fallback_hresult);
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"neural allocator creation");
            Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"neural list creation");
            Check(commands->Close(),"neural initial close"); Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)),"neural fence");
            Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&host_submitted)),"neural host fence");
            event=CreateEventW(nullptr,FALSE,FALSE,nullptr); if (!event) throw std::runtime_error("neural event");
            for (unsigned i=0;i<(prefetch_enabled ? 2U:1U);++i) {
                auto& input=inputs[i];
                input.texture=Texture(width*2,k2vr::ipc::StereoAtlasHeight(height));
                if (!input.guides.Initialize(device.Get(),input.texture.Get(),width,height,work_width,work_height,
                    work_width!=width || work_height!=height))
                    throw std::runtime_error("neural guide initialization");
            }
            const auto output_height=retain_source_depth ? k2vr::ipc::StereoAtlasHeight(height):k2vr::ipc::StereoDepthOffset(height);
            for (auto& output:outputs) output.texture=Texture(width*2,output_height);
            for (unsigned eye=0;eye<2;++eye) {
                if (stopping.load()) return;
                if (!eyes[eye].Start(device.Get(),workers/(eye ? "right":"left")/"kotor2vr-neural-worker.exe",work_width,work_height,transport_only,width,height))
                    throw std::runtime_error(eyes[eye].error());
            }
            phase.store(Running,std::memory_order_release);
            while (!Ended()) {
                const auto idle_begin=Clock::now();
                const auto idle_qpc=trace ? TraceQpc():0;
                Input* input=nullptr;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock,[&]{
                        for (auto& candidate:inputs) if (candidate.state==InputState::Queued) {
                            if (prefetch_age.enabled() && !prefetch_age.TryBegin(candidate.admitted_at,Clock::now(),candidate.reset_reasons,candidate.reset)) {
                                // No worker read was submitted for this slot. Its
                                // old and future input copies share the serialized
                                // host queue; keep the texture and fences alive.
                                if (trace) {
                                    ++trace->age_dropped_inputs; // Includes drops beyond captured rows.
                                    if (candidate.trace) candidate.trace->discarded_age=true;
                                }
                                candidate.state=InputState::Empty;
                                continue;
                            }
                            candidate.reset=candidate.reset || candidate.reset_reasons!=0;
                            input=&candidate; return true;
                        }
                        return Ended();
                    });
                    if (Ended()) break;
                    input->state=InputState::Working;
                }
                const auto begin=Clock::now();
                auto* pair=input->trace;
                if (pair) { pair->idle_begin_qpc=idle_qpc; pair->work_start_qpc=TraceQpc(); }
                const auto frame=input->frame;
                Check(queue->Wait(host_submitted.Get(),input->ready),"neural input wait");
                if (pair) pair->input_wait_enqueued_qpc=TraceQpc();
                bool reset=previous.ready_value==0 || frame.request.history_reset_reasons!=0 || frame.request.header.generation!=previous.request.header.generation ||
                    frame.request.presentation_state!=previous.request.presentation_state ||
                    frame.request.predicted_display_time_ns-previous.request.predicted_display_time_ns>250000000 || input->reset;
                if (!reset) for (unsigned eye=0;eye<2;++eye) {
                    float translation_change=0;
                    for (unsigned i=12;i<15;++i) translation_change+=std::abs(frame.view_projection[eye].value[i]-previous.view_projection[eye].value[i]);
                    if (translation_change>10.F) reset=true; // conservative camera-cut protection
                }
                if (pair) pair->reset=reset;
                Begin();
                for (unsigned eye=0;eye<2;++eye) {
                    const bool scale=work_width!=width || work_height!=height;
                    if (!scale) CopyRect(commands.Get(),input->texture.Get(),eyes[eye].color(),eye*width,0,width,height);
                    if (!input->guides.Record(commands.Get(),input->texture.Get(),eye,frame.view_projection[eye],
                        reset ? frame.view_projection[eye]:previous.view_projection[eye],reset,eyes[eye].depth(),eyes[eye].motion(),scale ? eyes[eye].color():nullptr))
                        throw std::runtime_error("neural guide recording");
                }
                Execute(pair);
                const auto guided=std::chrono::steady_clock::now();
                const auto submit_eye=[&](unsigned i) {
                    if (pair) pair->eye_submit_begin_qpc[i]=TraceQpc();
                    if (!eyes[i].Submit(queue.Get(),reset)) throw std::runtime_error(eyes[i].error());
                    if (pair) {
                        pair->eye_submit_end_qpc[i]=TraceQpc();
                        pair->eye_sequence[i]=eyes[i].sequence(); pair->eye_pid[i]=eyes[i].pid();
                    }
                };
                ULONGLONG deadline{};
                const auto wait_eye_mask=[&](unsigned mask) {
                // One shared five-second budget, starting after the initial
                // submit batch as before; serial mode does not double it.
                if (!deadline) deadline=GetTickCount64()+5000;
                bool complete[2]{(mask&1U)==0,(mask&2U)==0};
                while (!complete[0] || !complete[1]) {
                    for (unsigned i=0;i<2;++i) if (!complete[i]) {
                        const auto status=eyes[i].Poll();
                        if (status==NeuralPoll::Failed) throw std::runtime_error(eyes[i].error());
                        complete[i]=status==NeuralPoll::Complete;
                        if (pair && complete[i]) pair->eye_complete_observed_qpc[i]=TraceQpc();
                    }
                    if (GetTickCount64()>=deadline) throw std::runtime_error("neural pair output timeout");
                    if (!complete[0] || !complete[1]) {
                        const auto wait_begin=pair ? TraceQpc():0;
#if defined(KOTOR2VR_BENCHMARK_POLLING)
                        Sleep(1); // Reference executable only; never the production host.
#else
                        HANDLE events[2]{}; DWORD count{};
                        for (unsigned i=0;i<2;++i) if (mask&(1U<<i)) events[count++]=eyes[i].completion_event();
                        if (WaitForMultipleObjects(count,events,TRUE,100)==WAIT_FAILED) throw std::runtime_error("neural output event wait");
#endif
                        if (pair) { pair->eye_wait_ticks+=TraceQpc()-wait_begin; ++pair->eye_wait_calls; }
                    }
                }
                };
                RunEyeSchedule(serial_eyes,submit_eye,wait_eye_mask);
                for (auto& eye:eyes) if (!eye.QueueWaitCompletedOutput(queue.Get())) throw std::runtime_error("neural completed fence wait");
                const auto evaluated=Clock::now();
                if (pair) pair->eyes_complete_qpc=TraceQpc();
                Output* output=nullptr;
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock,[&]{
                        for (auto& candidate:outputs) if (candidate.state==OutputState::Empty) { output=&candidate; return true; }
                        // A mailbox never read by XR can be superseded as a
                        // whole pair. Displaying/Retiring slots are off limits.
                        for (auto& candidate:outputs) if (candidate.state==OutputState::Complete) { output=&candidate; return true; }
                        return Ended();
                    });
                    if (Ended()) break;
                    if (output->state==OutputState::Complete && output->trace) output->trace->superseded_output=true;
                    output->state=OutputState::Working;
                }
                const auto compose_begin=Clock::now();
                if (pair) pair->compose_begin_qpc=TraceQpc();
                // Retired is published only AFTER its signal is enqueued on
                // the host queue. XR never waits on us: no GPU wait cycle.
                if (output->retired) Check(queue->Wait(host_submitted.Get(),output->retired),"neural display retire wait");
                // HUD and optional original packed depth are contiguous in the
                // input atlas. Copy them from the SAME owned input before the
                // output's composition fence/metadata publication.
                Begin(); CopyRect(commands.Get(),input->texture.Get(),output->texture.Get(),0,height,width*2,
                    k2vr::ipc::kStereoHudMaximumHeight+(retain_source_depth ? height:0U),0,height);
                for (unsigned eye=0;eye<2;++eye) CopyRect(commands.Get(),eyes[eye].output(),output->texture.Get(),0,0,width,height,eye*width);
                Execute(pair,true); previous=frame;
                ++processed_timing.pair_count;
                if (pair) pair->processed_pair=processed_timing.pair_count;
                processed_timing.guide_ms=std::chrono::duration<double,std::milli>(guided-begin).count();
                processed_timing.worker_ms=std::chrono::duration<double,std::milli>(evaluated-guided).count();
                processed_timing.compose_ms=std::chrono::duration<double,std::milli>(Clock::now()-compose_begin).count();
                processed_timing.idle_ms=std::chrono::duration<double,std::milli>(begin-idle_begin).count();
                processed_timing.input_queue_ms=std::chrono::duration<double,std::milli>(begin-input->submitted_at).count();
                processed_timing.output_wait_ms=std::chrono::duration<double,std::milli>(compose_begin-evaluated).count();
                if (input->prefetched) ++processed_timing.prefetched_pair_count;
                {
                    std::lock_guard lock(mutex);
                    processed_timing.replaced_input_count=replaced_inputs;
                    output->frame=frame; output->timing=processed_timing; output->completed_at=GetTickCount64();
                    output->trace=pair;
                    output->state=OutputState::Complete;
                    if (pair) pair->work_published=true;
                    input->state=InputState::Empty;
                }
                // Immediately evaluate the newest prefetched input, independent
                // of when XR next consumes the completed output mailbox.
            }
        } catch (const std::exception& error) {
            Fail(error.what());
        }
        // Keep eye handles with the atlases until the owner proves retirement.
    }
};
NeuralStereoPipeline::NeuralStereoPipeline()=default;
NeuralStereoPipeline::~NeuralStereoPipeline() {
    if (impl_ && !impl_->StopAndRetire()) {
        OutputDebugStringA("KOTOR2VR neural pipeline: retirement unproven; whole pipeline quarantined until process exit\n");
        (void)impl_.release();
    }
}
bool NeuralStereoPipeline::Start(ID3D12Device* device,UINT width,UINT height,const std::filesystem::path& workers,UINT work_percent,bool transport_only,bool retain_source_depth) {
    if (impl_ || !device || !width || !height || width>4096 || height>4096 || work_percent<50 || work_percent>100 || (transport_only && work_percent!=100)) return false;
    impl_=std::make_unique<Impl>(); impl_->device=device; impl_->width=width; impl_->height=height; impl_->workers=workers;
    impl_->transport_only=transport_only;
    impl_->retain_source_depth=retain_source_depth;
    impl_->serial_eyes=ReadSerialEyesOption();
    wchar_t prefetch[2]{};
    impl_->prefetch_enabled=GetEnvironmentVariableW(L"KOTOR2VR_NEURAL_PREFETCH",prefetch,2)==1 && prefetch[0]==L'1';
    if (impl_->prefetch_enabled) {
        wchar_t max_age[64]{};
        const DWORD length=GetEnvironmentVariableW(L"KOTOR2VR_NEURAL_PREFETCH_MAX_AGE_MS",max_age,64);
        if (length>0 && length<64)
            impl_->prefetch_age.max_age_ms=ParsePrefetchMaxAgeMs(impl_->prefetch_enabled,max_age);
    }
    impl_->work_width=(width*work_percent+50)/100; impl_->work_height=(height*work_percent+50)/100;
    impl_->trace=NeuralTimingTrace::Create();
    if (impl_->trace) impl_->trace->serial_eyes=impl_->serial_eyes;
    if (impl_->trace) impl_->trace->direct_backend=GetEnvironmentVariableW(L"KOTOR2VR_NEURAL_DIRECT_ROOT",nullptr,0)>1;
    if (impl_->trace) impl_->trace->prefetch_max_age_ms=impl_->prefetch_age.max_age_ms;
    impl_->thread=std::thread([state=impl_.get()]{state->Run();}); return true;
}
bool NeuralStereoPipeline::RecordInput(ID3D12GraphicsCommandList* list,ID3D12Resource* source,const k2vr::ipc::StereoFrameMetadata& frame,std::int64_t admission_predicted_display_time_ns) {
    auto* trace=impl_ ? impl_->trace.get():nullptr;
    const auto request_qpc=trace ? TraceQpc():0;
    if (trace) ++trace->request_calls;
    if (!impl_ || impl_->phase.load(std::memory_order_acquire)!=Impl::Running || impl_->recorded_input>=0 || frame.guide_mask!=3 || frame.eyes_complete!=3 ||
        !k2vr::ipc::ValidStereoHud(frame) ||
        !k2vr::ipc::ValidStereoGuides(frame) || !k2vr::ipc::ValidStereoRequest(frame.request) || frame.ready_value<=impl_->last_input_ready ||
        frame.request.render_width!=impl_->width || frame.request.render_height!=impl_->height || !source || !list) return false;
    const auto desc=source->GetDesc();
    if (desc.Width!=impl_->width*2 || desc.Height!=k2vr::ipc::StereoAtlasHeight(impl_->height) || desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM) return false;
    std::lock_guard lock(impl_->mutex);
    // Default preserves the lower-latency XR-paced admission policy. In the
    // isolated 72-Hz test, continuous prefetch raised throughput only ~6% but
    // increased frame age. Opt in explicitly for controlled live comparisons.
    if (!impl_->prefetch_enabled) {
        for (const auto& input:impl_->inputs) if (input.state!=Impl::InputState::Empty) return false;
        for (const auto& output:impl_->outputs) if (output.state==Impl::OutputState::Complete) return false;
    }
    int index=-1;
    // Replace only a queued input whose GPU read has not begun. Recording
    // reserves that slot until SubmitRecordedInput publishes its new fence.
    for (int i=0;i<2;++i) if (impl_->inputs[i].state==Impl::InputState::Queued) { index=i; break; }
    if (index<0) for (int i=0;i<(impl_->prefetch_enabled ? 2:1);++i) if (impl_->inputs[i].state==Impl::InputState::Empty) { index=i; break; }
    if (index<0) return false;
    auto& input=impl_->inputs[index];
    const bool replaced=input.state==Impl::InputState::Queued;
    if (replaced && input.trace) input.trace->replaced_input=true;
    input.reset_reasons=PrefetchAgePolicy::AdmissionResetReasons(replaced,input.reset_reasons,frame.request.history_reset_reasons);
    input.reset=(replaced && input.reset) || input.reset_reasons!=0;
    if (replaced) ++impl_->replaced_inputs;
    input.prefetched=false;
    for (const auto& candidate:impl_->inputs) if (candidate.state==Impl::InputState::Working) input.prefetched=true;
    input.state=Impl::InputState::Recording; input.frame=frame; impl_->recorded_input=index;
    // Admission-to-start age includes delayed host submission, even without tracing.
    if (impl_->prefetch_age.enabled()) input.admitted_at=Impl::Clock::now();
    input.trace=trace ? trace->Admit(frame,request_qpc,input.prefetched,admission_predicted_display_time_ns):nullptr;
    CopyRect(list,source,input.texture.Get(),0,0,impl_->width*2,k2vr::ipc::StereoAtlasHeight(impl_->height));
    return true;
}
void NeuralStereoPipeline::SubmitRecordedInput(ID3D12CommandQueue* queue) {
    if (!impl_ || impl_->phase.load(std::memory_order_acquire)==Impl::Starting || !queue || !impl_->host_submitted) return;
    const HRESULT result=queue->Signal(impl_->host_submitted.Get(),++impl_->host_sequence);
    if (FAILED(result)) { impl_->Fail("neural host submission signal HRESULT="+std::to_string(result)); return; }
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->recorded_input>=0) {
            auto& input=impl_->inputs[impl_->recorded_input];
            input.ready=impl_->host_sequence; input.submitted_at=Impl::Clock::now();
            if (input.trace) { input.trace->submitted_qpc=TraceQpc(); input.trace->host_fence=input.ready; }
            input.state=Impl::InputState::Queued;
            impl_->last_input_ready=input.frame.ready_value; impl_->recorded_input=-1;
        }
        for (auto& output:impl_->outputs) if (output.state==Impl::OutputState::Retiring) {
            output.retired=impl_->host_sequence; output.state=Impl::OutputState::Empty;
        }
        if (impl_->display_recorded && impl_->display_index>=0) {
            auto* pair=impl_->outputs[impl_->display_index].trace;
            if (pair && pair->display_accept_qpc && !pair->display_submit_qpc) {
                pair->display_submit_qpc=TraceQpc(); pair->display_host_fence=impl_->host_sequence;
            }
        }
        impl_->display_recorded=false;
        impl_->TryExportTrace();
    }
    impl_->wake.notify_one();
}
bool NeuralStereoPipeline::RecordDisplay(ID3D12GraphicsCommandList* list,ID3D12Resource* destination,k2vr::ipc::StereoFrameMetadata& frame,bool copy_to_destination,std::int64_t display_predicted_display_time_ns) {
    if (impl_ && impl_->trace) ++impl_->trace->display_calls;
    if (!impl_ || !list || (copy_to_destination && !destination) || impl_->display_recorded) return false;
    const auto phase=impl_->phase.load(std::memory_order_acquire);
    if (phase==Impl::Failed || phase==Impl::Starting) return false;
    if (copy_to_destination) {
        const auto desc=destination->GetDesc();
        if (desc.Width<impl_->width*2 || desc.Height<k2vr::ipc::StereoDepthOffset(impl_->height) ||
            (desc.Format!=DXGI_FORMAT_R8G8B8A8_TYPELESS && desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) return false;
    }
    std::lock_guard lock(impl_->mutex);
    int complete=-1;
    for (int i=0;i<2;++i) if (impl_->outputs[i].state==Impl::OutputState::Complete &&
        (complete<0 || impl_->outputs[i].frame.ready_value>impl_->outputs[complete].frame.ready_value)) complete=i;
    if (complete>=0) {
        for (int i=0;i<2;++i) if (i!=complete && impl_->outputs[i].state==Impl::OutputState::Complete) {
            if (impl_->outputs[i].trace) impl_->outputs[i].trace->superseded_output=true;
            impl_->outputs[i].state=Impl::OutputState::Empty;
        }
        if (impl_->display_index>=0) impl_->outputs[impl_->display_index].state=Impl::OutputState::Retiring;
        impl_->display_index=complete; impl_->outputs[complete].state=Impl::OutputState::Displaying;
    }
    if (impl_->display_index<0) return false;
    const auto& display=impl_->outputs[impl_->display_index];
    if (GetTickCount64()-display.completed_at>500) return false;
    if (copy_to_destination) CopyRect(list,display.texture.Get(),destination,0,0,impl_->width*2,k2vr::ipc::StereoDepthOffset(impl_->height));
    if (display.trace) TraceDisplay(*display.trace,display_predicted_display_time_ns);
    frame=display.frame; impl_->displayed_timing=display.timing; impl_->display_recorded=true; return true;
}
ID3D12Resource* NeuralStereoPipeline::displayed_texture() const noexcept {
    return impl_ && impl_->display_recorded && impl_->display_index>=0 ? impl_->outputs[impl_->display_index].texture.Get():nullptr;
}
bool NeuralStereoPipeline::failed() const noexcept { return impl_ && impl_->phase.load(std::memory_order_acquire)==Impl::Failed; }
NeuralPairTiming NeuralStereoPipeline::timing() const noexcept { return impl_ ? impl_->displayed_timing:NeuralPairTiming{}; }
std::string NeuralStereoPipeline::error() const { return failed() ? impl_->failure:std::string{}; }
}
