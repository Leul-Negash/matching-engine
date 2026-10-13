#include "ob/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace ob::log {

namespace {

std::atomic<int> g_level{static_cast<int>(Level::Info)};

const char* name(Level l) {
    switch (l) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO ";
        case Level::Warn: return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?";
}

}  // namespace

void set_level(Level level) {
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

bool enabled(Level level) {
    return static_cast<int>(level) >= g_level.load(std::memory_order_relaxed);
}

void write(Level level, const char* fmt, ...) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t secs = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    gmtime_r(&secs, &tm);

    char msg[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);

    std::fprintf(stderr, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ %s %s\n", tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms), name(level), msg);
}

}  // namespace ob::log
