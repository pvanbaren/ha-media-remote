#include "ui/search.h"

#include "ui/artwork.h"

#include <Arduino.h>

#include "board/board.h"
#include "config.h"
#include "hardware/display_font.h"
#include "services/display_settings.h"
#include "services/ha_client.h"
#include "services/search.h"
#include "ui/canvas.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace ui::search {
namespace {

/** In a layout's rows, backspace. A space is the space bar. Both are drawn as
 *  glyphs. */
constexpr char kBackspace = '\b';

/** One keyboard: its rows of keys, each row centred, and how big a key is.
 *  The space bar spans `space_keys` keys' width, gaps included. */
struct Layout {
  const char* const* rows;
  int row_count;
  int key_width;
  int space_keys;
  int text_px;
};

/** Alphabetical, the default. This is a keyboard someone uses a few times a
 *  year, where hunting for a letter in a familiar-but-scrambled layout is
 *  slower than reading straight down the alphabet. Seven to a row, the last
 *  two keys space and backspace. */
constexpr const char* kAlphabeticalRows[] = {
    "ABCDEFG",
    "HIJKLMN",
    "OPQRSTU",
    "VWXYZ \b",
};
/** QWERTY, for fingers that already know it: the three letter rows of a
 *  computer keyboard, backspace where it would be beside M, and the space
 *  bar on a row of its own. */
constexpr const char* kQwertyRows[] = {
    "QWERTYUIOP",
    "ASDFGHJKL",
    "ZXCVBNM\b",
    " ",
};
static_assert(sizeof(kAlphabeticalRows) / sizeof(kAlphabeticalRows[0]) <=
                  theme::kKeyRows,
              "rows would reach the SEARCH key");
static_assert(sizeof(kQwertyRows) / sizeof(kQwertyRows[0]) <= theme::kKeyRows,
              "rows would reach the SEARCH key");

constexpr Layout kAlphabetical = {
    kAlphabeticalRows,
    sizeof(kAlphabeticalRows) / sizeof(kAlphabeticalRows[0]), theme::kKeyWidth,
    1, theme::kKeyTextPx};
constexpr Layout kQwerty = {kQwertyRows,
                            sizeof(kQwertyRows) / sizeof(kQwertyRows[0]),
                            theme::kQwertyKeyWidth, theme::kQwertySpaceKeys,
                            theme::kQwertyKeyTextPx};

/** The layout chosen in the portal, read each time so a change applies the
 *  next time search opens. */
const Layout& layout() {
  return services::display::keyboardLayout() ==
                 services::display::KeyboardLayout::kQwerty
             ? kQwerty
             : kAlphabetical;
}

bool s_showing_results = false;
/** Pixels the result list is scrolled by. Results can outnumber the rows that
 *  fit, which on a round panel is only three or four. */
int s_scroll_px = 0;

int rowY(int row) { return theme::kKeyRow0Y + row * theme::kKeyRowPitch; }

int keyWidth(const Layout& l, char key) {
  return key == ' ' ? l.space_keys * l.key_width +
                          (l.space_keys - 1) * theme::kKeyGap
                    : l.key_width;
}

/** Left edge of a centred row's first key. */
int rowLeft(const Layout& l, const char* keys) {
  int width = 0;
  for (const char* k = keys; *k != '\0'; ++k) {
    width += keyWidth(l, *k) + (k == keys ? 0 : theme::kKeyGap);
  }
  return theme::kCenterX - width / 2;
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

void drawKey(lgfx::LovyanGFX& gfx, const Layout& l, char label, int x, int y,
             int w) {
  gfx.fillRoundRect(x, y, w, theme::kKeyHeight, theme::kKeyRadius,
                    theme::kSurface);

  const int cx = x + w / 2;
  const int cy = y + theme::kKeyHeight / 2;
  if (label == kBackspace) {
    drawBackspaceGlyph(gfx, cx, cy, theme::kTextSecondary);
    return;
  }
  if (label == ' ') {
    drawSpaceGlyph(gfx, cx, cy, theme::kTextSecondary);
    return;
  }

  displayFontApplyHeight(gfx, l.text_px);
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

  const Layout& l = layout();
  for (int row = 0; row < l.row_count; ++row) {
    int x = rowLeft(l, l.rows[row]);
    for (const char* k = l.rows[row]; *k != '\0'; ++k) {
      const int w = keyWidth(l, *k);
      drawKey(gfx, l, *k, x, rowY(row), w);
      x += w + theme::kKeyGap;
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
  // At the row's first and last lines on the panel: chordHalfWidth() is 0
  // off it, and top + height is one past the row besides, so a row reaching
  // the bottom edge used to get no width and was not drawn. See
  // rowHalfWidth() in browse_list.cpp.
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

/** Draw every result that crosses rows [y0, y1) of the panel, at the
 *  current scroll. The caller has set a clip no wider than the results
 *  view: rows are drawn whole and left to the clip to trim. */
void drawResultRows(lgfx::LovyanGFX& gfx, int y0, int y1) {
  const int count = services::search::count();
  for (int i = 0; i < count; ++i) {
    const int top = resultTop(i);
    if (top >= y1) {
      break;
    }
    if (top + theme::kListRowHeight > y0) {
      drawResultRow(gfx, i, top);
    }
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
  drawResultRows(gfx, kResultViewTop, theme::kSize);
  gfx.clearClipRect();
}

/** The same backdrop the browse list paints, from the shape: flat on a
 *  square panel, a circle on black on a round one. It used to be the round
 *  one everywhere -- a full-screen black fill and a circle over it -- which
 *  on the square panel put a circular edge behind rows that run the full
 *  width, and a background that changes with height is one a scroll cannot
 *  move rows across. */
void beginFrame(lgfx::LovyanGFX& gfx) {
  displayFontEnsureLoaded(gfx);
  theme::fillListBackdrop(gfx, 0, theme::kSize);
}

/**
 * What the panel holds of the results, so a scroll can build on it -- the
 * same bookkeeping browse_list keeps, for the same reason. `valid` means the
 * results view on the glass is these results composed at `scroll_px`, and
 * `writes` is canvasPanelWrites() just after they got there; anything else
 * drawn on the panel since shows up as a different count.
 */
struct PanelState {
  bool valid = false;
  int scroll_px = 0;
  uint32_t writes = 0;
};
PanelState s_panel;

void notePanelMatches() {
  s_panel.valid = true;
  s_panel.scroll_px = s_scroll_px;
  s_panel.writes = ui::canvasPanelWrites();
}

/**
 * Repaint a scroll of the results by moving what is already on the glass.
 *
 * The browse list's scrollInPlace(), without the indicator column: the
 * results have no scroll bar. Every kept row is on the panel already, only
 * elsewhere, so the view is moved in place and only the strip the move
 * uncovers is composed and presented. The canvas is left stale outside that
 * strip until the next whole draw(), which PanelState makes safe.
 *
 * Square panels only. On a round one a row's width follows the chord at its
 * height, so a moved row is not the row that belongs there -- and the panel
 * is SPI, which cannot move its frame anyway.
 *
 * False when it cannot, with nothing sent to the panel; the caller draws the
 * screen whole instead.
 */
bool scrollInPlace() {
  if constexpr (board::kDisplayIsRound) {
    return false;
  }
  if (!s_showing_results || services::search::count() == 0 ||
      !s_panel.valid || s_panel.writes != ui::canvasPanelWrites() ||
      !ui::canvasCanScroll()) {
    return false;
  }
  // How far the picture moves: scrolling further down the results moves it
  // up.
  const int dy = s_panel.scroll_px - s_scroll_px;
  if (dy == 0) {
    return true;  // already what the panel shows
  }
  if (dy > kResultViewHeight / 2 || -dy > kResultViewHeight / 2) {
    return false;  // mostly new rows; a whole draw costs about the same
  }

  lgfx::LovyanGFX& gfx = ui::canvas();

  // The rows the move uncovers: at the bottom when the picture moves up.
  const int strip_y = dy < 0 ? theme::kSize + dy : kResultViewTop;
  const int strip_h = dy < 0 ? -dy : dy;
  displayFontEnsureLoaded(gfx);
  gfx.setClipRect(0, strip_y, theme::kSize, strip_h);
  theme::fillListBackdrop(gfx, strip_y, strip_h);
  drawResultRows(gfx, strip_y, strip_y + strip_h);
  gfx.clearClipRect();

  if (!ui::canvasScrollPanel(kResultViewTop, kResultViewHeight, dy)) {
    return false;  // nothing moved; the canvas strip is harmless
  }

  ui::canvasPresentRegion(0, strip_y, theme::kSize, strip_h);
  notePanelMatches();
  return true;
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

uint32_t updateThumbs() {
  return artwork::update(artwork::Kind::kSearch, services::search::count(),
                         resultImageUrl);
}

bool anyResultVisible(uint32_t results) {
  const int count = services::search::count();
  for (int i = 0; i < count && i < 32; ++i) {
    if ((results & (1u << i)) == 0) {
      continue;
    }
    const int top = resultTop(i);
    if (top < theme::kSize && top + theme::kListRowHeight > kResultViewTop) {
      return true;
    }
  }
  return false;
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
  if (s_showing_results) {
    notePanelMatches();
  } else {
    s_panel.valid = false;  // the keyboard, not results, is on the glass
  }
}

void redrawResults() {
  if (!scrollInPlace()) {
    draw();
  }
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

  const Layout& l = layout();
  for (int row = 0; row < l.row_count; ++row) {
    const int top = rowY(row);
    if (y < top || y >= top + theme::kKeyHeight) {
      continue;
    }
    int left = rowLeft(l, l.rows[row]);
    for (const char* k = l.rows[row]; *k != '\0'; ++k) {
      const int w = keyWidth(l, *k);
      // The gap after a key belongs to it, so a tap landing between two
      // still does something.
      if (x >= left && x < left + w + theme::kKeyGap) {
        if (*k == kBackspace) {
          return services::search::backspace() ? Result::kChanged
                                               : Result::kNone;
        }
        return services::search::append(*k) ? Result::kChanged : Result::kNone;
      }
      left += w + theme::kKeyGap;
    }
    return Result::kNone;
  }

  return Result::kNone;
}

}  // namespace ui::search
