#include "build_descriptor.hpp"
#include "engine_state.hpp"
#include "gpu_stream_contract.hpp"
#include "hook_validation.hpp"
#include "ipc_protocol.hpp"
#include "math.hpp"
#include "ui_mapping.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>

namespace {

int g_failures = 0;

void Check(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++g_failures;
    }
}

[[nodiscard]] bool Near(float lhs, float rhs, float epsilon = 1.0e-4F) {
    return std::fabs(lhs - rhs) <= epsilon;
}

void TestIpcLayoutAndValidation() {
    using namespace k2vr::ipc;

    Hello hello{};
    hello.header = MakeHeader<Hello>(MessageType::Hello, 7, {11, 22});
    hello.process_id = 42;
    hello.endian_tag = kLittleEndianTag;
    hello.pointer_width_bits = 32;
    hello.requested_capabilities = static_cast<std::uint64_t>(
        BitOr(Capability::AtomicStereoPair, Capability::SharedColorTexture));
    Check(HeaderMatches<Hello>(hello.header, MessageType::Hello),
          "Hello header is self-describing");
    Check(hello.header.structure_size == 120, "Hello wire size stays fixed");
    Check(IsValid(hello.header.session_nonce),
          "session nonce is carried in every header");
    Check(HasFlag(static_cast<Capability>(hello.requested_capabilities),
                  Capability::AtomicStereoPair),
          "capability bit test");

    constexpr MessageHeader golden_header = MakeHeader<Hello>(
        MessageType::Hello, 0x0102030405060708ULL,
        {0x1112131415161718ULL, 0x2122232425262728ULL},
        0x3132333435363738ULL);
    constexpr std::array<std::uint8_t, sizeof(MessageHeader)> golden_bytes{
        0x4B, 0x32, 0x56, 0x52, 0x01, 0x00, 0x00, 0x00,
        0x30, 0x00, 0x01, 0x00, 0x78, 0x00, 0x00, 0x00,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
        0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21,
        0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31};
    Check(std::bit_cast<std::array<std::uint8_t, sizeof(MessageHeader)>>(
              golden_header) == golden_bytes,
          "golden IPC header bytes match identically in x86 and x64 tests");

    FrameSubmit frame{};
    frame.header = MakeHeader<FrameSubmit>(MessageType::FrameSubmit, 8,
                                           {11, 22}, 3);
    frame.frame_id = 11;
    frame.predicted_display_time_ns = 123456;
    frame.eye_count = kStereoEyeCount;
    frame.eyes[0].eye = Eye::Left;
    frame.eyes[1].eye = Eye::Right;
    for (std::size_t index = 0; index < frame.eyes.size(); ++index) {
        EyeFrame& eye = frame.eyes[index];
        eye.slot_index = static_cast<std::uint32_t>(index);
        eye.width = 2016;
        eye.height = 2208;
        eye.shared_color_handle = 1 + index;
        eye.color_format = 28;
        eye.producer_fence_handle = 10;
        eye.producer_fence_value = 11;
        eye.flags = static_cast<std::uint32_t>(BitOr(
            EyeFrameFlag::ColorValid, EyeFrameFlag::ProducerFenceValid));
    }
    Check(IsAtomicStereoPair(frame), "complete left/right pair is accepted");
    frame.eyes[1].eye = Eye::Left;
    Check(!IsAtomicStereoPair(frame), "duplicate left eye is rejected");
    frame.eyes[1].eye = Eye::Right;
    frame.eye_count = 1;
    Check(!IsAtomicStereoPair(frame), "single-eye submission is rejected");
    frame.eye_count = kStereoEyeCount;
    frame.eyes[1].width = frame.eyes[0].width + 1;
    Check(!IsAtomicStereoPair(frame), "mismatched stereo dimensions are rejected");
    frame.eyes[1].width = frame.eyes[0].width;
    frame.header.protocol_major = kProtocolMajor + 1;
    Check(!IsAtomicStereoPair(frame), "future major protocol is rejected");
    frame.header.protocol_major = kProtocolMajor;
    frame.header.session_nonce = {};
    Check(!IsAtomicStereoPair(frame), "zero session nonce is rejected");
    frame.header.session_nonce = {11, 22};
    frame.eyes[1].producer_fence_value = 0;
    Check(!IsAtomicStereoPair(frame), "missing producer fence value is rejected");
    frame.eyes[1].producer_fence_value = 11;
    frame.eyes[1].shared_color_handle = frame.eyes[0].shared_color_handle;
    Check(!IsAtomicStereoPair(frame), "aliased eye color resources are rejected");
    frame.eyes[1].shared_color_handle = 2;
    frame.eyes[1].eye = static_cast<Eye>(99);
    Check(!IsAtomicStereoPair(frame), "invalid wire eye value is rejected");

    RenderRequest request{};
    request.header = MakeHeader<RenderRequest>(MessageType::RenderRequest, 9,
                                               {11, 22}, 3);
    request.presentation_state = PresentationState::DialogueStereo;
    request.views[0].fov.angle_left = -0.8F;
    request.views[1].fov.angle_right = 0.8F;
    Check(HeaderMatches<RenderRequest>(request.header,
                                       MessageType::RenderRequest),
          "RenderRequest carries predicted stereo views in the shared ABI");

    UiFrame ui{};
    ui.header = MakeHeader<UiFrame>(MessageType::UiFrame, 10, {11, 22}, 3);
    ui.logical_width = 1920;
    ui.logical_height = 1080;
    ui.mode = UiPresentationMode::ViewLockedHud;
    Check(sizeof(ui) == 104 && ui.logical_width == 1920,
          "UI frame has a fixed logical-resolution contract");
}

void TestGpuStreamContract() {
    using namespace k2vr::ipc;

    constexpr SessionNonce nonce{0xFEDCBA9876543210ULL,
                                 0x0123456789ABCDEFULL};
    constexpr auto color = MakeGpuStreamObjectName(
        nonce, GpuStreamObjectKind::Color);
    constexpr auto ready = MakeGpuStreamObjectName(
        nonce, GpuStreamObjectKind::ReadyFence);
    constexpr auto consumed = MakeGpuStreamObjectName(
        nonce, GpuStreamObjectKind::ConsumedFence);
    Check(color.valid() &&
              std::wstring_view(color.c_str()) ==
                  L"Local\\Kotor2VR-gpu-stream-v1-"
                  L"0123456789ABCDEFFEDCBA9876543210-color",
          "GPU stream color name carries the nonce in HIGH/LOW order");
    Check(ready.valid() && consumed.valid() &&
              std::wstring_view(ready.c_str()).ends_with(L"-ready") &&
              std::wstring_view(consumed.c_str()).ends_with(L"-consumed"),
          "GPU stream fences have distinct deterministic names");
    Check(!MakeGpuStreamObjectName({}, GpuStreamObjectKind::Color).valid(),
          "zero nonce cannot name a GPU stream object");
    Check(std::wstring_view(MakeGpuStreamColorObjectName(nonce, 0).c_str()) ==
                  L"Local\\Kotor2VR-gpu-stream-v1-"
                  L"0123456789ABCDEFFEDCBA9876543210-color-0" &&
              std::wstring_view(
                  MakeGpuStreamColorObjectName(nonce, 2).c_str()) ==
                  L"Local\\Kotor2VR-gpu-stream-v1-"
                  L"0123456789ABCDEFFEDCBA9876543210-color-2" &&
              !MakeGpuStreamColorObjectName(nonce, kGpuStreamSlotCount)
                   .valid(),
          "triple-buffer color names identify exactly three stable slots");
    static_assert(GpuStreamSlotForSequence(1) == 0);
    static_assert(GpuStreamSlotForSequence(2) == 1);
    static_assert(GpuStreamSlotForSequence(3) == 2);
    static_assert(GpuStreamSlotForSequence(4) == 0);
    Check(kGpuStreamWidth == 1024 && kGpuStreamHeight == 576 &&
              kGpuStreamDxgiFormatRgba8Unorm == 28 &&
              kGpuStreamSlotCount == 3,
          "GPU stream milestone has one exact pixel contract");

    Check(CanProduceGpuStreamFrame(0, 0),
          "the first producer frame never waits for an acknowledgement");
    Check(!CanProduceGpuStreamFrame(7, 6) &&
              CanProduceGpuStreamFrame(7, 7),
          "producer drops rather than overwrites an unconsumed texture");
    Check(CanReuseGpuStreamSlot(0, 0) &&
              !CanReuseGpuStreamSlot(7, 6) &&
              CanReuseGpuStreamSlot(7, 7) &&
              CanReuseGpuStreamSlot(7, 9),
          "a ring slot is reusable only after its prior publication is consumed");
    Check(HasNewGpuStreamFrame(8, 7) &&
              !HasNewGpuStreamFrame(7, 7) &&
              !HasNewGpuStreamFrame(
                  (std::numeric_limits<std::uint64_t>::max)(), 7),
          "consumer accepts only a newer usable ready-fence value");
}

void TestPoseMath() {
    using namespace k2vr::math;

    const Pose parent{{10.0F, 0.0F, 0.0F},
                      FromAxisAngle({0.0F, 1.0F, 0.0F}, kPi * 0.5F)};
    const Pose local{{0.0F, 0.0F, -2.0F}, {}};
    const Pose composed = Compose(parent, local);
    Check(Near(composed.position.x, 8.0F) &&
              Near(composed.position.y, 0.0F) &&
              Near(composed.position.z, 0.0F),
          "pose composition rotates local translation before adding it");

    const Pose relative = RelativeTo(parent, composed);
    Check(Near(relative.position.x, local.position.x) &&
              Near(relative.position.y, local.position.y) &&
              Near(relative.position.z, local.position.z),
          "relative pose reverses composition");

    const Pose unbounded_delta{{3.0F, 4.0F, 0.0F},
                               FromAxisAngle({0.0F, 1.0F, 0.0F}, kPi)};
    const Pose bounded = ClampPoseDelta(unbounded_delta, {2.0F, kPi / 4.0F});
    Check(Near(Length(bounded.position), 2.0F),
          "translation clamp preserves direction");
    Check(Near(AngularDistanceFromIdentity(bounded.orientation), kPi / 4.0F),
          "rotation clamp uses shortest angular distance");

    Pose invalid{};
    invalid.position.x = std::numeric_limits<float>::quiet_NaN();
    const Pose rejected = ClampPoseDelta(invalid, {1.0F, 1.0F});
    Check(Near(Length(rejected.position), 0.0F) &&
              Near(rejected.orientation.w, 1.0F),
          "non-finite tracked delta fails to identity");

    const Pose base{{1.0F, 2.0F, 3.0F}, {}};
    const Pose head{{0.5F, 0.0F, 0.0F}, {}};
    const Pose eye{{0.03F, 0.0F, 0.0F}, {}};
    const Pose tracked = ComposeTrackedEye(base, head, eye, {0.25F, kPi});
    Check(Near(tracked.position.x, 1.28F),
          "base, bounded head, then eye composition order");

    const Vec3 leaned = ClampSeatedLean({0.3F, 0.2F, 0.4F}, {0.20F, 0.10F});
    Check(Near(std::sqrt(leaned.x * leaned.x + leaned.z * leaned.z), 0.20F) &&
              Near(leaned.y, 0.10F),
          "seated leaning clamps horizontal radius and height independently");

    const Pose left_eye = ComposeTrackedEye(base, {}, {{-0.032F, 0.0F, 0.0F}, {}},
                                            {0.25F, kPi});
    const Pose right_eye = ComposeTrackedEye(base, {}, {{0.032F, 0.0F, 0.0F}, {}},
                                             {0.25F, kPi});
    Check(Near(right_eye.position.x - left_eye.position.x, 0.064F),
          "per-eye offsets preserve a 64 mm IPD");

    const auto projection = OpenGlProjection(
        {-0.70F, 0.90F, 0.80F, -0.60F}, 0.05F, 500.0F);
    Check(projection.has_value(), "asymmetric OpenXR FOV builds a GL projection");
    if (projection) {
        Check(!Near(projection->value[8], 0.0F) &&
                  !Near(projection->value[9], 0.0F),
              "asymmetric projection retains horizontal and vertical offsets");
        Check(Near(projection->value[11], -1.0F),
              "projection has right-handed perspective divide");
    }
    Check(!OpenGlProjection({-0.7F, 0.7F, 0.7F, -0.7F}, 1.0F, 0.5F),
          "invalid near/far range is rejected");
}

void TestEngineStates() {
    using namespace k2vr::engine;

    Check(Classify(EngineFlag::None) == EngineState::Unknown,
          "empty evidence is unknown");
    Check(Classify(EngineFlag::ProcessAttached) == EngineState::Boot,
          "attached process classifies as boot");
    Check(Classify(EngineFlag::WorldLoaded | EngineFlag::DialogueActive) ==
              EngineState::Dialogue,
          "dialogue wins over world evidence");
    Check(Classify(EngineFlag::WorldLoaded | EngineFlag::DialogueActive |
                   EngineFlag::MovieActive) == EngineState::Movie,
          "movie wins over dialogue evidence");
    Check(Classify(EngineFlag::WorldLoaded | EngineFlag::CinematicActive) ==
              EngineState::Cinematic,
          "non-dialogue engine cinematic uses its own policy state");
    Check(Classify(EngineFlag::CinematicActive | EngineFlag::DialogueActive) ==
              EngineState::Dialogue,
          "dialogue stays stereo even with an animated cinematic camera");
    Check(RendersStereoWorld(EngineState::Dialogue),
          "dialogue retains native stereo camera");
    Check(UsesTheaterLayer(EngineState::Pazaak) &&
              UsesTheaterLayer(EngineState::MiniGame) &&
              UsesTheaterLayer(EngineState::Movie) &&
              UsesTheaterLayer(EngineState::Cinematic),
          "special modes use a theater layer");
    Check(!UsesTheaterLayer(EngineState::Dialogue),
          "dialogue is not forced to theater mode");

    using k2vr::ipc::PresentationState;
    Check(ClassifyPresentation(EngineFlag::WorldLoaded, true) ==
              PresentationState::WorldFirstPerson,
          "world defaults to first-person presentation");
    Check(ClassifyPresentation(EngineFlag::WorldLoaded, false) ==
              PresentationState::WorldThirdPerson,
          "manual third-person selection remains distinct");
    Check(ClassifyPresentation(EngineFlag::WorldLoaded |
                                   EngineFlag::DialogueActive,
                               true) == PresentationState::DialogueStereo,
          "dialogue remains stereo");
    Check(ClassifyPresentation(EngineFlag::WorldLoaded |
                                   EngineFlag::SwoopActive,
                               true) == PresentationState::SwoopTheater,
          "swoop overrides world stereo");
    Check(ClassifyPresentation(EngineFlag::MiniGameActive, true) ==
              PresentationState::UnknownSafe,
          "unclassified minigame fails to full 2D safe mode");
}

void TestUiMapping() {
    using namespace k2vr::ui;
    const auto fitted = FitLogicalSurface({0.0F, 0.0F, 2000.0F, 1200.0F},
                                          1920, 1080);
    Check(fitted.has_value(), "16:9 UI fits an available surface");
    if (!fitted) {
        return;
    }
    Check(Near(fitted->width, 2000.0F) && Near(fitted->height, 1125.0F) &&
              Near(fitted->y, 37.5F),
          "aspect fitting creates centered letterbox padding");

    const auto top_left =
        MapPointerToLogical({fitted->x, fitted->y}, *fitted, 1920, 1080);
    const auto bottom_right = MapPointerToLogical(
        {fitted->x + fitted->width, fitted->y + fitted->height}, *fitted,
        1920, 1080);
    Check(top_left && Near(top_left->x, 0.0F) && Near(top_left->y, 0.0F),
          "UI top-left maps exactly");
    Check(bottom_right && Near(bottom_right->x, 1919.0F) &&
              Near(bottom_right->y, 1079.0F),
          "UI bottom-right clamps to the last logical pixel");
    Check(!MapPointerToLogical({1000.0F, 0.0F}, *fitted, 1920, 1080),
          "letterbox padding is not clickable");
}

void TestBuildIdentity() {
    using namespace k2vr::builds;

    constexpr std::string_view steam_hash =
        "6A522E71631DCEE93467BD2010F3B23D9145326E1E2E89305F13AB104DBBFFEF";
    const auto parsed = ParseSha256(steam_hash);
    Check(parsed.has_value(), "known Steam SHA-256 parses");
    Check(!ParseSha256("not-a-hash").has_value(),
          "malformed SHA-256 is rejected");
    if (parsed) {
        const auto hex = ToHex(*parsed);
        Check(std::string_view(hex.data(), 64) == steam_hash,
              "SHA-256 round trip preserves canonical digest");
    }

    const ExactBuildDescriptor* steam =
        parsed ? FindExactBuild(*parsed, kPeMachineI386) : nullptr;
    Check(steam != nullptr, "exact Steam build is recognized");
    Check(parsed && FindExactBuild(*parsed, 0x8664U) == nullptr,
          "matching hash with wrong PE machine is rejected");
    if (steam != nullptr) {
        Check(steam->support_level == SupportLevel::ProbeOnly,
              "known build cannot authorize hooks yet");
        Check(steam->executable_file_size == 0x648800ULL,
              "Steam executable size is pinned in the descriptor");
        const KnownAddress* app = FindAddress(*steam, SymbolId::AppManagerPointer);
        Check(app != nullptr && app->preferred_virtual_address == 0x00A1B4A4U,
              "Steam APP_MANAGER_PTR descriptor");
        Check(app != nullptr && ToRva(*steam, *app) == 0x0061B4A4U,
              "preferred VA converts to image-relative RVA");
    }


    static_assert(!std::is_default_constructible_v<VerifiedBuild>);
    static_assert(!std::is_constructible_v<VerifiedBuild,
                                           const ExactBuildDescriptor*,
                                           std::uintptr_t, std::uint32_t>);
    const BuildVerificationResult running_test = VerifyMainExecutable();
    Check(!running_test.IsVerified(),
          "an unrelated test executable cannot mint a verified KOTOR build");
    Check(!running_test.verified_build.has_value(),
          "failed verification returns no hook-authority capability");
}

void TestPatternValidation() {
    using namespace k2vr::builds;
    using namespace k2vr::hooks;

    constexpr std::array<std::uint8_t, 3> bytes{0xAA, 0xBB, 0xCC};
    constexpr std::array<std::uint8_t, 3> exact_mask{0xFF, 0xFF, 0xFF};
    constexpr std::array<std::uint8_t, 3> wildcard_mask{0xFF, 0x00, 0xFF};
    constexpr std::array<std::uint8_t, 3> invalid_mask{0xFF, 0x7F, 0xFF};
    constexpr std::array<std::uint8_t, 3> no_compared_bytes{0, 0, 0};
    constexpr std::array<std::uint8_t, 8> unique_haystack{
        0, 0xAA, 0x12, 0xCC, 0, 0xAA, 0x13, 0};

    Check(IsValidPattern({bytes, exact_mask}), "strict pattern is valid");
    Check(!IsValidPattern({bytes, invalid_mask}),
          "partial-bit masks are rejected");
    Check(!IsValidPattern({bytes, no_compared_bytes}),
          "all-wildcard pattern is rejected");
    Check(MatchPattern(std::span(unique_haystack).subspan(1),
                       {bytes, wildcard_mask}) == PatternStatus::Match,
          "explicit wildcard byte works");

    const PatternSearchResult one =
        FindUniquePattern(unique_haystack, {bytes, wildcard_mask});
    Check(one.status == PatternStatus::Match && one.offset == 1 &&
              one.match_count == 1,
          "unique signature search reports its offset");
    constexpr std::array<std::uint8_t, 7> ambiguous_haystack{
        0xAA, 1, 0xCC, 0, 0xAA, 2, 0xCC};
    const PatternSearchResult many =
        FindUniquePattern(ambiguous_haystack, {bytes, wildcard_mask});
    Check(many.status == PatternStatus::Ambiguous && many.match_count == 2,
          "ambiguous signature cannot select a site");

    const auto builds = KnownBuilds();
    Check(!builds.empty(), "test has a known build descriptor");
    if (builds.empty()) {
        return;
    }

    std::array<std::uint8_t, 8> image{0, 0, 0xAA, 0xBB, 0xCC, 0, 0, 0};
    HookCandidate candidate{HookId::CameraUpdate,
                            builds.front().id,
                            "synthetic test candidate",
                            2,
                            {bytes, exact_mask},
                            HookReadiness::Installable};
    Check(ValidateHookCandidate(nullptr, candidate, image).status ==
              HookGateStatus::UnknownBuild,
          "a public descriptor or synthetic byte buffer cannot substitute for a verified build");

    for (const HookCandidate& known : KnownProbeCandidates()) {
        Check(known.readiness == HookReadiness::ProbeOnly,
              "every public K2 anchor remains probe-only");
        const ExactBuildDescriptor* descriptor = nullptr;
        for (const ExactBuildDescriptor& build : KnownBuilds()) {
            if (build.id == known.build_id) {
                descriptor = &build;
                break;
            }
        }
        Check(descriptor != nullptr &&
                  descriptor->support_level == SupportLevel::ProbeOnly,
              "no checked-in K2 build or anchor is hook-ready");
    }
}

} // namespace

int main() {
    TestIpcLayoutAndValidation();
    TestGpuStreamContract();
    TestPoseMath();
    TestEngineStates();
    TestUiMapping();
    TestBuildIdentity();
    TestPatternValidation();

    if (g_failures != 0) {
        std::cerr << g_failures << " common-core test(s) failed\n";
        return 1;
    }
    std::cout << "All KOTOR2VR common-core tests passed\n";
    return 0;
}
