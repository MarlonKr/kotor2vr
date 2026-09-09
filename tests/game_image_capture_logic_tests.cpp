#include "game_image_capture.hpp"
#include "gl_context_recovery.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

void Check(bool condition) {
    if (!condition) {
        std::cerr << "Game-image capture policy check failed.\n";
        std::abort();
    }
}

} // namespace

int main() {
    using namespace k2vr::game32;

    // Graphics reset: context and DC numeric handles may be reused or changed,
    // and the window creator need not be the GL owner. Recovery is selected by
    // the actual target window, with thread ownership left to each producer.
    Check(IsGameContextRecoverySurface(0x20000,10,10,true,42,42));
    Check(IsGameContextRecoverySurface(0x30000,10,10,true,42,42));
    Check(IsGameContextRecoverySurface(0x30000,20,20,true,42,42));
    // A wrong-window present must defer recovery; the next verified world
    // camera call on the replacement window remains eligible.
    Check(!IsGameContextRecoverySurface(0x30000,20,10,true,42,42));
    Check(IsGameContextRecoverySurface(0x30000,20,20,true,42,42));
    Check(!IsGameContextRecoverySurface(0,20,20,true,42,42));
    Check(!IsGameContextRecoverySurface(0x30000,0,0,true,42,42));
    Check(!IsGameContextRecoverySurface(0x30000,20,20,false,42,42));
    Check(!IsGameContextRecoverySurface(0x30000,20,20,true,99,42));
    Check(!IsGameContextRecoverySurface(0x30000,20,20,true,0,0));

    GlContextRecoveryRetry recovery;
    Check(recovery.BeginAttempt(100,false));
    // A failed import remains retryable without busy-looping every frame.
    Check(!recovery.imported());
    Check(!recovery.BeginAttempt(1099,false));
    Check(recovery.BeginAttempt(1100,false));
    recovery.MarkImported();
    // Shader/FBO failure keeps the imports for the next attempt, and a
    // temporary context cannot consume or reset that successful import.
    Check(!recovery.BeginAttempt(2100,false));
    Check(recovery.imported());
    Check(recovery.BeginAttempt(2100,true));
    Check(recovery.imported());
    // Another confirmed deletion abandons imports and restarts immediately,
    // including when Windows recycles the same HGLRC numeric value.
    recovery.Reset();
    Check(!recovery.imported());
    Check(recovery.BeginAttempt(2101,false));

    static_assert(sizeof(GameImageSmokeBootstrapV1) == 32);
    static_assert(sizeof(GameImageSharedHeaderV1) == 64);
    static_assert(offsetof(GameImageSharedHeaderV1, sequence) == 16);
    static_assert(offsetof(GameImageSharedHeaderV1, frame_id) == 36);
    static_assert(sizeof(GameImageGpuDiagnosticV1) == 20);
    constexpr GameImageGpuDiagnosticV1 gpu_diagnostic{
        kGameImageGpuDiagnosticMagic, 5U, 12U, 0x80004005U, 87U};
    static_assert(gpu_diagnostic.magic == 0x31555047U);
    static_assert(gpu_diagnostic.producer_status == 5U);
    static_assert(gpu_diagnostic.interop_status == 12U);
    static_assert(gpu_diagnostic.hresult_bits == 0x80004005U);
    static_assert(gpu_diagnostic.win32_error == 87U);

    constexpr GameImageSmokeBootstrapV1 valid{
        sizeof(GameImageSmokeBootstrapV1), kGameImageMajor,
        kGameImageMinor, {0xFEDCBA9876543210ULL, 0x0123456789ABCDEFULL}, 1};
    static_assert(ValidateGameImageSmokeBootstrap(valid) ==
                  GameImageCaptureResult::Ok);
    constexpr GameImageSmokeBootstrapV1 wrong_version{
        sizeof(GameImageSmokeBootstrapV1), kGameImageMajor,
        static_cast<std::uint16_t>(kGameImageMinor + 1), {1, 2}, 1};
    static_assert(ValidateGameImageSmokeBootstrap(wrong_version) ==
                  GameImageCaptureResult::VersionMismatch);
    constexpr GameImageSmokeBootstrapV1 zero_session{
        sizeof(GameImageSmokeBootstrapV1), kGameImageMajor,
        kGameImageMinor, {}, 1};
    static_assert(ValidateGameImageSmokeBootstrap(zero_session) ==
                  GameImageCaptureResult::InvalidSession);

    static_assert(EvaluateGameImageInput(true, false, false, true)
                      .should_capture);
    static_assert(!EvaluateGameImageInput(false, false, false, true)
                       .should_capture);
    static_assert(!EvaluateGameImageInput(true, true, false, true)
                       .should_capture);
    static_assert(!EvaluateGameImageInput(true, false, true, true)
                       .should_capture);
    static_assert(!EvaluateGameImageInput(true, false, true, false)
                       .next_was_down);

    constexpr GameImageLayout one_pixel = ComputeGameImageLayout(1, 1);
    static_assert(one_pixel.ok());
    static_assert(one_pixel.stride == 4);
    static_assert(one_pixel.pixel_bytes == 4);
    static_assert(one_pixel.mapping_bytes == 68);
    constexpr GameImageLayout maximum =
        ComputeGameImageLayout(kGameImageMaximumDimension,
                               kGameImageMaximumDimension);
    static_assert(maximum.ok());
    static_assert(maximum.stride == 16384);
    static_assert(maximum.pixel_bytes == 67108864);
    static_assert(maximum.mapping_bytes == 67108928);
    static_assert(!ComputeGameImageLayout(0, 1080).ok());
    static_assert(!ComputeGameImageLayout(1920, 0).ok());
    static_assert(!ComputeGameImageLayout(4097, 1080).ok());
    static_assert(!ComputeGameImageLayout(1920, 4097).ok());

    static_assert(SourceRowForTopDown(0, 3) == 2);
    static_assert(SourceRowForTopDown(1, 3) == 1);
    static_assert(SourceRowForTopDown(2, 3) == 0);

    Check(GetGameImageCapturePolicy() == GameImageCapturePolicy::Default);
    {
        const ScopedGameImageCapturePolicy vr_capture(
            GameImageCapturePolicy::VrCapture);
        Check(GetGameImageCapturePolicy() ==
              GameImageCapturePolicy::VrCapture);
        {
            const ScopedGameImageCapturePolicy suppressed(
                GameImageCapturePolicy::Suppressed);
            Check(GetGameImageCapturePolicy() ==
                  GameImageCapturePolicy::Suppressed);
        }
        Check(GetGameImageCapturePolicy() ==
              GameImageCapturePolicy::VrCapture);

        bool worker_started_with_default = false;
        bool worker_change_was_isolated = false;
        std::thread worker([&]() noexcept {
            worker_started_with_default =
                GetGameImageCapturePolicy() ==
                GameImageCapturePolicy::Default;
            (void)SetGameImageCapturePolicy(
                GameImageCapturePolicy::Suppressed);
            worker_change_was_isolated =
                GetGameImageCapturePolicy() ==
                GameImageCapturePolicy::Suppressed;
        });
        worker.join();
        Check(worker_started_with_default);
        Check(worker_change_was_isolated);
        Check(GetGameImageCapturePolicy() ==
              GameImageCapturePolicy::VrCapture);
    }
    Check(GetGameImageCapturePolicy() == GameImageCapturePolicy::Default);

    std::cout << "Game-image capture contract and decisions passed.\n";
    return 0;
}
