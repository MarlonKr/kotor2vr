#pragma once

#include "ipc_protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace k2vr::ipc {

inline constexpr std::uint32_t kSharedMemoryMagic = 0x4D32564BU;
inline constexpr std::uint16_t kSharedMemoryAbiMajor = 1;
inline constexpr std::uint16_t kSharedMemoryAbiMinor = 0;
inline constexpr std::size_t kSharedMemoryNameCapacity = 96;

enum class SharedMemoryRole : std::uint32_t {
    None = 0,
    HostCreator,
    GameOpener,
};

enum class SharedMemoryStatus : std::uint32_t {
    Ok = 0,
    InvalidArgument,
    UnsupportedPlatform,
    AlreadyOpen,
    NotOpen,
    NameFailure,
    MappingCreateFailure,
    MappingAlreadyExists,
    MappingOpenFailure,
    MappingViewFailure,
    HeaderNotReady,
    MagicMismatch,
    VersionMismatch,
    SizeMismatch,
    NonceMismatch,
    GenerationMismatch,
    RoleViolation,
    MessageHeaderMismatch,
    SnapshotUnavailable,
    SnapshotContended,
    WriterContended,
};

struct SharedMemoryObjectName {
    std::array<wchar_t, kSharedMemoryNameCapacity> characters{};

    [[nodiscard]] const wchar_t* c_str() const noexcept {
        return characters.data();
    }
    [[nodiscard]] bool valid() const noexcept {
        return characters[0] != L'\0';
    }
};

// All fields before magic are initialized before the creator publishes magic
// with an interlocked exchange. The header is immutable afterwards.
struct alignas(64) SharedMemoryHeader {
    std::uint32_t magic;
    std::uint16_t abi_major;
    std::uint16_t abi_minor;
    std::uint32_t header_size;
    std::uint32_t region_size;
    std::uint32_t protocol_magic;
    std::uint16_t protocol_major;
    std::uint16_t protocol_minor;
    SessionNonce session_nonce;
    std::uint64_t generation;
    std::uint32_t creator_process_id;
    std::uint32_t reserved;
    std::uint64_t created_qpc;
};

// Sequence zero means no payload has been published. An odd sequence means a
// writer owns the slot; an equal, non-zero even value before and after memcpy
// is a consistent snapshot.
struct alignas(64) SharedRenderRequestSlot {
    std::uint32_t sequence;
    std::uint32_t payload_size;
    RenderRequest payload;
    std::array<std::uint8_t, 16> reserved;
};

struct alignas(64) SharedHealthSlot {
    std::uint32_t sequence;
    std::uint32_t payload_size;
    HealthState payload;
    std::array<std::uint8_t, 8> reserved;
};

// Host is the sole writer of render_request and host_health. Game32 is the
// sole writer of game_health. This makes every seqlock slot single-writer while
// still allowing bidirectional heartbeat/health publication without a mutex.
struct alignas(64) SharedMemoryRegion {
    SharedMemoryHeader header;
    SharedRenderRequestSlot render_request;
    SharedHealthSlot host_health;
    SharedHealthSlot game_health;
};

static_assert(sizeof(SharedMemoryHeader) == 64);
static_assert(sizeof(SharedRenderRequestSlot) == 192);
static_assert(sizeof(SharedHealthSlot) == 128);
static_assert(sizeof(SharedMemoryRegion) == 512);
static_assert(offsetof(SharedMemoryRegion, render_request) == 64);
static_assert(offsetof(SharedMemoryRegion, host_health) == 256);
static_assert(offsetof(SharedMemoryRegion, game_health) == 384);
static_assert(offsetof(SharedRenderRequestSlot, sequence) % 4 == 0);
static_assert(offsetof(SharedHealthSlot, sequence) % 4 == 0);
static_assert(std::is_standard_layout_v<SharedMemoryRegion>);
static_assert(std::is_trivially_copyable_v<SharedMemoryRegion>);

[[nodiscard]] SharedMemoryObjectName MakeSharedMemoryObjectName(
    SessionNonce nonce) noexcept;

[[nodiscard]] const char* ToString(SharedMemoryStatus status) noexcept;

class SharedMemoryChannel final {
public:
    SharedMemoryChannel() noexcept = default;
    ~SharedMemoryChannel();

    SharedMemoryChannel(const SharedMemoryChannel&) = delete;
    SharedMemoryChannel& operator=(const SharedMemoryChannel&) = delete;
    SharedMemoryChannel(SharedMemoryChannel&& other) noexcept;
    SharedMemoryChannel& operator=(SharedMemoryChannel&& other) noexcept;

    // The host creates and initializes a new Local\\ named mapping. Existing
    // objects are rejected rather than silently attached to another session.
    [[nodiscard]] static SharedMemoryStatus CreateHost(
        SessionNonce nonce, std::uint64_t generation,
        SharedMemoryChannel& output) noexcept;

    // Game32 opens only the mapping derived from nonce, then verifies every
    // immutable ABI/header field before retaining the view.
    [[nodiscard]] static SharedMemoryStatus OpenGame(
        SessionNonce nonce, std::uint64_t expected_generation,
        SharedMemoryChannel& output) noexcept;

    void Close() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return region_ != nullptr; }
    [[nodiscard]] SharedMemoryRole role() const noexcept { return role_; }
    [[nodiscard]] SessionNonce session_nonce() const noexcept { return nonce_; }
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_;
    }

    [[nodiscard]] SharedMemoryStatus PublishRenderRequest(
        const RenderRequest& request) noexcept;
    [[nodiscard]] SharedMemoryStatus ReadRenderRequest(
        RenderRequest& output) const noexcept;

    // Publishes to the role-owned health slot and reads the opposite endpoint.
    [[nodiscard]] SharedMemoryStatus PublishLocalHealth(
        const HealthState& health) noexcept;
    [[nodiscard]] SharedMemoryStatus ReadPeerHealth(
        HealthState& output) const noexcept;

private:
    void MoveFrom(SharedMemoryChannel&& other) noexcept;

    void* mapping_handle_{nullptr};
    SharedMemoryRegion* region_{nullptr};
    SharedMemoryRole role_{SharedMemoryRole::None};
    SessionNonce nonce_{};
    std::uint64_t generation_{0};
};

} // namespace k2vr::ipc
