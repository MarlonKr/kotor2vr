#include "bink_summary_guard.hpp"

#include <cstdint>
#include <iostream>

using namespace k2vr::game32::bink;

int main() {
    unsigned failures = 0;
    const auto check = [&](const bool condition, const char* label) {
        if (!condition) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
    };
    constexpr std::uintptr_t base = 0x30000000U;
    constexpr auto fault = base + kSummaryOverflowRva;
    constexpr auto entry = base + kSummaryEntryRva;
    check(kSummaryBytes == 124, "summary has exactly 31 DWORDs");
    check(IsKnownSummaryOverflow(kIntegerOverflowException, fault, base, entry), "confirmed overflow matches");
    check(!IsKnownSummaryOverflow(0xC0000094U, fault, base, entry), "divide by zero at same instruction propagates");
    check(!IsKnownSummaryOverflow(0xC0000005U, fault, base, entry), "access violation propagates");
    check(!IsKnownSummaryOverflow(0xE06D7363U, fault, base, entry), "C++ exception propagates");
    check(!IsKnownSummaryOverflow(kIntegerOverflowException, fault + 1, base, entry), "adjacent instruction rejected");
    check(!IsKnownSummaryOverflow(kIntegerOverflowException, base + 0x122F0U, base, entry), "later summary overflow rejected");
    check(!IsKnownSummaryOverflow(kIntegerOverflowException, fault, 0, entry), "unvalidated module rejected");
    check(!IsKnownSummaryOverflow(kIntegerOverflowException, fault, base, entry + 1), "wrong original function rejected");
    check(!IsKnownSummaryOverflow(kIntegerOverflowException, fault, base, 0), "null original function rejected");
    check(!IsKnownSummaryOverflow(kIntegerOverflowException, fault, base + 0x1000, entry), "wrong module base rejected");
    constexpr std::uintptr_t relocated = 0x41000000U;
    check(IsKnownSummaryOverflow(kIntegerOverflowException, relocated + kSummaryOverflowRva,
        relocated, relocated + kSummaryEntryRva), "validated relocation is supported");
    check(!IsKnownSummaryOverflow(kIntegerOverflowException, kSummaryOverflowRva - 1,
        UINTPTR_MAX, kSummaryEntryRva - 1), "address wrapping rejected");
    if (failures != 0) return 1;
    std::cout << "Bink summary filter: 13 checks passed. No Bink DLL or game was loaded.\n";
    return 0;
}
