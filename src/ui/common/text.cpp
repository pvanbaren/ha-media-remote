#include "ui/text.h"

#include <cstdio>
#include <cstring>

namespace ui::text {
namespace {

constexpr char kEllipsis[] = "...";

/** Length of the UTF-8 sequence starting at `s`. */
int glyphLen(const char* s) {
  const unsigned char c = static_cast<unsigned char>(*s);
  if (c < 0x80) {
    return 1;
  }
  if ((c & 0xE0) == 0xC0) {
    return 2;
  }
  if ((c & 0xF0) == 0xE0) {
    return 3;
  }
  if ((c & 0xF8) == 0xF0) {
    return 4;
  }
  return 1;  // stray continuation byte; step over it rather than stall
}

}  // namespace

void ellipsize(lgfx::LGFXBase& gfx, const char* src, int max_width, char* out,
               size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (src == nullptr || src[0] == '\0' || max_width <= 0) {
    return;
  }

  if (gfx.textWidth(src) <= max_width) {
    snprintf(out, out_len, "%s", src);
    return;
  }

  const int ellipsis_width = gfx.textWidth(kEllipsis);
  const int budget = max_width - ellipsis_width;
  if (budget <= 0) {
    snprintf(out, out_len, "%s", kEllipsis);
    return;
  }

  // Walk forward a glyph at a time; measuring is cheap next to the redraw and
  // this keeps multi-byte characters whole.
  char probe[kMaxLineLen];
  size_t kept = 0;
  const char* p = src;
  while (*p != '\0') {
    const int len = glyphLen(p);
    // Room for this glyph in the probe, and room for it *plus* the ellipsis
    // in the caller's buffer. Written as an addition rather than
    // `out_len - sizeof(kEllipsis)`, which wraps for a buffer smaller than
    // the ellipsis and quietly disables the second half of the test.
    if (kept + static_cast<size_t>(len) >= sizeof(probe) ||
        kept + static_cast<size_t>(len) + sizeof(kEllipsis) > out_len) {
      break;
    }
    memcpy(probe + kept, p, len);
    probe[kept + len] = '\0';
    if (gfx.textWidth(probe) > budget) {
      break;
    }
    kept += len;
    p += len;
  }

  // Trim a trailing space so the ellipsis reads as elision, not a gap.
  while (kept > 0 && probe[kept - 1] == ' ') {
    --kept;
  }
  probe[kept] = '\0';
  snprintf(out, out_len, "%s%s", probe, kEllipsis);
}

int wrap(lgfx::LGFXBase& gfx, const char* src, int max_width, int max_lines,
         char lines[][kMaxLineLen]) {
  if (src == nullptr || src[0] == '\0' || max_lines <= 0 || max_width <= 0) {
    return 0;
  }

  int line_count = 0;
  const char* cursor = src;

  while (*cursor != '\0' && line_count < max_lines) {
    // Last line takes whatever is left, ellipsised.
    if (line_count == max_lines - 1) {
      ellipsize(gfx, cursor, max_width, lines[line_count], kMaxLineLen);
      if (lines[line_count][0] != '\0') {
        ++line_count;
      }
      break;
    }

    char probe[kMaxLineLen] = {};
    size_t kept = 0;
    // Last position where a break would land between words.
    size_t break_at = 0;
    const char* p = cursor;
    const char* break_src = nullptr;

    while (*p != '\0') {
      const int len = glyphLen(p);
      if (kept + len >= sizeof(probe)) {
        break;
      }
      memcpy(probe + kept, p, len);
      probe[kept + len] = '\0';
      if (gfx.textWidth(probe) > max_width) {
        probe[kept] = '\0';
        break;
      }
      kept += len;
      p += len;
      if (*p == ' ') {
        break_at = kept;
        break_src = p;
      }
    }

    if (*p == '\0') {
      // Everything left fits on this line.
      snprintf(lines[line_count], kMaxLineLen, "%s", probe);
      ++line_count;
      cursor = p;
      break;
    }

    if (break_at > 0) {
      probe[break_at] = '\0';
      cursor = break_src + 1;  // step past the space
    } else {
      // A single word wider than the line: hard-break it.
      cursor += kept > 0 ? kept : glyphLen(cursor);
    }

    snprintf(lines[line_count], kMaxLineLen, "%s", probe);
    ++line_count;

    while (*cursor == ' ') {
      ++cursor;
    }
  }

  return line_count;
}

void formatDuration(float seconds, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  if (seconds < 0.0f) {
    snprintf(out, out_len, "--:--");
    return;
  }
  const unsigned long total = static_cast<unsigned long>(seconds + 0.5f);
  const unsigned long hours = total / 3600;
  const unsigned long minutes = (total % 3600) / 60;
  const unsigned long secs = total % 60;
  if (hours > 0) {
    snprintf(out, out_len, "%lu:%02lu:%02lu", hours, minutes, secs);
  } else {
    snprintf(out, out_len, "%lu:%02lu", minutes, secs);
  }
}

}  // namespace ui::text
