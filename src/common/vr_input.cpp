#include "vr_input.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Xinput.h>
#include <array>
namespace k2vr::input {
namespace {
bool Key(int key) noexcept { return (GetAsyncKeyState(key)&0x8000)!=0; }
int F10Mode() noexcept {
    static int held_mode{};
    if (!Key(VK_F10)) held_mode=0;
    else if (!held_mode) held_mode=Key(VK_CONTROL) ? 2:1;
    return held_mode;
}
std::array<WORD,4> Buttons() noexcept {
    using GetState=DWORD(WINAPI*)(DWORD,XINPUT_STATE*);
    static const HMODULE library=LoadLibraryExW(L"xinput1_4.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    static const auto get=library ? reinterpret_cast<GetState>(GetProcAddress(library,"XInputGetState")) : nullptr;
    if (!get) return {};
    // Failed controller probes are expensive. Cache all four at a modest rate.
    static ULONGLONG next{}; static std::array<WORD,4> buttons{};
    const auto now=GetTickCount64();
    if (now>=next) {
        buttons.fill(0); next=now+16;
        for (DWORD i=0;i<4;++i) { XINPUT_STATE s{}; if (get(i,&s)==ERROR_SUCCESS) buttons[i]=s.Gamepad.wButtons; }
    }
    return buttons;
}
bool Chord(WORD mask) noexcept {
    for (const auto buttons:Buttons()) if ((buttons&mask)==mask) return true;
    return false;
}
}
bool StartStreamDown() noexcept { return Key(VK_F7)||Chord(XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_START); }
bool RecenterDown() noexcept {
    static ULONGLONG since{};
    const bool held=Chord(XINPUT_GAMEPAD_LEFT_THUMB|XINPUT_GAMEPAD_RIGHT_THUMB);
    const auto now=GetTickCount64();
    if (!held) since=0; else if (!since) since=now;
    return Key(VK_F11)||Chord(XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_A)||(held && now-since>=650);
}
bool TheaterDown() noexcept { return F10Mode()==2||Chord(XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_X); }
bool CameraToggleDown() noexcept { return F10Mode()==1||Chord(XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_Y); }
bool NeuralToggleDown() noexcept { return Key(VK_F9)||Chord(XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_B); }
// F8 also belongs to legacy Game32 double-pass diagnostics; keep those disabled
// in the native launcher when using this HUD binding.
bool HudToggleDown() noexcept { return Key(VK_F8)||Chord(XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_DPAD_DOWN); }
}
