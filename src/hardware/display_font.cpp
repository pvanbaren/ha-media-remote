#include "hardware/display_font.h"

#include "hardware/font_table.h"

#include <esp_heap_caps.h>

#include <cmath>
#include <new>

#include "hardware/display.h"
#include "log.h"

// One embedded VLW per on-screen height. The symbol names come from the paths
// in platformio.ini's board_build.embed_files.

namespace {

/** More than any board is likely to embed; the real count comes from
 *  hw::fontTable(). */
constexpr size_t kMaxFonts = 8;

struct FontEntry {
  const uint8_t* data = nullptr;
  /** The face, parsed once at init and kept, rather than loaded onto each
   *  canvas as it is wanted: selecting a kept face is setFont(), which only
   *  copies its metrics, where loadFont() parsed the whole glyph table again
   *  at every change of size. Read through `source`, which it keeps a
   *  pointer to. Drawn from the loop only -- `source` has one read position. */
  lgfx::PointerWrapper source;
  lgfx::VLWfont font;
  bool loaded = false;
  /** fontHeight() at size 1.0, measured in displayFontInit() rather than
   *  trusted from the filename -- the font is the authority on its own
   *  height, and a regenerated set should not need this file edited. */
  float native_h = 0.0f;
};

/** Filled from hw::fontTable() at init, which is where the per-board list of
 *  embedded faces lives. In PSRAM, claimed once and kept: as a static array
 *  it took 680 bytes of internal RAM, which a board holding two TLS sessions
 *  has none to spare. */
FontEntry* s_fonts = nullptr;
size_t s_font_count = 0;

/** Body text, and what displayFontEnsureLoaded() selects: the 20 px face. */
constexpr size_t kDefaultFont = 2;

bool s_vlw_loaded = false;

/**
 * Parse one embedded face, and give it the line height in its header.
 *
 * LovyanGFX sets a face's line height from its tallest glyph, sparing only
 * U+00A0..U+00FF, so Latin-1's accented capitals overhang the line rather
 * than push it apart. Latin Extended-A's capitals -- Č, Ś, Ž, Ă -- sit outside
 * that range, and left to it, the tallest of them would set every line on
 * the panel: each face reporting a taller fontHeight() than ui/theme.h was
 * written against, every layout shifted to fit an accent that is on almost
 * nothing. scripts/build_ui_font.py measures the header without them, so the
 * header is the line as it always was; this puts it back after the load, and
 * those capitals overhang as the Latin-1 ones do.
 */
bool loadFace(FontEntry& face) {
  face.source.set(face.data);
  if (!face.font.loadFont(&face.source)) {
    return false;
  }
  face.font.maxAscent = static_cast<uint16_t>(face.font.ascent);
  face.font.maxDescent = static_cast<uint16_t>(face.font.descent);
  face.font.yAdvance =
      static_cast<uint16_t>(face.font.ascent + face.font.descent);
  return true;
}

/** Select face `index` on gfx. setFont() does nothing when it is already
 *  the one there, so a run of draws at one size costs nothing to repeat. */
bool useFont(lgfx::LGFXBase& gfx, size_t index) {
  if (index >= s_font_count || !s_fonts[index].loaded) {
    return false;
  }
  gfx.setFont(&s_fonts[index].font);
  return true;
}

}  // namespace

bool displayFontInit() {
  size_t count = 0;
  const uint8_t* const* blobs = hw::fontTable(count);
  if (count > kMaxFonts) {
    count = kMaxFonts;
  }
  if (s_fonts == nullptr && count > 0) {
    void* mem = heap_caps_malloc(sizeof(FontEntry) * count,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem == nullptr) {
      mem = heap_caps_malloc(sizeof(FontEntry) * count, MALLOC_CAP_8BIT);
    }
    if (mem == nullptr) {
      count = 0;
    } else {
      s_fonts = static_cast<FontEntry*>(mem);
      for (size_t i = 0; i < count; ++i) {
        new (&s_fonts[i]) FontEntry();
      }
    }
  }
  for (size_t i = 0; i < count; ++i) {
    s_fonts[i].data = blobs[i];
    s_fonts[i].loaded = loadFace(s_fonts[i]);
  }
  s_font_count = count;
  if (s_font_count == 0) {
    LOG_WARN("Fonts: board embedded none - using bitmap fallback");
    return false;
  }

  s_vlw_loaded = useFont(tft, kDefaultFont);
  if (!s_vlw_loaded) {
    LOG_ERROR("Smooth font load failed - using bitmap fallback");
    return false;
  }

  // Measure each font once. displayFontApplyHeight() matches against these.
  for (size_t i = 0; i < s_font_count; ++i) {
    if (useFont(tft, i)) {
      tft.setTextSize(1.0f);
      s_fonts[i].native_h = static_cast<float>(tft.fontHeight());
    }
    if (s_fonts[i].native_h <= 0.0f) {
      s_fonts[i].native_h = 1.0f;  // guard against divide-by-zero
    }
  }

  // One line, built first: the log writes whole lines.
  char sizes[96] = {};
  size_t used = 0;
  for (size_t i = 0; i < s_font_count && used < sizeof(sizes); ++i) {
    const int n = snprintf(sizes + used, sizeof(sizes) - used, "%s%.0f",
                           i ? ", " : "", s_fonts[i].native_h);
    used += n > 0 ? static_cast<size_t>(n) : 0;
  }
  LOG_INFO("Fonts: %u native sizes (%s px)",
           static_cast<unsigned>(s_font_count), sizes);

  useFont(tft, kDefaultFont);
  return true;
}

bool displayFontIsSmooth() { return s_vlw_loaded; }

bool displayFontEnsureLoaded(lgfx::LGFXBase& gfx) {
  if (!s_vlw_loaded) {
    return false;
  }
  return useFont(gfx, kDefaultFont);
}

void displayFontApplyHeight(lgfx::LGFXBase& gfx, float target_px) {
  if (!s_vlw_loaded) {
    return;
  }

  size_t best = 0;
  float best_err = -1.0f;
  for (size_t i = 0; i < s_font_count; ++i) {
    const float err = std::fabs(s_fonts[i].native_h - target_px);
    if (best_err < 0.0f || err < best_err) {
      best_err = err;
      best = i;
    }
  }
  useFont(gfx, best);

  // Native unless the nearest font is off by more than a pixel. Every size in
  // ui/theme.h has its own font, so this is 1.0 in practice; the rescale is
  // the fallback for a panel the set was not generated for, where drawing the
  // wrong size outright would be worse than a soft one.
  const float native_h = s_fonts[best].native_h;
  const bool worth_rescaling = std::fabs(target_px - native_h) > 1.0f;
  gfx.setTextSize(worth_rescaling ? target_px / native_h : 1.0f);
}

void displayFontSetBitmap(lgfx::LGFXBase& gfx, const lgfx::GFXfont* font) {
  gfx.setFont(font);
  gfx.setTextSize(1);
}
