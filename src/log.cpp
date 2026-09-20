#include "log.h"

#include <Arduino.h>

#include <esp_heap_caps.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace logging {
namespace {

char letter(config::LogLevel level) {
  switch (level) {
    case config::LogLevel::kError:
      return 'E';
    case config::LogLevel::kWarn:
      return 'W';
    case config::LogLevel::kInfo:
      return 'I';
    case config::LogLevel::kDebug:
      return 'D';
    default:
      return 'V';
  }
}

void emit(const char* line, size_t len) {
  Serial.write(reinterpret_cast<const uint8_t*>(line), len);
}

}  // namespace

void write(config::LogLevel level, const char* fmt, ...) {
  // Most lines fit here, on the caller's stack. A response body logged after
  // a failed request does not, and gets a PSRAM buffer of its own below.
  char line[256];
  const int prefix = snprintf(line, sizeof(line), "%8lu %c ",
                              static_cast<unsigned long>(millis()),
                              letter(level));
  if (prefix <= 0) {
    return;
  }

  va_list args;
  va_start(args, fmt);
  va_list again;
  va_copy(again, args);
  const int n = vsnprintf(line + prefix, sizeof(line) - prefix, fmt, args);
  va_end(args);
  if (n < 0) {
    va_end(again);
    return;
  }

  const size_t total = static_cast<size_t>(prefix) + static_cast<size_t>(n);
  if (total + 1 < sizeof(line)) {
    line[total] = '\n';
    emit(line, total + 1);
    va_end(again);
    return;
  }

  // Longer than the stack buffer: the whole of it, in one write, so it
  // cannot be split by another task's line.
  char* big = static_cast<char*>(
      heap_caps_malloc(total + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (big == nullptr) {
    big = static_cast<char*>(malloc(total + 2));
  }
  if (big == nullptr) {
    // No room for it: the truncated line is better than none.
    line[sizeof(line) - 2] = '\n';
    emit(line, sizeof(line) - 1);
    va_end(again);
    return;
  }
  memcpy(big, line, static_cast<size_t>(prefix));
  vsnprintf(big + prefix, total + 2 - prefix, fmt, again);
  va_end(again);
  big[total] = '\n';
  emit(big, total + 1);
  free(big);
}

}  // namespace logging
