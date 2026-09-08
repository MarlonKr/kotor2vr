#pragma once

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace kotorvr::host {

enum class LogLevel {
    trace,
    info,
    warning,
    error,
};

struct LogField {
    std::string_view key;
    std::string value;
};

class Logger final {
public:
    explicit Logger(const std::filesystem::path& file_path = {});
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void write(LogLevel level,
               std::string_view event,
               std::string_view message,
               std::initializer_list<LogField> fields = {});

    [[nodiscard]] std::uint64_t dropped_lines() const noexcept;

private:
    struct QueuedLine {
        LogLevel level{LogLevel::info};
        std::string text;
    };

    void drain() noexcept;

    static constexpr std::size_t max_queued_lines_ = 4096;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<QueuedLine> queue_;
    std::ofstream file_;
    std::chrono::steady_clock::time_point process_start_;
    std::thread writer_;
    std::atomic_uint64_t dropped_lines_{};
    bool stopping_{};
};

struct FrameTiming {
    std::uint64_t frame_id{};
    std::int64_t predicted_display_time_ns{};
    double wait_frame_ms{};
    double render_wait_ms{};
    double dlss_ms{};
    double submit_ms{};
};

struct HealthSnapshot {
    std::uint64_t frames_seen{};
    std::uint64_t frames_submitted{};
    std::uint64_t incomplete_pairs{};
    std::uint64_t dropped_frames{};
    std::uint64_t validation_failures{};
    std::uint64_t history_resets{};
    std::uint64_t log_lines_dropped{};
    bool xr_available{};
    bool xr_rendering{};
};

void log_frame_timing(Logger& logger, const FrameTiming& timing);
void log_health(Logger& logger, const HealthSnapshot& health);

} // namespace kotorvr::host
