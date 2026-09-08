#pragma once

// Optional scene diagnostics; header-only so the standalone scene-replay target
// shares the same implementation without another build-system dependency.
// Game opt-in: [camera] gl_gpu_timing=1 in the DLL-local camera INI. The
// explicit loader value (including default false) overrides the environment.
// Unconfigured standalone callers can use KOTOR2VR_GL_GPU_TIMING=1 (exactly),
// but still must establish the deletion-hook lifecycle contract before arming.
// GPU timestamps bracket queued scene commands, NOT pure GPU busy time. They
// include intervening scheduling/preemption/CPU submission gaps. Camera setup,
// eye capture/depth packing, HUD and SwapBuffers outside the scene are excluded.
// No TIME_ELAPSED queries, waits, flushes, finishes, GL error consumption or
// render-state changes. Availability checks bound work, not driver call latency.
// Aggregates use the existing synchronous probe logger every five seconds; this
// is nonblocking GPU polling, not a guarantee of nonblocking file/driver calls.
// No final drain on hook stop. Logs are cumulative; invalid includes aborted
// passes and backwards timestamps. Global counters repeat in each pass line.
// Compare interval deltas of total_ms/samples/issued/drops. Unavailable samples
// can lag a log window; nested scene spans, if any, must not be summed as busy
// time. Abnormal cleanup replays outside RenderSceneWithReplay are not sampled.
//
// One context/thread for the process lifetime, 32 pairs (64 query names).
// Any successful context-deletion notification or owner change disables timing
// permanently. Pending data is abandoned. No GL calls from teardown/destructors:
// names live until their context/process is destroyed (bounded diagnostic cost).
// The game DLL is process-pinned by probe initialization. Notification must be
// forwarded even when deletion happens on a thread other than the render owner.
// Begin stays inert until the validated game's deletion IAT hook is installed.
// This relies on the existing engine context-deletion path, not arbitrary third
// party context replacement bypassing that hook.
// No live GPU validation has been performed. CPU invariants below are constexpr
// assertions for the parent's compiler, not a claim of executed GPU tests.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include "probe.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <gl/GL.h>

namespace k2vr::game32::gpu_timing {
enum class Pass : unsigned { BaseScene, StereoCapture, StereoReplay, MonitorReplay, Count };
constexpr unsigned capacity=32;
constexpr unsigned invalid=capacity;
enum class Phase : unsigned char { Free, Open, Pending };
constexpr bool MayRead(Phase phase,bool begin_available,bool end_available) noexcept {
    return phase==Phase::Pending && begin_available && end_available;
}
constexpr bool ValidSpan(std::uint64_t begin,std::uint64_t end) noexcept {
    return end>=begin; // Require 64-bit timestamp counters; reject wrap/reset.
}
constexpr bool EnabledValue(const char* value,unsigned size) noexcept {
    return size==1 && value[0]=='1';
}
// -1 means no explicit configuration; 0/1 are authoritative OFF/ON.
constexpr bool EffectiveEnabled(int configuration,bool environment) noexcept {
    return configuration<0 ? environment:configuration==1;
}
static_assert(!EffectiveEnabled(0,true) && !EffectiveEnabled(0,false));
static_assert(EffectiveEnabled(1,false) && EffectiveEnabled(1,true));
static_assert(EffectiveEnabled(-1,true) && !EffectiveEnabled(-1,false));
constexpr bool SupportedVersion(const char* value) noexcept {
    return value && value[0]>='3' && value[0]<='9' && value[1]=='.' &&
        value[2]>='0' && value[2]<='9' && (value[0]>'3' || value[2]>='3');
}
constexpr bool SupportsLegacyListCheck(int profile,int flags) noexcept {
    // Accept the observed legacy zero mask as well as explicit compatibility.
    // Core, unknown masks and forward-compatible contexts cannot safely use
    // the GL_LIST_INDEX guard. Zero is not a general proof of compatibility:
    // this exception is for KOTOR's already-running display-list renderer.
    return (profile==0 || profile==2) && (flags&1)==0;
}
static_assert(SupportsLegacyListCheck(0,0) && SupportsLegacyListCheck(2,0));
static_assert(!SupportsLegacyListCheck(1,0) && !SupportsLegacyListCheck(3,0) &&
    !SupportsLegacyListCheck(4,0) && !SupportsLegacyListCheck(-1,0));
static_assert(!SupportsLegacyListCheck(0,1) && !SupportsLegacyListCheck(2,1));
static_assert(SupportsLegacyListCheck(0,2) && SupportsLegacyListCheck(2,2)); // debug flag is OK
static_assert(MayRead(Phase::Pending,true,true));
static_assert(!MayRead(Phase::Open,true,true) && !MayRead(Phase::Free,true,true));
static_assert(!MayRead(Phase::Pending,false,true) && !MayRead(Phase::Pending,true,false));
static_assert(ValidSpan(9,9) && ValidSpan(9,10) && !ValidSpan(10,9));
static_assert(EnabledValue("1",1) && !EnabledValue("0",1) && !EnabledValue("10",2));
static_assert(SupportedVersion("3.3 vendor") && SupportedVersion("4.6 vendor"));
static_assert(!SupportedVersion("3.2") && !SupportedVersion("OpenGL ES 3.3") &&
    !SupportedVersion(nullptr) && !SupportedVersion("") && !SupportedVersion("3."));

namespace detail {
using GenQueries=void(APIENTRY*)(GLsizei,GLuint*);
using Counter=void(APIENTRY*)(GLuint,GLenum);
using GetQuery=void(APIENTRY*)(GLenum,GLenum,GLint*);
using GetObject=void(APIENTRY*)(GLuint,GLenum,GLuint*);
using GetObject64=void(APIENTRY*)(GLuint,GLenum,std::uint64_t*);
constexpr GLenum timestamp=0x8E28, available=0x8867, result=0x8866;
struct Sample { Phase phase{}; Pass pass{}; bool aborted{}; };
constexpr unsigned FindFree(const std::array<Sample,capacity>& slots,unsigned cursor) noexcept {
    for (unsigned n=0;n<capacity;++n) {
        const unsigned index=(cursor+n)%capacity;
        if (slots[index].phase==Phase::Free) return index;
    }
    return invalid;
}
static_assert([]() constexpr {
    std::array<Sample,capacity> slots{};
    // Exhaustion must not reuse either an unended or an unavailable query.
    for (unsigned i=0;i<capacity;++i) slots[i].phase=i%2 ? Phase::Open:Phase::Pending;
    if (FindFree(slots,31)!=invalid) return false;
    slots[0].phase=Phase::Free;
    if (FindFree(slots,31)!=0) return false; // wraps without skipping legal slot
    slots[31].phase=Phase::Free;
    return FindFree(slots,31)==31 && FindFree(slots,0)==0;
}());
struct Stats {
    std::uint64_t issued{},samples{},drops{},invalid_spans{};
    double total_ms{},max_ms{};
};
struct State {
    HGLRC context{};
    std::uint64_t thread{};
    bool initialized{},disabled{};
    unsigned poll_cursor{},allocate_cursor{};
    ULONGLONG next_log{};
    Counter counter{};
    GetObject get{};
    GetObject64 get64{};
    std::array<GLuint,capacity*2> names{};
    std::array<Sample,capacity> slots{};
    std::array<Stats,static_cast<unsigned>(Pass::Count)> stats{};
};
inline State state{};
inline std::atomic_flag locked=ATOMIC_FLAG_INIT;
inline std::atomic_bool deleted{false};
inline std::atomic_bool lifecycle_ready{false};
inline std::atomic<int> configuration{-1};
inline std::atomic<std::uint64_t> thread_sequence{0};
inline std::atomic<unsigned> lock_drops{0},end_abandoned{0};
inline bool Enabled() noexcept {
    const int configured=configuration.load(std::memory_order_acquire);
    if (configured>=0) return EffectiveEnabled(configured,false);
    static const bool enabled=[]() noexcept {
        char value[3]{};
        const DWORD size=GetEnvironmentVariableA("KOTOR2VR_GL_GPU_TIMING",value,3);
        return EnabledValue(value,size);
    }();
    return EffectiveEnabled(configured,enabled);
}
inline std::uint64_t Thread() noexcept {
    // Unlike a DWORD thread id this cookie is not reused after thread exit.
    static thread_local const auto cookie=thread_sequence.fetch_add(1)+1;
    return cookie;
}
template<class T> inline bool Resolve(T& output,const char* name) noexcept {
    const auto pointer=wglGetProcAddress(name);
    const auto value=reinterpret_cast<std::uintptr_t>(pointer);
    if (value==0 || value<=3 || value==static_cast<std::uintptr_t>(-1)) return false;
    output=reinterpret_cast<T>(pointer); return true;
}
inline void Disable(const char* reason) noexcept {
    if (!state.disabled) {
        char line[240]{};
        unsigned abandoned=0;
        for (const auto& slot:state.slots) if (slot.phase!=Phase::Free) ++abandoned;
        std::snprintf(line,sizeof(line),"gl-gpu-timing disabled reason=%s abandoned=%u cleanup=context-lifetime",reason,abandoned);
        (void)AppendPersistentProbeLogLine(line);
        state.disabled=true;
    }
}
inline bool Compatible() noexcept {
    if (state.disabled) return false;
    if (deleted.load(std::memory_order_acquire)) { Disable("context-deleted"); return false; }
    if (state.context && (wglGetCurrentContext()!=state.context || Thread()!=state.thread)) {
        Disable("owner-changed"); return false;
    }
    return true;
}
inline void LogCapabilities(const char* version,int profile,int flags,int bits,bool functions) noexcept {
    char line[440]{};
    // -1 means not queried, not an observed GL value. Bound driver string output.
    std::snprintf(line,sizeof(line),
        "gl-gpu-timing capabilities version=\"%.160s\" profile=%d flags=%d timestamp_bits=%d functions=%u unqueried=-1",
        version ? version:"unavailable",profile,flags,bits,functions ? 1U:0U);
    (void)AppendPersistentProbeLogLine(line);
}
inline bool Initialize() noexcept {
    if (state.initialized) return !state.disabled;
    const auto context=wglGetCurrentContext();
    if (!context) return false;
    state.initialized=true; state.context=context; state.thread=Thread();
    // Conservative core-version gate; older extension-only drivers stay off.
    // Timestamp queries are not compatibility-specific. The profile gate is
    // solely for our later legacy GL_LIST_INDEX check. Query profile/flags only
    // after establishing a desktop 3.3+ version, where these enums are defined.
    const auto* version=reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (!SupportedVersion(version)) {
        LogCapabilities(version,-1,-1,-1,false);
        Disable("requires-gl33"); return false;
    }
    // Sentinel prevents an unwritten getter result from masquerading as the
    // legacy zero-mask exception. Do not consume the engine's GL error state.
    GLint profile=-1;
    glGetIntegerv(0x9126,&profile); // CONTEXT_PROFILE_MASK
    GLint flags=-1;
    glGetIntegerv(0x821E,&flags); // CONTEXT_FLAGS, forward-compatible bit = 1
    GenQueries gen{}; GetQuery get_query{};
    bool functions=Resolve(gen,"glGenQueries");
    functions=Resolve(state.counter,"glQueryCounter") && functions;
    functions=Resolve(get_query,"glGetQueryiv") && functions;
    functions=Resolve(state.get,"glGetQueryObjectuiv") && functions;
    functions=Resolve(state.get64,"glGetQueryObjectui64v") && functions;
    GLint bits=-1;
    if (functions) get_query(timestamp,0x8864,&bits); // QUERY_COUNTER_BITS
    LogCapabilities(version,profile,flags,bits,functions);
    // No legacy getters or query allocation on rejected core/forward contexts.
    if (!SupportsLegacyListCheck(profile,flags)) { Disable("legacy-list-guard-unsupported"); return false; }
    if (!functions) { Disable("timestamp-functions"); return false; }
    if (bits!=64) { Disable("requires-64bit-timestamps"); return false; }
    gen(static_cast<GLsizei>(state.names.size()),state.names.data());
    for (auto name:state.names) if (!name) { Disable("query-allocation"); return false; }
    state.next_log=GetTickCount64()+5000;
    (void)AppendPersistentProbeLogLine("gl-gpu-timing enabled ring=32 poll_budget=4 interval_ms=5000 units=ms scope=scene-gpu-queue-span-not-busy-time cleanup=context-lifetime");
    return true;
}
inline void Poll() noexcept {
    // Inspect at most four slots per scene entry, including unavailable slots.
    // No head-of-line wait and no RESULT read until BOTH timestamps are ready.
    for (unsigned n=0;n<4;++n) {
        const auto index=state.poll_cursor;
        state.poll_cursor=(index+1)%capacity;
        auto& slot=state.slots[index];
        if (slot.phase!=Phase::Pending) continue;
        GLuint first=0,last=0;
        state.get(state.names[2*index],available,&first);
        state.get(state.names[2*index+1],available,&last);
        if (!MayRead(slot.phase,first!=0,last!=0)) continue;
        std::uint64_t begin=0,end=0;
        state.get64(state.names[2*index],result,&begin);
        state.get64(state.names[2*index+1],result,&end);
        auto& stats=state.stats[static_cast<unsigned>(slot.pass)];
        if (!slot.aborted && ValidSpan(begin,end)) {
            const double ms=static_cast<double>(end-begin)/1000000.0;
            ++stats.samples; stats.total_ms+=ms;
            if (ms>stats.max_ms) stats.max_ms=ms;
        } else ++stats.invalid_spans;
        slot.phase=Phase::Free;
    }
    const auto now=GetTickCount64();
    if (now<state.next_log) return;
    state.next_log=now+5000;
    constexpr const char* labels[]={"base_scene","stereo_capture_scene","stereo_replay_scene","monitor_replay_scene"};
    // Four cumulative lines at most every five seconds; no per-frame output.
    for (unsigned i=0;i<static_cast<unsigned>(Pass::Count);++i) {
        const auto& s=state.stats[i];
        unsigned pending=0;
        for (const auto& slot:state.slots)
            if (slot.phase!=Phase::Free && static_cast<unsigned>(slot.pass)==i) ++pending;
        char line[512]{};
        std::snprintf(line,sizeof(line),"gl-gpu-timing pass=%s scope=gpu-queue-span-not-busy-time cumulative=1 issued=%llu samples=%llu drops=%llu invalid=%llu pending=%u total_ms=%.4f mean_ms=%.4f max_ms=%.4f global_lock_drops=%u global_end_abandoned=%u",
            labels[i],static_cast<unsigned long long>(s.issued),static_cast<unsigned long long>(s.samples),
            static_cast<unsigned long long>(s.drops),static_cast<unsigned long long>(s.invalid_spans),pending,
            s.total_ms,s.samples ? s.total_ms/static_cast<double>(s.samples):0.0,s.max_ms,
            lock_drops.load(std::memory_order_relaxed),end_abandoned.load(std::memory_order_relaxed));
        (void)AppendPersistentProbeLogLine(line);
    }
}
} // namespace detail

inline void Configure(bool enabled) noexcept {
    // Called by the camera INI loader before installing scene hooks/first Begin.
    // CPU-only; never arms lifecycle or revives a disabled/deleted context.
    detail::configuration.store(enabled ? 1:0,std::memory_order_release);
}

inline void ArmAfterDeletionHookInstalled() noexcept {
    // Installer owns this transition, after proving both live IAT wrappers.
    // Log once even when opt-in is off: launcher environment/manifest alone
    // does not prove the separately bootstrapped game inherited that setting.
    // No GL work here. Do not clear deleted/disabled or relax Begin's guard.
    const bool enabled=detail::Enabled();
    if (!detail::lifecycle_ready.exchange(true,std::memory_order_acq_rel)) {
        char value[3]{};
        const DWORD size=GetEnvironmentVariableA("KOTOR2VR_GL_GPU_TIMING",value,3);
        const char* observed=size==0 ? "missing-or-empty":
            EnabledValue(value,size) ? "1":"other";
        char line[280]{};
        std::snprintf(line,sizeof(line),
            "gl-gpu-timing armed pid=%lu hook=verified-present-and-delete source=%s env=%s enabled=%u context_deleted=%u",
            static_cast<unsigned long>(GetCurrentProcessId()),
            detail::configuration.load(std::memory_order_acquire)<0 ? "environment":"camera-ini",
            observed,enabled ? 1U:0U,
            detail::deleted.load(std::memory_order_acquire) ? 1U:0U);
        (void)AppendPersistentProbeLogLine(line);
    }
}

inline void ContextDeleted(void* context) noexcept {
    // Called AFTER successful deletion. Never inspect thread-owned GL objects,
    // acquire a blocking lock, or issue GL calls here. Even unrelated deletion
    // conservatively disables diagnostics, covering handle reuse across threads.
    if (context) detail::deleted.store(true,std::memory_order_release);
}

inline unsigned Begin(Pass pass) noexcept {
    using namespace detail;
    if (!Enabled() || !lifecycle_ready.load(std::memory_order_acquire) ||
        static_cast<unsigned>(pass)>=static_cast<unsigned>(Pass::Count)) return invalid;
    if (locked.test_and_set(std::memory_order_acquire)) { ++lock_drops; return invalid; }
    unsigned token=invalid;
    // No nontrivial automatic objects: valid in callers using Windows SEH.
    if (Compatible() && Initialize()) {
        GLint compiling=0;
        glGetIntegerv(GL_LIST_INDEX,&compiling);
        auto& stats=state.stats[static_cast<unsigned>(pass)];
        if (compiling) ++stats.drops;
        else {
            Poll();
            const unsigned index=FindFree(state.slots,state.allocate_cursor);
            if (index!=invalid) {
                state.slots[index]={Phase::Open,pass,false};
                state.counter(state.names[2*index],timestamp);
                ++stats.issued; token=index;
                state.allocate_cursor=(index+1)%capacity;
            }
            if (token==invalid) ++stats.drops;
        }
    }
    locked.clear(std::memory_order_release);
    return token;
}
inline void End(unsigned token,bool aborted=false) noexcept {
    using namespace detail;
    if (token>=capacity) return;
    // Contention abandons this Open slot permanently; it is never reused with
    // an incomplete pair. Ring pressure subsequently drops new samples safely.
    if (locked.test_and_set(std::memory_order_acquire)) { ++end_abandoned; return; }
    if (Compatible() && state.slots[token].phase==Phase::Open) {
        GLint compiling=0;
        glGetIntegerv(GL_LIST_INDEX,&compiling);
        if (compiling) Disable("unbalanced-display-list");
        else {
            state.counter(state.names[2*token+1],timestamp);
            state.slots[token].aborted=aborted;
            state.slots[token].phase=Phase::Pending;
        }
    }
    locked.clear(std::memory_order_release);
}
} // namespace k2vr::game32::gpu_timing
