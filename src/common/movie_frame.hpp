#pragma once

#include "ipc_protocol.hpp"
#include <cstdint>
#include <span>
#include <vector>

namespace k2vr::ipc {
inline constexpr std::uint32_t kMovieMaximumWidth=3840, kMovieMaximumHeight=2160;
inline constexpr std::uint32_t kMovieMaximumBytes=kMovieMaximumWidth*kMovieMaximumHeight*4;
inline constexpr std::uint64_t kMovieFrameTimeoutMs=2000;

struct MovieFrame {
    std::uint64_t sequence{}, tick_ms{};
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> pixels; // tightly packed, top-down BGRA8
};
enum class MovieRead { Unavailable, Inactive, Unchanged, Fresh };

// Separate from the OpenGL stream: Bink presents through DirectDraw and may
// never call SwapBuffers. A bounded, nonblocking mutex protects complete frames.
class MovieFrameChannel final {
public:
    MovieFrameChannel() noexcept=default;
    ~MovieFrameChannel();
    MovieFrameChannel(const MovieFrameChannel&)=delete;
    MovieFrameChannel& operator=(const MovieFrameChannel&)=delete;
    bool Open(SessionNonce nonce,bool writer) noexcept;
    void Close() noexcept;
    bool Publish(std::uint32_t width,std::uint32_t height,
                 std::span<const std::uint8_t> pixels) noexcept;
    void EndMovie() noexcept;
    MovieRead ReadLatest(MovieFrame& frame) noexcept;
private:
    void* mapping_{}; void* mutex_{}; void* view_{};
    SessionNonce nonce_{};
    bool writer_{};
};
}
