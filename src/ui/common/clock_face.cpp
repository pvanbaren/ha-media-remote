#include "ui/clock_face.h"

#include <cstdio>

#include "config.h"
#include "hardware/display_font.h"
#include "hardware/outline_fonts.h"
#include "ui/canvas.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace ui {
namespace clock_face {
namespace {

/** Where the face sits, one step a minute: a few pixels about the centre and
 *  back, so a panel left on it all afternoon does not keep the same pixels
 *  lit. An AMOLED's pixels age with their hours on. */
constexpr int8_t kWander[][2] = {{0, 0},  {5, 2},  {-3, 5},  {-6, -1},
                                 {2, -5}, {6, 4},  {-4, -4}, {1, 6}};
constexpr int kWanderSteps = sizeof(kWander) / sizeof(kWander[0]);

}  // namespace

bool now(struct tm& out) {
  const time_t t = time(nullptr);
  if (t < config::kClockSetAfter) {
    return false;
  }
  localtime_r(&t, &out);
  return true;
}

void draw(const services::ha::PlayerState* state, const struct tm& local) {
  lgfx::LovyanGFX& gfx = canvas();
  gfx.fillScreen(theme::kBackground);
  const int8_t* step = kWander[local.tm_min % kWanderSteps];
  const int x = theme::kCenterX + step[0];
  const int y = theme::kCenterY + step[1];

  char hhmm[8];
  strftime(hhmm, sizeof(hhmm), "%H:%M", &local);
  // From the font pack at the clock's size, where it has the digits --
  // the outline fonts standing alone, no face behind them, their digits
  // rendered once and kept; else the largest face, scaled up to it.
  static OutlineFallback s_clock_font;
  if (s_clock_font.attachAtHeight(theme::kClockTextPx, "0123456789:")) {
    gfx.setFont(&s_clock_font);
    gfx.setTextSize(1.0f);
  } else {
    displayFontApplyHeight(gfx, theme::kClockTextPx);
  }
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(theme::kTextMuted);
  gfx.drawString(hhmm, x, y);

  // The song, and the artist on a line of its own under it, each cut to
  // the width of the panel at its own height.
  if (state != nullptr && state->title[0] != '\0') {
    displayFontApplyHeight(gfx, theme::kElapsedTextPx);
    const int line_gap = theme::kElapsedTextPx * 4 / 3;
    const int title_y = y + theme::kClockTextPx / 2 + theme::kElapsedTextPx;
    const char* const lines[2] = {state->title, state->artist};
    char shown[text::kMaxLineLen];
    for (int i = 0; i < 2; ++i) {
      if (lines[i][0] == '\0') {
        continue;
      }
      const int line_y = title_y + i * line_gap;
      text::ellipsize(gfx, lines[i],
                      theme::usableWidthAt(line_y, theme::kTextEdgeInset),
                      shown, sizeof(shown));
      gfx.drawString(shown, x, line_y);
    }
  }
  canvasPresent();
}

}  // namespace clock_face
}  // namespace ui
