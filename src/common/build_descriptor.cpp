#include "build_descriptor.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

#include <vector>

#if defined(_MSC_VER)
#pragma comment(lib, "bcrypt.lib")
#endif
#endif

namespace k2vr::builds {
namespace {

[[nodiscard]] constexpr int HexNibble(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

template <std::size_t Size>
[[nodiscard]] consteval Sha256Digest DigestLiteral(const char (&hex)[Size]) {
    static_assert(Size == 65, "SHA-256 literals contain exactly 64 hex digits");
    Sha256Digest result{};
    for (std::size_t index = 0; index < result.bytes.size(); ++index) {
        const int high = HexNibble(hex[index * 2]);
        const int low = HexNibble(hex[index * 2 + 1]);
        if (high < 0 || low < 0) {
            throw "invalid SHA-256 literal";
        }
        result.bytes[index] =
            static_cast<std::uint8_t>((high << 4) | low);
    }
    return result;
}

inline constexpr std::string_view kKpmAddressDatabase =
    "https://github.com/LaneDibello/Kotor-Patch-Manager/tree/32290d5879b4f543a3392670fb2151ef76b855a4/AddressDatabases";
inline constexpr std::string_view kKpmLetterboxPatch =
    "https://github.com/LaneDibello/Kotor-Patch-Manager/tree/32290d5879b4f543a3392670fb2151ef76b855a4/Patches/SemiTransparentLetterbox";

inline constexpr std::array<KnownAddress, 9> kSteamAddresses{{
    {SymbolId::SetCameraMode, AddressKind::Function, 0x007B5E50U,
     kKpmAddressDatabase},
    {SymbolId::GuiManagerPointer, AddressKind::GlobalPointerSlot, 0x00A1B49CU,
     kKpmAddressDatabase},
    {SymbolId::AppManagerPointer, AddressKind::GlobalPointerSlot, 0x00A1B4A4U,
     kKpmAddressDatabase},
    {SymbolId::RenderGuiFlag, AddressKind::GlobalValue, 0x00A32A30U,
     kKpmAddressDatabase},
    {SymbolId::RenderWireframeFlag, AddressKind::GlobalValue, 0x00A32A50U,
     kKpmAddressDatabase},
    {SymbolId::DialogLetterboxConstructorAnchor, AddressKind::InstructionAnchor,
     0x008BAA5EU, kKpmLetterboxPatch},
    {SymbolId::DialogLetterboxResetFadeAnchor, AddressKind::InstructionAnchor,
     0x008BB043U, kKpmLetterboxPatch},
    {SymbolId::DialogLetterboxSetFadeAnchor, AddressKind::InstructionAnchor,
     0x008BB108U, kKpmLetterboxPatch},
    {SymbolId::DialogLetterboxDrawAlphaStoreAnchor,
     AddressKind::InstructionAnchor, 0x008BB3C3U, kKpmLetterboxPatch},
}};

inline constexpr std::array<KnownAddress, 9> kGogAddresses{{
    {SymbolId::SetCameraMode, AddressKind::Function, 0x00462BC0U,
     kKpmAddressDatabase},
    {SymbolId::GuiManagerPointer, AddressKind::GlobalPointerSlot, 0x00A11BFCU,
     kKpmAddressDatabase},
    {SymbolId::AppManagerPointer, AddressKind::GlobalPointerSlot, 0x00A11C04U,
     kKpmAddressDatabase},
    {SymbolId::RenderGuiFlag, AddressKind::GlobalValue, 0x00A34C00U,
     kKpmAddressDatabase},
    {SymbolId::RenderWireframeFlag, AddressKind::GlobalValue, 0x00A3272CU,
     kKpmAddressDatabase},
    {SymbolId::DialogLetterboxConstructorAnchor, AddressKind::InstructionAnchor,
     0x005AF1AEU, kKpmLetterboxPatch},
    {SymbolId::DialogLetterboxResetFadeAnchor, AddressKind::InstructionAnchor,
     0x005AF793U, kKpmLetterboxPatch},
    {SymbolId::DialogLetterboxSetFadeAnchor, AddressKind::InstructionAnchor,
     0x005AF858U, kKpmLetterboxPatch},
    {SymbolId::DialogLetterboxDrawAlphaStoreAnchor,
     AddressKind::InstructionAnchor, 0x005AFB13U, kKpmLetterboxPatch},
}};

inline constexpr std::array<ExactBuildDescriptor, 2> kBuilds{{
    {
        GameBuildId::SteamAspyr2015Build817494,
        Distribution::Steam,
        SupportLevel::ProbeOnly,
        "KOTOR II Steam Aspyr 2015 (build 817494)",
        DigestLiteral(
            "6A522E71631DCEE93467BD2010F3B23D9145326E1E2E89305F13AB104DBBFFEF"),
        kPeMachineI386,
        0x00400000U,
        {1, 0, 2, 0},
        817494U,
        0x648800ULL,
        kSteamAddresses,
        {// Read-only layout from the extant Voice-of-Old-Republic K2 probe.
         true, 0x04U, 0x04U, 0x18U, 0x40U, 0xA8U, 0xB4U},
    },
    {
        GameBuildId::GogAspyr,
        Distribution::Gog,
        SupportLevel::ProbeOnly,
        "KOTOR II GOG Aspyr",
        DigestLiteral(
            "777BEE235A9E8BDD9863F6741BC3AC54BB6A113B62B1D2E4D12BBE6DB963A914"),
        kPeMachineI386,
        0x00400000U,
        {},
        0U,
        0x648F98ULL,
        kGogAddresses,
        {},
    },
}};

#if defined(_WIN32)

struct PeIdentity {
    std::uint16_t machine{0};
    std::uint32_t preferred_image_base{0};
    std::uint32_t image_size{0};
    std::uint32_t timestamp{0};
    std::uint32_t checksum{0};
    std::uint32_t size_of_headers{0};
    std::uint32_t entry_point{0};
};

[[nodiscard]] bool NtSucceeded(NTSTATUS status) noexcept {
    return status >= 0;
}

[[nodiscard]] bool ReadFileExactly(HANDLE file, std::uint64_t offset,
                                   void* destination,
                                   std::size_t size) noexcept {
    if (file == INVALID_HANDLE_VALUE || destination == nullptr || size == 0 ||
        size > static_cast<std::size_t>(
                   (std::numeric_limits<DWORD>::max)())) {
        return false;
    }

    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        return false;
    }
    DWORD read = 0;
    return ReadFile(file, destination, static_cast<DWORD>(size), &read,
                    nullptr) != FALSE &&
           read == size;
}

[[nodiscard]] bool InspectPeFile(HANDLE file, std::uint64_t file_size,
                                 PeIdentity& output) noexcept {
    IMAGE_DOS_HEADER dos{};
    if (!ReadFileExactly(file, 0, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
        return false;
    }

    const std::uint64_t nt_offset = static_cast<std::uint32_t>(dos.e_lfanew);
    if (nt_offset > file_size ||
        sizeof(IMAGE_NT_HEADERS32) > file_size - nt_offset) {
        return false;
    }

    IMAGE_NT_HEADERS32 nt{};
    if (!ReadFileExactly(file, nt_offset, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.OptionalHeader.SizeOfImage == 0 ||
        nt.OptionalHeader.SizeOfHeaders == 0) {
        return false;
    }

    output.machine = nt.FileHeader.Machine;
    output.preferred_image_base = nt.OptionalHeader.ImageBase;
    output.image_size = nt.OptionalHeader.SizeOfImage;
    output.timestamp = nt.FileHeader.TimeDateStamp;
    output.checksum = nt.OptionalHeader.CheckSum;
    output.size_of_headers = nt.OptionalHeader.SizeOfHeaders;
    output.entry_point = nt.OptionalHeader.AddressOfEntryPoint;
    return true;
}

template <typename Value>
[[nodiscard]] bool SafeReadProcess(std::uintptr_t address,
                                   Value& output) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    if (address == 0) {
        return false;
    }
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<const void*>(address), &output,
                             sizeof(output), &read) != FALSE &&
           read == sizeof(output);
}

[[nodiscard]] bool InspectLoadedMainPe(std::uintptr_t& runtime_base,
                                       PeIdentity& output) noexcept {
    const HMODULE module = GetModuleHandleW(nullptr);
    if (module == nullptr) {
        return false;
    }
    runtime_base = reinterpret_cast<std::uintptr_t>(module);

    IMAGE_DOS_HEADER dos{};
    if (!SafeReadProcess(runtime_base, dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
        return false;
    }
    const std::uintptr_t nt_offset =
        static_cast<std::uint32_t>(dos.e_lfanew);
    if (nt_offset > 1024U * 1024U ||
        runtime_base >
            (std::numeric_limits<std::uintptr_t>::max)() - nt_offset) {
        return false;
    }

    IMAGE_NT_HEADERS32 nt{};
    if (!SafeReadProcess(runtime_base + nt_offset, nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt.OptionalHeader.SizeOfImage == 0 ||
        nt.OptionalHeader.SizeOfHeaders == 0) {
        return false;
    }

    output.machine = nt.FileHeader.Machine;
    output.preferred_image_base = nt.OptionalHeader.ImageBase;
    output.image_size = nt.OptionalHeader.SizeOfImage;
    output.timestamp = nt.FileHeader.TimeDateStamp;
    output.checksum = nt.OptionalHeader.CheckSum;
    output.size_of_headers = nt.OptionalHeader.SizeOfHeaders;
    output.entry_point = nt.OptionalHeader.AddressOfEntryPoint;
    return true;
}

[[nodiscard]] bool SamePeIdentity(const PeIdentity& lhs,
                                  const PeIdentity& rhs) noexcept {
    return lhs.machine == rhs.machine &&
           lhs.preferred_image_base == rhs.preferred_image_base &&
           lhs.image_size == rhs.image_size &&
           lhs.timestamp == rhs.timestamp && lhs.checksum == rhs.checksum &&
           lhs.size_of_headers == rhs.size_of_headers &&
           lhs.entry_point == rhs.entry_point;
}

[[nodiscard]] bool HashOpenFileSha256(HANDLE file,
                                      Sha256Digest& output) noexcept {
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) {
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool success = false;
    try {
        DWORD object_size = 0;
        DWORD result_size = 0;
        DWORD hash_size = 0;
        std::vector<UCHAR> object;
        std::array<UCHAR, 32> digest{};

        do {
            if (!NtSucceeded(BCryptOpenAlgorithmProvider(
                    &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
                break;
            }
            if (!NtSucceeded(BCryptGetProperty(
                    algorithm, BCRYPT_OBJECT_LENGTH,
                    reinterpret_cast<PUCHAR>(&object_size),
                    sizeof(object_size), &result_size, 0)) ||
                object_size == 0) {
                break;
            }
            if (!NtSucceeded(BCryptGetProperty(
                    algorithm, BCRYPT_HASH_LENGTH,
                    reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size),
                    &result_size, 0)) ||
                hash_size != digest.size()) {
                break;
            }

            object.resize(object_size);
            if (!NtSucceeded(BCryptCreateHash(
                    algorithm, &hash, object.data(),
                    static_cast<ULONG>(object.size()), nullptr, 0, 0))) {
                break;
            }

            std::array<UCHAR, 64U * 1024U> buffer{};
            for (;;) {
                DWORD bytes_read = 0;
                if (!ReadFile(file, buffer.data(),
                              static_cast<DWORD>(buffer.size()), &bytes_read,
                              nullptr)) {
                    break;
                }
                if (bytes_read == 0) {
                    if (NtSucceeded(BCryptFinishHash(
                            hash, digest.data(),
                            static_cast<ULONG>(digest.size()), 0))) {
                        std::copy(digest.begin(), digest.end(),
                                  output.bytes.begin());
                        success = true;
                    }
                    break;
                }
                if (!NtSucceeded(
                        BCryptHashData(hash, buffer.data(), bytes_read, 0))) {
                    break;
                }
            }
        } while (false);
    } catch (...) {
        success = false;
    }

    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return success;
}

#endif

} // namespace

VerifiedBuild::VerifiedBuild(const ExactBuildDescriptor* descriptor,
                             std::uintptr_t runtime_image_base,
                             std::uint32_t runtime_image_size) noexcept
    : descriptor_(descriptor), runtime_image_base_(runtime_image_base),
      runtime_image_size_(runtime_image_size) {}

const ExactBuildDescriptor& VerifiedBuild::descriptor() const noexcept {
    return *descriptor_;
}

std::uintptr_t VerifiedBuild::runtime_image_base() const noexcept {
    return runtime_image_base_;
}

std::uint32_t VerifiedBuild::runtime_image_size() const noexcept {
    return runtime_image_size_;
}

std::optional<Sha256Digest> ParseSha256(std::string_view hex) noexcept {
    if (hex.size() != 64) {
        return std::nullopt;
    }

    Sha256Digest result{};
    for (std::size_t index = 0; index < result.bytes.size(); ++index) {
        const int high = HexNibble(hex[index * 2]);
        const int low = HexNibble(hex[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        result.bytes[index] =
            static_cast<std::uint8_t>((high << 4) | low);
    }
    return result;
}

std::array<char, 65> ToHex(const Sha256Digest& digest) noexcept {
    constexpr char digits[] = "0123456789ABCDEF";
    std::array<char, 65> result{};
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        const std::uint8_t value = digest.bytes[index];
        result[index * 2] = digits[value >> 4];
        result[index * 2 + 1] = digits[value & 0x0F];
    }
    result.back() = '\0';
    return result;
}

std::span<const ExactBuildDescriptor> KnownBuilds() noexcept {
    return kBuilds;
}

BuildVerificationResult VerifyMainExecutable() noexcept {
    BuildVerificationResult result{};
#if !defined(_WIN32)
    result.status = BuildVerificationStatus::UnsupportedPlatform;
    return result;
#else
    const HMODULE main_module = GetModuleHandleW(nullptr);
    if (main_module == nullptr) {
        result.status = BuildVerificationStatus::MainModuleUnavailable;
        return result;
    }

    PeIdentity loaded{};
    std::uintptr_t runtime_base = 0;
    if (!InspectLoadedMainPe(runtime_base, loaded)) {
        result.status = BuildVerificationStatus::LoadedImageInspectionFailure;
        return result;
    }
    result.pe_machine = loaded.machine;
    result.runtime_image_size = loaded.image_size;

    std::array<wchar_t, 32768> executable_path{};
    const DWORD path_length = GetModuleFileNameW(
        main_module, executable_path.data(),
        static_cast<DWORD>(executable_path.size()));
    if (path_length == 0 || path_length >= executable_path.size()) {
        result.status = BuildVerificationStatus::ExecutablePathUnavailable;
        return result;
    }

    const HANDLE file = CreateFileW(
        executable_path.data(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.status = BuildVerificationStatus::FileOpenFailure;
        return result;
    }

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart < 0) {
        CloseHandle(file);
        result.status = BuildVerificationStatus::FileInspectionFailure;
        return result;
    }
    result.executable_file_size =
        static_cast<std::uint64_t>(file_size.QuadPart);

    PeIdentity disk{};
    if (!InspectPeFile(file, result.executable_file_size, disk)) {
        CloseHandle(file);
        result.status = BuildVerificationStatus::FileInspectionFailure;
        return result;
    }
    if (!SamePeIdentity(loaded, disk)) {
        CloseHandle(file);
        result.status = BuildVerificationStatus::LoadedImageDoesNotMatchFile;
        return result;
    }
    if (!HashOpenFileSha256(file, result.executable_sha256)) {
        CloseHandle(file);
        result.status = BuildVerificationStatus::HashFailure;
        return result;
    }
    CloseHandle(file);

    const ExactBuildDescriptor* descriptor =
        FindExactBuild(result.executable_sha256, result.pe_machine);
    if (descriptor == nullptr) {
        result.status = BuildVerificationStatus::UnknownBuild;
        return result;
    }
    if (descriptor->executable_file_size != result.executable_file_size ||
        descriptor->preferred_image_base != disk.preferred_image_base ||
        descriptor->pe_machine != loaded.machine) {
        result.status = BuildVerificationStatus::DescriptorMismatch;
        return result;
    }

    result.verified_build =
        VerifiedBuild(descriptor, runtime_base, loaded.image_size);
    result.status = BuildVerificationStatus::Verified;
    return result;
#endif
}

const ExactBuildDescriptor* FindExactBuild(
    const Sha256Digest& executable_sha256,
    std::uint16_t pe_machine) noexcept {
    const auto found = std::find_if(
        kBuilds.begin(), kBuilds.end(), [&](const ExactBuildDescriptor& build) {
            return build.pe_machine == pe_machine &&
                   build.executable_sha256 == executable_sha256;
        });
    return found == kBuilds.end() ? nullptr : &*found;
}

const KnownAddress* FindAddress(const ExactBuildDescriptor& build,
                                SymbolId symbol) noexcept {
    const auto found = std::find_if(
        build.known_addresses.begin(), build.known_addresses.end(),
        [symbol](const KnownAddress& address) { return address.symbol == symbol; });
    return found == build.known_addresses.end() ? nullptr : &*found;
}

std::string_view ToString(GameBuildId id) noexcept {
    switch (id) {
    case GameBuildId::Unknown:
        return "unknown";
    case GameBuildId::SteamAspyr2015Build817494:
        return "steam-aspyr-2015-817494";
    case GameBuildId::GogAspyr:
        return "gog-aspyr";
    }
    return "invalid";
}

std::string_view ToString(SymbolId id) noexcept {
    switch (id) {
    case SymbolId::SetCameraMode:
        return "CClientOptions::SetCameraMode";
    case SymbolId::GuiManagerPointer:
        return "GUI_MANAGER_PTR";
    case SymbolId::AppManagerPointer:
        return "APP_MANAGER_PTR";
    case SymbolId::RenderGuiFlag:
        return "RENDER_GUI";
    case SymbolId::RenderWireframeFlag:
        return "RENDER_WIREFRAME";
    case SymbolId::DialogLetterboxConstructorAnchor:
        return "CSWGuiDialogLetterbox::ctor anchor";
    case SymbolId::DialogLetterboxResetFadeAnchor:
        return "CSWGuiDialogLetterbox::ResetFade anchor";
    case SymbolId::DialogLetterboxSetFadeAnchor:
        return "CSWGuiDialogLetterbox::SetFade anchor";
    case SymbolId::DialogLetterboxDrawAlphaStoreAnchor:
        return "CSWGuiDialogLetterbox::Draw alpha-store anchor";
    }
    return "invalid";
}

std::string_view ToString(BuildVerificationStatus status) noexcept {
    switch (status) {
    case BuildVerificationStatus::Verified:
        return "verified";
    case BuildVerificationStatus::UnsupportedPlatform:
        return "unsupported-platform";
    case BuildVerificationStatus::MainModuleUnavailable:
        return "main-module-unavailable";
    case BuildVerificationStatus::ExecutablePathUnavailable:
        return "executable-path-unavailable";
    case BuildVerificationStatus::FileOpenFailure:
        return "file-open-failure";
    case BuildVerificationStatus::FileInspectionFailure:
        return "file-inspection-failure";
    case BuildVerificationStatus::HashFailure:
        return "hash-failure";
    case BuildVerificationStatus::LoadedImageInspectionFailure:
        return "loaded-image-inspection-failure";
    case BuildVerificationStatus::LoadedImageDoesNotMatchFile:
        return "loaded-image-does-not-match-file";
    case BuildVerificationStatus::UnknownBuild:
        return "unknown-build";
    case BuildVerificationStatus::DescriptorMismatch:
        return "descriptor-mismatch";
    }
    return "invalid";
}

} // namespace k2vr::builds
