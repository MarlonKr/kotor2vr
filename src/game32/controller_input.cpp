#include "controller_input.hpp"
#include "controller_input_filter.hpp"
#include "build_descriptor.hpp"
#include "probe.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTINPUT_VERSION 0x0800
#include <Windows.h>
#include <dinput.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cwctype>

static_assert(sizeof(void*) == 4, "The qualified controller call site is x86 only");
static_assert(sizeof(DIJOYSTATE2) == 0x110);
static_assert(offsetof(DIJOYSTATE2, rgbButtons) == 0x30);
static_assert(offsetof(DIJOYSTATE2, rgdwPOV) == 0x20);

namespace k2vr::game32 {
namespace {
constexpr std::uintptr_t kCallRva = 0x003314D1;
// Whole instruction window from buffer setup to HRESULT test; no absolute
// addresses/relocations. This proves argument size, receiver and continuation.
constexpr std::array<unsigned char, 69> kCallContext{
    0x8D,0x8D,0xE8,0xFE,0xFF,0xFF,0x51,0x68,0x10,0x01,0x00,0x00,
    0x8B,0x95,0xAC,0xFE,0xFF,0xFF,0x8B,0x42,0x2C,0x8B,0x4D,0x08,
    0x8B,0x14,0x88,0x8B,0x85,0xAC,0xFE,0xFF,0xFF,0x8B,0x48,0x2C,
    0x8B,0x45,0x08,0x8B,0x0C,0x81,0x8B,0x12,0x51,0x8B,0x42,0x24,
    0xFF,0xD0,0x89,0x85,0xE4,0xFE,0xFF,0xFF,0x83,0xBD,0xE4,0xFE,
    0xFF,0xFF,0x00,0x0F,0x84,0x02,0x01,0x00,0x00};
constexpr std::uintptr_t kContextRva = 0x003314A4;

struct DeviceState {
    IDirectInputDevice8A* device{}; // borrowed only; never dereferenced after poll
    GUID instance{};
    ULONGLONG last_poll{};
    bool has_identity{};
    bool supported{};
    controller::ViewChordFilter filter;
};
std::array<DeviceState, 16> g_devices{};
SRWLOCK g_lock = SRWLOCK_INIT;
bool g_installed{};
std::atomic<std::uint64_t> g_polls{},g_filtered{},g_taps{},g_unsupported{},g_failed{};

struct ExclusiveLock {
    ExclusiveLock() noexcept { AcquireSRWLockExclusive(&g_lock); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&g_lock); }
};

DeviceState& FindDevice(IDirectInputDevice8A* device) noexcept {
    for (auto& slot : g_devices) if (slot.device == device) return slot;
    auto* available = &g_devices.front();
    for (auto& slot : g_devices) {
        if (!slot.device) { available = &slot; break; }
        if (slot.last_poll < available->last_poll) available = &slot;
    }
    *available = {};
    available->device = device;
    return *available;
}

bool IsXboxDirectInputDevice(IDirectInputDevice8A* device) noexcept {
    // Do not apply Xbox button numbers to arbitrary DirectInput/SDL pads.
    DIPROPGUIDANDPATH path{};
    path.diph.dwSize = sizeof(path);
    path.diph.dwHeaderSize = sizeof(path.diph);
    path.diph.dwHow = DIPH_DEVICE;
    if (FAILED(device->GetProperty(DIPROP_GUIDANDPATH, &path.diph))) return false;
    bool xinput_path = false;
    for (std::size_t i=0; i+2<std::size(path.wszPath) && path.wszPath[i]; ++i) {
        if (std::towupper(path.wszPath[i]) == L'I' &&
            std::towupper(path.wszPath[i+1]) == L'G' && path.wszPath[i+2] == L'_') {
            xinput_path = true; break;
        }
    }
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    return xinput_path && SUCCEEDED(device->GetCapabilities(&caps)) &&
           caps.dwButtons >= 10 && caps.dwPOVs >= 1;
}

HRESULT WINAPI FilteredGetDeviceState(IDirectInputDevice8A* device,
                                      DWORD size, LPVOID output) noexcept {
    // Original COM method remains unchanged, including HRESULT and analog axes.
    const HRESULT result = device->GetDeviceState(size, output);
    g_polls.fetch_add(1, std::memory_order_relaxed);
    ExclusiveLock lock;
    auto& slot = FindDevice(device);
    const auto now = GetTickCount64();
    if (FAILED(result) || size != sizeof(DIJOYSTATE2) || !output) {
        slot.filter.Disconnect();
        slot.has_identity = false;
        slot.last_poll = now;
        g_failed.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    DIDEVICEINSTANCEA info{};
    info.dwSize = sizeof(info);
    if (FAILED(device->GetDeviceInfo(&info))) {
        slot.filter.Disconnect();
        slot.has_identity = false;
        g_unsupported.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    if (!slot.has_identity || !IsEqualGUID(slot.instance, info.guidInstance) ||
        now - slot.last_poll > 2000) {
        slot.filter.Disconnect();
        slot.instance = info.guidInstance;
        slot.has_identity = true;
        slot.supported = IsXboxDirectInputDevice(device);
        (void)AppendPersistentProbeLogLine(slot.supported ?
            "controller-input-device xbox-directinput=1" : "controller-input-device xbox-directinput=0 passthrough=1");
    }
    slot.last_poll = now;
    if (!slot.supported) {
        g_unsupported.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    auto& state = *static_cast<DIJOYSTATE2*>(output);
    std::uint32_t raw{};
    for (unsigned i=0; i<16; ++i) if (state.rgbButtons[i] & 0x80) raw |= 1U << i;
    if (controller::PovHasDown(state.rgdwPOV[0])) raw |= controller::kDown;
    const auto filtered = slot.filter.Apply(raw);
    for (unsigned i=0; i<16; ++i) {
        if ((raw ^ filtered.buttons) & (1U << i))
            state.rgbButtons[i] = (filtered.buttons & (1U << i)) ? 0x80 : 0;
    }
    // A consumed down diagonal suppresses the whole hat until it leaves down;
    // it must not turn into an unintended horizontal menu navigation event.
    if ((raw & controller::kDown) && !(filtered.buttons & controller::kDown))
        state.rgdwPOV[0] = 0xFFFFFFFFU;
    if (filtered.buttons != raw) g_filtered.fetch_add(1, std::memory_order_relaxed);
    if (filtered.standalone_view_tap) g_taps.fetch_add(1, std::memory_order_relaxed);
    static ULONGLONG next_log{};
    if (now>=next_log) {
        next_log=now+5000;
        char line[180]{};
        std::snprintf(line,sizeof(line),"controller-input polls=%llu filtered=%llu standalone_taps=%llu",
            static_cast<unsigned long long>(g_polls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_filtered.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_taps.load(std::memory_order_relaxed)));
        (void)AppendPersistentProbeLogLine(line);
    }
    return result;
}
} // namespace

ControllerInputInstallResult InstallControllerInputFilter() noexcept {
    ExclusiveLock lock;
    if (g_installed) return ControllerInputInstallResult::AlreadyInstalled;
    const auto verification = builds::VerifyMainExecutable();
    if (!verification.IsVerified() || verification.verified_build->descriptor().id !=
        builds::GameBuildId::SteamAspyr2015Build817494)
        return ControllerInputInstallResult::UnsupportedBuild;
    const auto& build = *verification.verified_build;
    const auto base = build.runtime_image_base();
    if (kContextRva + kCallContext.size() > build.runtime_image_size() ||
        std::memcmp(reinterpret_cast<const void*>(base+kContextRva),
                    kCallContext.data(), kCallContext.size()) != 0)
        return ControllerInputInstallResult::SignatureMismatch;
    HMODULE self{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&FilteredGetDeviceState), &self))
        return ControllerInputInstallResult::ModulePinFailure;
    auto* site = reinterpret_cast<unsigned char*>(base+kCallRva);
    std::array<unsigned char,5> replacement{0xE8,0,0,0,0};
    // rel32 arithmetic intentionally wraps in the qualified 32-bit process.
    const auto relative = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(&FilteredGetDeviceState) - (base+kCallRva+5));
    std::memcpy(replacement.data()+1, &relative, sizeof(relative));
    DWORD old{}, ignored{};
    if (!VirtualProtect(site, replacement.size(), PAGE_EXECUTE_READWRITE, &old))
        return ControllerInputInstallResult::ProtectionFailure;
    std::memcpy(site, replacement.data(), replacement.size());
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), site, replacement.size()) != FALSE;
    const bool restored = VirtualProtect(site, replacement.size(), old, &ignored) != FALSE;
    if (!flushed || !restored) {
        // Caller excludes input polls, so rollback is safe before returning.
        DWORD writable{};
        if (VirtualProtect(site, replacement.size(), PAGE_EXECUTE_READWRITE, &writable)) {
            std::memcpy(site, kCallContext.data()+(kCallRva-kContextRva), replacement.size());
            (void)FlushInstructionCache(GetCurrentProcess(), site, replacement.size());
            (void)VirtualProtect(site, replacement.size(), old, &ignored);
        } else {
            // Do not claim that a still-installed/pinned callback disappeared.
            g_installed = true;
        }
        return ControllerInputInstallResult::ProtectionFailure;
    }
    g_installed = true;
    return ControllerInputInstallResult::Installed;
}

ControllerInputDiagnostics GetControllerInputDiagnostics() noexcept {
    return {g_polls.load(std::memory_order_relaxed),g_filtered.load(std::memory_order_relaxed),
        g_taps.load(std::memory_order_relaxed),g_unsupported.load(std::memory_order_relaxed),
        g_failed.load(std::memory_order_relaxed)};
}
} // namespace k2vr::game32
