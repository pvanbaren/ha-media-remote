#include "ui/clock_face.h"

#include <cstdio>

#include "config.h"
#include "hardware/display_font.h"
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
  displayFontApplyHeight(gfx, theme::kClockTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(theme::kTextMuted);
  gfx.drawString(hhmm, x, y);

  if (state != nullptr && state->title[0] != '\0') {
    char line[sizeof(state->title) + sizeof(state->artist) + 4];
    if (state->artist[0] != '\0') {
      snprintf(line, sizeof(line), "%s \xC2\xB7 %s", state->title,
               state->artist);
    } else {
      snprintf(line, sizeof(line), "%s", state->title);
    }
    const int line_y = y + theme::kClockTextPx / 2 + theme::kElapsedTextPx;
    displayFontApplyHeight(gfx, theme::kElapsedTextPx);
    char shown[text::kMaxLineLen];
    text::ellipsize(gfx, line,
                    theme::usableWidthAt(line_y, theme::kTextEdgeInset), shown,
                    sizeof(shown));
    gfx.drawString(shown, x, line_y);
  }
  canvasPresent();
}

}  // namespace clock_face
}  // namespace ui
