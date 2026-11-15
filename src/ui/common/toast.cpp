#include "ui/toast.h"

#include <Arduino.h>

#include <cstdio>

#include "hardware/display_font.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/ui.h"

/**
 * A line over now playing for a few seconds: the words a voice request
 * heard. It is drawn into now playing's compose rather than over whatever is
 * on the panel, so it needs nothing of the frame underneath, and goes when
 * now playing is composed again without it.
 */

namespace ui {
namespace {

char s_text[120] = {};
unsigned long s_until_ms = 0;
bool s_showing = false;

/** Where it sits: the middle of the glass, in up to two lines. On a round
 *  panel that is where it is widest -- up at the rooms chip the circle left
 *  room for about half of "play the Beatles in the kitchen" -- and a
 *  square one shows it the same way. */
constexpr int kToastY = theme::kCenterY;
constexpr int kToastMaxLines = 2;
/** Between a two-line toast's lines. */
constexpr int kToastLineGap = theme::px(2);

}  // namespace

void showToast(const char* text, uint32_t ms) {
  snprintf(s_text, sizeof(s_text), "%s", text);
  s_until_ms = millis() + ms;
  s_showing = true;
}

bool toastExpired() {
  if (!s_showing || static_cast<long>(millis() - s_until_ms) < 0) {
    return false;
  }
  s_showing = false;
  return true;
}

namespace toast {

void draw(lgfx::LovyanGFX& gfx) {
  if (!s_showing || static_cast<long>(millis() - s_until_ms) >= 0) {
    return;
  }
  displayFontApplyHeight(gfx, theme::kSubtitleTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  const int text_h = gfx.fontHeight();
  // As wide as the glass allows at the top edge of the tallest pill, the
  // narrower one on a round panel.
  const int tallest = kToastMaxLines * text_h +
                      (kToastMaxLines - 1) * kToastLineGap +
                      2 * theme::kElapsedPillPadY;
  const int max_w = theme::usableWidthAt(kToastY - tallest / 2,
                                         theme::kTextEdgeInset);
  char lines[kToastMaxLines][text::kMaxLineLen];
  const int count = text::wrap(gfx, s_text, max_w - 2 * theme::kElapsedPillPadX,
                               kToastMaxLines, lines);
  if (count <= 0) {
    return;
  }
  int text_w = 0;
  for (int i = 0; i < count; ++i) {
    const int lw = gfx.textWidth(lines[i]);
    text_w = lw > text_w ? lw : text_w;
  }
  const int pitch = text_h + kToastLineGap;
  const int h = count * text_h + (count - 1) * kToastLineGap +
                2 * theme::kElapsedPillPadY;
  const int w = text_w + 2 * theme::kElapsedPillPadX;
  gfx.fillRoundRect(theme::kCenterX - w / 2, kToastY - h / 2, w, h,
                    theme::kElapsedPillRadius, theme::kSurface);
  gfx.setTextColor(theme::kAccent);
  const int first_y = kToastY - (count - 1) * pitch / 2;
  for (int i = 0; i < count; ++i) {
    gfx.drawString(lines[i], theme::kCenterX, first_y + i * pitch);
  }
}

}  // namespace toast
}  // namespace ui
