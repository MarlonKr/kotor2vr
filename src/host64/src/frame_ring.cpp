#include "kotorvr/host/frame_ring.hpp"

#include <algorithm>
#include <limits>

namespace kotorvr::host {

namespace {

[[nodiscard]] bool optional_texture_layout_matches(const NativeTexture& left,
                                                    const NativeTexture& right) noexcept {
    if (left.valid() != right.valid()) {
        return false;
    }
    return !left.valid() ||
           (left.extent == right.extent && left.format == right.format);
}

[[nodiscard]] bool frames_form_complete_pair(const SubmittedEyeFrame& left,
                                             const SubmittedEyeFrame& right) noexcept {
    return left.eye == Eye::left && right.eye == Eye::right &&
           left.frame_id == right.frame_id &&
           left.stream_generation == right.stream_generation &&
           left.predicted_display_time_ns == right.predicted_display_time_ns &&
           left.color.valid() && right.color.valid() &&
           left.color.extent == right.color.extent &&
           left.color.format == right.color.format &&
           optional_texture_layout_matches(left.depth, right.depth) &&
           optional_texture_layout_matches(left.motion_vectors, right.motion_vectors) &&
           left.producer_fence_handle != 0 && right.producer_fence_handle != 0 &&
           left.producer_fence_value != 0 && right.producer_fence_value != 0;
}

} // namespace

StereoFrameRing::StereoFrameRing() {
    for (std::size_t eye = 0; eye < eye_count; ++eye) {
        for (auto& slot : slots_[eye]) {
            slot.eye = static_cast<Eye>(eye);
            slot.frame.eye = static_cast<Eye>(eye);
        }
    }
}

std::optional<ProducerLease> StereoFrameRing::begin_produce(
    const Eye eye,
    const std::uint64_t frame_id,
    const std::uint64_t stream_generation) {
    std::scoped_lock lock(mutex_);
    if (!valid_eye(eye) || frame_id == 0 || stream_generation == 0 ||
        (stream_generation_ != 0 && stream_generation != stream_generation_)) {
        ++stats_.rejected;
        return std::nullopt;
    }
    if (stream_generation_ == 0) {
        stream_generation_ = stream_generation;
    }
    if (has_acquired_frame_ && frame_id <= last_acquired_frame_id_) {
        ++stats_.rejected;
        return std::nullopt;
    }

    auto& eye_slots = slots_[eye_index(eye)];
    const auto start = next_slot_[eye_index(eye)];
    Slot* selected = nullptr;
    std::uint32_t selected_index{};

    for (std::uint32_t offset = 0; offset < ring_slot_count; ++offset) {
        const auto index = static_cast<std::uint32_t>((start + offset) % ring_slot_count);
        if (eye_slots[index].state == FrameSlotState::free) {
            selected = &eye_slots[index];
            selected_index = index;
            break;
        }
    }

    if (selected == nullptr) {
        std::uint64_t oldest_frame = std::numeric_limits<std::uint64_t>::max();
        for (std::uint32_t index = 0; index < ring_slot_count; ++index) {
            auto& candidate = eye_slots[index];
            if (candidate.state == FrameSlotState::ready &&
                candidate.frame.frame_id < oldest_frame) {
                oldest_frame = candidate.frame.frame_id;
                selected = &candidate;
                selected_index = index;
            }
        }
        if (selected == nullptr) {
            ++stats_.rejected;
            return std::nullopt;
        }
        ++stats_.dropped;
    }

    selected->state = FrameSlotState::producer_owned;
    selected->lease_id = next_lease_id_++;
    selected->expected_frame_id = frame_id;
    selected->expected_generation = stream_generation;
    selected->consumer_fence_value = 0;
    selected->frame = {};
    selected->frame.eye = eye;
    next_slot_[eye_index(eye)] = (selected_index + 1U) % ring_slot_count;
    return ProducerLease{eye, selected_index, selected->lease_id};
}

bool StereoFrameRing::publish(const ProducerLease& lease,
    const SubmittedEyeFrame& frame,
    std::string& error) {
    std::scoped_lock lock(mutex_);
    if (!valid_eye(lease.eye)) {
        error = "eye identity is outside the negotiated stereo view set";
        ++stats_.rejected;
        return false;
    }
    Slot* slot = find_slot(lease);
    if (slot == nullptr || slot->state != FrameSlotState::producer_owned) {
        error = "producer lease is stale or does not own the slot";
        ++stats_.rejected;
        return false;
    }
    if (!valid_eye(frame.eye)) {
        error = "frame eye identity is outside the negotiated stereo view set";
        abandon_producer_slot(*slot);
        ++stats_.rejected;
        return false;
    }
    if (frame.eye != lease.eye || frame.eye != slot->eye) {
        error = "eye identity does not match the producer lease";
        abandon_producer_slot(*slot);
        ++stats_.rejected;
        return false;
    }
    if (frame.frame_id != slot->expected_frame_id ||
        frame.stream_generation != slot->expected_generation ||
        frame.stream_generation != stream_generation_) {
        error = "frame id or stream generation does not match the producer lease";
        abandon_producer_slot(*slot);
        ++stats_.rejected;
        return false;
    }
    if (frame.predicted_display_time_ns <= 0) {
        error = "predicted OpenXR display time must be positive";
        abandon_producer_slot(*slot);
        ++stats_.rejected;
        return false;
    }
    if (!frame.color.valid()) {
        error = "color texture handle and extent are required";
        abandon_producer_slot(*slot);
        ++stats_.rejected;
        return false;
    }
    if (frame.producer_fence_handle == 0 || frame.producer_fence_value == 0) {
        error = "a signaled producer fence is required";
        abandon_producer_slot(*slot);
        ++stats_.rejected;
        return false;
    }
    if ((!frame.depth.valid() && !frame.depth.empty()) ||
        (!frame.motion_vectors.valid() && !frame.motion_vectors.empty())) {
        error = "optional depth and motion textures must be either complete or empty";
        abandon_producer_slot(*slot);
        ++stats_.rejected;
        return false;
    }
    if ((frame.depth.valid() && frame.depth.extent != frame.color.extent) ||
        (frame.motion_vectors.valid() &&
         frame.motion_vectors.extent != frame.color.extent)) {
        error = "depth and motion-vector extents must match color";
        abandon_producer_slot(*slot);
        ++stats_.rejected;
        return false;
    }

    slot->frame = frame;
    slot->state = FrameSlotState::ready;
    ++stats_.published;
    error.clear();
    return true;
}

bool StereoFrameRing::cancel_produce(const ProducerLease& lease,
                                     std::string& error) {
    std::scoped_lock lock(mutex_);
    if (!valid_eye(lease.eye)) {
        error = "eye identity is outside the negotiated stereo view set";
        ++stats_.rejected;
        return false;
    }
    Slot* slot = find_slot(lease);
    if (slot == nullptr || slot->state != FrameSlotState::producer_owned) {
        error = "producer lease is stale or does not own the slot";
        ++stats_.rejected;
        return false;
    }
    abandon_producer_slot(*slot);
    error.clear();
    return true;
}

std::optional<StereoFramePair> StereoFrameRing::acquire_latest_complete_pair() {
    std::scoped_lock lock(mutex_);
    Slot* selected_left = nullptr;
    Slot* selected_right = nullptr;
    std::uint32_t selected_left_index{};
    std::uint32_t selected_right_index{};
    std::uint64_t newest_frame{};

    auto& left_slots = slots_[eye_index(Eye::left)];
    auto& right_slots = slots_[eye_index(Eye::right)];
    for (std::uint32_t left_index = 0; left_index < ring_slot_count; ++left_index) {
        auto& left = left_slots[left_index];
        if (left.state != FrameSlotState::ready) {
            continue;
        }
        for (std::uint32_t right_index = 0; right_index < ring_slot_count; ++right_index) {
            auto& right = right_slots[right_index];
            if (right.state != FrameSlotState::ready ||
                !frames_form_complete_pair(left.frame, right.frame) ||
                left.frame.stream_generation != stream_generation_) {
                continue;
            }
            if (selected_left == nullptr || left.frame.frame_id > newest_frame) {
                selected_left = &left;
                selected_right = &right;
                selected_left_index = left_index;
                selected_right_index = right_index;
                newest_frame = left.frame.frame_id;
            }
        }
    }

    if (selected_left == nullptr || selected_right == nullptr) {
        return std::nullopt;
    }

    selected_left->state = FrameSlotState::consumer_owned;
    selected_right->state = FrameSlotState::consumer_owned;

    StereoFramePair pair{};
    pair.eyes[eye_index(Eye::left)] = selected_left->frame;
    pair.eyes[eye_index(Eye::right)] = selected_right->frame;
    pair.slot_indices[eye_index(Eye::left)] = selected_left_index;
    pair.slot_indices[eye_index(Eye::right)] = selected_right_index;
    pair.consumer_lease_ids[eye_index(Eye::left)] = selected_left->lease_id;
    pair.consumer_lease_ids[eye_index(Eye::right)] = selected_right->lease_id;
    last_acquired_frame_id_ = pair.left().frame_id;
    has_acquired_frame_ = true;
    discard_older_ready_frames(pair.left().stream_generation, pair.left().frame_id);
    ++stats_.acquired_pairs;
    return pair;
}

bool StereoFrameRing::release_pair(const StereoFramePair& pair,
                                   const std::uint64_t consumer_fence_value,
                                   std::string& error) {
    std::scoped_lock lock(mutex_);
    if (consumer_fence_value == 0) {
        error = "consumer fence value must be non-zero";
        ++stats_.rejected;
        return false;
    }
    for (const Eye eye : {Eye::left, Eye::right}) {
        const auto index = pair.slot_indices[eye_index(eye)];
        if (index >= ring_slot_count) {
            error = "slot index is outside the per-eye triple ring";
            ++stats_.rejected;
            return false;
        }
        const auto& expected = pair.eyes[eye_index(eye)];
        const auto& slot = slots_[eye_index(eye)][index];
        if (slot.state != FrameSlotState::consumer_owned ||
            slot.eye != eye || slot.frame.eye != eye ||
            slot.lease_id != pair.consumer_lease_ids[eye_index(eye)] ||
            slot.frame.frame_id != expected.frame_id ||
            slot.frame.stream_generation != expected.stream_generation) {
            error = "stereo pair no longer owns both ring slots";
            ++stats_.rejected;
            return false;
        }
    }

    for (const Eye eye : {Eye::left, Eye::right}) {
        auto& slot = slots_[eye_index(eye)][pair.slot_indices[eye_index(eye)]];
        slot.consumer_fence_value = consumer_fence_value;
        slot.state = FrameSlotState::consumer_pending;
    }
    error.clear();
    return true;
}

void StereoFrameRing::retire_completed(
    const std::uint64_t completed_consumer_fence_value) {
    std::scoped_lock lock(mutex_);
    for (auto& eye_slots : slots_) {
        for (auto& slot : eye_slots) {
            if (slot.state == FrameSlotState::consumer_pending &&
                slot.consumer_fence_value <= completed_consumer_fence_value) {
                slot.state = FrameSlotState::free;
                slot.consumer_fence_value = 0;
                ++stats_.retired;
            }
        }
    }
}

bool StereoFrameRing::reset(const std::uint64_t new_stream_generation,
                            std::string& error) {
    std::scoped_lock lock(mutex_);
    if (new_stream_generation == 0 ||
        (stream_generation_ != 0 && new_stream_generation <= stream_generation_)) {
        error = "stream generation must increase monotonically and remain non-zero";
        ++stats_.rejected;
        return false;
    }
    for (const auto& eye_slots : slots_) {
        for (const auto& slot : eye_slots) {
            if (slot.state == FrameSlotState::producer_owned ||
                slot.state == FrameSlotState::consumer_owned ||
                slot.state == FrameSlotState::consumer_pending) {
                error = "cannot reset while a producer or GPU consumer owns a ring slot";
                ++stats_.rejected;
                return false;
            }
        }
    }
    stream_generation_ = new_stream_generation;
    next_slot_.fill(0);
    last_acquired_frame_id_ = 0;
    has_acquired_frame_ = false;
    for (std::size_t eye = 0; eye < eye_count; ++eye) {
        for (auto& slot : slots_[eye]) {
            slot = {};
            slot.eye = static_cast<Eye>(eye);
            slot.frame.eye = static_cast<Eye>(eye);
        }
    }
    error.clear();
    return true;
}

FrameRingStats StereoFrameRing::stats() const {
    std::scoped_lock lock(mutex_);
    return stats_;
}

StereoFrameRing::Slot* StereoFrameRing::find_slot(const ProducerLease& lease) {
    if (!valid_eye(lease.eye) || lease.slot_index >= ring_slot_count) {
        return nullptr;
    }
    Slot& slot = slots_[eye_index(lease.eye)][lease.slot_index];
    return slot.lease_id == lease.lease_id ? &slot : nullptr;
}

const StereoFrameRing::Slot* StereoFrameRing::find_slot(const ProducerLease& lease) const {
    if (!valid_eye(lease.eye) || lease.slot_index >= ring_slot_count) {
        return nullptr;
    }
    const Slot& slot = slots_[eye_index(lease.eye)][lease.slot_index];
    return slot.lease_id == lease.lease_id ? &slot : nullptr;
}

void StereoFrameRing::abandon_producer_slot(Slot& slot) noexcept {
    slot.state = FrameSlotState::free;
    slot.expected_frame_id = 0;
    slot.expected_generation = 0;
    slot.consumer_fence_value = 0;
    slot.frame = {};
    slot.frame.eye = slot.eye;
    ++stats_.cancelled;
}

void StereoFrameRing::discard_older_ready_frames(const std::uint64_t generation,
                                                 const std::uint64_t frame_id) {
    for (auto& eye_slots : slots_) {
        for (auto& slot : eye_slots) {
            if (slot.state == FrameSlotState::ready &&
                (slot.frame.stream_generation < generation ||
                 (slot.frame.stream_generation == generation &&
                  slot.frame.frame_id < frame_id))) {
                slot.state = FrameSlotState::free;
                ++stats_.dropped;
            }
        }
    }
}

} // namespace kotorvr::host
