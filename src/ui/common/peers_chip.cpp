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

/** The player is playing, but not here: this room's volume device -- a
 *  separate one, a Triad zone -- is off, so whatever is heard is heard in
 *  other rooms. Without saying so, now playing looks exactly as it does
 *  when this room is listening. */
bool offHere(const services::ha::PlayerState& state) {
  return state.control_off && !state.hold_off_here &&
         services::ha::controlIsSeparate() &&
         state.playback == services::ha::PlaybackState::kPlaying;
}

}  // namespace

void draw(lgfx::LovyanGFX& gfx, const services::ha::PlayerState& state) {
  // "Off in this room" in the chip's place, in amber: a state to notice, not
  // a control of its own. It sits in the top half, where a tap switches the
  // volume device on, so it is not a hit target -- the tap goes through and
  // does exactly that. The rooms chip cannot be wanted at the same time:
  // other rooms on this room's input only matter while this room is on.
  const bool off_here = offHere(state);
  s_shown = !off_here && state.peer_count > 0;
  if (!s_shown && !off_here) {
    return;
  }
  char label[24];
  if (off_here) {
    snprintf(label, sizeof(label), "Off in this room");
  } else {
    snprintf(label, sizeof(label), "+%d room%s", state.peer_count,
             state.peer_count == 1 ? "" : "s");
  }

  // The elapsed chip's look, in the accent colour when it is a control.
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
  gfx.setTextColor(off_here ? theme::kWarning : theme::kAccent);
  gfx.drawString(label, theme::kCenterX, kChipY);
}

bool hit(int x, int y) {
  return s_shown && x >= s_left - kHitPad &&
         x <= s_left + s_width + kHitPad && y >= s_top - kHitPad &&
         y <= s_top + s_height + kHitPad;
}

}  // namespace ui::peers_chip
