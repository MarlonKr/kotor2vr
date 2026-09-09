#pragma once

#include <cstddef>
#include <cstdint>

namespace k2vr::game32::bink {

inline constexpr std::size_t kSummaryBytes = 31U * sizeof(std::uint32_t);
inline constexpr std::uintptr_t kSummaryEntryRva = 0x12190U;
inline constexpr std::uintptr_t kSummaryOverflowRva = 0x12296U;
inline constexpr std::uint32_t kIntegerOverflowException = 0xC0000095U;

// The caller must establish the supported DLL's identity and instruction bytes
// before supplying a nonzero module base. A base alone is not authentication.
// Also bind recovery to the original export, not another function at that base.
[[nodiscard]] constexpr bool IsKnownSummaryOverflow(
    const std::uint32_t exception_code,
    const std::uintptr_t exception_address,
    const std::uintptr_t validated_module_base,
    const std::uintptr_t original_function_address) noexcept {
    return validated_module_base != 0 &&
           validated_module_base <= UINTPTR_MAX - kSummaryOverflowRva &&
           original_function_address == validated_module_base + kSummaryEntryRva &&
           exception_code == kIntegerOverflowException &&
           exception_address == validated_module_base + kSummaryOverflowRva;
}

#if defined(_WIN32) && defined(_MSC_VER)
using GetSummaryFunction = void(__stdcall*)(void* bink, void* summary);

// Calls the original statistics routine. Returns true only after recovering the
// exact known integer overflow and clearing all 124 output bytes. Returns false
// on an ordinary return, without changing the original result. Every other SEH
// exception continues searching; an invalid output pointer is not suppressed.
// No noexcept: this wrapper must not turn unrelated exceptions into termination.
[[nodiscard]] bool GetSummaryGuarded(
    GetSummaryFunction original,
    void* bink,
    void* summary,
    std::uintptr_t validated_module_base);
#endif

} // namespace k2vr::game32::bink
