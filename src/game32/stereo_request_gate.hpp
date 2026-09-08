#pragma once
#include "render_trace.hpp"
#include "stereo_stream.hpp"
#include <array>
#include <cstring>

namespace k2vr::game32 {
// Optional XR-paced sampling, not proof that animation/HUD pixels are identical.
// Commit only after the complete eyes/HUD publication fence has been signalled.
struct StereoCaptureKey {
    ipc::RenderRequest request{};
    std::array<EngineCameraPoseWxyz,2> eyes{};
    std::uintptr_t camera{};
};
class StereoRequestGate {
public:
    // Refresh even if the host temporarily stops publishing new requests.
    static constexpr std::uint64_t maximum_skip_age_ms=20;
    // Opt-in once-per-full-XR-request sampling, including during walking.
    // GetTickCount64 milliseconds are coarse, not QPC/GPU timing. The watchdog
    // is measured from successful publication; reuse checks never extend it.
    static constexpr std::uint64_t cadence_maximum_skip_age_ms=100;
    bool SameRequest(const StereoCaptureKey& key) const noexcept {
        return published_ && key.camera==last_.camera &&
            std::memcmp(&key.request,&last_.request,sizeof(key.request))==0;
    }
    bool CanReuseView(const StereoCaptureKey& key,std::uint64_t now_ms,bool cadence_lock=false) const noexcept {
        return SameRequest(key) && !dialogue_ && key.camera!=0 &&
            key.request.history_reset_reasons==0 &&
            (key.request.presentation_state==ipc::PresentationState::WorldFirstPerson ||
             key.request.presentation_state==ipc::PresentationState::WorldThirdPerson) &&
            now_ms>=published_at_ms_ && now_ms-published_at_ms_<
                (cadence_lock ? cadence_maximum_skip_age_ms:maximum_skip_age_ms) &&
            (cadence_lock || std::memcmp(key.eyes.data(),last_.eyes.data(),sizeof(key.eyes))==0);
    }
    void Published(const StereoCaptureKey& key,std::uint64_t now_ms,bool dialogue) noexcept {
        last_=key; published_at_ms_=now_ms; dialogue_=dialogue; published_=true;
    }
    void Reset() noexcept { published_=false; }
private:
    StereoCaptureKey last_{};
    std::uint64_t published_at_ms_{};
    bool published_{},dialogue_{};
};
}
