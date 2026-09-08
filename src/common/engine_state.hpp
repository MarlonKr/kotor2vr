#pragma once

#include "ipc_protocol.hpp"

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace k2vr::engine {

enum class EngineState : std::uint32_t {
    Unknown = 0,
    Boot,
    MainMenu,
    Loading,
    World,
    Dialogue,
    Cinematic,
    Pazaak,
    MiniGame,
    Movie,
    Paused,
    Shutdown,
};

enum class EngineFlag : std::uint64_t {
    None = 0,
    ProcessAttached = 1ULL << 0,
    ExactBuildMatched = 1ULL << 1,
    ProbeOnly = 1ULL << 2,
    GameFocused = 1ULL << 3,
    WorldLoaded = 1ULL << 4,
    CameraAvailable = 1ULL << 5,
    CameraAnimated = 1ULL << 6,
    CameraCut = 1ULL << 7,
    GuiVisible = 1ULL << 8,
    FullscreenGui = 1ULL << 9,
    DialogueActive = 1ULL << 10,
    DialogLetterboxActive = 1ULL << 11,
    PazaakActive = 1ULL << 12,
    MiniGameActive = 1ULL << 13,
    SwoopActive = 1ULL << 14,
    TurretActive = 1ULL << 15,
    MovieActive = 1ULL << 16,
    Loading = 1ULL << 17,
    Paused = 1ULL << 18,
    ShutdownRequested = 1ULL << 19,
    CinematicActive = 1ULL << 20,
};

[[nodiscard]] constexpr EngineFlag operator|(EngineFlag lhs,
                                             EngineFlag rhs) noexcept {
    return static_cast<EngineFlag>(static_cast<std::uint64_t>(lhs) |
                                   static_cast<std::uint64_t>(rhs));
}

[[nodiscard]] constexpr EngineFlag operator&(EngineFlag lhs,
                                             EngineFlag rhs) noexcept {
    return static_cast<EngineFlag>(static_cast<std::uint64_t>(lhs) &
                                   static_cast<std::uint64_t>(rhs));
}

constexpr EngineFlag& operator|=(EngineFlag& lhs, EngineFlag rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

[[nodiscard]] constexpr bool HasFlag(EngineFlag value, EngineFlag flag) noexcept {
    return (value & flag) == flag;
}

struct EngineStateSnapshot {
    std::uint64_t game_tick{0};
    EngineState state{EngineState::Unknown};
    EngineFlag flags{EngineFlag::None};
    std::uint32_t camera_mode{0};
    std::uint32_t active_gui_name_hash{0};
};

// Priority is deliberate. Modes which must use a theater layer override world
// and GUI evidence; dialogue stays stereoscopic per the product requirements.
[[nodiscard]] constexpr EngineState Classify(EngineFlag flags) noexcept {
    if (HasFlag(flags, EngineFlag::ShutdownRequested)) {
        return EngineState::Shutdown;
    }
    if (HasFlag(flags, EngineFlag::MovieActive)) {
        return EngineState::Movie;
    }
    if (HasFlag(flags, EngineFlag::PazaakActive)) {
        return EngineState::Pazaak;
    }
    if (HasFlag(flags, EngineFlag::MiniGameActive) ||
        HasFlag(flags, EngineFlag::SwoopActive) ||
        HasFlag(flags, EngineFlag::TurretActive)) {
        return EngineState::MiniGame;
    }
    if (HasFlag(flags, EngineFlag::DialogueActive)) {
        return EngineState::Dialogue;
    }
    if (HasFlag(flags, EngineFlag::CinematicActive)) {
        return EngineState::Cinematic;
    }
    if (HasFlag(flags, EngineFlag::Loading)) {
        return EngineState::Loading;
    }
    if (HasFlag(flags, EngineFlag::Paused)) {
        return EngineState::Paused;
    }
    if (HasFlag(flags, EngineFlag::WorldLoaded)) {
        return EngineState::World;
    }
    if (HasFlag(flags, EngineFlag::FullscreenGui)) {
        return EngineState::MainMenu;
    }
    if (HasFlag(flags, EngineFlag::ProcessAttached)) {
        return EngineState::Boot;
    }
    return EngineState::Unknown;
}

[[nodiscard]] constexpr bool UsesTheaterLayer(EngineState state) noexcept {
    switch (state) {
    case EngineState::MainMenu:
    case EngineState::Loading:
    case EngineState::Cinematic:
    case EngineState::Pazaak:
    case EngineState::MiniGame:
    case EngineState::Movie:
    case EngineState::Paused:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr bool RendersStereoWorld(EngineState state) noexcept {
    return state == EngineState::World || state == EngineState::Dialogue;
}

// This is the sole conversion into the stable public presentation contract.
// Distinct minigames are tested before the generic flag so they cannot be
// silently collapsed into an unsafe stereo mode.
[[nodiscard]] constexpr ipc::PresentationState ClassifyPresentation(
    EngineFlag flags, bool first_person) noexcept {
    using ipc::PresentationState;
    if (HasFlag(flags, EngineFlag::ShutdownRequested)) {
        return PresentationState::UnknownSafe;
    }
    if (HasFlag(flags, EngineFlag::MovieActive)) {
        return PresentationState::MovieTheater;
    }
    if (HasFlag(flags, EngineFlag::PazaakActive)) {
        return PresentationState::PazaakTheater;
    }
    if (HasFlag(flags, EngineFlag::SwoopActive)) {
        return PresentationState::SwoopTheater;
    }
    if (HasFlag(flags, EngineFlag::TurretActive)) {
        return PresentationState::TurretTheater;
    }
    if (HasFlag(flags, EngineFlag::MiniGameActive)) {
        return PresentationState::UnknownSafe;
    }
    if (HasFlag(flags, EngineFlag::DialogueActive)) {
        return PresentationState::DialogueStereo;
    }
    if (HasFlag(flags, EngineFlag::CinematicActive)) {
        return PresentationState::CutsceneTheater;
    }
    if (HasFlag(flags, EngineFlag::Loading)) {
        return PresentationState::LoadingTheater;
    }
    if (HasFlag(flags, EngineFlag::FullscreenGui) ||
        HasFlag(flags, EngineFlag::Paused)) {
        return PresentationState::FullscreenUi;
    }
    if (HasFlag(flags, EngineFlag::WorldLoaded)) {
        return first_person ? PresentationState::WorldFirstPerson
                            : PresentationState::WorldThirdPerson;
    }
    return PresentationState::UnknownSafe;
}

[[nodiscard]] constexpr std::string_view ToString(EngineState state) noexcept {
    switch (state) {
    case EngineState::Unknown:
        return "unknown";
    case EngineState::Boot:
        return "boot";
    case EngineState::MainMenu:
        return "main-menu";
    case EngineState::Loading:
        return "loading";
    case EngineState::World:
        return "world";
    case EngineState::Dialogue:
        return "dialogue";
    case EngineState::Cinematic:
        return "cinematic";
    case EngineState::Pazaak:
        return "pazaak";
    case EngineState::MiniGame:
        return "minigame";
    case EngineState::Movie:
        return "movie";
    case EngineState::Paused:
        return "paused";
    case EngineState::Shutdown:
        return "shutdown";
    }
    return "invalid";
}

static_assert(std::is_standard_layout_v<EngineStateSnapshot>);
static_assert(std::is_trivially_copyable_v<EngineStateSnapshot>);

} // namespace k2vr::engine
