#include "kotorvr/host/game_image_snapshot.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>

namespace kotorvr::host {

GameImageMappingName MakeGameImageMappingName(
    const k2vr::ipc::SessionNonce nonce) noexcept {
    GameImageMappingName result{};
    if (!k2vr::ipc::IsValid(nonce)) {
        return result;
    }
    const int count = swprintf_s(
        result.characters.data(), result.characters.size(),
        L"Local\\Kotor2VR-game-image-v1-%016llX%016llX",
        static_cast<unsigned long long>(nonce.high),
        static_cast<unsigned long long>(nonce.low));
    if (count <= 0 || static_cast<std::size_t>(count) >= result.characters.size()) {
        result.characters.fill(L'\0');
    }
    return result;
}

GameImageSnapshotStatus ValidateGameImageSnapshotHeader(
    const GameImageSnapshotHeader& header,
    const std::size_t mapped_size) noexcept {
    if (header.magic != game_image_magic ||
        header.version_major != game_image_version_major ||
        header.version_minor != game_image_version_minor ||
        header.header_size != sizeof(GameImageSnapshotHeader) ||
        header.mapping_size < sizeof(GameImageSnapshotHeader) ||
        header.mapping_size > mapped_size) {
        return GameImageSnapshotStatus::InvalidHeader;
    }
    if (header.pixel_format != game_image_pixel_format_bgra8_unorm) {
        return GameImageSnapshotStatus::UnsupportedPixelFormat;
    }
    if (header.frame_id == 0 || header.width == 0 || header.height == 0 ||
        header.width > game_image_max_dimension ||
        header.height > game_image_max_dimension ||
        header.width > (std::numeric_limits<std::uint32_t>::max)() / 4U ||
        header.stride < header.width * 4U ||
        header.stride > game_image_max_dimension * 4U) {
        return GameImageSnapshotStatus::InvalidDimensions;
    }
    const std::uint64_t pixel_bytes =
        static_cast<std::uint64_t>(header.stride) * header.height;
    const std::uint64_t required = sizeof(GameImageSnapshotHeader) + pixel_bytes;
    if (required > header.mapping_size || required > mapped_size) {
        return GameImageSnapshotStatus::InvalidDimensions;
    }
    return GameImageSnapshotStatus::Ok;
}

std::string_view ToString(const GameImageSnapshotStatus status) noexcept {
    switch (status) {
    case GameImageSnapshotStatus::Ok: return "ok";
    case GameImageSnapshotStatus::Unchanged: return "unchanged";
    case GameImageSnapshotStatus::MappingUnavailable: return "mapping-unavailable";
    case GameImageSnapshotStatus::MappingViewFailure: return "mapping-view-failure";
    case GameImageSnapshotStatus::NoFrame: return "no-frame";
    case GameImageSnapshotStatus::SnapshotContended: return "snapshot-contended";
    case GameImageSnapshotStatus::InvalidHeader: return "invalid-header";
    case GameImageSnapshotStatus::InvalidDimensions: return "invalid-dimensions";
    case GameImageSnapshotStatus::UnsupportedPixelFormat:
        return "unsupported-pixel-format";
    case GameImageSnapshotStatus::AllocationFailure: return "allocation-failure";
    }
    return "unknown";
}

std::string_view GpuProducerStatusName(const std::uint32_t status) noexcept {
    constexpr std::array<std::string_view, 24> names{
        "ok", "already-started", "invalid-session", "out-of-memory",
        "missing-gl-entry-point", "interop-initialization-failure",
        "texture-registration-failure", "fence-creation-failure",
        "fence-share-failure", "framebuffer-creation-failure",
        "no-current-gl-context", "invalid-viewport",
        "resolve-target-failure", "framebuffer-incomplete",
        "interop-lock-failure", "gl-blit-failure",
        "interop-unlock-failure", "fence-signal-failure",
        "fence-device-removed", "fence-value-exhausted", "not-started",
        "context-mismatch", "cleanup-failure", "unknown"};
    return status < names.size() - 1U ? names[status] : names.back();
}

std::string_view GpuInteropStatusName(const std::uint32_t status) noexcept {
    constexpr std::array<std::string_view, 31> names{
        "ok", "already-initialized", "not-initialized",
        "invalid-texture-description", "no-current-gl-context",
        "opengl-runtime-unavailable", "missing-wgl-entry-point",
        "dxgi-factory-failure", "nvidia-adapter-unavailable",
        "requested-adapter-unavailable", "d3d11-device-creation-failure",
        "d3d11-fence-interface-unavailable", "interop-device-open-failure",
        "texture-already-registered", "invalid-shared-object-name",
        "unsupported-texture-format", "texture-creation-failure",
        "shared-resource-interface-unavailable",
        "shared-handle-creation-failure", "gl-texture-creation-failure",
        "share-handle-association-failure", "texture-registration-failure",
        "context-mismatch", "texture-not-registered", "already-locked",
        "not-locked", "lock-failure", "unlock-failure", "cleanup-failure",
        "out-of-memory", "unknown"};
    if (status == game_image_interop_status_unavailable) {
        return "not-reported";
    }
    return status < names.size() - 1U ? names[status] : names.back();
}

GameImageSnapshotReader::~GameImageSnapshotReader() { Close(); }

GameImageSnapshotStatus GameImageSnapshotReader::TryOpen(
    const k2vr::ipc::SessionNonce nonce) noexcept {
    if (is_open()) {
        return GameImageSnapshotStatus::Ok;
    }
    const GameImageMappingName name = MakeGameImageMappingName(nonce);
    if (!name.valid()) {
        return GameImageSnapshotStatus::InvalidHeader;
    }

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (mapping == nullptr) {
        return GameImageSnapshotStatus::MappingUnavailable;
    }
    void* const view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        CloseHandle(mapping);
        return GameImageSnapshotStatus::MappingViewFailure;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(view, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.RegionSize < sizeof(GameImageSnapshotHeader)) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return GameImageSnapshotStatus::MappingViewFailure;
    }

    mapping_handle_ = mapping;
    mapped_view_ = view;
    mapped_size_ = memory.RegionSize;
    last_sequence_ = 0;
    latest_ = {};
    scratch_.clear();
    return GameImageSnapshotStatus::Ok;
}

GameImageSnapshotStatus GameImageSnapshotReader::ReadLatest() noexcept {
    if (!is_open()) {
        return GameImageSnapshotStatus::MappingUnavailable;
    }
    auto* const bytes = static_cast<std::uint8_t*>(mapped_view_);
    auto* const sequence_pointer = reinterpret_cast<std::uint32_t*>(
        bytes + offsetof(GameImageSnapshotHeader, sequence));
    std::atomic_ref<std::uint32_t> sequence(*sequence_pointer);

    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const std::uint32_t before = sequence.load(std::memory_order_acquire);
        if (before == 0) {
            return GameImageSnapshotStatus::NoFrame;
        }
        if ((before & 1U) != 0) {
            YieldProcessor();
            continue;
        }

        GameImageSnapshotHeader header{};
        std::memcpy(&header, bytes, sizeof(header));
        const GameImageSnapshotStatus validation =
            ValidateGameImageSnapshotHeader(header, mapped_size_);
        if (validation != GameImageSnapshotStatus::Ok) {
            return validation;
        }
        if (before == last_sequence_) {
            const std::uint32_t after = sequence.load(std::memory_order_acquire);
            if (after == before) {
                return GameImageSnapshotStatus::Unchanged;
            }
            continue;
        }

        const std::size_t tight_stride = static_cast<std::size_t>(header.width) * 4U;
        const std::size_t tight_size = tight_stride * header.height;
        try {
            scratch_.resize(tight_size);
        } catch (...) {
            return GameImageSnapshotStatus::AllocationFailure;
        }
        const std::uint8_t* source = bytes + sizeof(GameImageSnapshotHeader);
        for (std::uint32_t row = 0; row < header.height; ++row) {
            std::memcpy(scratch_.data() + static_cast<std::size_t>(row) * tight_stride,
                        source + static_cast<std::size_t>(row) * header.stride,
                        tight_stride);
        }

        const std::uint32_t after = sequence.load(std::memory_order_acquire);
        if (after != before || (after & 1U) != 0) {
            continue;
        }
        latest_.frame_id = header.frame_id;
        latest_.sequence = after;
        latest_.width = header.width;
        latest_.height = header.height;
        latest_.gpu_diagnostic = DecodeGameImageGpuDiagnostic(header);
        latest_.pixels.swap(scratch_);
        last_sequence_ = after;
        return GameImageSnapshotStatus::Ok;
    }
    return GameImageSnapshotStatus::SnapshotContended;
}

void GameImageSnapshotReader::Close() noexcept {
    latest_ = {};
    scratch_.clear();
    last_sequence_ = 0;
    mapped_size_ = 0;
    if (mapped_view_ != nullptr) {
        UnmapViewOfFile(mapped_view_);
        mapped_view_ = nullptr;
    }
    if (mapping_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapping_handle_));
        mapping_handle_ = nullptr;
    }
}

} // namespace kotorvr::host
