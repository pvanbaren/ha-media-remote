#include "hardware/display_font.h"

#include <cmath>

#include "hardware/display.h"
#include "log.h"

// One embedded VLW per on-screen height. The symbol names come from the paths
// in platformio.ini's board_build.embed_files.
extern "C" {
#define VLW_SYM(name) extern const uint8_t name[] asm(#name)
VLW_SYM(_binary_data_ui_font_15_vlw_start);
VLW_SYM(_binary_data_ui_font_17_vlw_start);
VLW_SYM(_binary_data_ui_font_20_vlw_start);
VLW_SYM(_binary_data_ui_font_24_vlw_start);
#undef VLW_SYM
}

namespace {

struct FontEntry {
  const uint8_t* data;
  /** fontHeight() at size 1.0, measured in displayFontInit() rather than
   *  trusted from the filename -- the font is the authority on its own
   *  height, and a regenerated set should not need this file edited. */
  float native_h;
};

FontEntry s_fonts[] = {
    {_binary_data_ui_font_15_vlw_start, 0.0f},
    {_binary_data_ui_font_17_vlw_start, 0.0f},
    {_binary_data_ui_font_20_vlw_start, 0.0f},
    {_binary_data_ui_font_24_vlw_start, 0.0f},
};
constexpr size_t kFontCount = sizeof(s_fonts) / sizeof(s_fonts[0]);
/** Body text, and what displayFontEnsureLoaded() selects: the 20 px face. */
constexpr size_t kDefaultFont = 2;

bool s_vlw_loaded = false;

// One-slot cache of the font currently loaded on a given instance, so a run of
// draws at the same size -- a whole list of rows, say -- reloads only once.
lgfx::LGFXBase* s_active_gfx = nullptr;
const uint8_t* s_active_data = nullptr;

/** Load `data` on gfx, skipping the reload when it is already active there. */
bool useFont(lgfx::LGFXBase& gfx, const uint8_t* data) {
  if (s_active_gfx == &gfx && s_active_data == data) {
    return true;
  }
  if (!gfx.loadFont(data, lgfx::IFont::font_type_t::ft_vlw)) {
    return false;
  }
  s_active_gfx = &gfx;
  s_active_data = data;
  return true;
}

}  // namespace

bool displayFontInit() {
  s_vlw_loaded = useFont(tft, s_fonts[kDefaultFont].data);
  if (!s_vlw_loaded) {
    LOG_ERROR("Smooth font load failed - using bitmap fallback");
    return false;
  }

  // Measure each font once. displayFontApplyHeight() matches against these.
  for (size_t i = 0; i < kFontCount; ++i) {
    if (useFont(tft, s_fonts[i].data)) {
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
  for (size_t i = 0; i < kFontCount && used < sizeof(sizes); ++i) {
    const int n = snprintf(sizes + used, sizeof(sizes) - used, "%s%.0f",
                           i ? ", " : "", s_fonts[i].native_h);
    used += n > 0 ? static_cast<size_t>(n) : 0;
  }
  LOG_INFO("Fonts: %u native sizes (%s px)",
           static_cast<unsigned>(kFontCount), sizes);

  useFont(tft, s_fonts[kDefaultFont].data);
  return true;
}

bool displayFontIsSmooth() { return s_vlw_loaded; }

bool displayFontEnsureLoaded(lgfx::LGFXBase& gfx) {
  if (!s_vlw_loaded) {
    return false;
  }
  return useFont(gfx, s_fonts[kDefaultFont].data);
}

void displayFontApplyHeight(lgfx::LGFXBase& gfx, float target_px) {
  if (!s_vlw_loaded) {
    return;
  }

  size_t best = 0;
  float best_err = -1.0f;
  for (size_t i = 0; i < kFontCount; ++i) {
    const float err = std::fabs(s_fonts[i].native_h - target_px);
    if (best_err < 0.0f || err < best_err) {
      best_err = err;
      best = i;
    }
  }
  useFont(gfx, s_fonts[best].data);

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
  s_active_gfx = nullptr;
  s_active_data = nullptr;
}
