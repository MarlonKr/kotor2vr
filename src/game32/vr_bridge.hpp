#pragma once

#include "../common/ipc_protocol.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if defined(_WIN32)
#if defined(K2VR_GAME32_BUILD)
#define K2VR_VR_BRIDGE_EXPORT extern "C" __declspec(dllexport)
#else
#define K2VR_VR_BRIDGE_EXPORT extern "C" __declspec(dllimport)
#endif
#define K2VR_VR_BRIDGE_THREAD_CALL __stdcall
#else
#define K2VR_VR_BRIDGE_EXPORT extern "C"
#define K2VR_VR_BRIDGE_THREAD_CALL
#endif

namespace k2vr::game32 {

inline constexpr std::uint16_t kVrBridgeBootstrapMajor = 1;
inline constexpr std::uint16_t kVrBridgeBootstrapMinor = 0;

#pragma pack(push, 1)

// Pointer-free x86/x64 bootstrap contract. The injector must keep the remote
// buffer alive only until K2VR_VrBridgeBootstrap returns; game32 copies the
// complete value synchronously and never retains the caller's pointer.
struct VrBridgeBootstrapV1 {
    std::uint32_t structure_size;
    std::uint16_t version_major;
    std::uint16_t version_minor;
    ipc::SessionNonce session_nonce;
    std::uint64_t generation;
};

#pragma pack(pop)

enum class VrBridgeResult : std::uint32_t {
    Ok = 0,
    InvalidArgument = 1,
    VersionMismatch = 2,
    InvalidSession = 3,
    AlreadyRunning = 4,
    Busy = 5,
    PersistentLogFailure = 6,
    ModulePinFailure = 7,
    ChannelOpenFailure = 8,
    SynchronizationFailure = 9,
    WorkerStartFailure = 10,
    WorkerReadyTimeout = 11,
    InitialHealthFailure = 12,
    StopTimeout = 13,
    AlreadyStopped = 14,
};

[[nodiscard]] constexpr VrBridgeResult ValidateVrBridgeBootstrap(
    const VrBridgeBootstrapV1& bootstrap) noexcept {
    if (bootstrap.structure_size != sizeof(VrBridgeBootstrapV1)) {
        return VrBridgeResult::InvalidArgument;
    }
    if (bootstrap.version_major != kVrBridgeBootstrapMajor ||
        bootstrap.version_minor != kVrBridgeBootstrapMinor) {
        return VrBridgeResult::VersionMismatch;
    }
    if (!ipc::IsValid(bootstrap.session_nonce) ||
        bootstrap.generation == 0) {
        return VrBridgeResult::InvalidSession;
    }
    return VrBridgeResult::Ok;
}

// WaitForSingleObject proves shutdown only for WAIT_OBJECT_0. Every other
// result keeps the module, mapping, and handles alive so a worker can never
// continue through freed state.
[[nodiscard]] constexpr bool VrBridgeWorkerStopWasObserved(
    std::uint32_t wait_result) noexcept {
    return wait_result == 0U;
}

[[nodiscard]] constexpr bool SameVrBridgeSessionNonce(
    ipc::SessionNonce lhs, ipc::SessionNonce rhs) noexcept {
    return lhs.low == rhs.low && lhs.high == rhs.high;
}

// Mirrors SharedMemoryChannel's RenderRequest validation at the point where
// the bridge publishes its private render-thread snapshot. Keeping this check
// here makes the snapshot self-contained: readers never need to consult the
// mapped channel or mutable bootstrap storage.
[[nodiscard]] constexpr bool IsValidVrBridgeRenderRequest(
    const ipc::RenderRequest& request, ipc::SessionNonce expected_nonce,
    std::uint64_t expected_generation) noexcept {
    return ipc::HeaderMatches<ipc::RenderRequest>(
               request.header, ipc::MessageType::RenderRequest) &&
           ipc::IsValid(expected_nonce) && expected_generation != 0 &&
           SameVrBridgeSessionNonce(request.header.session_nonce,
                                    expected_nonce) &&
           request.header.generation == expected_generation &&
           request.header.sequence != 0 && request.frame_id != 0 &&
           request.predicted_display_time_ns > 0 &&
           request.render_width != 0 && request.render_height != 0;
}

[[nodiscard]] constexpr bool ShouldPublishVrBridgeRenderRequest(
    const ipc::RenderRequest& request,
    std::uint64_t last_request_sequence,
    std::uint64_t last_frame_id) noexcept {
    return request.header.sequence != last_request_sequence &&
           request.frame_id > last_frame_id;
}

[[nodiscard]] constexpr bool IsVrBridgeSnapshotFresh(
    std::uint64_t published_at_ms, std::uint64_t now_ms,
    std::uint32_t max_age_ms) noexcept {
    return now_ms >= published_at_ms &&
           now_ms - published_at_ms <= max_age_ms;
}

namespace detail {

// The payload is ordinary pointer-free data. The seqlock stores its object
// representation in atomic 32-bit words, avoiding both torn reads and C++ data
// races while keeping the render-thread read bounded and lock-free on Win32.
struct VrBridgeRenderRequestSnapshotPod {
    ipc::RenderRequest request{};
    ipc::SessionNonce expected_nonce{};
    std::uint64_t expected_generation{};
    std::uint64_t published_at_ms{};
    std::uint32_t valid{};
    std::uint32_t reserved{};
};

[[nodiscard]] constexpr bool IsValidVrBridgeRenderRequestSnapshot(
    const VrBridgeRenderRequestSnapshotPod& snapshot,
    std::uint64_t now_ms, std::uint32_t max_age_ms) noexcept {
    return snapshot.valid == 1U && snapshot.reserved == 0U &&
           IsValidVrBridgeRenderRequest(
               snapshot.request, snapshot.expected_nonce,
               snapshot.expected_generation) &&
           IsVrBridgeSnapshotFresh(snapshot.published_at_ms, now_ms,
                                   max_age_ms);
}

class VrBridgeRenderRequestSeqlock final {
public:
    VrBridgeRenderRequestSeqlock() noexcept {
        for (auto& word : words_) {
            word.store(0U, std::memory_order_relaxed);
        }
    }

    VrBridgeRenderRequestSeqlock(
        const VrBridgeRenderRequestSeqlock&) = delete;
    VrBridgeRenderRequestSeqlock& operator=(
        const VrBridgeRenderRequestSeqlock&) = delete;

    // BridgeWorker is the sole publisher while it is alive. Bootstrap and
    // teardown invalidate only while no worker can be publishing.
    void Publish(
        const VrBridgeRenderRequestSnapshotPod& snapshot) noexcept {
        std::array<std::uint32_t, kWordCount> source_words{};
        std::memcpy(source_words.data(), &snapshot, sizeof(snapshot));

        const std::uint32_t stable =
            sequence_.load(std::memory_order_relaxed);
        const std::uint32_t writing = stable >= 0xFFFFFFFCU
                                          ? 1U
                                          : (stable & ~1U) + 1U;
        (void)sequence_.exchange(writing, std::memory_order_acq_rel);
        for (std::size_t index = 0; index < kWordCount; ++index) {
            words_[index].store(source_words[index],
                                std::memory_order_relaxed);
        }
        sequence_.store(writing + 1U, std::memory_order_release);
    }

    void Invalidate() noexcept {
        Publish({});
    }

    [[nodiscard]] bool TryRead(
        VrBridgeRenderRequestSnapshotPod& output) const noexcept {
        for (unsigned attempt = 0; attempt < 64U; ++attempt) {
            const std::uint32_t before =
                sequence_.load(std::memory_order_acquire);
            if (before == 0U) {
                return false;
            }
            if ((before & 1U) != 0U) {
                continue;
            }

            std::array<std::uint32_t, kWordCount> copied_words{};
            for (std::size_t index = 0; index < kWordCount; ++index) {
                copied_words[index] =
                    words_[index].load(std::memory_order_relaxed);
            }
            const std::uint32_t after =
                sequence_.load(std::memory_order_acquire);
            if (before == after && (after & 1U) == 0U) {
                VrBridgeRenderRequestSnapshotPod candidate{};
                std::memcpy(&candidate, copied_words.data(),
                            sizeof(candidate));
                output = candidate;
                return true;
            }
        }
        return false;
    }

private:
    static constexpr std::size_t kWordCount =
        sizeof(VrBridgeRenderRequestSnapshotPod) / sizeof(std::uint32_t);

    static_assert(sizeof(VrBridgeRenderRequestSnapshotPod) %
                          sizeof(std::uint32_t) ==
                      0);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

    std::atomic<std::uint32_t> sequence_{0U};
    std::array<std::atomic<std::uint32_t>, kWordCount> words_{};
};

} // namespace detail

// Render-thread API. A successful read is a coherent, recently published
// private copy. Failure leaves output unchanged. This function never reads
// g_channel or any mapped shared-memory address.
[[nodiscard]] bool TryReadLatestRenderRequest(
    ipc::RenderRequest& output,
    std::uint32_t max_age_ms = 250U) noexcept;

static_assert(sizeof(VrBridgeBootstrapV1) == 32);
static_assert(offsetof(VrBridgeBootstrapV1, session_nonce) == 8);
static_assert(offsetof(VrBridgeBootstrapV1, generation) == 24);
static_assert(std::is_standard_layout_v<VrBridgeBootstrapV1>);
static_assert(std::is_trivially_copyable_v<VrBridgeBootstrapV1>);
static_assert(std::is_standard_layout_v<
              detail::VrBridgeRenderRequestSnapshotPod>);
static_assert(std::is_trivially_copyable_v<
              detail::VrBridgeRenderRequestSnapshotPod>);

} // namespace k2vr::game32

K2VR_VR_BRIDGE_EXPORT std::uint32_t K2VR_VR_BRIDGE_THREAD_CALL
K2VR_VrBridgeBootstrap(void* bootstrap_v1) noexcept;

K2VR_VR_BRIDGE_EXPORT std::uint32_t K2VR_VR_BRIDGE_THREAD_CALL
K2VR_VrBridgeStop(void* reserved) noexcept;
