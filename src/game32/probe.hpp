#pragma once

#include <cstdint>
#include <string_view>

#if defined(_WIN32)
#if defined(K2VR_GAME32_BUILD)
#define K2VR_GAME32_EXPORT extern "C" __declspec(dllexport)
#else
#define K2VR_GAME32_EXPORT extern "C" __declspec(dllimport)
#endif
#define K2VR_GAME32_CALL __cdecl
#define K2VR_GAME32_THREAD_CALL __stdcall
#else
#define K2VR_GAME32_EXPORT extern "C"
#define K2VR_GAME32_CALL
#define K2VR_GAME32_THREAD_CALL
#endif

namespace k2vr::game32 {

enum class ProbeResult : std::uint32_t {
    OkExactBuildProbeOnly = 0,
    AlreadyInitialized = 1,
    UnknownExecutable = 2,
    UnsupportedArchitecture = 3,
    HashFailure = 4,
    PeInspectionFailure = 5,
    NotInitialized = 6,
    ProbeUnavailable = 7,
    LifetimePinFailure = 8,
    PersistentLogFailure = 9,
    SamplerStartFailure = 10,
    InvalidBootstrapArgument = 11,
    SamplerStopTimeout = 12,
};

// WaitForSingleObject returns WAIT_OBJECT_0 (zero) only after the sampler has
// definitely stopped. Every other result must preserve its handle and keep the
// bootstrap terminal so another sampler cannot be started concurrently.
[[nodiscard]] constexpr bool SamplerStopWasObserved(
    std::uint32_t wait_result) noexcept {
    return wait_result == 0U;
}

// Called from DllMain only to retain the DLL module handle. It performs no I/O,
// allocation, hashing, engine access, or hook installation.
void SetSelfModule(void* module) noexcept;

// Internal worker entry: one bounded camera-chain sample. It remains read-only
// and returns NotInitialized after shutdown so the bootstrap loop can stop.
ProbeResult RunReadOnlyCameraSample() noexcept;

// Shared persistent control log used by explicit post-LoadLibrary workers.
// This is never called from DllMain or from a render hot path.
bool AppendPersistentProbeLogLine(std::string_view message) noexcept;

} // namespace k2vr::game32

// Authoritative post-LoadLibrary entry point. Its signature is deliberately
// compatible with LPTHREAD_START_ROUTINE so an x86 injector can call it with
// CreateRemoteThread and verify the returned ProbeResult. Protocol v1 requires
// a null argument. InvalidBootstrapArgument is returned otherwise.
K2VR_GAME32_EXPORT std::uint32_t K2VR_GAME32_THREAD_CALL
K2VR_ProbeBootstrap(void* reserved) noexcept;
K2VR_GAME32_EXPORT std::uint32_t K2VR_GAME32_CALL K2VR_ProbeInitialize() noexcept;
K2VR_GAME32_EXPORT std::uint32_t K2VR_GAME32_CALL K2VR_RunReadOnlyProbe() noexcept;
K2VR_GAME32_EXPORT void K2VR_GAME32_CALL K2VR_ProbeShutdown() noexcept;
K2VR_GAME32_EXPORT std::uint32_t K2VR_GAME32_CALL K2VR_ProtocolVersion() noexcept;
