#include "kotorvr/host/logger.hpp"

#include <ctime>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace kotorvr::host {
namespace {

std::string escape_json(const std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) >= 0x20U) {
                result += character;
            }
            break;
        }
    }
    return result;
}

std::string_view level_name(const LogLevel level) {
    switch (level) {
    case LogLevel::trace: return "trace";
    case LogLevel::info: return "info";
    case LogLevel::warning: return "warning";
    case LogLevel::error: return "error";
    }
    return "unknown";
}

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now.time_since_epoch()) %
                              std::chrono::seconds(1);
    std::tm utc{};
#if defined(_MSC_VER)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0')
           << std::setw(3) << milliseconds.count() << 'Z';
    return stream.str();
}

std::string decimal(const double value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << value;
    return stream.str();
}

} // namespace

Logger::Logger(const std::filesystem::path& file_path)
    : process_start_(std::chrono::steady_clock::now()) {
    if (!file_path.empty()) {
        std::error_code error;
        if (const auto parent = file_path.parent_path(); !parent.empty()) {
            std::filesystem::create_directories(parent, error);
        }
        file_.open(file_path, std::ios::out | std::ios::app);
    }
    writer_ = std::thread(&Logger::drain, this);
}

Logger::~Logger() {
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_one();
    if (writer_.joinable()) {
        writer_.join();
    }
}

void Logger::write(const LogLevel level,
                   const std::string_view event,
                   const std::string_view message,
                   const std::initializer_list<LogField> fields) {
    const auto uptime = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - process_start_);

    std::ostringstream line;
    line << "{\"utc\":\"" << utc_timestamp() << "\",\"uptime_ms\":"
         << std::fixed << std::setprecision(3) << uptime.count() << ",\"level\":\""
         << level_name(level) << "\",\"event\":\"" << escape_json(event)
         << "\",\"message\":\"" << escape_json(message) << '"';
    for (const auto& [key, value] : fields) {
        line << ",\"" << escape_json(key) << "\":\"" << escape_json(value) << '"';
    }
    line << '}';

    {
        std::scoped_lock lock(mutex_);
        if (queue_.size() >= max_queued_lines_) {
            if (level == LogLevel::trace) {
                dropped_lines_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const auto trace = std::find_if(queue_.begin(), queue_.end(), [](const QueuedLine& item) {
                return item.level == LogLevel::trace;
            });
            if (trace != queue_.end()) {
                queue_.erase(trace);
            } else {
                queue_.pop_front();
            }
            dropped_lines_.fetch_add(1, std::memory_order_relaxed);
        }
        queue_.push_back({level, line.str()});
    }
    ready_.notify_one();
}

std::uint64_t Logger::dropped_lines() const noexcept {
    return dropped_lines_.load(std::memory_order_relaxed);
}

void Logger::drain() noexcept {
    std::deque<QueuedLine> batch;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            queue_.swap(batch);
            if (batch.empty() && stopping_) {
                break;
            }
        }

        for (const auto& item : batch) {
            // Per-frame timing remains fully persisted, but echoing it to the
            // launcher's inherited console can build hundreds of thousands of
            // buffered lines during a live headset session. Keep the console
            // useful for lifecycle, health, warnings, and errors.
            if (item.level != LogLevel::trace) {
                std::clog << item.text << '\n';
            }
            if (file_.is_open()) {
                file_ << item.text << '\n';
            }
        }
        std::clog.flush();
        if (file_.is_open()) {
            file_.flush();
        }
        batch.clear();

        std::scoped_lock lock(mutex_);
        if (stopping_ && queue_.empty()) {
            break;
        }
    }
}

void log_frame_timing(Logger& logger, const FrameTiming& timing) {
    logger.write(LogLevel::trace,
                 "frame_timing",
                 "OpenXR host frame timing",
                 {{"frame_id", std::to_string(timing.frame_id)},
                  {"predicted_display_time_ns", std::to_string(timing.predicted_display_time_ns)},
                  {"wait_frame_ms", decimal(timing.wait_frame_ms)},
                  {"render_wait_ms", decimal(timing.render_wait_ms)},
                  {"dlss_ms", decimal(timing.dlss_ms)},
                  {"submit_ms", decimal(timing.submit_ms)}});
}

void log_health(Logger& logger, const HealthSnapshot& health) {
    logger.write(LogLevel::info,
                 "health",
                 "Periodic host health snapshot",
                 {{"frames_seen", std::to_string(health.frames_seen)},
                  {"frames_submitted", std::to_string(health.frames_submitted)},
                  {"incomplete_pairs", std::to_string(health.incomplete_pairs)},
                  {"dropped_frames", std::to_string(health.dropped_frames)},
                  {"validation_failures", std::to_string(health.validation_failures)},
                  {"history_resets", std::to_string(health.history_resets)},
                  {"log_lines_dropped", std::to_string(health.log_lines_dropped)},
                  {"xr_available", health.xr_available ? "true" : "false"},
                  {"xr_rendering", health.xr_rendering ? "true" : "false"}});
}

} // namespace kotorvr::host
