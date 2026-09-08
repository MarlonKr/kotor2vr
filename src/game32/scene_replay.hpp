#pragma once
#include <cstdint>

namespace k2vr::game32 {
using SceneRenderMethod = std::uint64_t(__thiscall*)(void*);
// A frame contains one CPU scene evaluation, then GL-only right/monitor views.
[[nodiscard]] bool BeginSceneReplayFrame() noexcept;
void FinishSceneReplayFrame() noexcept;
void NotifySceneReplayContextDeleted(void* context) noexcept;
// The optional callback must preserve GL state and return true only after a
// complete monitor image was written. Failure keeps ordinary rasterization.
using SceneReplayMirror = bool(*)() noexcept;
void SetSceneReplayFinalPass(SceneReplayMirror mirror = nullptr) noexcept;
// Engine framebuffer reads/copies make a raster-free replay unsafe.
void NotifySceneReplayFramebufferRead() noexcept;
// These markers are recorded by reference: hidden in both eyes, visible again
// in the monitor replay, without evaluating the game's scene a second time.
[[nodiscard]] bool BeginSceneReplayHiddenDraw() noexcept;
void EndSceneReplayHiddenDraw() noexcept;
[[nodiscard]] std::uint64_t RenderSceneWithReplay(void* scene, SceneRenderMethod render) noexcept;
}
