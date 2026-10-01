#include "ui/player_pick.h"

#include <Arduino.h>

#include <cstring>

#include "board/board.h"
#include "config.h"
#include "hardware/display_font.h"
#include "services/player_list.h"
#include "ui/canvas.h"
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

/** As the other lists' rows: the chord at whichever edge is further in. */
int rowHalfWidth(int top) {
  const int first = top > 0 ? top : 0;
  const int bottom = top + theme::kListRowHeight - 1;
  const int last = bottom < theme::kSize - 1 ? bottom : theme::kSize - 1;
  if (last < first) {
    return 0;
  }
  const int a = theme::chordHalfWidth(first);
  const int b = theme::chordHalfWidth(last);
  const int half = (a < b ? a : b) - theme::kListRowInset;
  return half > 0 ? half : 0;
}

void drawRow(lgfx::LovyanGFX& gfx, int row, int top) {
  const int half = rowHalfWidth(top);
  if (half <= 0) {
    return;
  }
  const int x = theme::kCenterX - half;
  const int w = half * 2;
  const int cy = top + theme::kListRowHeight / 2;
  gfx.fillRoundRect(x, top, w, theme::kListRowHeight, theme::kListRowRadius,
                    theme::kSurface);

  displayFontApplyHeight(gfx, theme::kListRowTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  const services::ha::PlayerEntry* entry = services::players::entryAt(row);
  if (entry == nullptr) {
    gfx.setTextColor(theme::kAccent);
    gfx.drawString("Refresh", theme::kCenterX, cy);
    return;
  }
  // Unavailable players are still offered -- one that is off now may well
  // be on later -- but greyed, so the live ones stand out.
  gfx.setTextColor(entry->available ? theme::kTextPrimary : theme::kTextMuted);
  char label[text::kMaxLineLen];
  text::ellipsize(gfx, entry->name[0] != '\0' ? entry->name : entry->entity_id,
                  w - 2 * theme::kListTextInset, label, sizeof(label));
  gfx.drawString(label, theme::kCenterX, cy);
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
  text::ellipsize(gfx, noted ? s_note : "Choose a player",
                  theme::usableWidthAt(kTitleY, theme::kTextEdgeInset), line,
                  sizeof(line));
  gfx.drawString(line, theme::kCenterX, kTitleY);
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
