#pragma once

#include "ipc_protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace k2vr::ipc {

// Fast functional milestone: one continuously refreshed, world-locked 16:9
// texture. Stereo eye resources reuse this synchronization contract later.
inline constexpr std::uint16_t kGpuStreamMajor = 1;
inline constexpr std::uint16_t kGpuStreamMinor = 0;
inline constexpr std::uint32_t kGpuStreamWidth = 1024;
inline constexpr std::uint32_t kGpuStreamHeight = 576;
inline constexpr std::uint32_t kGpuStreamDxgiFormatRgba8Unorm = 28;
inline constexpr std::size_t kGpuStreamSlotCount = 3;

enum class GpuStreamObjectKind : std::uint8_t {
    Color = 0,
    ReadyFence = 1,
    ConsumedFence = 2,
};

struct GpuStreamObjectName {
    std::array<wchar_t, 128> characters{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return characters[0] != L'\0';
    }
    [[nodiscard]] constexpr const wchar_t* c_str() const noexcept {
        return characters.data();
    }
};

[[nodiscard]] constexpr std::wstring_view GpuStreamObjectSuffix(
    const GpuStreamObjectKind kind) noexcept {
    switch (kind) {
    case GpuStreamObjectKind::Color: return L"-color";
    case GpuStreamObjectKind::ReadyFence: return L"-ready";
    case GpuStreamObjectKind::ConsumedFence: return L"-consumed";
    }
    return {};
}

// The nonce is written in the same HIGH/LOW order used by every existing
// KOTOR2VR named transport object. Both processes include this exact helper.
[[nodiscard]] constexpr GpuStreamObjectName MakeGpuStreamObjectName(
    const SessionNonce nonce, const GpuStreamObjectKind kind) noexcept {
    GpuStreamObjectName result{};
    if (!IsValid(nonce)) {
        return result;
    }
    constexpr std::wstring_view prefix = L"Local\\Kotor2VR-gpu-stream-v1-";
    constexpr std::wstring_view digits = L"0123456789ABCDEF";
    const std::wstring_view suffix = GpuStreamObjectSuffix(kind);
    if (suffix.empty() || prefix.size() + 32U + suffix.size() + 1U >
                              result.characters.size()) {
        return {};
    }

    std::size_t cursor = 0;
    for (const wchar_t character : prefix) {
        result.characters[cursor++] = character;
    }
    const auto append_hex = [&](const std::uint64_t value) constexpr {
        for (int shift = 60; shift >= 0; shift -= 4) {
            result.characters[cursor++] =
                digits[(value >> static_cast<unsigned>(shift)) & 0xFU];
        }
    };
    append_hex(nonce.high);
    append_hex(nonce.low);
    for (const wchar_t character : suffix) {
        result.characters[cursor++] = character;
    }
    result.characters[cursor] = L'\0';
    return result;
}

// The proven WGL/NV fallback retains the original unsuffixed Color object.
// The primary GL_EXT/D3D12 path publishes a three-texture ring instead, using
// explicit slot names so the x64 host can discover it without another shared
// control object or a startup race.
[[nodiscard]] constexpr GpuStreamObjectName MakeGpuStreamColorObjectName(
    const SessionNonce nonce, const std::size_t slot) noexcept {
    GpuStreamObjectName result{};
    if (!IsValid(nonce) || slot >= kGpuStreamSlotCount) {
        return result;
    }
    constexpr std::wstring_view prefix = L"Local\\Kotor2VR-gpu-stream-v1-";
    constexpr std::wstring_view digits = L"0123456789ABCDEF";
    constexpr std::wstring_view suffix = L"-color-";
    if (prefix.size() + 32U + suffix.size() + 2U >
        result.characters.size()) {
        return {};
    }

    std::size_t cursor = 0;
    for (const wchar_t character : prefix) {
        result.characters[cursor++] = character;
    }
    const auto append_hex = [&](const std::uint64_t value) constexpr {
        for (int shift = 60; shift >= 0; shift -= 4) {
            result.characters[cursor++] =
                digits[(value >> static_cast<unsigned>(shift)) & 0xFU];
        }
    };
    append_hex(nonce.high);
    append_hex(nonce.low);
    for (const wchar_t character : suffix) {
        result.characters[cursor++] = character;
    }
    result.characters[cursor++] =
        static_cast<wchar_t>(L'0' + static_cast<wchar_t>(slot));
    result.characters[cursor] = L'\0';
    return result;
}

[[nodiscard]] constexpr std::size_t GpuStreamSlotForSequence(
    const std::uint64_t sequence) noexcept {
    return sequence == 0U
               ? 0U
               : static_cast<std::size_t>((sequence - 1U) %
                                          kGpuStreamSlotCount);
}

[[nodiscard]] constexpr bool IsUsableSharedFenceValue(
    const std::uint64_t value) noexcept {
    return value != 0 && value != (std::numeric_limits<std::uint64_t>::max)();
}

// A single shared texture is never overwritten until the host has signalled
// that its previous copy completed. When the host pauses, the game drops
// transport frames instead of blocking its render thread.
[[nodiscard]] constexpr bool CanProduceGpuStreamFrame(
    const std::uint64_t last_produced,
    const std::uint64_t consumed) noexcept {
    return last_produced == 0 || consumed >= last_produced;
}

// A ring slot can be written immediately on first use. Afterwards the x64
// consumer's cumulative timeline value must have passed that slot's prior
// publication before GL reacquires and overwrites it.
[[nodiscard]] constexpr bool CanReuseGpuStreamSlot(
    const std::uint64_t slot_last_use,
    const std::uint64_t consumed) noexcept {
    return slot_last_use == 0U || consumed >= slot_last_use;
}

[[nodiscard]] constexpr bool HasNewGpuStreamFrame(
    const std::uint64_t ready,
    const std::uint64_t last_consumed) noexcept {
    return IsUsableSharedFenceValue(ready) && ready > last_consumed;
}

} // namespace k2vr::ipc
