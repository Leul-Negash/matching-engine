#pragma once

// Minimal leveled logger to stderr with UTC millisecond timestamps.
// Each call is one fprintf, so lines from different threads don't interleave.

namespace ob::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void set_level(Level level);
bool enabled(Level level);

void write(Level level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

}  // namespace ob::log

#define OB_LOG(level, ...)                                               \
    do {                                                                 \
        if (::ob::log::enabled(::ob::log::Level::level)) ::ob::log::write(::ob::log::Level::level, __VA_ARGS__); \
    } while (0)
