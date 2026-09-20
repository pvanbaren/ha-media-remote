#pragma once

#include <LovyanGFX.hpp>

#include <cstddef>

namespace ui::text {

constexpr size_t kMaxLineLen = 64;

/** Copy `src` into `out`, truncating with a trailing ellipsis if it would not
 *  fit in `max_width` at the font currently set on `gfx`. */
void ellipsize(lgfx::LGFXBase& gfx, const char* src, int max_width, char* out,
               size_t out_len);

/** Break `src` on word boundaries into at most `max_lines` lines of
 *  `max_width`, ellipsising the last line if the text runs past it.
 *  `lines` must hold `max_lines` buffers of kMaxLineLen. Returns the number of
 *  lines written. */
int wrap(lgfx::LGFXBase& gfx, const char* src, int max_width, int max_lines,
         char lines[][kMaxLineLen]);

/** mm:ss, or h:mm:ss past an hour. */
void formatDuration(float seconds, char* out, size_t out_len);

}  // namespace ui::text
