#include "gpu_stream_contract.hpp"
#include "kotorvr/host/gpu_stream_consumer.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

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

} // namespace

int main() {
    using namespace k2vr::ipc;
    using namespace kotorvr::host;

    constexpr SessionNonce nonce{0x1122334455667788ULL,
                                 0x8877665544332211ULL};
    Check(std::wstring_view(MakeGpuStreamObjectName(
                                nonce, GpuStreamObjectKind::Color)
                                .c_str()) ==
              L"Local\\Kotor2VR-gpu-stream-v1-"
              L"88776655443322111122334455667788-color" &&
              std::wstring_view(MakeGpuStreamObjectName(
                                    nonce, GpuStreamObjectKind::ReadyFence)
                                    .c_str()) ==
                  L"Local\\Kotor2VR-gpu-stream-v1-"
                  L"88776655443322111122334455667788-ready" &&
              std::wstring_view(MakeGpuStreamObjectName(
                                    nonce, GpuStreamObjectKind::ConsumedFence)
                                    .c_str()) ==
                  L"Local\\Kotor2VR-gpu-stream-v1-"
                  L"88776655443322111122334455667788-consumed",
          "consumer uses the immutable HIGH/LOW object names");
    Check(std::wstring_view(MakeGpuStreamColorObjectName(nonce, 0).c_str()) ==
                  L"Local\\Kotor2VR-gpu-stream-v1-"
                  L"88776655443322111122334455667788-color-0" &&
              std::wstring_view(
                  MakeGpuStreamColorObjectName(nonce, 2).c_str()) ==
                  L"Local\\Kotor2VR-gpu-stream-v1-"
                  L"88776655443322111122334455667788-color-2",
          "consumer ring names select three distinct shared colors");

    constexpr GpuStreamTextureDescription valid{
        gpu_stream_texture2d_dimension,
        kGpuStreamWidth,
        kGpuStreamHeight,
        1,
        1,
        kGpuStreamDxgiFormatRgba8Unorm,
        1,
        0};
    static_assert(ValidateGpuStreamTextureDescription(valid));
    constexpr GpuStreamTextureDescription stereo_guides{
        gpu_stream_texture2d_dimension,4080,4872,1,1,kGpuStreamDxgiFormatRgba8Unorm,1,0};
    static_assert(ValidateGpuStreamTextureDescription(stereo_guides,{4080,4872}));
    static_assert(IsGpuStreamDestinationCompatible({4080,4872},kGpuStreamDxgiFormatRgba8Unorm,{4080,4872}));
    static_assert(IsGpuStreamDestinationCompatible({4080,3204},kGpuStreamDxgiFormatRgba8Unorm,{4080,4872},true));
    static_assert(!IsGpuStreamDestinationCompatible({4080,3204},kGpuStreamDxgiFormatRgba8Unorm,{4080,4872}));
    static_assert(!IsGpuStreamDestinationCompatible({4079,3204},kGpuStreamDxgiFormatRgba8Unorm,{4080,4872},true));
    static_assert(!IsGpuStreamDestinationCompatible({8192,8193},kGpuStreamDxgiFormatRgba8Unorm,{8192,8193}));
    auto invalid = valid;
    invalid.width++;
    Check(!ValidateGpuStreamTextureDescription(invalid),
          "wrong shared texture width is rejected");
    invalid = valid;
    invalid.format = gpu_stream_dxgi_format_rgba8_unorm_srgb;
    Check(!ValidateGpuStreamTextureDescription(invalid),
          "producer texture must be non-sRGB RGBA8 format 28");
    invalid = valid;
    invalid.sample_count = 2;
    Check(!ValidateGpuStreamTextureDescription(invalid),
          "multisampled producer texture is rejected");
    invalid = valid;
    invalid.mip_levels = 2;
    Check(!ValidateGpuStreamTextureDescription(invalid),
          "multi-mip producer texture is rejected");

    constexpr Extent2D stream_extent{kGpuStreamWidth, kGpuStreamHeight};
    Check(IsGpuStreamDestinationCompatible(
              stream_extent, kGpuStreamDxgiFormatRgba8Unorm) &&
              IsGpuStreamDestinationCompatible(
                  stream_extent, gpu_stream_dxgi_format_rgba8_typeless) &&
              IsGpuStreamDestinationCompatible(
                  stream_extent, gpu_stream_dxgi_format_rgba8_unorm_srgb),
          "XR RGBA8 typeless, UNORM, and sRGB destinations share the copy family");
    Check(!IsGpuStreamDestinationCompatible(
              stream_extent, 87) &&
              !IsGpuStreamDestinationCompatible(
                  {kGpuStreamWidth - 1, kGpuStreamHeight},
                  kGpuStreamDxgiFormatRgba8Unorm),
          "BGRA and mismatched extents cannot use the direct GPU copy");

    Check(SelectGpuStreamFrame(0, 0).status ==
                  GpuStreamConsumerStatus::NoNewFrame &&
              SelectGpuStreamFrame(7, 7).status ==
                  GpuStreamConsumerStatus::NoNewFrame &&
              SelectGpuStreamFrame(6, 7).status ==
                  GpuStreamConsumerStatus::NoNewFrame,
          "zero, duplicate, and stale ready values do not recopy");
    const auto fresh = SelectGpuStreamFrame(8, 7);
    Check(fresh.has_frame() && fresh.frame.ready_value == 8 &&
              fresh.frame.slot == 1,
          "strictly newer ready value selects its triple-buffer slot");
    const auto jumped = SelectGpuStreamFrame(12, 8);
    Check(jumped.has_frame() && jumped.frame.slot == 2,
          "consumer may skip stale frames and copy the newest completed slot");
    const auto legacy = SelectGpuStreamFrame(8, 7, 1);
    Check(legacy.has_frame() && legacy.frame.slot == 0,
          "single-slot WGL fallback always selects its legacy color");
    static_assert(IsGpuStreamFrameTokenCompatible({8, 1}, 3));
    static_assert(!IsGpuStreamFrameTokenCompatible({8, 2}, 3));
    static_assert(IsGpuStreamFrameTokenCompatible({8, 0}, 1));
    static_assert(!IsGpuStreamFrameTokenCompatible({8, 1}, 1));
    Check(SelectGpuStreamFrame(8, 7, 2).status ==
              GpuStreamConsumerStatus::InvalidArgument,
          "unsupported partial rings are rejected");
    Check(SelectGpuStreamFrame(
              (std::numeric_limits<std::uint64_t>::max)(), 7)
                  .status == GpuStreamConsumerStatus::FenceQueryFailed,
          "device-removed fence sentinel fails safely");

    GpuStreamConsumer consumer;
    Check(consumer.TryOpen(nullptr, nonce) ==
                  GpuStreamConsumerStatus::InvalidArgument &&
              consumer.PollLatest().status ==
                  GpuStreamConsumerStatus::NotOpen &&
              !consumer.has_cached_frame() &&
              consumer.slot_count() == 0 &&
              consumer.QueueWait(nullptr, {1}) ==
                  GpuStreamConsumerStatus::InvalidFrameToken &&
              consumer.RecordCachedCopy(nullptr, nullptr) ==
                  GpuStreamConsumerStatus::InvalidArgument &&
              consumer.SignalConsumed(nullptr, {1}) ==
                  GpuStreamConsumerStatus::InvalidFrameToken,
          "closed consumer rejects device, poll, wait, and signal calls");
    consumer.Close();

    if (failures == 0) {
        std::cout << "All GPU stream consumer tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
