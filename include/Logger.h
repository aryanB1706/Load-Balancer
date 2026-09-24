#pragma once

#include <chrono>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>

namespace lb {

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

inline const char* to_string(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::ERROR: return "ERROR";
    }
    return "INFO";
}

inline std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

// Thread-safe, timestamped log to stderr. Never throws.
inline void log(LogLevel lvl, const std::string& msg) noexcept {
    try {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf {};
        localtime_r(&t, &tm_buf);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

        std::lock_guard<std::mutex> lk(log_mutex());
        std::cerr << "[" << ts << "] [" << to_string(lvl) << "] " << msg << "\n";
    } catch (...) {
        // logging must never throw
    }
}

}  // namespace lb

#define LOG_DEBUG(m) ::lb::log(::lb::LogLevel::DEBUG, (m))
#define LOG_INFO(m)  ::lb::log(::lb::LogLevel::INFO, (m))
#define LOG_WARN(m)  ::lb::log(::lb::LogLevel::WARN, (m))
#define LOG_ERROR(m) ::lb::log(::lb::LogLevel::ERROR, (m))
