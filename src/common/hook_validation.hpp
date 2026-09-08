#pragma once

#include "build_descriptor.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace k2vr::hooks {

enum class HookId : std::uint32_t {
    None = 0,
    DialogLetterboxDrawProbe,
    WorldRender,
    CameraUpdate,
    GuiDraw,
};

enum class HookReadiness : std::uint32_t {
    ProbeOnly = 0,
    Installable,
};

// Mask bytes must be either 0xFF (compare) or 0x00 (wildcard). Rejecting all
// other masks makes accidentally permissive signatures fail closed.
struct BytePatternView {
    std::span<const std::uint8_t> bytes;
    std::span<const std::uint8_t> mask;
};

enum class PatternStatus : std::uint32_t {
    Match = 0,
    NoMatch,
    InvalidPattern,
    NotFound,
    Ambiguous,
};

struct PatternSearchResult {
    PatternStatus status{PatternStatus::InvalidPattern};
    std::size_t offset{0};
    std::size_t match_count{0};
};

struct HookCandidate {
    HookId id{HookId::None};
    builds::GameBuildId build_id{builds::GameBuildId::Unknown};
    std::string_view name;
    std::uint32_t rva{0};
    BytePatternView expected;
    HookReadiness readiness{HookReadiness::ProbeOnly};
};

enum class HookGateStatus : std::uint32_t {
    Validated = 0,
    UnknownBuild,
    BuildNotHookReady,
    CandidateNotInstallable,
    CandidateBuildMismatch,
    InvalidPattern,
    RuntimeImageMismatch,
    AddressOutOfRange,
    ByteMismatch,
};

struct HookGateResult {
    HookGateStatus status{HookGateStatus::UnknownBuild};
    std::uint32_t rva{0};
    std::size_t compared_byte_count{0};

    [[nodiscard]] constexpr bool CanInstall() const noexcept {
        return status == HookGateStatus::Validated;
    }
};

[[nodiscard]] bool IsValidPattern(BytePatternView pattern) noexcept;
[[nodiscard]] PatternStatus MatchPattern(std::span<const std::uint8_t> observed,
                                         BytePatternView pattern) noexcept;
[[nodiscard]] PatternSearchResult FindUniquePattern(
    std::span<const std::uint8_t> haystack,
    BytePatternView pattern) noexcept;

// This function only authorizes a hook site; it never writes memory. The build
// must be an exact SHA-256 match and explicitly promoted to HookReady, and the
// candidate must be explicitly promoted to Installable.
[[nodiscard]] HookGateResult ValidateHookCandidate(
    const builds::VerifiedBuild* verified_build,
    const HookCandidate& candidate,
    std::span<const std::uint8_t> runtime_image) noexcept;

// Current public K2 anchors are retained for read-only diagnostics. Every one
// is ProbeOnly, so ValidateHookCandidate can never authorize installation.
[[nodiscard]] std::span<const HookCandidate> KnownProbeCandidates() noexcept;

[[nodiscard]] std::string_view ToString(HookGateStatus status) noexcept;

} // namespace k2vr::hooks
