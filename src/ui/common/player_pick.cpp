#include "ui/player_pick.h"

#include <Arduino.h>

#include <cstring>

#include "board/board.h"
#include "config.h"
#include "hardware/display_font.h"
#include "services/player_list.h"
#include "ui/canvas.h"
#include "ui/status_screens.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace ui::player_pick {
namespace {

char s_note[64] = {};
char s_chosen[config::kEntityIdMaxLen] = {};
int s_scroll_px = 0;

constexpr int kTitleY = theme::kSearchQueryY;
/** Rows live below the title and slide under it as they scroll. */
constexpr int kListTop = theme::kSearchQueryY + theme::px(18);
constexpr int kPitch = theme::kListRowHeight + theme::kListRowGap;

/** The players, then one more row to fetch the list again. */
int rowCount() { return services::players::entryCount() + 1; }

int rowTop(int row) { return kListTop + row * kPitch - s_scroll_px; }

int maxScrollPx() {
  // Far enough for the last row's centre to reach the middle of the panel,
  // as on the other lists: on a circle it would otherwise rest in the
  // pinched bottom, the hardest place to hit.
  const int over = (rowCount() - 1) * kPitch + theme::kListRowHeight / 2 +
                   kListTop - theme::kCenterY;
  return over > 0 ? over : 0;
}

void clampScroll() {
  const int limit = maxScrollPx();
  s_scroll_px = s_scroll_px > limit ? limit : s_scroll_px;
  s_scroll_px = s_scroll_px < 0 ? 0 : s_scroll_px;
}

/** The row's tile: see theme::listRowHalfWidth(). */
int rowHalfWidth(int top) {
  return theme::listRowHalfWidth(top, theme::kListRowHeight);
}

/** A pressed row is lit in the accent with dark text, as a settings row is,
 *  so a tap is seen to land before the choice is stored and the next screen
 *  loads. */
void drawRow(lgfx::LovyanGFX& gfx, int row, int top, bool pressed = false) {
  const int half = rowHalfWidth(top);
  if (half <= 0) {
    return;
  }
  const int x = theme::kCenterX - half;
  const int w = half * 2;
  const int cy = top + theme::kListRowHeight / 2;
  gfx.fillRoundRect(x, top, w, theme::kListRowHeight, theme::kListRowRadius,
                    pressed ? theme::kAccent : theme::kSurface);

  displayFontApplyHeight(gfx, theme::kListRowTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  const services::ha::PlayerEntry* entry = services::players::entryAt(row);
  if (entry == nullptr) {
    gfx.setTextColor(pressed ? theme::kBackground : theme::kAccent);
    gfx.drawString("Refresh", theme::kCenterX, cy);
    return;
  }
  // Unavailable players are still offered -- one that is off now may well
  // be on later -- but greyed, so the live ones stand out.
  gfx.setTextColor(pressed           ? theme::kBackground
                   : entry->available ? theme::kTextPrimary
                                      : theme::kTextMuted);
  char label[text::kMaxLineLen];
  text::ellipsize(gfx, entry->name[0] != '\0' ? entry->name : entry->entity_id,
                  w - 2 * theme::kListTextInset, label, sizeof(label));
  gfx.drawString(label, theme::kCenterX, cy);
}

/** Light `row` on the glass at once, over the frame as last composed: its
 *  own rectangle, less any of it scrolled up under the title. */
void showPressed(int row) {
  const int top = rowTop(row);
  const int y0 = top > kListTop ? top : kListTop;
  const int bottom = top + theme::kListRowHeight;
  const int y1 = bottom < theme::kSize ? bottom : theme::kSize;
  const int half = rowHalfWidth(top);
  if (y1 <= y0 || half <= 0) {
    return;
  }
  lgfx::LovyanGFX& gfx = ui::canvas();
  gfx.setClipRect(0, y0, theme::kSize, y1 - y0);
  drawRow(gfx, row, top, true);
  gfx.clearClipRect();
  ui::canvasPresentRegion(theme::kCenterX - half, y0, half * 2, y1 - y0);
}

}  // namespace

void open(const char* note) {
  snprintf(s_note, sizeof(s_note), "%s", note != nullptr ? note : "");
  s_chosen[0] = '\0';
  s_scroll_px = 0;
}

void draw() {
  lgfx::LovyanGFX& gfx = ui::canvas();
  displayFontEnsureLoaded(gfx);
  theme::fillListBackdrop(gfx, 0, theme::kSize);

  clampScroll();
  gfx.setClipRect(0, kListTop, theme::kSize, theme::kSize - kListTop);
  for (int row = 0; row < rowCount(); ++row) {
    const int top = rowTop(row);
    if (top >= theme::kSize) {
      break;
    }
    if (top + theme::kListRowHeight > kListTop) {
      drawRow(gfx, row, top);
    }
  }
  gfx.clearClipRect();

  // The title last, over anything that scrolled up behind it.
  const bool noted = s_note[0] != '\0';
  displayFontApplyHeight(gfx, theme::kListTitleTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(noted ? theme::kWarning : theme::kTextPrimary);
  char line[text::kMaxLineLen];
  // Centred between the gear on the right and as much again on the left.
  text::ellipsize(gfx, noted ? s_note : "Choose a player",
                  theme::usableWidthAt(kTitleY, theme::kTextEdgeInset) -
                      2 * statusScreenGearClearance(),
                  line, sizeof(line));
  gfx.drawString(line, theme::kCenterX, kTitleY);
  // The settings, which are otherwise only reached from now playing: the
  // network, the transmit power and a restart are as much use with no player
  // chosen as with one.
  statusScreenDrawGear(gfx, theme::kTextSecondary, theme::kBackground);
  ui::canvasPresent();
}

bool scrollByPx(int delta) {
  const int before = s_scroll_px;
  s_scroll_px += delta;
  clampScroll();
  return s_scroll_px != before;
}

bool atScrollLimit(int direction) {
  return direction < 0 ? s_scroll_px <= 0 : s_scroll_px >= maxScrollPx();
}

Result handleTap(int x, int y) {
  if (y < kListTop) {
    return Result::kNone;
  }
  for (int row = 0; row < rowCount(); ++row) {
    const int top = rowTop(row);
    if (y < top || y >= top + theme::kListRowHeight) {
      continue;
    }
    const int half = rowHalfWidth(top);
    if (x < theme::kCenterX - half || x > theme::kCenterX + half) {
      return Result::kNone;
    }
    // Lit before anything else happens: storing the choice and loading what
    // follows, or fetching the list again, takes a moment, and the screen
    // that comes next replaces the lit row.
    showPressed(row);
    const services::ha::PlayerEntry* entry = services::players::entryAt(row);
    if (entry == nullptr) {
      return Result::kRefresh;
    }
    snprintf(s_chosen, sizeof(s_chosen), "%s", entry->entity_id);
    return Result::kChosen;
  }
  return Result::kNone;
}

const char* chosenEntity() { return s_chosen; }

}  // namespace ui::player_pick
