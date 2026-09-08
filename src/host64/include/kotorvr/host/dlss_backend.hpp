#pragma once

#include "kotorvr/host/logger.hpp"
#include "kotorvr/host/types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace kotorvr::host {

enum class HistoryResetReason : std::uint32_t {
    none = 0,
    startup = 1U << 0U,
    resolution_change = 1U << 1U,
    swapchain_recreation = 1U << 2U,
    runtime_restart = 1U << 3U,
    camera_cut = 1U << 4U,
    tracking_discontinuity = 1U << 5U,
    user_requested = 1U << 6U,
};

[[nodiscard]] constexpr HistoryResetReason operator|(const HistoryResetReason lhs,
                                                     const HistoryResetReason rhs) noexcept {
    return static_cast<HistoryResetReason>(static_cast<std::uint32_t>(lhs) |
                                           static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] constexpr bool has_reset_reason(const HistoryResetReason reasons,
                                              const HistoryResetReason reason) noexcept {
    return (static_cast<std::uint32_t>(reasons) & static_cast<std::uint32_t>(reason)) != 0;
}

[[nodiscard]] std::string to_string(HistoryResetReason reasons);

struct DlssHistoryToken {
    Eye eye{Eye::left};
    std::uint64_t identity{};
    std::uint64_t generation{};
};

struct DlssFrameRequest {
    Eye eye{Eye::left};
    std::uint64_t frame_id{};
    std::uint64_t stream_generation{};
    DlssHistoryToken history{};
    HistoryResetReason reset_reason{HistoryResetReason::none};
    NativeTexture input_color{};
    NativeTexture input_depth{};
    NativeTexture input_motion_vectors{};
    NativeTexture output_color{};
    EyeView current_view{};
    EyeView previous_view{};
};

enum class DlssResultCode {
    passthrough,
    invalid_request,
    unavailable,
    device_lost,
};

struct DlssResult {
    DlssResultCode code{DlssResultCode::unavailable};
    std::string message;

    [[nodiscard]] bool accepted() const noexcept {
        return code == DlssResultCode::passthrough;
    }
};

class IDlssBackend {
public:
    virtual ~IDlssBackend() = default;

    virtual bool initialize(Extent2D render_extent,
                            std::uint64_t stream_generation,
                            Logger& logger) = 0;
    [[nodiscard]] virtual DlssHistoryToken history_token(Eye eye) const = 0;
    // Resets are stereo-atomic. A runtime/camera discontinuity can never leave
    // one eye advancing old temporal state while the other starts fresh.
    virtual bool reset_histories(Extent2D render_extent,
                                 std::uint64_t stream_generation,
                                 HistoryResetReason reason) = 0;
    [[nodiscard]] virtual DlssResult evaluate(const DlssFrameRequest& request) = 0;
    virtual void shutdown() noexcept = 0;
};

// Validation-only backend. It intentionally never loads NGX, RenoDX, ReShade,
// or any redistributable/proprietary binary. A successful evaluation means that
// the request obeyed the stereo temporal contract and must be copied unchanged.
class SafeDlssStub final : public IDlssBackend {
public:
    bool initialize(Extent2D render_extent,
                    std::uint64_t stream_generation,
                    Logger& logger) override;
    [[nodiscard]] DlssHistoryToken history_token(Eye eye) const override;
    bool reset_histories(Extent2D render_extent,
                         std::uint64_t stream_generation,
                         HistoryResetReason reason) override;
    [[nodiscard]] DlssResult evaluate(const DlssFrameRequest& request) override;
    void shutdown() noexcept override;

private:
    struct EyeHistory {
        DlssHistoryToken token{};
        std::uint64_t last_frame_id{};
        std::uint64_t stream_generation{};
        Extent2D extent{};
        HistoryResetReason pending_reset{HistoryResetReason::none};
        bool has_frame{};
    };

    [[nodiscard]] DlssResult invalid(Eye eye, std::string message);

    std::array<EyeHistory, eye_count> histories_{};
    Logger* logger_{};
    bool initialized_{};
};

} // namespace kotorvr::host
