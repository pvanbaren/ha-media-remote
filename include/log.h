#pragma once

#include "config.h"

/**
 * The serial console, with levels.
 *
 *   LOG_ERROR("Qualia: get_frame_buffer failed");
 *   LOG_INFO("HA: stream open, following %s", entity_id);
 *
 * printf-style, one line per call: no trailing newline, the log adds it.
 * A line goes out as
 *
 *      12345 I HA: stream open, following media_player.kitchen
 *
 * -- milliseconds since boot, the level's letter (E W I D V), the message --
 * in a single write, so lines from different tasks cannot interleave
 * mid-line the way several printf calls could.
 *
 * config::kLogLevel decides, at build time, what is printed. A call below it
 * is discarded by `if constexpr`: its arguments are not evaluated, and it
 * costs nothing in the image.
 *
 * Messages carry their subsystem as a prefix in the text ("WiFi:", "HA:"), so
 * one subsystem's lines grep together.
 */
namespace logging {

/** Format and write one line. Use the macros, which skip it at compile time
 *  below config::kLogLevel. From any task. */
void write(config::LogLevel level, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

/** Whether `level` is compiled in, for work done only to feed a log line. */
constexpr bool enabled(config::LogLevel level) {
  return level != config::LogLevel::kNone && config::kLogLevel >= level;
}

}  // namespace logging

#define LOG_AT(level, ...)                         \
  do {                                             \
    if constexpr (::logging::enabled(level)) {     \
      ::logging::write((level), __VA_ARGS__);      \
    }                                              \
  } while (0)

#define LOG_ERROR(...) LOG_AT(::config::LogLevel::kError, __VA_ARGS__)
#define LOG_WARN(...) LOG_AT(::config::LogLevel::kWarn, __VA_ARGS__)
#define LOG_INFO(...) LOG_AT(::config::LogLevel::kInfo, __VA_ARGS__)
#define LOG_DEBUG(...) LOG_AT(::config::LogLevel::kDebug, __VA_ARGS__)
#define LOG_VERBOSE(...) LOG_AT(::config::LogLevel::kVerbose, __VA_ARGS__)
