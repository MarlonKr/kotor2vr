#include "gl_ext_d3d12_bridge.hpp"

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

    Check(kGlExtD3D12GenericAllAccess == 0x10000000U,
          "the named D3D12 handle contract uses GENERIC_ALL");
    Check(kGlExtD3D12RequiredExtensionMask == 0xFU,
          "all four external-object extensions are required");
    Check(kGlExtD3D12StreamSlotCount == 3U,
          "the primary interop stream exposes three color slots");

    Check(!IsUsableGlExtD3D12ProcAddressValue(0U) &&
              !IsUsableGlExtD3D12ProcAddressValue(1U) &&
              !IsUsableGlExtD3D12ProcAddressValue(2U) &&
              !IsUsableGlExtD3D12ProcAddressValue(3U),
          "WGL null and small failure sentinels are rejected");
    Check(!IsUsableGlExtD3D12ProcAddressValue(
              static_cast<std::uintptr_t>(-1)),
          "the WGL minus-one failure sentinel is rejected");
    Check(IsUsableGlExtD3D12ProcAddressValue(0x1000U),
          "an ordinary procedure address is accepted");

    constexpr std::string_view extensions =
        "GL_EXT_memory_object GL_EXT_memory_object_win32 "
        "GL_EXT_semaphore GL_EXT_semaphore_win32";
    Check(HasExactGlExtD3D12ExtensionToken(
              extensions, "GL_EXT_memory_object_win32"),
          "an exact extension token is found");
    Check(!HasExactGlExtD3D12ExtensionToken(
              extensions, "GL_EXT_memory_object_win") &&
              !HasExactGlExtD3D12ExtensionToken(extensions, "") &&
              !HasExactGlExtD3D12ExtensionToken(
                  extensions, "GL_EXT_memory_object extra"),
          "extension prefixes and invalid requests are rejected");

    constexpr GlExtD3D12AdapterLuid expected{0x11223344U, 0x55667788};
    Check(IsValidGlExtD3D12AdapterLuid(expected),
          "a nonzero adapter LUID is valid");
    Check(!IsValidGlExtD3D12AdapterLuid({}),
          "an empty adapter LUID is invalid");
    Check(SameGlExtD3D12AdapterLuid(expected, expected) &&
              !SameGlExtD3D12AdapterLuid(
                  expected, {0x11223345U, 0x55667788}),
          "adapter LUID equality compares both halves");

    Check(ValidateGlExtD3D12TextureDescription({1U, 1U}) ==
              GlExtD3D12Status::Ok,
          "a small RGBA8 texture is valid");
    Check(ValidateGlExtD3D12TextureDescription({3840U, 2160U}) ==
              GlExtD3D12Status::Ok,
          "a 4K RGBA8 texture is valid");
    Check(ValidateGlExtD3D12TextureDescription({0U, 1U}) ==
              GlExtD3D12Status::InvalidTextureDescription,
          "zero dimensions are rejected");
    Check(ValidateGlExtD3D12TextureDescription(
              {kGlExtD3D12MaximumDimension + 1U, 1U}) ==
              GlExtD3D12Status::InvalidTextureDescription,
          "oversized textures are rejected");
    Check(ValidateGlExtD3D12TextureDescription({16U, 16U, 87U}) ==
              GlExtD3D12Status::InvalidTextureDescription,
          "unimplemented DXGI formats fail closed");

    Check(IsValidGlExtD3D12SharedObjectName(
              L"Local\\Kotor2VR-gpu-stream-v1-0123-color"),
          "a bounded Local object name is accepted");
    const std::wstring oversized_name(128U, L'x');
    Check(!IsValidGlExtD3D12SharedObjectName(L"") &&
              !IsValidGlExtD3D12SharedObjectName(oversized_name),
          "empty and oversized object names are rejected");
    constexpr wchar_t embedded_zero[]{L'a', L'\0', L'b'};
    Check(!IsValidGlExtD3D12SharedObjectName(
              std::wstring_view(embedded_zero, 3U)),
          "embedded nulls are rejected");

    Check(IsValidGlExtD3D12StreamSlot(0U, 1U) &&
              !IsValidGlExtD3D12StreamSlot(1U, 1U),
          "the legacy stream exposes only slot zero");
    Check(IsValidGlExtD3D12StreamSlot(0U, 3U) &&
              IsValidGlExtD3D12StreamSlot(1U, 3U) &&
              IsValidGlExtD3D12StreamSlot(2U, 3U) &&
              !IsValidGlExtD3D12StreamSlot(3U, 3U),
          "the ring exposes exactly three slots");
    Check(!IsValidGlExtD3D12StreamSlot(0U, 0U) &&
              !IsValidGlExtD3D12StreamSlot(0U, 4U),
          "empty and oversized slot sets fail closed");

    constexpr GlExtD3D12RingObjectNames distinct_ring_names{
        {L"Local\\Kotor2VR-color-0", L"Local\\Kotor2VR-color-1",
         L"Local\\Kotor2VR-color-2"},
        L"Local\\Kotor2VR-ready", L"Local\\Kotor2VR-consumed"};
    constexpr GlExtD3D12RingObjectNames duplicate_color_names{
        {L"Local\\Kotor2VR-color-0", L"Local\\Kotor2VR-color-1",
         L"Local\\Kotor2VR-color-0"},
        L"Local\\Kotor2VR-ready", L"Local\\Kotor2VR-consumed"};
    constexpr GlExtD3D12RingObjectNames color_fence_collision{
        {L"Local\\Kotor2VR-color-0", L"Local\\Kotor2VR-ready",
         L"Local\\Kotor2VR-color-2"},
        L"Local\\Kotor2VR-ready", L"Local\\Kotor2VR-consumed"};
    constexpr GlExtD3D12RingObjectNames duplicate_fence_names{
        {L"Local\\Kotor2VR-color-0", L"Local\\Kotor2VR-color-1",
         L"Local\\Kotor2VR-color-2"},
        L"Local\\Kotor2VR-fence", L"Local\\Kotor2VR-fence"};
    Check(AreDistinctGlExtD3D12RingObjectNames(distinct_ring_names),
          "all three colors and both fences can have distinct names");
    Check(!AreDistinctGlExtD3D12RingObjectNames(duplicate_color_names) &&
              !AreDistinctGlExtD3D12RingObjectNames(
                  color_fence_collision) &&
              !AreDistinctGlExtD3D12RingObjectNames(
                  duplicate_fence_names),
          "duplicate color or fence names fail closed");

    Check(CanSignalGlExtD3D12StreamSlot(0U, 0U),
          "an unused color slot is immediately writable");
    Check(!CanSignalGlExtD3D12StreamSlot(7U, 6U),
          "a ready slot cannot be overwritten before consumption");
    Check(CanSignalGlExtD3D12StreamSlot(7U, 7U) &&
              CanSignalGlExtD3D12StreamSlot(7U, 9U),
          "an exactly or cumulatively consumed slot can be reused");

    Check(DecideGlExtD3D12CreateStream(false, false, false) ==
              GlExtD3D12Status::NotInitialized,
          "stream creation requires initialization");
    Check(DecideGlExtD3D12CreateStream(true, true, true) ==
              GlExtD3D12Status::StreamAlreadyCreated,
          "a second stream is rejected");
    Check(DecideGlExtD3D12CreateStream(true, false, false) ==
              GlExtD3D12Status::ContextMismatch,
          "stream creation requires the owning GL context");
    Check(DecideGlExtD3D12CreateStream(true, false, true) ==
              GlExtD3D12Status::Ok,
          "an initialized bridge on its context can create a stream");

    Check(DecideGlExtD3D12SemaphoreOperation(
              false, false, false, 1U, 0U) ==
              GlExtD3D12Status::NotInitialized,
          "semaphore use requires initialization");
    Check(DecideGlExtD3D12SemaphoreOperation(
              true, false, true, 1U, 0U) ==
              GlExtD3D12Status::StreamNotCreated,
          "semaphore use requires a complete stream");
    Check(DecideGlExtD3D12SemaphoreOperation(
              true, true, false, 1U, 0U) ==
              GlExtD3D12Status::ContextMismatch,
          "semaphore use requires the owning GL context");
    Check(DecideGlExtD3D12SemaphoreOperation(
              true, true, true, 0U, 0U) ==
              GlExtD3D12Status::InvalidFenceValue,
          "zero is not a frame fence value");
    Check(DecideGlExtD3D12SemaphoreOperation(
              true, true, true, 9U, 9U) ==
              GlExtD3D12Status::InvalidFenceValue,
          "duplicate fence operations are rejected");
    Check(DecideGlExtD3D12SemaphoreOperation(
              true, true, true, 10U, 9U) ==
              GlExtD3D12Status::Ok,
          "strictly increasing fence values are accepted");

    if (failures == 0) {
        std::cout << "GL_EXT/D3D12 bridge decision tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
