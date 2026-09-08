#pragma once

#include <cstdint>

namespace k2vr::game32 {

enum class ControllerInputInstallResult {
    Installed,
    AlreadyInstalled,
    UnsupportedBuild,
    SignatureMismatch,
    ProtectionFailure,
    ModulePinFailure,
};

// Call once on the GAME INPUT/MAIN THREAD, between input polls, outside DllMain,
// after native VR is authorized. Do not call from a concurrent worker: replacing
// five code bytes requires the caller to exclude concurrent game input polls.
// Pins this DLL until process exit; deliberately no unsafe live unload/unhook.
//
// Verified local build: SHA256 6A522E71631DCEE93467BD2010F3B23D9145326E1E2E89305F13AB104DBBFFEF.
// EXE imports DINPUT8!DirectInput8Create, no XInput or SDL imports/strings.
// 00730A91 selects DIDATAFORMAT 009B9124: cbData=0x110 (DIJOYSTATE2).
// 007314AB pushes 0x110. At 007314D1: 8B 42 24 FF D0 invokes
// IDirectInputDevice8::GetDeviceState (vtable slot 9), stdcall(this,size,out).
// 00731691..0073173E then compares rgbButtons (state+0x30) and emits events.
// Only this game call site is redirected; the COM vtable, system XInput and
// common/vr_input raw polling are untouched. No keyboard/mouse emulation.
//
// Profile: Windows XInput-backed HID devices (IG_ path), standard Xbox DI
// buttons A/B/X/Y=0..3, View=6, Menu=7, Dpad=POV0. Other devices pass through.
// Standalone View emits one state-poll press on release, then a release next
// poll. View chords consume their members until physical release. If a partner
// was pressed BEFORE View, its earlier game press cannot be retroactively hidden.
[[nodiscard]] ControllerInputInstallResult InstallControllerInputFilter() noexcept;

struct ControllerInputDiagnostics {
    std::uint64_t polls{};
    std::uint64_t filtered_polls{};
    std::uint64_t standalone_view_taps{};
    std::uint64_t unsupported_device_polls{};
    std::uint64_t failed_reads{};
};
[[nodiscard]] ControllerInputDiagnostics GetControllerInputDiagnostics() noexcept;

} // namespace k2vr::game32
