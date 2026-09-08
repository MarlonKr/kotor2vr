#pragma once
#include <cstdint>
namespace k2vr::input {
// Keyboard and controller chords are read without moving focus or generating input.
[[nodiscard]] bool StartStreamDown() noexcept; // F7 or View+Menu
[[nodiscard]] bool RecenterDown() noexcept; // F11, View+A, or L3+R3 held for 650 ms
[[nodiscard]] bool TheaterDown() noexcept; // Ctrl+F10 or View+X
[[nodiscard]] bool CameraToggleDown() noexcept; // F10 or View+Y
[[nodiscard]] bool NeuralToggleDown() noexcept; // F9 or View+B
[[nodiscard]] bool HudToggleDown() noexcept; // F8 or View+DpadDown; native XR HUD only
}
