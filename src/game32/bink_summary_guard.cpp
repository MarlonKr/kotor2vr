#include "bink_summary_guard.hpp"

#if !defined(_WIN32) || !defined(_MSC_VER)
#error The Bink summary runtime guard requires Windows MSVC structured exceptions.
#endif

#include <Windows.h>
#include <cstring>

namespace k2vr::game32::bink {
namespace {

int FilterSummaryException(
    const EXCEPTION_POINTERS* information,
    const std::uintptr_t validated_module_base,
    const GetSummaryFunction original) noexcept {
    if (information == nullptr || information->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto* record = information->ExceptionRecord;
    return IsKnownSummaryOverflow(
        record->ExceptionCode,
        reinterpret_cast<std::uintptr_t>(record->ExceptionAddress),
        validated_module_base,
        reinterpret_cast<std::uintptr_t>(original))
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

bool GetSummaryGuarded(
    const GetSummaryFunction original,
    void* const bink,
    void* const summary,
    const std::uintptr_t validated_module_base) {
    // Keep this SEH frame free of objects that require C++ unwinding (/EHsc).
    __try {
        original(bink, summary);
    } __except (FilterSummaryException(GetExceptionInformation(), validated_module_base, original)) {
        // The original routine may already have populated some fields. Publishing
        // a partial summary would be misleading. A bad pointer faults normally;
        // this write is outside the protected original-call body.
        std::memset(summary, 0, kSummaryBytes);
        return true;
    }
    return false;
}

} // namespace k2vr::game32::bink
