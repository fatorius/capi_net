#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string_view>

namespace capi {

// Log mínimo em stderr, com timestamp UTC; seguro entre threads.
inline void log_line(std::string_view level, std::string_view msg) {
    static std::mutex mutex;
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&now, &tm);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
    std::lock_guard lock(mutex);
    std::fprintf(stderr, "%s [%.*s] %.*s\n", ts, static_cast<int>(level.size()), level.data(),
                 static_cast<int>(msg.size()), msg.data());
}

inline void log_info(std::string_view msg) { log_line("info", msg); }
inline void log_warn(std::string_view msg) { log_line("warn", msg); }
inline void log_error(std::string_view msg) { log_line("error", msg); }

}  // namespace capi
