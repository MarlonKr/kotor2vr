#include "gpu_stream_producer.hpp"

#include <cstdint>
#include <iostream>
#include <limits>

namespace {

int failures = 0;

void Check(const bool condition, const char* const message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

} // namespace

int main() {
    using namespace k2vr::game32;

    constexpr GpuStreamProducerDiagnostic diagnostic =
        MakeGpuStreamProducerDiagnostic(
            GpuStreamProducerStatus::InteropInitializationFailure, 12U,
            static_cast<std::int32_t>(0x80004005U), 87U);
    static_assert(diagnostic.status ==
                  GpuStreamProducerStatus::InteropInitializationFailure);
    static_assert(diagnostic.interop_status == 12U);
    static_assert(static_cast<std::uint32_t>(diagnostic.hresult) ==
                  0x80004005U);
    static_assert(diagnostic.win32_error == 87U);
    static_assert(
        EncodeGpuStreamInteropStatus(GpuStreamInteropBackend::GlExtD3D12,
                                     7U) ==
        (kGpuStreamGlExtInteropStatusTag | 7U));
    static_assert(
        EncodeGpuStreamInteropStatus(GpuStreamInteropBackend::NvDxInterop,
                                     7U) == 7U);
    static_assert(!ShouldWaitForGpuStreamConsumed(
        GpuStreamInteropBackend::GlExtD3D12, 0U));
    static_assert(ShouldWaitForGpuStreamConsumed(
        GpuStreamInteropBackend::GlExtD3D12, 1U));
    static_assert(!ShouldWaitForGpuStreamConsumed(
        GpuStreamInteropBackend::NvDxInterop, 1U));

    const GpuStreamFrameDecision first = DecideGpuStreamFrame(0U, 0U);
    Check(first.disposition == GpuStreamFrameDisposition::Produce &&
              first.next_ready_value == 1U,
          "the empty slot produces fence value one");

    const auto ring_first = DecideGpuStreamRingFrame(0U, 0U, 0U);
    const auto ring_second = DecideGpuStreamRingFrame(1U, 0U, 0U);
    const auto ring_third = DecideGpuStreamRingFrame(2U, 0U, 0U);
    Check(ring_first.disposition == GpuStreamFrameDisposition::Produce &&
              ring_second.disposition == GpuStreamFrameDisposition::Produce &&
              ring_third.disposition == GpuStreamFrameDisposition::Produce,
          "three unused ring slots can publish without a roundtrip");
    Check(DecideGpuStreamRingFrame(3U, 1U, 0U).disposition ==
                  GpuStreamFrameDisposition::DropConsumerBusy &&
              DecideGpuStreamRingFrame(3U, 1U, 1U).disposition ==
                  GpuStreamFrameDisposition::Produce,
          "the fourth publication waits only for its selected slot");

    const GpuStreamFrameDecision busy = DecideGpuStreamFrame(7U, 6U);
    Check(busy.disposition ==
              GpuStreamFrameDisposition::DropConsumerBusy &&
              busy.next_ready_value == 0U,
          "a busy single slot drops without advancing the ready fence");

    const GpuStreamFrameDecision exact_release =
        DecideGpuStreamFrame(7U, 7U);
    Check(exact_release.disposition ==
              GpuStreamFrameDisposition::Produce &&
              exact_release.next_ready_value == 8U,
          "an exactly consumed slot can be overwritten");

    const GpuStreamFrameDecision later_release =
        DecideGpuStreamFrame(7U, 11U);
    Check(later_release.disposition ==
              GpuStreamFrameDisposition::Produce &&
              later_release.next_ready_value == 8U,
          "a later consumed value also releases the slot");

    constexpr std::uint64_t invalid =
        (std::numeric_limits<std::uint64_t>::max)();
    const GpuStreamFrameDecision removed =
        DecideGpuStreamFrame(12U, invalid);
    Check(removed.disposition ==
              GpuStreamFrameDisposition::StopDeviceRemoved,
          "UINT64_MAX completion means device removal");

    const GpuStreamFrameDecision exhausted =
        DecideGpuStreamFrame(invalid - 1U, invalid - 1U);
    Check(exhausted.disposition ==
              GpuStreamFrameDisposition::StopFenceValueExhausted,
          "the producer never signals the reserved invalid fence value");

    if (failures == 0) {
        std::cout << "GPU stream producer decision tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
