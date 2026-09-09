// Explicit, standalone x86 CPU test. Loads the user-supplied, hash-pinned Bink
// DLL and calls only its summary API with a synthetic statistics state. It does
// not open a movie, decoder, game, window, audio device, GPU or OpenXR session.
#include "bink_summary_guard.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
static_assert(sizeof(void*) == 4, "Build this fixture for x86 only.");

using namespace k2vr::game32::bink;

namespace {
constexpr std::array<unsigned char, 32> kExpectedHash{
    0xD9,0x63,0x11,0x2E,0xA8,0x54,0x5C,0x8A,0xAA,0x6B,0xD4,0x8A,0x9D,0x2D,0x22,0x96,
    0x05,0x80,0x6A,0xE1,0xBB,0xC0,0x3F,0x90,0xB0,0x10,0x6D,0xB3,0x03,0x44,0x66,0xED};

bool HashMatches(HANDLE file) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024) return false;
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD read{};
    if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) return false;
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    std::array<unsigned char, 32> hash{};
    const auto status = BCryptHash(algorithm, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()),
        hash.data(), static_cast<ULONG>(hash.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status >= 0 && hash == kExpectedHash;
}

struct Observation {
    bool returned{};
    bool recovered{};
    std::uint32_t exception_code{};
    std::uintptr_t exception_address{};
};

int ObserveFault(const EXCEPTION_POINTERS* information, Observation* output) noexcept {
    output->exception_code = information->ExceptionRecord->ExceptionCode;
    output->exception_address = reinterpret_cast<std::uintptr_t>(information->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER; // Test observer only, never production recovery.
}

Observation CallOriginal(GetSummaryFunction original, void* state, void* output) {
    Observation result{};
    __try {
        original(state, output);
        result.returned = true;
    } __except (ObserveFault(GetExceptionInformation(), &result)) {}
    return result;
}

Observation CallGuard(GetSummaryFunction original, void* state, void* output, const std::uintptr_t base) {
    Observation result{};
    __try {
        result.recovered = GetSummaryGuarded(original, state, output, base);
        result.returned = true;
    } __except (ObserveFault(GetExceptionInformation(), &result)) {}
    return result;
}

// Disassembly-backed minimal storage through the highest summary field +0x30C.
// Zero +0x27C skips pending-time bookkeeping. Zero +0x28C makes helper RVA
// 0x11E60 return immediately. The later successful path needs [state+0x10C]
// readable, nonzero frame count and nonzero frame-rate divisor.
struct Fixture {
    std::array<std::uint32_t, 0x310 / 4> state{};
    std::uint32_t first_frame_offset{};
    std::array<std::uint32_t, 33> output{};
    void Reset() {
        state.fill(0);
        first_frame_offset = 0;
        state[0x000 / 4] = 320;
        state[0x004 / 4] = 180;
        state[0x008 / 4] = 1; // frames, denominator of RVA 0x122F9
        state[0x014 / 4] = 30;
        state[0x018 / 4] = 1;
        state[0x028 / 4] = 2;
        state[0x10C / 4] = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&first_frame_offset));
        state[0x134 / 4] = 1;
        state[0x13C / 4] = 0; // divisor increments to 1 at RVA 0x12277
        state[0x2BC / 4] = 1;
        state[0x2C0 / 4] = 1; // later product denominator frames * frame-rate divisor
        output.fill(0xA5A5A5A5U);
        output.front() = 0x13579BDFU;
        output.back() = 0x2468ACE0U;
    }
    void* Summary() { return output.data() + 1; }
    bool CanariesIntact() const { return output.front() == 0x13579BDFU && output.back() == 0x2468ACE0U; }
    bool SummaryZero() const { return std::all_of(output.begin() + 1, output.end() - 1, [](auto v) { return v == 0; }); }
};

int RunChecks(GetSummaryFunction original, const std::uintptr_t base) {
    unsigned failures{};
    const auto check = [&](const bool passed, const char* label, const Observation& observation) {
        std::cout << (passed ? "PASS: " : "FAIL: ") << label;
        if (observation.exception_code != 0) {
            std::cout << " code=0x" << std::hex << observation.exception_code
                << " rva=0x" << (observation.exception_address - base) << std::dec;
        }
        std::cout << '\n';
        if (!passed) ++failures;
    };
    Fixture fixture;
    fixture.Reset();
    auto result = CallGuard(original, fixture.state.data(), fixture.Summary(), base);
    check(result.returned && !result.recovered && fixture.output[1] == 320 && fixture.output[2] == 180 &&
        fixture.output[1 + 0x4C / 4] == 1000 && fixture.CanariesIntact(),
        "normal original result preserved", result);

    fixture.Reset();
    fixture.state[0x134 / 4] = UINT32_MAX;
    result = CallOriginal(original, fixture.state.data(), fixture.Summary());
    check(!result.returned && result.exception_code == kIntegerOverflowException &&
        result.exception_address == base + kSummaryOverflowRva && !fixture.SummaryZero() && fixture.CanariesIntact(),
        "unguarded real DLL reproduces the exact reported overflow", result);

    fixture.Reset();
    fixture.state[0x134 / 4] = UINT32_MAX;
    result = CallGuard(original, fixture.state.data(), fixture.Summary(), base);
    check(result.returned && result.recovered && fixture.SummaryZero() && fixture.CanariesIntact(),
        "guard recovers and zeroes exactly 124 bytes", result);

    fixture.Reset();
    fixture.state[0x134 / 4] = UINT32_MAX;
    result = CallGuard(original, fixture.state.data(), fixture.Summary(), 0);
    check(!result.returned && result.exception_code == kIntegerOverflowException &&
        result.exception_address == base + kSummaryOverflowRva,
        "unvalidated DLL base does not recover", result);

    fixture.Reset();
    fixture.state[0x13C / 4] = UINT32_MAX; // inc wraps divisor to zero
    result = CallGuard(original, fixture.state.data(), fixture.Summary(), base);
    check(!result.returned && result.exception_code == EXCEPTION_INT_DIVIDE_BY_ZERO &&
        result.exception_address == base + kSummaryOverflowRva,
        "division by zero at the same instruction propagates", result);

    fixture.Reset();
    fixture.state[0x028 / 4] = UINT32_MAX;
    fixture.state[0x2BC / 4] = 2; // later numerator UINT32_MAX*2 divided by 1
    result = CallGuard(original, fixture.state.data(), fixture.Summary(), base);
    check(!result.returned && result.exception_code == kIntegerOverflowException &&
        result.exception_address == base + 0x122F0U,
        "overflow at a different summary instruction propagates", result);
    return failures == 0 ? 0 : 1;
}
} // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc != 2) {
        std::cerr << "Usage: bink_summary_live_smoke ABSOLUTE_PATH_TO_BINKW32_DLL\n";
        return 2;
    }
    // Explicit full path; do not load a Bink DLL discovered through PATH.
    if (GetModuleHandleW(L"binkw32.dll") != nullptr || std::wcslen(argv[1]) < 3 || argv[1][1] != L':' ||
        (argv[1][2] != L'\\' && argv[1][2] != L'/')) {
        std::cerr << "Require an explicit absolute DLL path and no previously loaded Bink module.\n";
        return 2;
    }
    const auto file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { std::cerr << "Cannot lock DLL for read-only hash validation.\n"; return 2; }
    if (!HashMatches(file)) { CloseHandle(file); std::cerr << "Unsupported Bink DLL hash. Nothing loaded.\n"; return 2; }
    // Keep the locked original file open until unloading. No write/delete sharing.
    const auto module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) { CloseHandle(file); std::cerr << "Cannot load the validated Bink DLL.\n"; return 2; }
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto address = GetProcAddress(module, "_BinkGetSummary@8");
    GetSummaryFunction original{};
    static_assert(sizeof(original) == sizeof(address));
    std::memcpy(&original, &address, sizeof(original));
    constexpr std::array<unsigned char, 12> fault_bytes{0xF7,0xE1,0x8B,0x4C,0x24,0x14,0xF7,0xF1,0x89,0x43,0x4C,0x8B};
    int result = 2;
    if (reinterpret_cast<std::uintptr_t>(original) == base + kSummaryEntryRva &&
        std::memcmp(reinterpret_cast<const void*>(base + 0x12290U), fault_bytes.data(), fault_bytes.size()) == 0) {
        result = RunChecks(original, base);
    } else {
        std::cerr << "Loaded DLL export/instruction identity mismatch.\n";
    }
    FreeLibrary(module);
    CloseHandle(file);
    return result;
}
