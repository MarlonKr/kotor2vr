#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace k2vr::builds {

inline constexpr std::uint16_t kPeMachineI386 = 0x014CU;

enum class GameBuildId : std::uint32_t {
    Unknown = 0,
    SteamAspyr2015Build817494 = 0x53544131U,
    GogAspyr = 0x474F4731U,
};

enum class Distribution : std::uint32_t {
    Unknown = 0,
    Steam,
    Gog,
};

enum class SupportLevel : std::uint32_t {
    Unknown = 0,
    ProbeOnly,
    HookReady,
};

enum class AddressKind : std::uint32_t {
    Function = 0,
    GlobalPointerSlot,
    GlobalValue,
    InstructionAnchor,
};

enum class SymbolId : std::uint32_t {
    SetCameraMode = 1,
    GuiManagerPointer,
    AppManagerPointer,
    RenderGuiFlag,
    RenderWireframeFlag,
    DialogLetterboxConstructorAnchor,
    DialogLetterboxResetFadeAnchor,
    DialogLetterboxSetFadeAnchor,
    DialogLetterboxDrawAlphaStoreAnchor,
};

struct Sha256Digest {
    std::array<std::uint8_t, 32> bytes{};

    [[nodiscard]] friend constexpr bool operator==(const Sha256Digest&,
                                                   const Sha256Digest&) noexcept =
        default;
};

struct FileVersion {
    std::uint16_t major{0};
    std::uint16_t minor{0};
    std::uint16_t patch{0};
    std::uint16_t build{0};
};

struct KnownAddress {
    SymbolId symbol;
    AddressKind kind;
    std::uint32_t preferred_virtual_address;
    std::string_view provenance;
};

// This layout is intentionally probe-only. It is useful for comparing live
// camera data against existing reverse engineering, but is not hook authority.
struct CameraProbeLayout {
    bool available{false};
    std::uint32_t app_manager_to_facade{0};
    std::uint32_t facade_to_internal{0};
    std::uint32_t internal_to_module{0};
    std::uint32_t module_to_camera{0};
    std::uint32_t camera_position{0};
    std::uint32_t camera_orientation{0};
};

struct ExactBuildDescriptor {
    GameBuildId id;
    Distribution distribution;
    SupportLevel support_level;
    std::string_view name;
    Sha256Digest executable_sha256;
    std::uint16_t pe_machine;
    std::uint32_t preferred_image_base;
    FileVersion file_version;
    std::uint32_t storefront_build;
    std::uint64_t executable_file_size;
    std::span<const KnownAddress> known_addresses;
    CameraProbeLayout camera_probe;
};

enum class BuildVerificationStatus : std::uint32_t {
    Verified = 0,
    UnsupportedPlatform,
    MainModuleUnavailable,
    ExecutablePathUnavailable,
    FileOpenFailure,
    FileInspectionFailure,
    HashFailure,
    LoadedImageInspectionFailure,
    LoadedImageDoesNotMatchFile,
    UnknownBuild,
    DescriptorMismatch,
};

struct BuildVerificationResult;

// An immutable capability issued only by VerifyMainExecutable after it has
// inspected the live process' main PE image and hashed/inspected the executable
// through one open file handle. Public build descriptors are intentionally not
// convertible to this type: copying or modifying one can never grant hook
// authority.
class VerifiedBuild final {
public:
    VerifiedBuild(const VerifiedBuild&) noexcept = default;
    VerifiedBuild& operator=(const VerifiedBuild&) noexcept = default;
    VerifiedBuild(VerifiedBuild&&) noexcept = default;
    VerifiedBuild& operator=(VerifiedBuild&&) noexcept = default;

    [[nodiscard]] const ExactBuildDescriptor& descriptor() const noexcept;
    [[nodiscard]] std::uintptr_t runtime_image_base() const noexcept;
    [[nodiscard]] std::uint32_t runtime_image_size() const noexcept;

private:
    friend BuildVerificationResult VerifyMainExecutable() noexcept;

    VerifiedBuild(const ExactBuildDescriptor* descriptor,
                  std::uintptr_t runtime_image_base,
                  std::uint32_t runtime_image_size) noexcept;

    const ExactBuildDescriptor* descriptor_;
    std::uintptr_t runtime_image_base_;
    std::uint32_t runtime_image_size_;
};

struct BuildVerificationResult {
    BuildVerificationStatus status{BuildVerificationStatus::UnsupportedPlatform};
    std::optional<VerifiedBuild> verified_build;
    Sha256Digest executable_sha256{};
    std::uint16_t pe_machine{0};
    std::uint32_t runtime_image_size{0};
    std::uint64_t executable_file_size{0};

    [[nodiscard]] bool IsVerified() const noexcept {
        return status == BuildVerificationStatus::Verified &&
               verified_build.has_value();
    }
};

[[nodiscard]] std::optional<Sha256Digest> ParseSha256(std::string_view hex) noexcept;
[[nodiscard]] std::array<char, 65> ToHex(const Sha256Digest& digest) noexcept;

[[nodiscard]] std::span<const ExactBuildDescriptor> KnownBuilds() noexcept;

// Verifies only the current process' main executable. The implementation gets
// the module and path itself so callers cannot mint a proof for an unrelated
// descriptor or buffer. The opened file is both PE-inspected and SHA-256-hashed
// before the proof is issued.
[[nodiscard]] BuildVerificationResult VerifyMainExecutable() noexcept;

// Matching is exact by design. A near match, file-version match, or storefront
// label match is never enough to authorize memory access or hook installation.
[[nodiscard]] const ExactBuildDescriptor* FindExactBuild(
    const Sha256Digest& executable_sha256,
    std::uint16_t pe_machine) noexcept;

[[nodiscard]] const KnownAddress* FindAddress(const ExactBuildDescriptor& build,
                                              SymbolId symbol) noexcept;

[[nodiscard]] constexpr std::uint32_t ToRva(const ExactBuildDescriptor& build,
                                            const KnownAddress& address) noexcept {
    return address.preferred_virtual_address - build.preferred_image_base;
}

[[nodiscard]] constexpr std::uintptr_t ToRuntimeAddress(
    const ExactBuildDescriptor& build,
    const KnownAddress& address,
    std::uintptr_t runtime_image_base) noexcept {
    return runtime_image_base + ToRva(build, address);
}

[[nodiscard]] std::string_view ToString(GameBuildId id) noexcept;
[[nodiscard]] std::string_view ToString(SymbolId id) noexcept;
[[nodiscard]] std::string_view ToString(BuildVerificationStatus status) noexcept;

} // namespace k2vr::builds
