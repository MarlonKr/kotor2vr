#include "shared_memory_channel.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace k2vr::ipc {
namespace {

[[nodiscard]] constexpr bool SameNonce(SessionNonce lhs,
                                       SessionNonce rhs) noexcept {
    return lhs.low == rhs.low && lhs.high == rhs.high;
}

[[nodiscard]] bool ValidRenderRequest(const RenderRequest& request,
                                      SessionNonce nonce,
                                      std::uint64_t generation) noexcept {
    return HeaderMatches<RenderRequest>(request.header,
                                        MessageType::RenderRequest) &&
           SameNonce(request.header.session_nonce, nonce) &&
           request.header.generation == generation &&
           request.header.sequence != 0 && request.frame_id != 0 &&
           request.predicted_display_time_ns > 0 &&
           request.render_width != 0 && request.render_height != 0;
}

[[nodiscard]] bool ValidHealth(const HealthState& health,
                               SessionNonce nonce,
                               std::uint64_t generation) noexcept {
    return HeaderMatches<HealthState>(health.header,
                                      MessageType::HealthState) &&
           SameNonce(health.header.session_nonce, nonce) &&
           health.header.generation == generation &&
           health.header.sequence != 0;
}

#if defined(_WIN32)

[[nodiscard]] LONG AtomicRead(const std::uint32_t& value) noexcept {
    return InterlockedCompareExchange(
        reinterpret_cast<volatile LONG*>(
            const_cast<std::uint32_t*>(&value)),
        0, 0);
}

template <typename Slot, typename Payload>
[[nodiscard]] SharedMemoryStatus PublishSnapshot(
    Slot& slot, const Payload& payload) noexcept {
    static_assert(std::is_trivially_copyable_v<Payload>);
    static_assert(sizeof(slot.payload) == sizeof(Payload));

    const LONG stable = AtomicRead(slot.sequence);
    const auto stable_bits = static_cast<std::uint32_t>(stable);
    if ((stable_bits & 1U) != 0 || stable_bits >= 0xFFFFFFFCU) {
        return SharedMemoryStatus::WriterContended;
    }
    const LONG writing = static_cast<LONG>(stable_bits + 1U);
    if (InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(&slot.sequence), writing,
            stable) != stable) {
        return SharedMemoryStatus::WriterContended;
    }

    std::memcpy(&slot.payload, &payload, sizeof(payload));
    MemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&slot.sequence),
                        static_cast<LONG>(stable_bits + 2U));
    return SharedMemoryStatus::Ok;
}

template <typename Slot, typename Payload, typename Validator>
[[nodiscard]] SharedMemoryStatus ReadSnapshot(
    const Slot& slot, Payload& output, Validator&& validator) noexcept {
    static_assert(std::is_trivially_copyable_v<Payload>);
    static_assert(sizeof(slot.payload) == sizeof(Payload));

    bool saw_publication = false;
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        const LONG before = AtomicRead(slot.sequence);
        const auto before_bits = static_cast<std::uint32_t>(before);
        if (before_bits == 0) {
            return SharedMemoryStatus::SnapshotUnavailable;
        }
        saw_publication = true;
        if ((before_bits & 1U) != 0) {
            YieldProcessor();
            continue;
        }

        Payload candidate{};
        std::memcpy(&candidate, &slot.payload, sizeof(candidate));
        MemoryBarrier();
        const LONG after = AtomicRead(slot.sequence);
        if (before == after &&
            (static_cast<std::uint32_t>(after) & 1U) == 0) {
            if (!validator(candidate)) {
                return SharedMemoryStatus::MessageHeaderMismatch;
            }
            output = candidate;
            return SharedMemoryStatus::Ok;
        }
    }
    return saw_publication ? SharedMemoryStatus::SnapshotContended
                           : SharedMemoryStatus::SnapshotUnavailable;
}

[[nodiscard]] SharedMemoryStatus ValidateRegion(
    const SharedMemoryRegion& region, SessionNonce nonce,
    std::uint64_t expected_generation) noexcept {
    const LONG magic = AtomicRead(region.header.magic);
    if (magic == 0) {
        return SharedMemoryStatus::HeaderNotReady;
    }
    if (static_cast<std::uint32_t>(magic) != kSharedMemoryMagic) {
        return SharedMemoryStatus::MagicMismatch;
    }
    MemoryBarrier();
    if (region.header.abi_major != kSharedMemoryAbiMajor ||
        region.header.abi_minor != kSharedMemoryAbiMinor ||
        region.header.protocol_magic != kProtocolMagic ||
        region.header.protocol_major != kProtocolMajor ||
        region.header.protocol_minor != kProtocolMinor) {
        return SharedMemoryStatus::VersionMismatch;
    }
    if (region.header.header_size != sizeof(SharedMemoryHeader) ||
        region.header.region_size != sizeof(SharedMemoryRegion) ||
        region.render_request.payload_size != sizeof(RenderRequest) ||
        region.host_health.payload_size != sizeof(HealthState) ||
        region.game_health.payload_size != sizeof(HealthState)) {
        return SharedMemoryStatus::SizeMismatch;
    }
    if (!SameNonce(region.header.session_nonce, nonce)) {
        return SharedMemoryStatus::NonceMismatch;
    }
    if (region.header.generation != expected_generation) {
        return SharedMemoryStatus::GenerationMismatch;
    }
    if (region.header.creator_process_id == 0) {
        return SharedMemoryStatus::HeaderNotReady;
    }
    return SharedMemoryStatus::Ok;
}

#endif

} // namespace

SharedMemoryObjectName MakeSharedMemoryObjectName(SessionNonce nonce) noexcept {
    SharedMemoryObjectName result{};
    if (!IsValid(nonce)) {
        return result;
    }
#if defined(_WIN32)
    const int count = swprintf_s(
        result.characters.data(), result.characters.size(),
        L"Local\\Kotor2VR-ipc-v1-%016llX%016llX",
        static_cast<unsigned long long>(nonce.high),
        static_cast<unsigned long long>(nonce.low));
    if (count <= 0 ||
        static_cast<std::size_t>(count) >= result.characters.size()) {
        result.characters.fill(L'\0');
    }
#else
    (void)nonce;
#endif
    return result;
}

const char* ToString(SharedMemoryStatus status) noexcept {
    switch (status) {
    case SharedMemoryStatus::Ok: return "ok";
    case SharedMemoryStatus::InvalidArgument: return "invalid-argument";
    case SharedMemoryStatus::UnsupportedPlatform: return "unsupported-platform";
    case SharedMemoryStatus::AlreadyOpen: return "already-open";
    case SharedMemoryStatus::NotOpen: return "not-open";
    case SharedMemoryStatus::NameFailure: return "name-failure";
    case SharedMemoryStatus::MappingCreateFailure: return "mapping-create-failure";
    case SharedMemoryStatus::MappingAlreadyExists: return "mapping-already-exists";
    case SharedMemoryStatus::MappingOpenFailure: return "mapping-open-failure";
    case SharedMemoryStatus::MappingViewFailure: return "mapping-view-failure";
    case SharedMemoryStatus::HeaderNotReady: return "header-not-ready";
    case SharedMemoryStatus::MagicMismatch: return "magic-mismatch";
    case SharedMemoryStatus::VersionMismatch: return "version-mismatch";
    case SharedMemoryStatus::SizeMismatch: return "size-mismatch";
    case SharedMemoryStatus::NonceMismatch: return "nonce-mismatch";
    case SharedMemoryStatus::GenerationMismatch: return "generation-mismatch";
    case SharedMemoryStatus::RoleViolation: return "role-violation";
    case SharedMemoryStatus::MessageHeaderMismatch: return "message-header-mismatch";
    case SharedMemoryStatus::SnapshotUnavailable: return "snapshot-unavailable";
    case SharedMemoryStatus::SnapshotContended: return "snapshot-contended";
    case SharedMemoryStatus::WriterContended: return "writer-contended";
    }
    return "unknown";
}

SharedMemoryChannel::~SharedMemoryChannel() { Close(); }

SharedMemoryChannel::SharedMemoryChannel(SharedMemoryChannel&& other) noexcept {
    MoveFrom(std::move(other));
}

SharedMemoryChannel& SharedMemoryChannel::operator=(
    SharedMemoryChannel&& other) noexcept {
    if (this != &other) {
        Close();
        MoveFrom(std::move(other));
    }
    return *this;
}

void SharedMemoryChannel::MoveFrom(SharedMemoryChannel&& other) noexcept {
    mapping_handle_ = other.mapping_handle_;
    region_ = other.region_;
    role_ = other.role_;
    nonce_ = other.nonce_;
    generation_ = other.generation_;
    other.mapping_handle_ = nullptr;
    other.region_ = nullptr;
    other.role_ = SharedMemoryRole::None;
    other.nonce_ = {};
    other.generation_ = 0;
}

SharedMemoryStatus SharedMemoryChannel::CreateHost(
    SessionNonce nonce, std::uint64_t generation,
    SharedMemoryChannel& output) noexcept {
    if (!IsValid(nonce) || generation == 0) {
        return SharedMemoryStatus::InvalidArgument;
    }
    if (output.is_open()) {
        return SharedMemoryStatus::AlreadyOpen;
    }
#if !defined(_WIN32)
    (void)nonce;
    (void)generation;
    (void)output;
    return SharedMemoryStatus::UnsupportedPlatform;
#else
    const SharedMemoryObjectName name = MakeSharedMemoryObjectName(nonce);
    if (!name.valid()) {
        return SharedMemoryStatus::NameFailure;
    }

    HANDLE mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(SharedMemoryRegion)), name.c_str());
    if (mapping == nullptr) {
        return SharedMemoryStatus::MappingCreateFailure;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mapping);
        return SharedMemoryStatus::MappingAlreadyExists;
    }

    auto* region = static_cast<SharedMemoryRegion*>(MapViewOfFile(
        mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
        sizeof(SharedMemoryRegion)));
    if (region == nullptr) {
        CloseHandle(mapping);
        return SharedMemoryStatus::MappingViewFailure;
    }

    std::memset(region, 0, sizeof(*region));
    region->header.abi_major = kSharedMemoryAbiMajor;
    region->header.abi_minor = kSharedMemoryAbiMinor;
    region->header.header_size = sizeof(SharedMemoryHeader);
    region->header.region_size = sizeof(SharedMemoryRegion);
    region->header.protocol_magic = kProtocolMagic;
    region->header.protocol_major = kProtocolMajor;
    region->header.protocol_minor = kProtocolMinor;
    region->header.session_nonce = nonce;
    region->header.generation = generation;
    region->header.creator_process_id = GetCurrentProcessId();
    LARGE_INTEGER created{};
    if (QueryPerformanceCounter(&created) != FALSE) {
        region->header.created_qpc =
            static_cast<std::uint64_t>(created.QuadPart);
    }
    region->render_request.payload_size = sizeof(RenderRequest);
    region->host_health.payload_size = sizeof(HealthState);
    region->game_health.payload_size = sizeof(HealthState);
    MemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&region->header.magic),
                        static_cast<LONG>(kSharedMemoryMagic));

    output.mapping_handle_ = mapping;
    output.region_ = region;
    output.role_ = SharedMemoryRole::HostCreator;
    output.nonce_ = nonce;
    output.generation_ = generation;
    return SharedMemoryStatus::Ok;
#endif
}

SharedMemoryStatus SharedMemoryChannel::OpenGame(
    SessionNonce nonce, std::uint64_t expected_generation,
    SharedMemoryChannel& output) noexcept {
    if (!IsValid(nonce) || expected_generation == 0) {
        return SharedMemoryStatus::InvalidArgument;
    }
    if (output.is_open()) {
        return SharedMemoryStatus::AlreadyOpen;
    }
#if !defined(_WIN32)
    (void)nonce;
    (void)expected_generation;
    (void)output;
    return SharedMemoryStatus::UnsupportedPlatform;
#else
    const SharedMemoryObjectName name = MakeSharedMemoryObjectName(nonce);
    if (!name.valid()) {
        return SharedMemoryStatus::NameFailure;
    }
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                      name.c_str());
    if (mapping == nullptr) {
        return SharedMemoryStatus::MappingOpenFailure;
    }
    auto* region = static_cast<SharedMemoryRegion*>(MapViewOfFile(
        mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
        sizeof(SharedMemoryRegion)));
    if (region == nullptr) {
        CloseHandle(mapping);
        return SharedMemoryStatus::MappingViewFailure;
    }

    const SharedMemoryStatus validation =
        ValidateRegion(*region, nonce, expected_generation);
    if (validation != SharedMemoryStatus::Ok) {
        UnmapViewOfFile(region);
        CloseHandle(mapping);
        return validation;
    }

    output.mapping_handle_ = mapping;
    output.region_ = region;
    output.role_ = SharedMemoryRole::GameOpener;
    output.nonce_ = nonce;
    output.generation_ = expected_generation;
    return SharedMemoryStatus::Ok;
#endif
}

void SharedMemoryChannel::Close() noexcept {
#if defined(_WIN32)
    if (region_ != nullptr) {
        UnmapViewOfFile(region_);
    }
    if (mapping_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(mapping_handle_));
    }
#endif
    mapping_handle_ = nullptr;
    region_ = nullptr;
    role_ = SharedMemoryRole::None;
    nonce_ = {};
    generation_ = 0;
}

SharedMemoryStatus SharedMemoryChannel::PublishRenderRequest(
    const RenderRequest& request) noexcept {
    if (!is_open()) {
        return SharedMemoryStatus::NotOpen;
    }
    if (role_ != SharedMemoryRole::HostCreator) {
        return SharedMemoryStatus::RoleViolation;
    }
    if (!ValidRenderRequest(request, nonce_, generation_)) {
        return SharedMemoryStatus::MessageHeaderMismatch;
    }
#if defined(_WIN32)
    return PublishSnapshot(region_->render_request, request);
#else
    return SharedMemoryStatus::UnsupportedPlatform;
#endif
}

SharedMemoryStatus SharedMemoryChannel::ReadRenderRequest(
    RenderRequest& output) const noexcept {
    if (!is_open()) {
        return SharedMemoryStatus::NotOpen;
    }
#if defined(_WIN32)
    return ReadSnapshot(region_->render_request, output,
                        [this](const RenderRequest& request) {
                            return ValidRenderRequest(request, nonce_,
                                                      generation_);
                        });
#else
    (void)output;
    return SharedMemoryStatus::UnsupportedPlatform;
#endif
}

SharedMemoryStatus SharedMemoryChannel::PublishLocalHealth(
    const HealthState& health) noexcept {
    if (!is_open()) {
        return SharedMemoryStatus::NotOpen;
    }
    if (!ValidHealth(health, nonce_, generation_)) {
        return SharedMemoryStatus::MessageHeaderMismatch;
    }
#if defined(_WIN32)
    SharedHealthSlot& slot = role_ == SharedMemoryRole::HostCreator
                                 ? region_->host_health
                                 : region_->game_health;
    return PublishSnapshot(slot, health);
#else
    return SharedMemoryStatus::UnsupportedPlatform;
#endif
}

SharedMemoryStatus SharedMemoryChannel::ReadPeerHealth(
    HealthState& output) const noexcept {
    if (!is_open()) {
        return SharedMemoryStatus::NotOpen;
    }
#if defined(_WIN32)
    const SharedHealthSlot& slot = role_ == SharedMemoryRole::HostCreator
                                       ? region_->game_health
                                       : region_->host_health;
    return ReadSnapshot(slot, output, [this](const HealthState& health) {
        return ValidHealth(health, nonce_, generation_);
    });
#else
    (void)output;
    return SharedMemoryStatus::UnsupportedPlatform;
#endif
}

} // namespace k2vr::ipc
