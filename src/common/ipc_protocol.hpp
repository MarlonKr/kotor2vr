#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace k2vr::ipc {

// Packed, little-endian, pointer-free wire ABI shared verbatim by the x86
// producer and x64 host. HANDLE values are uint64_t only after an explicit
// DuplicateHandle ownership transfer.
static_assert(std::endian::native == std::endian::little,
              "K2VR IPC currently supports little-endian hosts only");

inline constexpr std::uint32_t kProtocolMagic = 0x5256324BU;
inline constexpr std::uint16_t kProtocolMajor = 1;
inline constexpr std::uint16_t kProtocolMinor = 0;
inline constexpr std::uint32_t kLittleEndianTag = 0x01020304U;
inline constexpr std::uint32_t kStereoEyeCount = 2;
inline constexpr std::uint32_t kEyeRingSlotCount = 3;

enum class MessageType : std::uint16_t {
    Hello = 1,
    HelloAck = 2,
    RenderRequest = 3,
    FrameSubmit = 4,
    UiFrame = 5,
    HealthState = 6,
    ControlReset = 7,
};

enum class Eye : std::uint32_t {
    Left = 0,
    Right = 1,
    // Internal host spelling. These are aliases, not additional wire values.
    left = Left,
    right = Right,
};

// Stable public values. Zero is fail-safe so a cleared/unclassified state
// produces complete 2D output instead of guessed stereo.
enum class PresentationState : std::uint32_t {
    UnknownSafe = 0,
    WorldFirstPerson,
    WorldThirdPerson,
    DialogueStereo,
    FullscreenUi,
    MovieTheater,
    CutsceneTheater,
    PazaakTheater,
    SwoopTheater,
    TurretTheater,
    LoadingTheater,
};

enum class UiPresentationMode : std::uint32_t {
    ViewLockedHud = 0,
    WorldLockedFullscreen = 1,
    WorldLockedTheater = 2,
};

enum class HelloResult : std::uint32_t {
    Accepted = 0,
    ProtocolMismatch,
    UnsupportedBuild,
    UnsupportedPointerWidth,
    UnsupportedGraphicsPath,
    RuntimeUnavailable,
    AdapterMismatch,
};

enum class Capability : std::uint64_t {
    None = 0,
    AtomicStereoPair = 1ULL << 0,
    SharedColorTexture = 1ULL << 1,
    SharedDepthTexture = 1ULL << 2,
    SharedMotionTexture = 1ULL << 3,
    ProducerFence = 1ULL << 4,
    QuadLayerUi = 1ULL << 5,
    DlssInterop = 1ULL << 6,
};

enum class FrameFlag : std::uint32_t {
    None = 0,
    CameraCut = 1U << 0,
    TrackingDiscontinuity = 1U << 1,
    PosePredictionValid = 1U << 2,
};

enum class EyeFrameFlag : std::uint32_t {
    None = 0,
    ColorValid = 1U << 0,
    DepthValid = 1U << 1,
    MotionValid = 1U << 2,
    ProducerFenceValid = 1U << 3,
    TextureOriginBottomLeft = 1U << 4,
    DepthReversed = 1U << 5,
    DepthRangeZeroToOne = 1U << 6,
};

enum class UiFrameFlag : std::uint32_t {
    None = 0,
    TextureValid = 1U << 0,
    ProducerFenceValid = 1U << 1,
    PremultipliedAlpha = 1U << 2,
    CursorVisible = 1U << 3,
};

enum class RuntimeVisibility : std::uint32_t {
    None = 0,
    Visible = 1U << 0,
    Focused = 1U << 1,
};

enum class RuntimeHealth : std::uint32_t {
    None = 0,
    RuntimeReady = 1U << 0,
    SwapchainReady = 1U << 1,
    AdapterMatched = 1U << 2,
    ProducerStalled = 1U << 3,
    ConsumerStalled = 1U << 4,
    DeviceLost = 1U << 5,
    ProtocolError = 1U << 6,
    FlatFallbackActive = 1U << 7,
};

enum class ResetReason : std::uint32_t {
    None = 0,
    Startup = 1U << 0,
    ResolutionChange = 1U << 1,
    SwapchainRecreate = 1U << 2,
    RuntimeRestart = 1U << 3,
    DialogueCut = 1U << 4,
    Recenter = 1U << 5,
    CameraModeChange = 1U << 6,
    Loading = 1U << 7,
    Teleport = 1U << 8,
    DeviceReset = 1U << 9,
    UserRequested = 1U << 10,
};

template <typename Enum>
[[nodiscard]] constexpr Enum BitOr(Enum lhs, Enum rhs) noexcept {
    static_assert(std::is_enum_v<Enum>);
    using Underlying = std::underlying_type_t<Enum>;
    return static_cast<Enum>(static_cast<Underlying>(lhs) |
                             static_cast<Underlying>(rhs));
}

template <typename Enum>
[[nodiscard]] constexpr bool HasFlag(Enum value, Enum flag) noexcept {
    static_assert(std::is_enum_v<Enum>);
    using Underlying = std::underlying_type_t<Enum>;
    return (static_cast<Underlying>(value) & static_cast<Underlying>(flag)) ==
           static_cast<Underlying>(flag);
}

#pragma pack(push, 1)

struct SessionNonce {
    std::uint64_t low;
    std::uint64_t high;
};

struct MessageHeader {
    std::uint32_t magic;
    std::uint16_t protocol_major;
    std::uint16_t protocol_minor;
    std::uint16_t header_size;
    MessageType message_type;
    std::uint32_t structure_size;
    std::uint64_t sequence;
    SessionNonce session_nonce;
    std::uint64_t generation;
};

struct PoseF32 {
    float position_x;
    float position_y;
    float position_z;
    float orientation_x;
    float orientation_y;
    float orientation_z;
    float orientation_w;
};

struct FovF32 {
    float angle_left;
    float angle_right;
    float angle_up;
    float angle_down;
};

struct Matrix4x4F32 { std::array<float, 16> column_major; };

struct DepthRangeF32 {
    float near_z;
    float far_z;
    float min_depth;
    float max_depth;
};

struct Hello {
    MessageHeader header;
    std::uint32_t process_id;
    std::uint32_t endian_tag;
    std::uint32_t pointer_width_bits;
    std::uint32_t build_id;
    std::uint64_t requested_capabilities;
    std::uint64_t adapter_luid;
    std::uint32_t render_width;
    std::uint32_t render_height;
    std::array<std::uint8_t, 32> executable_sha256;
};

struct HelloAck {
    MessageHeader header;
    HelloResult result;
    std::uint16_t accepted_protocol_major;
    std::uint16_t accepted_protocol_minor;
    std::uint64_t accepted_capabilities;
    std::uint64_t adapter_luid;
    std::uint32_t render_width;
    std::uint32_t render_height;
};

struct RenderView {
    PoseF32 pose;
    FovF32 fov;
};

// One request advances the simulation once and renders the same state twice.
struct RenderRequest {
    MessageHeader header;
    std::uint64_t frame_id;
    std::int64_t predicted_display_time_ns;
    std::uint32_t render_width;
    std::uint32_t render_height;
    PresentationState presentation_state;
    std::uint32_t history_reset_reasons;
    std::array<RenderView, kStereoEyeCount> views;
};

struct EyeFrame {
    Eye eye;
    std::uint32_t slot_index;
    std::uint64_t shared_color_handle;
    std::uint64_t shared_depth_handle;
    std::uint64_t shared_motion_handle;
    std::uint64_t producer_fence_handle;
    std::uint64_t producer_fence_value;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t color_format;
    std::uint32_t depth_format;
    std::uint32_t motion_format;
    std::uint32_t flags;
    Matrix4x4F32 view_matrix;
    Matrix4x4F32 projection_matrix;
    PoseF32 render_pose;
    FovF32 fov;
    DepthRangeF32 depth;
};

// Protocol v1 deliberately has no single-eye submission.
struct FrameSubmit {
    MessageHeader header;
    std::uint64_t frame_id;
    std::int64_t predicted_display_time_ns;
    std::uint32_t flags;
    std::uint32_t eye_count;
    std::array<EyeFrame, kStereoEyeCount> eyes;
};

struct UiFrame {
    MessageHeader header;
    std::uint64_t frame_id;
    std::uint64_t shared_texture_handle;
    std::uint64_t producer_fence_handle;
    std::uint64_t producer_fence_value;
    std::uint32_t logical_width;
    std::uint32_t logical_height;
    float cursor_x;
    float cursor_y;
    UiPresentationMode mode;
    std::uint32_t flags;
};

struct HealthState {
    MessageHeader header;
    std::uint64_t game_heartbeat_qpc;
    std::uint64_t host_heartbeat_qpc;
    std::uint64_t presented_frame_count;
    std::uint64_t dropped_frame_count;
    std::uint64_t timeout_count;
    std::uint64_t duplicate_or_stale_count;
    std::uint32_t error_state;
    std::uint32_t visibility_flags;
    std::uint32_t health_flags;
    std::uint32_t reserved;
};

struct ControlReset {
    MessageHeader header;
    std::uint64_t next_generation;
    std::uint32_t reason_mask;
    std::uint32_t reserved;
};

#pragma pack(pop)

[[nodiscard]] constexpr bool IsValid(SessionNonce nonce) noexcept {
    return nonce.low != 0 || nonce.high != 0;
}

template <typename Message>
[[nodiscard]] constexpr MessageHeader MakeHeader(
    MessageType type, std::uint64_t sequence, SessionNonce nonce = {},
    std::uint64_t generation = 0) noexcept {
    static_assert(std::is_standard_layout_v<Message>);
    return MessageHeader{kProtocolMagic, kProtocolMajor, kProtocolMinor,
                         static_cast<std::uint16_t>(sizeof(MessageHeader)), type,
                         static_cast<std::uint32_t>(sizeof(Message)), sequence,
                         nonce, generation};
}

template <typename Message>
[[nodiscard]] constexpr bool HeaderMatches(const MessageHeader& header,
                                           MessageType type) noexcept {
    return header.magic == kProtocolMagic &&
           header.protocol_major == kProtocolMajor &&
           header.protocol_minor <= kProtocolMinor &&
           header.header_size == sizeof(MessageHeader) &&
           header.message_type == type &&
           header.structure_size == sizeof(Message);
}

[[nodiscard]] constexpr bool IsAtomicStereoPair(const FrameSubmit& frame) noexcept {
    if (!HeaderMatches<FrameSubmit>(frame.header, MessageType::FrameSubmit) ||
        !IsValid(frame.header.session_nonce) || frame.header.generation == 0 ||
        frame.header.sequence == 0 || frame.frame_id == 0 ||
        frame.predicted_display_time_ns <= 0 ||
        frame.eye_count != kStereoEyeCount) {
        return false;
    }
    const auto& left = frame.eyes[0];
    const auto& right = frame.eyes[1];
    constexpr std::uint32_t known_eye_flags =
        static_cast<std::uint32_t>(EyeFrameFlag::ColorValid) |
        static_cast<std::uint32_t>(EyeFrameFlag::DepthValid) |
        static_cast<std::uint32_t>(EyeFrameFlag::MotionValid) |
        static_cast<std::uint32_t>(EyeFrameFlag::ProducerFenceValid) |
        static_cast<std::uint32_t>(EyeFrameFlag::TextureOriginBottomLeft) |
        static_cast<std::uint32_t>(EyeFrameFlag::DepthReversed) |
        static_cast<std::uint32_t>(EyeFrameFlag::DepthRangeZeroToOne);
    constexpr std::uint32_t known_frame_flags =
        static_cast<std::uint32_t>(FrameFlag::CameraCut) |
        static_cast<std::uint32_t>(FrameFlag::TrackingDiscontinuity) |
        static_cast<std::uint32_t>(FrameFlag::PosePredictionValid);
    const auto valid_resource = [](std::uint32_t flags, EyeFrameFlag flag,
                                   std::uint64_t handle,
                                   std::uint32_t format) constexpr {
        const bool declared =
            (flags & static_cast<std::uint32_t>(flag)) != 0;
        return declared ? handle != 0 && format != 0
                        : handle == 0 && format == 0;
    };
    const auto valid_eye_frame = [&](const EyeFrame& eye) constexpr {
        const bool color_valid =
            (eye.flags & static_cast<std::uint32_t>(EyeFrameFlag::ColorValid)) != 0;
        const bool fence_valid =
            (eye.flags & static_cast<std::uint32_t>(
                             EyeFrameFlag::ProducerFenceValid)) != 0;
        const bool depth_valid =
            (eye.flags & static_cast<std::uint32_t>(EyeFrameFlag::DepthValid)) != 0;
        const std::uint32_t depth_metadata_flags =
            static_cast<std::uint32_t>(EyeFrameFlag::DepthReversed) |
            static_cast<std::uint32_t>(EyeFrameFlag::DepthRangeZeroToOne);
        return (eye.flags & ~known_eye_flags) == 0 && color_valid && fence_valid &&
               eye.slot_index < kEyeRingSlotCount && eye.width != 0 &&
               eye.height != 0 && eye.shared_color_handle != 0 &&
               eye.color_format != 0 && eye.producer_fence_handle != 0 &&
               eye.producer_fence_value != 0 &&
               valid_resource(eye.flags, EyeFrameFlag::DepthValid,
                              eye.shared_depth_handle, eye.depth_format) &&
               valid_resource(eye.flags, EyeFrameFlag::MotionValid,
                              eye.shared_motion_handle, eye.motion_format) &&
               (depth_valid || (eye.flags & depth_metadata_flags) == 0);
    };
    const bool left_depth =
        HasFlag(static_cast<EyeFrameFlag>(left.flags), EyeFrameFlag::DepthValid);
    const bool right_depth =
        HasFlag(static_cast<EyeFrameFlag>(right.flags), EyeFrameFlag::DepthValid);
    const bool left_motion =
        HasFlag(static_cast<EyeFrameFlag>(left.flags), EyeFrameFlag::MotionValid);
    const bool right_motion =
        HasFlag(static_cast<EyeFrameFlag>(right.flags), EyeFrameFlag::MotionValid);
    constexpr std::uint32_t layout_flags =
        static_cast<std::uint32_t>(EyeFrameFlag::TextureOriginBottomLeft) |
        static_cast<std::uint32_t>(EyeFrameFlag::DepthReversed) |
        static_cast<std::uint32_t>(EyeFrameFlag::DepthRangeZeroToOne);
    return (frame.flags & ~known_frame_flags) == 0 &&
           left.eye == Eye::Left && right.eye == Eye::Right &&
           valid_eye_frame(left) && valid_eye_frame(right) &&
           left.width != 0 && left.height != 0 && right.width != 0 &&
           right.height != 0 && left.width == right.width &&
           left.height == right.height &&
           left.color_format == right.color_format &&
           left.shared_color_handle != right.shared_color_handle &&
           (left.flags & layout_flags) == (right.flags & layout_flags) &&
           left_depth == right_depth && left_motion == right_motion &&
           left.depth_format == right.depth_format &&
           left.motion_format == right.motion_format &&
           (!left_depth ||
            left.shared_depth_handle != right.shared_depth_handle) &&
           (!left_motion ||
            left.shared_motion_handle != right.shared_motion_handle);
}

static_assert(sizeof(SessionNonce) == 16);
static_assert(sizeof(MessageHeader) == 48);
static_assert(sizeof(PoseF32) == 28);
static_assert(sizeof(FovF32) == 16);
static_assert(sizeof(Matrix4x4F32) == 64);
static_assert(sizeof(DepthRangeF32) == 16);
static_assert(sizeof(Hello) == 120);
static_assert(sizeof(HelloAck) == 80);
static_assert(sizeof(RenderView) == 44);
static_assert(sizeof(RenderRequest) == 168);
static_assert(sizeof(EyeFrame) == 260);
static_assert(sizeof(FrameSubmit) == 592);
static_assert(sizeof(UiFrame) == 104);
static_assert(sizeof(HealthState) == 112);
static_assert(sizeof(ControlReset) == 64);

static_assert(offsetof(MessageHeader, session_nonce) == 24);
static_assert(offsetof(MessageHeader, generation) == 40);
static_assert(offsetof(Hello, executable_sha256) == 88);
static_assert(offsetof(RenderRequest, views) == 80);
static_assert(offsetof(FrameSubmit, eyes) == 72);
static_assert(offsetof(EyeFrame, view_matrix) == 72);
static_assert(offsetof(EyeFrame, projection_matrix) == 136);

static_assert(std::is_standard_layout_v<RenderRequest>);
static_assert(std::is_trivially_copyable_v<RenderRequest>);
static_assert(std::is_standard_layout_v<FrameSubmit>);
static_assert(std::is_trivially_copyable_v<FrameSubmit>);
static_assert(std::is_standard_layout_v<UiFrame>);
static_assert(std::is_trivially_copyable_v<UiFrame>);
static_assert(std::is_standard_layout_v<HealthState>);
static_assert(std::is_trivially_copyable_v<HealthState>);

} // namespace k2vr::ipc
