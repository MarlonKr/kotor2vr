#include "nv_dx_interop_bridge.hpp"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(const bool condition, const char* const message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

} // namespace

int main() {
    using namespace k2vr::game32;

    Check(!IsUsableWglProcAddressValue(0U),
          "nullptr is not a usable WGL procedure");
    Check(!IsUsableWglProcAddressValue(1U) &&
              !IsUsableWglProcAddressValue(2U) &&
              !IsUsableWglProcAddressValue(3U),
          "documented WGL failure sentinels are rejected");
    Check(!IsUsableWglProcAddressValue(
              static_cast<std::uintptr_t>(-1)),
          "minus-one WGL failure sentinel is rejected");
    Check(IsUsableWglProcAddressValue(0x1000U),
          "ordinary procedure addresses are accepted");

    constexpr std::string_view extensions =
        "WGL_ARB_extensions_string WGL_NV_DX_interop "
        "WGL_NV_DX_interop2 WGL_EXT_swap_control";
    Check(HasExactExtensionToken(extensions, "WGL_NV_DX_interop2"),
          "exact extension token is found");
    Check(!HasExactExtensionToken(extensions, "WGL_NV_DX_interop" "2x"),
          "extension prefix is not accepted as a token");
    Check(!HasExactExtensionToken(extensions, "") &&
              !HasExactExtensionToken(extensions,
                                      "WGL_NV_DX_interop2 extra"),
          "empty and multi-token extension requests are rejected");

    constexpr NvDxAdapterLuid expected{0x11223344U, 0x55667788};
    constexpr NvDxAdapterCandidate matching{
        kNvidiaPciVendorId, 0U, expected};
    constexpr NvDxAdapterCandidate other_nvidia{
        kNvidiaPciVendorId, 0U, {0xAABBCCDDU, 0x12345678}};
    constexpr NvDxAdapterCandidate software{
        kNvidiaPciVendorId, kDxgiAdapterSoftwareFlag, expected};
    constexpr NvDxAdapterCandidate amd{0x1002U, 0U, expected};
    Check(IsEligibleNvDxAdapter(matching),
          "hardware NVIDIA adapter is eligible without a LUID contract");
    Check(IsEligibleNvDxAdapter(matching, expected),
          "exact requested LUID is eligible");
    Check(!IsEligibleNvDxAdapter(other_nvidia, expected),
          "non-matching NVIDIA LUID does not silently fall back");
    Check(!IsEligibleNvDxAdapter(software, expected) &&
              !IsEligibleNvDxAdapter(amd, expected),
          "software and non-NVIDIA adapters are rejected");

    Check(ValidateNvDxTextureDescription({1U, 1U}) ==
              NvDxInteropStatus::Ok,
          "small RGBA8 texture is valid");
    Check(ValidateNvDxTextureDescription({3840U, 2160U}) ==
              NvDxInteropStatus::Ok,
          "4K RGBA8 texture is valid");
    Check(ValidateNvDxTextureDescription({0U, 2160U}) ==
              NvDxInteropStatus::InvalidTextureDescription,
          "zero width is rejected");
    Check(ValidateNvDxTextureDescription(
              {kNvDxInteropMaximumDimension + 1U, 1U}) ==
              NvDxInteropStatus::InvalidTextureDescription,
          "oversized texture is rejected");
    Check(ValidateNvDxTextureDescription({16U, 16U, 87U}) ==
              NvDxInteropStatus::InvalidTextureDescription,
          "unimplemented DXGI formats fail closed");
    Check(ValidateNvDxTextureDescription(
              {16U, 16U, kDxgiFormatR8G8B8A8Unorm,
               static_cast<NvDxInteropAccess>(99U)}) ==
              NvDxInteropStatus::InvalidTextureDescription,
          "invalid WGL access enum fails closed");
    Check(IsValidNvDxSharedObjectName(
              L"Local\\Kotor2VR-gpu-stream-v1-0123-color"),
          "bounded Local shared object name is accepted");
    const std::wstring oversized_name(128U, L'x');
    Check(!IsValidNvDxSharedObjectName(L"") &&
              !IsValidNvDxSharedObjectName(oversized_name),
          "empty and oversized shared object names are rejected");
    constexpr wchar_t embedded_zero[]{L'a', L'\0', L'b'};
    Check(!IsValidNvDxSharedObjectName(
              std::wstring_view(embedded_zero, 3U)),
          "embedded null in a shared object name is rejected");

    Check(DecideNvDxLock(false, false, false, false) ==
              NvDxInteropStatus::NotInitialized,
          "lock requires initialization");
    Check(DecideNvDxLock(true, false, true, false) ==
              NvDxInteropStatus::TextureNotRegistered,
          "lock requires a registered texture");
    Check(DecideNvDxLock(true, true, false, false) ==
              NvDxInteropStatus::ContextMismatch,
          "lock requires the owning WGL context");
    Check(DecideNvDxLock(true, true, true, true) ==
              NvDxInteropStatus::AlreadyLocked,
          "double lock is rejected");
    Check(DecideNvDxLock(true, true, true, false) ==
              NvDxInteropStatus::Ok,
          "ready unlocked texture may be locked");

    Check(DecideNvDxUnlock(true, true, true, false) ==
              NvDxInteropStatus::NotLocked,
          "unlock of an unlocked object is rejected");
    Check(DecideNvDxUnlock(true, true, false, true) ==
              NvDxInteropStatus::ContextMismatch,
          "unlock requires the owning WGL context");
    Check(DecideNvDxUnlock(true, true, true, true) ==
              NvDxInteropStatus::Ok,
          "locked texture may be unlocked");

    if (failures == 0) {
        std::cout << "NV_DX_interop decision tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
