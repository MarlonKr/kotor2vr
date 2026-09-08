#include "stereo_stream.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstring>

namespace k2vr::ipc {
namespace {
struct alignas(8) Slot { volatile LONG sequence; StereoFrameMetadata frame; };
struct Shared { volatile LONG world_tick; Slot slots[kGpuStreamSlotCount]; };
static_assert(sizeof(Slot) == 392);
static_assert(offsetof(Slot,frame) == 8);
}
StereoFrameMapping::~StereoFrameMapping() { Close(); }
void StereoFrameMapping::MarkWorldRendered() noexcept {
    if (view_ && writer_) InterlockedExchange(&static_cast<Shared*>(view_)->world_tick,static_cast<LONG>(GetTickCount()));
}
bool StereoFrameMapping::WorldRecentlyRendered() const noexcept {
    if (!view_) return false;
    const auto tick=static_cast<DWORD>(static_cast<const Shared*>(view_)->world_tick);
    return tick!=0 && static_cast<DWORD>(GetTickCount()-tick)<500;
}
void StereoFrameMapping::Close() noexcept {
    if (view_) UnmapViewOfFile(view_);
    if (handle_) CloseHandle(handle_);
    view_ = nullptr; handle_ = nullptr; writer_ = false; nonce_ = {};
}
bool StereoFrameMapping::Open(SessionNonce nonce, bool create) noexcept {
    if (view_) return nonce.low == nonce_.low && nonce.high == nonce_.high && writer_ == create;
    if (!IsValid(nonce)) return false;
    auto name = MakeGpuStreamObjectName(StereoStreamNonce(nonce), GpuStreamObjectKind::Color);
    // Mapping and GPU object names must also be disjoint.
    const std::size_t end = std::wcslen(name.c_str());
    const wchar_t suffix[] = L"-metadata";
    std::memcpy(name.characters.data() + end, suffix, sizeof(suffix));
    HANDLE handle = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, sizeof(Shared), name.c_str()) :
        OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!handle) return false;
    if (create && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(handle); return false; }
    void* view = MapViewOfFile(handle, create ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ,
                              0, 0, sizeof(Shared));
    if (!view) { CloseHandle(handle); return false; }
    handle_ = handle; view_ = view; nonce_ = nonce; writer_ = create;
    return true;
}
bool StereoFrameMapping::Write(const StereoFrameMetadata& frame) noexcept {
    if (!view_ || !writer_ || !IsUsableSharedFenceValue(frame.ready_value) ||
        !ValidStereoMetadataHeader(frame) || frame.eyes_complete != 3 || frame.camera_frame_id==0 ||
        frame.request.header.session_nonce.low!=nonce_.low || frame.request.header.session_nonce.high!=nonce_.high ||
        !ValidStereoRequest(frame.request) || !ValidStereoHud(frame) || !ValidStereoGuides(frame)) return false;
    Slot& slot = static_cast<Shared*>(view_)->slots[GpuStreamSlotForSequence(frame.ready_value)];
    InterlockedIncrement(&slot.sequence);
    std::memcpy(&slot.frame, &frame, sizeof(frame));
    MemoryBarrier();
    InterlockedIncrement(&slot.sequence);
    return true;
}
bool StereoFrameMapping::Read(std::uint64_t ready, StereoFrameMetadata& frame) const noexcept {
    if (!view_ || !IsUsableSharedFenceValue(ready)) return false;
    const Slot& slot = static_cast<const Shared*>(view_)->slots[GpuStreamSlotForSequence(ready)];
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        const LONG before = slot.sequence;
        if (before == 0 || (before & 1)) continue;
        MemoryBarrier();
        StereoFrameMetadata copy{};
        std::memcpy(&copy, &slot.frame, sizeof(copy));
        MemoryBarrier();
        if (before != slot.sequence) continue;
        if (!ValidStereoMetadataHeader(copy) || copy.eyes_complete != 3 ||
            copy.ready_value != ready || copy.camera_frame_id == 0 ||
            copy.request.header.session_nonce.low != nonce_.low ||
            copy.request.header.session_nonce.high != nonce_.high ||
            !ValidStereoRequest(copy.request) || !ValidStereoHud(copy) || !ValidStereoGuides(copy)) return false;
        frame = copy; return true;
    }
    return false;
}
}
