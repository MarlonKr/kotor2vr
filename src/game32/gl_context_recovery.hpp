#pragma once
#include <cstdint>

namespace k2vr::game32 {
// Recovery may take several render boundaries. Keep a successful import while
// retrying dependent GL setup, and never reuse it in a different context.
class GlContextRecoveryRetry final {
public:
    void Reset() noexcept { imported_=false; next_attempt_=0; }
    [[nodiscard]] bool imported() const noexcept { return imported_; }
    void MarkImported() noexcept { imported_=true; }
    [[nodiscard]] bool BeginAttempt(std::uint64_t now,
        bool imported_context_is_current) noexcept {
        if (now<next_attempt_ || (imported_ && !imported_context_is_current)) return false;
        next_attempt_=now+1000U;
        return true;
    }
private:
    bool imported_{};
    std::uint64_t next_attempt_{};
};
}
