#include "ui/search.h"

#include "ui/artwork.h"

#include <Arduino.h>

#include "config.h"
#include "hardware/display_font.h"
#include "services/ha_client.h"
#include "services/search.h"
#include "ui/canvas.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace ui::search {
namespace {

/** Alphabetical rather than QWERTY. This is a keyboard someone uses a few
 *  times a year, where hunting for a letter in a familiar-but-scrambled
 *  layout is slower than reading straight down the alphabet.
 *
 *  The last two keys are space and backspace, drawn as glyphs. */
constexpr char kKeys[theme::kKeyRows][theme::kKeyCols + 1] = {
    "ABCDEFG",
    "HIJKLMN",
    "OPQRSTU",
    "VWXYZ  ",
};
constexpr int kSpaceCol = 5;      // row 3
constexpr int kBackspaceCol = 6;  // row 3

bool s_showing_results = false;
/** Pixels the result list is scrolled by. Results can outnumber the rows that
 *  fit, which on a round panel is only three or four. */
int s_scroll_px = 0;

int rowY(int row) { return theme::kKeyRow0Y + row * theme::kKeyRowPitch; }

/** Left edge of column `col` in a centred row of kKeyCols keys. */
int keyX(int col) {
  constexpr int kRowWidth =
      theme::kKeyCols * theme::kKeyWidth + (theme::kKeyCols - 1) * theme::kKeyGap;
  return theme::kCenterX - kRowWidth / 2 +
         col * (theme::kKeyWidth + theme::kKeyGap);
}

void drawBackspaceGlyph(lgfx::LovyanGFX& gfx, int cx, int cy, uint16_t colour) {
  const int h = theme::px(5);
  const int w = theme::px(6);
  gfx.fillTriangle(cx - w, cy, cx, cy - h, cx, cy + h, colour);
  gfx.fillRect(cx, cy - h / 2, w, h, colour);
}

void drawSpaceGlyph(lgfx::LovyanGFX& gfx, int cx, int cy, uint16_t colour) {
  const int w = theme::px(11);
  const int t = theme::px(2);
  gfx.fillRect(cx - w / 2, cy + theme::px(3), w, t, colour);
}

void drawKey(lgfx::LovyanGFX& gfx, int row, int col) {
  const int x = keyX(col);
  const int y = rowY(row);
  const char label = kKeys[row][col];

  // The two blanks on the bottom row are space and backspace.
  const bool is_space = row == theme::kKeyRows - 1 && col == kSpaceCol;
  const bool is_backspace = row == theme::kKeyRows - 1 && col == kBackspaceCol;
  if (label == ' ' && !is_space && !is_backspace) {
    return;
  }

  gfx.fillRoundRect(x, y, theme::kKeyWidth, theme::kKeyHeight,
                    theme::kKeyRadius, theme::kSurface);

  const int cx = x + theme::kKeyWidth / 2;
  const int cy = y + theme::kKeyHeight / 2;
  if (is_backspace) {
    drawBackspaceGlyph(gfx, cx, cy, theme::kTextSecondary);
    return;
  }
  if (is_space) {
    drawSpaceGlyph(gfx, cx, cy, theme::kTextSecondary);
    return;
  }

  displayFontApplyHeight(gfx, theme::kKeyTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(theme::kTextPrimary);
  const char text[2] = {label, '\0'};
  gfx.drawString(text, cx, cy);
}

/** The typed query, or a prompt when it is empty. */
void drawQuery(lgfx::LovyanGFX& gfx) {
  const char* q = services::search::query();
  const bool empty = q[0] == '\0';

  displayFontApplyHeight(gfx, theme::kSearchQueryTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(empty ? theme::kTextMuted : theme::kAccent);

  char line[text::kMaxLineLen];
  text::ellipsize(gfx, empty ? "Artist name" : q,
                  theme::usableWidthAt(theme::kSearchQueryY,
                                       theme::kTextEdgeInset),
                  line, sizeof(line));
  gfx.drawString(line, theme::kCenterX, theme::kSearchQueryY);
}

void drawKeyboard(lgfx::LovyanGFX& gfx) {
  drawQuery(gfx);

  for (int row = 0; row < theme::kKeyRows; ++row) {
    for (int col = 0; col < theme::kKeyCols; ++col) {
      drawKey(gfx, row, col);
    }
  }

  const int gx = theme::kCenterX - theme::kSearchGoWidth / 2;
  const int gy = theme::kSearchGoY - theme::kSearchGoHeight / 2;
  const bool ready = services::search::query()[0] != '\0';
  gfx.fillRoundRect(gx, gy, theme::kSearchGoWidth, theme::kSearchGoHeight,
                    theme::kKeyRadius,
                    ready ? theme::kAccent : theme::kSurface);
  displayFontApplyHeight(gfx, theme::kKeyTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(ready ? theme::kBackground : theme::kTextMuted);
  gfx.drawString("SEARCH", theme::kCenterX, theme::kSearchGoY);
}

constexpr int kResultPitch = theme::kListRowHeight + theme::kListRowGap;
/** Rows live below the query line and are clipped to that band, so a
 *  half-scrolled row slides under it rather than over it. */
constexpr int kResultViewTop = theme::kSearchResultTopY;
constexpr int kResultViewHeight = theme::kSize - theme::kSearchResultTopY;

int resultTop(int index) {
  return kResultViewTop + index * kResultPitch - s_scroll_px;
}

int maxScrollPx() {
  const int count = services::search::count();
  if (count <= 0) {
    return 0;
  }
  // Far enough that the last row's centre reaches the middle of the panel,
  // the same as the browse list: on a circle the final row would otherwise
  // come to rest in the pinched bottom, the narrowest and hardest to hit.
  const int over = (count - 1) * kResultPitch + theme::kListRowHeight / 2 +
                   kResultViewTop - theme::kCenterY;
  return over > 0 ? over : 0;
}

void clampScroll() {
  const int limit = maxScrollPx();
  if (s_scroll_px > limit) {
    s_scroll_px = limit;
  }
  if (s_scroll_px < 0) {
    s_scroll_px = 0;
  }
}

/** Widest the row can be: the chord at whichever edge is further out. */
int resultHalfWidth(int top) {
  const int a = theme::chordHalfWidth(top);
  const int b = theme::chordHalfWidth(top + theme::kListRowHeight);
  const int half = (a < b ? a : b) - theme::kListRowInset;
  return half > 0 ? half : 0;
}

/** One result row: artwork on the left where it has arrived, name beside it. */
void drawResultRow(lgfx::LovyanGFX& gfx, int index, int top) {
  const services::ha::LibraryItem* item = services::search::at(index);
  if (item == nullptr) {
    return;
  }
  const int half = resultHalfWidth(top);
  if (half <= 0) {
    return;
  }
  const int x = theme::kCenterX - half;
  const int w = half * 2;

  gfx.fillRoundRect(x, top, w, theme::kListRowHeight, theme::kListRowRadius,
                    theme::kSurface);

  int text_x = x + theme::kListTextInset;
  int text_w = w - 2 * theme::kListTextInset;
  bool centred = true;

  const int thumb = artwork::size();
  if (thumb > 0) {
    const int tx = x + (theme::kListRowHeight - thumb) / 2;
    const int ty = top + (theme::kListRowHeight - thumb) / 2;
    if (artwork::draw(artwork::Kind::kSearch, index, gfx, tx, ty)) {
      const int used = tx + thumb + theme::kListThumbGap - text_x;
      text_x += used;
      text_w -= used;
      centred = false;
    }
  }
  if (text_w <= 0) {
    return;
  }

  displayFontApplyHeight(gfx, theme::kListRowTextPx);
  gfx.setTextColor(theme::kTextPrimary);

  char label[text::kMaxLineLen];
  text::ellipsize(gfx, item->name, text_w, label, sizeof(label));
  if (centred) {
    // No artwork yet, so the name gets the whole row and sits in the middle
    // of it -- which also means it does not visibly shift when the picture
    // lands, because the row is redrawn whole.
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.drawString(label, theme::kCenterX, top + theme::kListRowHeight / 2);
  } else {
    gfx.setTextDatum(textdatum_t::middle_left);
    gfx.drawString(label, text_x, top + theme::kListRowHeight / 2);
  }
}

void drawResults(lgfx::LovyanGFX& gfx) {
  const int count = services::search::count();

  if (count == 0) {
    const char* title = "No matches";
    const char* detail = services::search::query();
    if (services::search::status() == services::search::Status::kFailed) {
      title = "Search failed";
      detail = services::ha::lastError();
    }
    displayFontApplyHeight(gfx, theme::kListTitleTextPx);
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.setTextColor(theme::kTextPrimary);
    gfx.drawString(title, theme::kCenterX,
                   theme::kCenterY - theme::kListRowHeight / 2);

    displayFontApplyHeight(gfx, theme::kListHeaderTextPx);
    gfx.setTextColor(theme::kTextMuted);
    char line[text::kMaxLineLen];
    text::ellipsize(gfx, detail,
                    theme::usableWidthAt(theme::kCenterY, theme::kListRowInset),
                    line, sizeof(line));
    gfx.drawString(line, theme::kCenterX, theme::kCenterY + theme::kListRowGap);
    return;
  }

  clampScroll();
  gfx.setClipRect(0, kResultViewTop, theme::kSize, kResultViewHeight);
  for (int i = 0; i < count; ++i) {
    const int top = resultTop(i);
    if (top >= theme::kSize) {
      break;
    }
    if (top + theme::kListRowHeight > kResultViewTop - theme::kListRowHeight) {
      drawResultRow(gfx, i, top);
    }
  }
  gfx.clearClipRect();
}

void beginFrame(lgfx::LovyanGFX& gfx) {
  displayFontEnsureLoaded(gfx);
  gfx.fillScreen(theme::kBackground);
  gfx.fillCircle(theme::kCenterX, theme::kCenterY, theme::kRadius,
                 theme::kListBackdrop);
}

}  // namespace

namespace {

/** ui::artwork::UrlFn over the current results. */
bool resultImageUrl(int index, char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return false;
  }
  out[0] = '\0';
  const services::ha::LibraryItem* item = services::search::at(index);
  if (item == nullptr) {
    return false;
  }
  snprintf(out, out_len, "%s", item->image);
  return true;
}

}  // namespace

bool loadNextThumb() {
  return artwork::loadNext(artwork::Kind::kSearch, services::search::count(),
                           resultImageUrl);
}

void reset() {
  services::search::clearQuery();
  artwork::forget(artwork::Kind::kSearch);
  s_showing_results = false;
  s_scroll_px = 0;
}

bool scrollByPx(int delta) {
  if (!s_showing_results) {
    return false;
  }
  const int before = s_scroll_px;
  s_scroll_px += delta;
  clampScroll();
  return s_scroll_px != before;
}

bool atScrollLimit(int direction) {
  if (direction < 0) {
    return s_scroll_px <= 0;
  }
  return s_scroll_px >= maxScrollPx();
}

bool showingResults() { return s_showing_results; }

bool backToKeyboard() {
  if (!s_showing_results) {
    return false;
  }
  s_showing_results = false;
  s_scroll_px = 0;
  return true;
}

void draw() {
  lgfx::LovyanGFX& gfx = ui::canvas();
  beginFrame(gfx);
  if (s_showing_results) {
    drawResults(gfx);
  } else {
    drawKeyboard(gfx);
  }
  ui::canvasPresent();
}

void showSearching() {
  lgfx::LovyanGFX& gfx = ui::canvas();
  beginFrame(gfx);

  displayFontApplyHeight(gfx, theme::kSearchQueryTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(theme::kAccent);
  char line[text::kMaxLineLen];
  text::ellipsize(gfx, services::search::query(),
                  theme::usableWidthAt(theme::kCenterY, theme::kTextEdgeInset),
                  line, sizeof(line));
  gfx.drawString(line, theme::kCenterX,
                 theme::kCenterY - theme::kListRowHeight / 2);

  displayFontApplyHeight(gfx, theme::kListHeaderTextPx);
  gfx.setTextColor(theme::kTextMuted);
  gfx.drawString("Searching...", theme::kCenterX,
                 theme::kCenterY + theme::kListRowGap);

  ui::canvasPresent();
  // The search that follows blocks, so this frame has to be on the panel
  // before it starts rather than after.
  s_showing_results = true;
  // New results land in the same slots, so the walk starts again.
  artwork::forget(artwork::Kind::kSearch);
  s_scroll_px = 0;
}

void showStarting(int index) {
  const services::ha::LibraryItem* item = services::search::at(index);
  if (item == nullptr) {
    return;
  }

  lgfx::LovyanGFX& gfx = ui::canvas();
  beginFrame(gfx);

  // The same card the list drew, alone and centred, so it reads as the row
  // that was touched rather than as a new screen.
  const int top = theme::kCenterY - theme::kListRowHeight / 2 - theme::px(8);
  drawResultRow(gfx, index, top);

  displayFontApplyHeight(gfx, theme::kListHeaderTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(theme::kTextMuted);
  gfx.drawString(config::kSearchPlaysRadio ? "Starting radio..."
                                           : "Starting...",
                 theme::kCenterX,
                 top + theme::kListRowHeight + theme::px(16));

  ui::canvasPresent();
}

Result handleTap(int x, int y, int& index) {
  index = -1;

  if (s_showing_results) {
    // Above the list is the strip rows scroll *under*, so nothing there is a
    // target even when a row's coordinates reach into it.
    if (y < kResultViewTop) {
      return Result::kBack;
    }
    const int count = services::search::count();
    for (int i = 0; i < count; ++i) {
      const int top = resultTop(i);
      if (y < top || y >= top + theme::kListRowHeight) {
        continue;
      }
      const int half = resultHalfWidth(top);
      if (x < theme::kCenterX - half || x > theme::kCenterX + half) {
        return Result::kNone;
      }
      index = i;
      return Result::kSelected;
    }
    // Anywhere else on the results screen goes back to editing the query,
    // which is what a miss usually means: that was not the artist.
    return Result::kBack;
  }

  // SEARCH
  const int gy = theme::kSearchGoY - theme::kSearchGoHeight / 2;
  if (y >= gy && y < gy + theme::kSearchGoHeight &&
      x >= theme::kCenterX - theme::kSearchGoWidth / 2 &&
      x < theme::kCenterX + theme::kSearchGoWidth / 2) {
    return services::search::query()[0] != '\0' ? Result::kSearch
                                                : Result::kNone;
  }

  for (int row = 0; row < theme::kKeyRows; ++row) {
    const int top = rowY(row);
    if (y < top || y >= top + theme::kKeyHeight) {
      continue;
    }
    for (int col = 0; col < theme::kKeyCols; ++col) {
      const int left = keyX(col);
      // The gap between keys belongs to whichever key is nearer, so a tap
      // landing between two still does something.
      if (x < left || x >= left + theme::kKeyWidth + theme::kKeyGap) {
        continue;
      }
      const bool last_row = row == theme::kKeyRows - 1;
      if (last_row && col == kBackspaceCol) {
        return services::search::backspace() ? Result::kChanged : Result::kNone;
      }
      if (last_row && col == kSpaceCol) {
        return services::search::append(' ') ? Result::kChanged : Result::kNone;
      }
      const char c = kKeys[row][col];
      if (c == ' ') {
        return Result::kNone;
      }
      return services::search::append(c) ? Result::kChanged : Result::kNone;
    }
    return Result::kNone;
  }

  return Result::kNone;
}

}  // namespace ui::search
