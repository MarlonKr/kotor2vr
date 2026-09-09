#include "kotorvr/host/d3d12_bgra_upload.hpp"
#include "kotorvr/host/ui_pose.hpp"
#include "kotorvr/host/game_image_snapshot.hpp"
#include "kotorvr/host/visible_smoke.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace {

int failures{};

void Check(const bool condition, const std::string_view description) {
    if (condition) {
        std::cout << "PASS: " << description << '\n';
    } else {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

kotorvr::host::GameImageSnapshotHeader ValidHeader() {
    using namespace kotorvr::host;
    GameImageSnapshotHeader header{};
    header.magic = game_image_magic;
    header.version_major = game_image_version_major;
    header.version_minor = game_image_version_minor;
    header.header_size = sizeof(header);
    header.mapping_size = sizeof(header) + 8U * 2U;
    header.sequence = 2;
    header.width = 2;
    header.height = 2;
    header.stride = 8;
    header.pixel_format = game_image_pixel_format_bgra8_unorm;
    header.frame_id = 17;
    return header;
}

} // namespace

int main() {
    using namespace kotorvr::host;

    {
        using namespace k2vr::math;
        const Vec3 head_position{1.2F,1.7F,-0.4F};
        for (const float yaw: {-2.9F,-0.7F,0.F,1.4F,3.F}) {
            for (const float pitch: {-1.4F,-0.5F,0.F,0.8F,1.4F}) {
                const auto gaze=Multiply(FromAxisAngle({0,1,0},yaw),FromAxisAngle({1,0,0},pitch));
                for (const float roll: {-1.F,0.6F,1.8F}) {
                    const k2vr::math::Pose raw{head_position,Multiply(gaze,FromAxisAngle({0,0,1},roll))};
                    const auto upright=MakeUprightUiHead(raw);
                    const auto right=Rotate(upright.pose.orientation,{1,0,0});
                    const auto forward=Rotate(upright.pose.orientation,{0,0,-1});
                    const auto expected=Rotate(raw.orientation,{0,0,-1});
                    const auto panel=PlaceUiPanel(upright.pose,1.5F);
                    Check(upright.valid && std::abs(right.y)<0.00001F &&
                        Length(forward-expected)<0.00001F &&
                        Length(panel.position-(head_position+expected*1.5F))<0.00001F,
                        "UI follows yaw/pitch and head position, independent of head roll");
                }
            }
        }
        const k2vr::math::Pose raw{{0,1.7F,0},Multiply(FromAxisAngle({0,1,0},0.7F),
            FromAxisAngle({1,0,0},kPi*0.5F))};
        const auto vertical=MakeUprightUiHead(raw,0.7F);
        Check(vertical.valid && vertical.yaw==0.7F && IsFinite(vertical.pose) &&
            std::abs(Rotate(vertical.pose.orientation,{1,0,0}).y)<0.00001F,
            "vertical gaze retains prior yaw without introducing roll");
        const auto tilted=MakeUprightUiHead({{0,1,0},Multiply(FromAxisAngle({1,0,0},0.4F),
            FromAxisAngle({0,0,1},0.8F))});
        const auto normal=PlaceUiPanel(tilted.pose,1.5F);
        const auto dialogue=PlaceUiPanel(tilted.pose,1.5F,0.1232F);
        Check(Length((dialogue.position-normal.position)-
            Rotate(tilted.pose.orientation,{0,0.1232F,0}))<0.00001F,
            "dialogue offset follows the level panel up axis");
        Check(!MakeUprightUiHead({{}, {0,0,0,0}}).valid &&
            !MakeUprightUiHead({{0,std::numeric_limits<float>::quiet_NaN(),0},{}}).valid,
            "invalid head poses cannot produce UI anchors");
    }

    constexpr k2vr::ipc::SessionNonce nonce{0x1122334455667788ULL,
                                             0x8877665544332211ULL};
    const GameImageMappingName name = MakeGameImageMappingName(nonce);
    Check(name.valid() &&
              std::wstring_view(name.c_str()) ==
                  L"Local\\Kotor2VR-game-image-v1-"
                  L"88776655443322111122334455667788",
          "mapping name renders HIGH then LOW nonce halves");
    Check(!MakeGameImageMappingName({}).valid(),
          "zero nonce cannot address a snapshot mapping");

    GameImageSnapshotHeader header = ValidHeader();
    Check(sizeof(header) == 64 && offsetof(GameImageSnapshotHeader, sequence) == 16 &&
              offsetof(GameImageSnapshotHeader, frame_id) == 36 &&
              ValidateGameImageSnapshotHeader(header, header.mapping_size) ==
                  GameImageSnapshotStatus::Ok,
          "packed 64-byte v1 wire header validates at exact offsets");
    header.reserved = {game_image_gpu_diagnostic_magic, 5U, 12U,
                       0x80004005U, 87U};
    const GameImageGpuDiagnostic diagnostic =
        DecodeGameImageGpuDiagnostic(header);
    Check(diagnostic.present() && diagnostic.producer_status == 5U &&
              diagnostic.interop_status == 12U &&
              diagnostic.hresult_bits == 0x80004005U &&
              diagnostic.win32_error == 87U &&
              GpuProducerStatusName(diagnostic.producer_status) ==
                  "interop-initialization-failure" &&
              GpuInteropStatusName(diagnostic.interop_status) ==
                  "interop-device-open-failure",
          "reserved snapshot words carry a named x86 GPU start diagnostic");
    header = ValidHeader();
    Check(!DecodeGameImageGpuDiagnostic(header).present() &&
              GpuInteropStatusName(game_image_interop_status_unavailable) ==
                  "not-reported",
          "zeroed legacy reserved words do not fabricate a GPU diagnostic");
    header.magic ^= 1U;
    Check(ValidateGameImageSnapshotHeader(header, 4096) ==
              GameImageSnapshotStatus::InvalidHeader,
          "wrong snapshot magic fails closed");
    header = ValidHeader();
    ++header.version_minor;
    Check(ValidateGameImageSnapshotHeader(header, 4096) ==
              GameImageSnapshotStatus::InvalidHeader,
          "unknown snapshot version fails closed");
    header = ValidHeader();
    header.pixel_format = 2;
    Check(ValidateGameImageSnapshotHeader(header, 4096) ==
              GameImageSnapshotStatus::UnsupportedPixelFormat,
          "non-BGRA snapshot format is rejected");
    header = ValidHeader();
    header.stride = 7;
    Check(ValidateGameImageSnapshotHeader(header, 4096) ==
              GameImageSnapshotStatus::InvalidDimensions,
          "short producer stride is rejected");
    header = ValidHeader();
    header.width = game_image_max_dimension + 1;
    Check(ValidateGameImageSnapshotHeader(header, 1U << 20U) ==
              GameImageSnapshotStatus::InvalidDimensions,
          "oversized producer image is rejected");
    header = ValidHeader();
    Check(ValidateGameImageSnapshotHeader(header, header.mapping_size - 1U) ==
              GameImageSnapshotStatus::InvalidHeader,
          "truncated mapping cannot validate");

    const BgraUploadLayout small = ComputeBgraUploadLayout({3, 2});
    Check(small.row_pitch == 256 && small.byte_size == 512,
          "D3D12 upload rows align to 256 bytes");
    const BgraUploadLayout aligned = ComputeBgraUploadLayout({1024, 576});
    Check(aligned.row_pitch == 4096 && aligned.byte_size == 4096ULL * 576ULL,
          "already aligned smoke extent preserves tight pitch");
    Check(!ComputeBgraUploadLayout({}).valid() &&
              !ComputeBgraUploadLayout(
                   {(std::numeric_limits<std::uint32_t>::max)(), 1})
                   .valid(),
          "invalid and overflowing upload extents are rejected");
    Check(ClassifyBgraUploadTarget(smoke_format_bgra8_unorm) ==
                  BgraUploadTarget::Bgra8 &&
              ClassifyBgraUploadTarget(smoke_format_rgba8_srgb) ==
                  BgraUploadTarget::Rgba8 &&
              ClassifyBgraUploadTarget(10) ==
                  BgraUploadTarget::Unsupported,
          "swapchain format selects channel order explicitly");

    GameImageFrame frame{};
    frame.frame_id = 17;
    frame.sequence = 2;
    frame.width = 2;
    frame.height = 2;
    frame.pixels = {
        1, 2, 3, 4,   5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16};

    std::array<std::uint8_t, 16> exact{};
    Check(ResampleBgra8(frame, {2, 2}, exact, 8,
                        BgraUploadTarget::Bgra8) &&
              std::equal(exact.begin(), exact.end(), frame.pixels.begin()),
          "same-size BGRA upload preserves every channel");

    std::array<std::uint8_t, 16> rgba{};
    Check(ResampleBgra8(frame, {2, 2}, rgba, 8,
                        BgraUploadTarget::Rgba8) &&
              rgba == std::array<std::uint8_t, 16>{
                          3, 2, 1, 4,   7, 6, 5, 8,
                          11, 10, 9, 12, 15, 14, 13, 16},
          "RGBA swapchain upload swaps red and blue only");

    std::vector<std::uint8_t> scaled(4U * 4U * 4U, 0);
    Check(ResampleBgra8(frame, {4, 4}, scaled, 16,
                        BgraUploadTarget::Bgra8) &&
              scaled[0] == 1 && scaled[4U * 3U] == 5 &&
              scaled[16U * 3U] == 9 && scaled[16U * 3U + 4U * 3U] == 13,
          "nearest-neighbour scaling maps all four source corners");
    Check(!ResampleBgra8(frame, {4, 4}, scaled, 15,
                         BgraUploadTarget::Bgra8) &&
              !ResampleBgra8(frame, {4, 4}, scaled, 16,
                             BgraUploadTarget::Unsupported),
          "short destination stride and unsupported target fail closed");

    std::array<std::uint8_t,32> letterboxed{};
    Check(ResampleBgra8(frame,{4,2},letterboxed,16,BgraUploadTarget::Bgra8,true) &&
        letterboxed[0]==0 && letterboxed[3]==255 && letterboxed[4]==frame.pixels[0] &&
        letterboxed[8]==frame.pixels[4] && letterboxed[12]==0 && letterboxed[15]==255,
        "square movie is pillarboxed instead of stretched into a wide theater");
    Check(ResampleBgra8(frame,{2,4},letterboxed,8,BgraUploadTarget::Bgra8,true) &&
        letterboxed[0]==0 && letterboxed[3]==255 && letterboxed[8]==frame.pixels[0] &&
        letterboxed[16]==frame.pixels[8] && letterboxed[24]==0 && letterboxed[31]==255,
        "square movie is letterboxed in a tall target with opaque bars");

    if (failures == 0) {
        std::cout << "All game-image smoke tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
