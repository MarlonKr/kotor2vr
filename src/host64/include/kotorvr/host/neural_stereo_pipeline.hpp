#pragma once
#include "stereo_stream.hpp"
#include <filesystem>
#include <memory>
#include <string>
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
namespace kotorvr::host {
struct NeuralPairTiming {
    std::uint64_t pair_count{};
    double guide_ms{},worker_ms{},compose_ms{};
    // CPU wall-clock spans, not isolated GPU timestamps. Input queue age is
    // measured from its XR submission; idle/output waits expose mailbox stalls.
    double input_queue_ms{},idle_ms{},output_wait_ms{};
    std::uint64_t prefetched_pair_count{},replaced_input_count{};
};
// Default input admission is XR-paced for low latency. Explicit environment
// KOTOR2VR_NEURAL_PREFETCH=1 at Start enables two bounded input slots (working +
// latest prefetched complete frame). Both modes have two output slots (display
// + whole-pair mailbox); XR never waits for neural work. A completed pair
// retains its original pose and matching HUD. Prefetch trades frame age for
// throughput and requires a controlled live comparison before enabling it.
// RecordInput/RecordDisplay/SubmitRecordedInput must be serialized on one host
// thread/queue. SubmitRecordedInput MUST follow each submitted command batch,
// even when RecordInput returned false: it also retires prior display reads.
//
// Optional scheduling experiment, read once at Start:
// KOTOR2VR_NEURAL_SERIAL_EYES=1 submits left, waits for its matching ACK AND
// completed output fence, then submits/waits for right. Unset or any other
// value retains concurrent left+right submission and the existing wait-for-both.
// Both workers and their independent histories remain alive. Reset decisions,
// input/output ownership and whole-pair publication are unchanged. The shared
// output timeout remains five seconds; this is not a quality/pacing default.
// Compare real whole-pair throughput/age before drawing a contention conclusion.
//
// Optional CPU timing trace (read once at Start):
//   KOTOR2VR_NEURAL_TIMING_TRACE=<absolute JSONL filename>
// or =1, with optional KOTOR2VR_NEURAL_TIMING_TRACE_PATH=<absolute filename>.
// =1 alone chooses %TEMP%/kotor2vr-neural-timing-<pid>-<start-qpc>.jsonl.
// Unset/empty/0 disables it. The parent directory must already exist. Trace
// failures are diagnostic only. At most 360 ADMITTED input pairs per Start
// occupy fixed CPU storage; replaced/unshown/unfinished pairs remain in it.
// Once the limit is reached and all rows have settled, SubmitRecordedInput
// copies the bounded trace under the ownership mutex and starts a separate
// writer thread that appends JSONL. If no live export started, destruction
// exports after joining the worker. Terminate/crash can still lose the trace.
// Trace instrumentation adds no file writes on the frame thread, query heaps,
// GPU queries, additional fence waits or scheduling changes. A session row
// supplies qpc_frequency, dimensions, total call/admission counts and schema.
// Session serial_eyes records the selected scheduling mode. Eye submit and
// completion stamps remain actual observations; serial mode must have left
// completion <= right submit. They are still CPU observations, not GPU timings.
// Pair rows join on (pid,trace_qpc,pair); source ready_value/xr_frame_id/
// camera_frame_id/generation correlate with the render trace. Each eye carries
// the actual worker_pid and worker_sequence for its IPC/GPU trace. XR frame_id
// is the SOURCE request, not the XR frame which eventually displays the pair.
//
// Measurement model: every *_qpc is an absolute CPU QueryPerformanceCounter
// tick. Zero means unobserved, never "instantaneous". Convert two nonzero
// stamps via (end-start)*1000/qpc_frequency; predicted_display_time_ns is an
// XR source timestamp, NOT a QPC value. admission_predicted_display_time_ns
// records the current XR predicted display time at admission; 0 is unavailable
// (including legacy harnesses). With both XR stamps nonzero, source age in ns
// is admission_predicted_display_time_ns - predicted_display_time_ns. Preserve
// negative ages for future sources; never subtract QPC from an XR timestamp.
// request_qpc is this RecordInput call's entry
// (only admitted calls get rows), not the original XR request publication.
// admission_qpc reserves the input slot; submitted_qpc follows the host fence
// Signal enqueue. WorkStart follows worker admission; input_wait_enqueued is
// a queue Wait ENQUEUE, not input-ready time. Guide/Compose submit stamps precede
// ExecuteCommandLists; *_wait_begin/end bracket EXISTING CPU fence waits.
// These wall spans include queue backlog and thread wake latency, NOT GPU time.
// Eye completion is first Poll()==Complete (ACK plus output fence), NOT the
// exact GPU completion: default wait-for-both may bunch observations; serial
// mode waits/polls only the submitted eye before proceeding to the next one.
// eye_wait_ticks/calls count existing event waits (Sleep in benchmark builds).
// Idle=work_start-idle_begin; input queue=work_start-submitted; output-slot
// wait=compose_begin-eyes_complete. First successful RecordDisplay sets
// display_accept, including external reads; first subsequent host Signal sets
// display_submit. Neither means xrEndFrame/scanout, nor GPU display completion.
// display_predicted_display_time_ns records the FIRST successful selection
// XR timestamp, including external reads. Zero is unavailable/legacy and stays
// zero on reuse. Subtract the source predicted_display_time_ns only when both
// are nonzero to measure signed XR source age at selection, never scanout age.
// display_reads counts successful selections including reuse. Superseded/replaced
// flags explain missing later stamps. Existing NeuralPairTiming is unchanged.
class NeuralStereoPipeline final {
public:
    NeuralStereoPipeline();
    ~NeuralStereoPipeline();
    // transport_only is solely for byte-exact offline ownership diagnostics.
    // retain_source_depth keeps each pair's original packed depths below its
    // matching HUD, at StereoDepthOffset(height); default memory use is unchanged.
    bool Start(ID3D12Device*,std::uint32_t width,std::uint32_t height,const std::filesystem::path& workers,std::uint32_t work_percent=100,bool transport_only=false,bool retain_source_depth=false);
    // Current XR predicted display timestamp is trace-only; 0 means unavailable.
    bool RecordInput(ID3D12GraphicsCommandList*,ID3D12Resource*,const k2vr::ipc::StereoFrameMetadata&,std::int64_t admission_predicted_display_time_ns=0);
    void SubmitRecordedInput(ID3D12CommandQueue*);
    // With copy_to_destination=false, destination may be null; selection still
    // reserves the whole output for external reads in this same command batch.
    // The default copy includes only color+HUD, even when depth is retained.
    bool RecordDisplay(ID3D12GraphicsCommandList*,ID3D12Resource*,k2vr::ipc::StereoFrameMetadata&,bool copy_to_destination=true,std::int64_t display_predicted_display_time_ns=0);
    // Borrowed, host-thread-only texture after successful RecordDisplay. Record
    // reads before the next RecordDisplay/SubmitRecordedInput, on the same host
    // queue, and restore COMMON. Null outside a recorded display batch. Do not
    // retain this pointer for later submissions; GPU retirement is fence-owned.
    ID3D12Resource* displayed_texture() const noexcept;
    bool failed() const noexcept;
    // Last pair accepted by RecordDisplay; read only on its calling thread.
    NeuralPairTiming timing() const noexcept;
    std::string error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
