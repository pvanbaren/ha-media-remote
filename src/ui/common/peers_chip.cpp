#include "ui/peers_chip.h"

#include <cstdio>

#include "hardware/display_font.h"
#include "ui/theme.h"

namespace ui::peers_chip {
namespace {

/** Below the volume -- clear of the round panel's arc and the square one's
 *  bar band -- and above the artist line, on both layouts. */
constexpr int kChipY = theme::px(42);
/** A finger's worth around the chip, which is small. */
constexpr int kHitPad = theme::px(8);

bool s_shown = false;
int s_left = 0;
int s_top = 0;
int s_width = 0;
int s_height = 0;

}  // namespace

void draw(lgfx::LovyanGFX& gfx, const services::ha::PlayerState& state) {
  s_shown = state.peer_count > 0;
  if (!s_shown) {
    return;
  }
  char label[24];
  snprintf(label, sizeof(label), "+%d room%s", state.peer_count,
           state.peer_count == 1 ? "" : "s");

  // The elapsed chip's look, in the accent colour: it is a control.
  displayFontApplyHeight(gfx, theme::kElapsedTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  const int text_w = gfx.textWidth(label);
  const int text_h = gfx.fontHeight();
  s_width = text_w + 2 * theme::kElapsedPillPadX;
  s_height = text_h + 2 * theme::kElapsedPillPadY;
  s_left = theme::kCenterX - s_width / 2;
  s_top = kChipY - s_height / 2;
  gfx.fillRoundRect(s_left, s_top, s_width, s_height,
                    theme::kElapsedPillRadius, theme::kSurface);
  gfx.setTextColor(theme::kAccent);
  gfx.drawString(label, theme::kCenterX, kChipY);
}

bool hit(int x, int y) {
  return s_shown && x >= s_left - kHitPad &&
         x <= s_left + s_width + kHitPad && y >= s_top - kHitPad &&
         y <= s_top + s_height + kHitPad;
}

}  // namespace ui::peers_chip
