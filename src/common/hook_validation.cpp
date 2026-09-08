#include "hook_validation.hpp"

#include <array>

namespace k2vr::hooks {
namespace {

inline constexpr std::array<std::uint8_t, 9> kLetterboxDrawBytes{
    0x8B, 0x4D, 0xCC, 0xD9, 0x45, 0xB0, 0xD9, 0x59, 0x50};
inline constexpr std::array<std::uint8_t, 9> kExactMask{
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

inline constexpr std::array<HookCandidate, 2> kProbeCandidates{{
    {HookId::DialogLetterboxDrawProbe,
     builds::GameBuildId::SteamAspyr2015Build817494,
     "Steam CSWGuiDialogLetterbox::Draw alpha-store",
     0x004BB3C3U,
     {kLetterboxDrawBytes, kExactMask},
     HookReadiness::ProbeOnly},
    {HookId::DialogLetterboxDrawProbe,
     builds::GameBuildId::GogAspyr,
     "GOG CSWGuiDialogLetterbox::Draw alpha-store",
     0x001AFB13U,
     {kLetterboxDrawBytes, kExactMask},
     HookReadiness::ProbeOnly},
}};

} // namespace

bool IsValidPattern(BytePatternView pattern) noexcept {
    if (pattern.bytes.empty() || pattern.bytes.size() != pattern.mask.size()) {
        return false;
    }

    bool has_compared_byte = false;
    for (const std::uint8_t mask_byte : pattern.mask) {
        if (mask_byte != 0x00U && mask_byte != 0xFFU) {
            return false;
        }
        has_compared_byte = has_compared_byte || mask_byte == 0xFFU;
    }
    return has_compared_byte;
}

PatternStatus MatchPattern(std::span<const std::uint8_t> observed,
                           BytePatternView pattern) noexcept {
    if (!IsValidPattern(pattern)) {
        return PatternStatus::InvalidPattern;
    }
    if (observed.size() < pattern.bytes.size()) {
        return PatternStatus::NoMatch;
    }

    for (std::size_t index = 0; index < pattern.bytes.size(); ++index) {
        if (pattern.mask[index] == 0xFFU &&
            observed[index] != pattern.bytes[index]) {
            return PatternStatus::NoMatch;
        }
    }
    return PatternStatus::Match;
}

PatternSearchResult FindUniquePattern(std::span<const std::uint8_t> haystack,
                                      BytePatternView pattern) noexcept {
    if (!IsValidPattern(pattern)) {
        return {PatternStatus::InvalidPattern, 0, 0};
    }
    if (haystack.size() < pattern.bytes.size()) {
        return {PatternStatus::NotFound, 0, 0};
    }

    std::size_t first_offset = 0;
    std::size_t match_count = 0;
    const std::size_t last_offset = haystack.size() - pattern.bytes.size();
    for (std::size_t offset = 0; offset <= last_offset; ++offset) {
        if (MatchPattern(haystack.subspan(offset), pattern) ==
            PatternStatus::Match) {
            if (match_count == 0) {
                first_offset = offset;
            }
            ++match_count;
        }
    }

    if (match_count == 0) {
        return {PatternStatus::NotFound, 0, 0};
    }
    if (match_count != 1) {
        return {PatternStatus::Ambiguous, first_offset, match_count};
    }
    return {PatternStatus::Match, first_offset, 1};
}

HookGateResult ValidateHookCandidate(
    const builds::VerifiedBuild* verified_build,
    const HookCandidate& candidate,
    std::span<const std::uint8_t> runtime_image) noexcept {
    if (verified_build == nullptr) {
        return {HookGateStatus::UnknownBuild, candidate.rva, 0};
    }
    const builds::ExactBuildDescriptor& exact_build =
        verified_build->descriptor();
    if (exact_build.support_level != builds::SupportLevel::HookReady) {
        return {HookGateStatus::BuildNotHookReady, candidate.rva, 0};
    }
    if (candidate.readiness != HookReadiness::Installable) {
        return {HookGateStatus::CandidateNotInstallable, candidate.rva, 0};
    }
    if (candidate.build_id != exact_build.id) {
        return {HookGateStatus::CandidateBuildMismatch, candidate.rva, 0};
    }
    if (!IsValidPattern(candidate.expected)) {
        return {HookGateStatus::InvalidPattern, candidate.rva, 0};
    }

    const auto verified_base = reinterpret_cast<const std::uint8_t*>(
        verified_build->runtime_image_base());
    if (runtime_image.data() != verified_base ||
        runtime_image.size() != verified_build->runtime_image_size()) {
        return {HookGateStatus::RuntimeImageMismatch, candidate.rva, 0};
    }

    const std::size_t rva = candidate.rva;
    const std::size_t pattern_size = candidate.expected.bytes.size();
    if (rva > runtime_image.size() ||
        pattern_size > runtime_image.size() - rva) {
        return {HookGateStatus::AddressOutOfRange, candidate.rva, 0};
    }

    if (MatchPattern(runtime_image.subspan(rva), candidate.expected) !=
        PatternStatus::Match) {
        return {HookGateStatus::ByteMismatch, candidate.rva, pattern_size};
    }
    return {HookGateStatus::Validated, candidate.rva, pattern_size};
}

std::span<const HookCandidate> KnownProbeCandidates() noexcept {
    return kProbeCandidates;
}

std::string_view ToString(HookGateStatus status) noexcept {
    switch (status) {
    case HookGateStatus::Validated:
        return "validated";
    case HookGateStatus::UnknownBuild:
        return "unknown-build";
    case HookGateStatus::BuildNotHookReady:
        return "build-not-hook-ready";
    case HookGateStatus::CandidateNotInstallable:
        return "candidate-not-installable";
    case HookGateStatus::CandidateBuildMismatch:
        return "candidate-build-mismatch";
    case HookGateStatus::InvalidPattern:
        return "invalid-pattern";
    case HookGateStatus::RuntimeImageMismatch:
        return "runtime-image-mismatch";
    case HookGateStatus::AddressOutOfRange:
        return "address-out-of-range";
    case HookGateStatus::ByteMismatch:
        return "byte-mismatch";
    }
    return "invalid";
}

} // namespace k2vr::hooks
