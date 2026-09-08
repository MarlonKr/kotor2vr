#pragma once

#include "kotorvr/host/types.hpp"

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace kotorvr::host {

enum class FrameSlotState : std::uint8_t {
    free,
    producer_owned,
    ready,
    consumer_owned,
    consumer_pending,
};

struct FrameRingStats {
    std::uint64_t published{};
    std::uint64_t acquired_pairs{};
    std::uint64_t retired{};
    std::uint64_t dropped{};
    std::uint64_t cancelled{};
    std::uint64_t rejected{};
};

struct ProducerLease {
    Eye eye{Eye::left};
    std::uint32_t slot_index{};
    std::uint64_t lease_id{};
};

class StereoFrameRing final {
public:
    StereoFrameRing();

    [[nodiscard]] std::optional<ProducerLease> begin_produce(
        Eye eye,
        std::uint64_t frame_id,
        std::uint64_t stream_generation);

    [[nodiscard]] bool publish(const ProducerLease& lease,
                               const SubmittedEyeFrame& frame,
                               std::string& error);
    [[nodiscard]] bool cancel_produce(const ProducerLease& lease,
                                      std::string& error);

    [[nodiscard]] std::optional<StereoFramePair> acquire_latest_complete_pair();
    [[nodiscard]] bool release_pair(const StereoFramePair& pair,
                                    std::uint64_t consumer_fence_value,
                                    std::string& error);
    void retire_completed(std::uint64_t completed_consumer_fence_value);

    [[nodiscard]] bool reset(std::uint64_t new_stream_generation,
                             std::string& error);
    [[nodiscard]] FrameRingStats stats() const;

private:
    struct Slot {
        FrameSlotState state{FrameSlotState::free};
        Eye eye{Eye::left};
        std::uint64_t lease_id{};
        std::uint64_t expected_frame_id{};
        std::uint64_t expected_generation{};
        std::uint64_t consumer_fence_value{};
        SubmittedEyeFrame frame{};
    };

    using EyeSlots = std::array<Slot, ring_slot_count>;

    [[nodiscard]] Slot* find_slot(const ProducerLease& lease);
    [[nodiscard]] const Slot* find_slot(const ProducerLease& lease) const;
    void abandon_producer_slot(Slot& slot) noexcept;
    void discard_older_ready_frames(std::uint64_t generation, std::uint64_t frame_id);

    mutable std::mutex mutex_;
    std::array<EyeSlots, eye_count> slots_{};
    std::array<std::uint32_t, eye_count> next_slot_{};
    std::uint64_t next_lease_id_{1};
    std::uint64_t stream_generation_{};
    std::uint64_t last_acquired_frame_id_{};
    bool has_acquired_frame_{};
    FrameRingStats stats_{};
};

} // namespace kotorvr::host
