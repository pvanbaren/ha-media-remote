#include "ui/browse_list.h"

#include "ui/artwork.h"

#include <Arduino.h>

#include "config.h"
#include "hardware/display_font.h"
#include "services/browse.h"
#include "services/ha_client.h"
#include "ui/canvas.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace ui::browse_list {
namespace {

/** Pixels the content is scrolled up by, not rows.
 *
 *  A row index cannot express "half a row showing", which is what following a
 *  finger requires -- and rows here are not even a uniform height, since a
 *  section heading is a line of text rather than a card. So everything below
 *  works in pixels and walks the rows to find where they land. */
int s_scroll_px = 0;

constexpr int kRowPitch = theme::kListRowHeight + theme::kListRowGap;
constexpr int kHeaderPitch = theme::kListHeaderHeight + theme::kListRowGap;

int pitchFor(int index) {
  services::browse::Row row;
  if (!services::browse::rowAt(index, row)) {
    return kRowPitch;
  }
  return row.header ? kHeaderPitch : kRowPitch;
}

int contentHeight() {
  const int count = services::browse::rowCount();
  int height = 0;
  for (int i = 0; i < count; ++i) {
    height += pitchFor(i);
  }
  return height;
}

/** Rows are drawn into the band between the title strip and the bottom of the
 *  panel, and clipped to it so a half-scrolled row does not spill over the
 *  strip that dismisses the list. */
constexpr int kViewTop = theme::kListTopY;
constexpr int kViewHeight = theme::kSize - theme::kListTopY;

/**
 * The band a scrolling repaint has to cover.
 *
 * The rows, and the indicator beside them, which moves with them and does
 * not necessarily stay level with them -- the round build's arc reaches
 * above the first row, so the shape is asked rather than assumed.
 *
 * What is deliberately left out is everything above that: the title strip,
 * and on a square panel the corners outside the circular backdrop. No row
 * can reach them, because rowHalfWidth() keeps every row inside the circle,
 * so during a scroll they are already correct -- and the full-screen black
 * fill that paints them is a megabyte spent on nothing.
 */
int bandTop() {
  const int indicator = theme::scrollIndicatorTopY();
  return indicator < kViewTop ? indicator : kViewTop;
}

int bandHeight() { return theme::kSize - bandTop(); }

int maxScrollPx() {
  const int count = services::browse::rowCount();
  if (count <= 0) {
    return 0;
  }
  services::browse::Row last;
  if (!services::browse::rowAt(count - 1, last)) {
    return 0;
  }
  const int last_height =
      last.header ? theme::kListHeaderHeight : theme::kListRowHeight;

  // Far enough that the final row's *centre* reaches the middle of the panel,
  // rather than merely far enough that its bottom reaches the bottom edge.
  // On a round display the last row would otherwise come to rest in the
  // pinched bottom of the circle, where it is both the narrowest row on screen
  // and the hardest to hit. Scrolling it up to the middle costs nothing but
  // empty space below it.
  //
  // Negative when everything already fits, which clamps to no scrolling.
  const int over = contentHeight() - pitchFor(count - 1) + last_height / 2 +
                   kViewTop - theme::kCenterY;
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

/** Widest a row can be without crossing the bezel: the narrower of its top and
 *  bottom edges, since a rectangle on a circle is pinched at whichever edge
 *  sits further from the centre. */
int rowHalfWidth(int top, int height) {
  // Measured at the first and last lines of the row that are on the panel,
  // not at top and top + height. chordHalfWidth() is 0 off the panel, and
  // top + height is one past the row's last line besides -- so asking there
  // gave every row that reached the bottom edge no width at all, and it was
  // not drawn. Easy to miss in a whole repaint, where the row simply arrived
  // late; fatal to scrollInPlace(), whose strip at the bottom is nearly
  // always that row, and which then moved a band of bare backdrop up the
  // screen on every step.
  const int first = top > 0 ? top : 0;
  const int last = top + height - 1 < theme::kSize - 1 ? top + height - 1
                                                       : theme::kSize - 1;
  if (last < first) {
    return 0;  // entirely off the panel
  }
  const int a = theme::chordHalfWidth(first);
  const int b = theme::chordHalfWidth(last);
  const int half = (a < b ? a : b) - theme::kListRowInset;
  return half > 0 ? half : 0;
}

/**
 * Let the scan-out have the bus back.
 *
 * Composing is the longest unbroken run of PSRAM writes this firmware does,
 * and on a panel read continuously out of the same memory that is what a
 * FIFO underrun is made of -- the whole picture breaking up rather than a
 * few lines. The copy in rgb.cpp has paused between slabs for this reason
 * since the tearing work; composing never did, which is why a repaint could
 * still glitch after the copy had been made polite.
 *
 * Nothing at all on a panel written over a bus: there is no continuous read
 * to starve, and a millisecond a slab would be pure latency. Nor while
 * composing an area no bigger than one slab of full-width rows -- the strip
 * a slow scroll uncovers, the column the indicator runs down -- where there
 * is no burst to break up and a pause per row would cost more than the
 * drawing (see composeClipped()).
 */
bool s_small_compose = false;

void standOff() {
  if constexpr (board::kComposeSlabRows > 0) {
    if (!s_small_compose) {
      delay(1);
    }
  }
}

void drawHeader(lgfx::LovyanGFX& gfx, int top, const char* title) {
  const int half = rowHalfWidth(top, theme::kListHeaderHeight);
  if (half <= 0) {
    return;
  }
  displayFontApplyHeight(gfx, theme::kListHeaderTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(theme::kAccent);

  char label[text::kMaxLineLen];
  text::ellipsize(gfx, title, half * 2, label, sizeof(label));
  gfx.drawString(label, theme::kCenterX,
                 top + theme::kListHeaderHeight / 2);
}

void drawItem(lgfx::LovyanGFX& gfx, int top, const services::browse::Row& row) {
  const int half = rowHalfWidth(top, theme::kListRowHeight);
  if (half <= 0) {
    return;
  }
  const int y = top;
  const int x = theme::kCenterX - half;
  const int w = half * 2;

  gfx.fillRoundRect(x, y, w, theme::kListRowHeight, theme::kListRowRadius,
                    theme::kSurface);
  // The longest single burst left in a compose: a full-width row fill is
  // over a hundred kilobytes in one call, longer than the backdrop ever goes
  // without letting go of the bus.
  standOff();

  // Artwork first, then the name in whatever is left. An item with no image
  // gives its space back to the text rather than leaving a hole, which also
  // means a board that could not claim the sprites renders a plain list
  // instead of a broken one.
  const int thumb = artwork::size();
  int text_x = x + theme::kListTextInset;
  int text_w = w - 2 * theme::kListTextInset;

  if (thumb > 0) {
    const int thumb_x = x + (theme::kListRowHeight - thumb) / 2;
    const int thumb_y = y + (theme::kListRowHeight - thumb) / 2;
    if (artwork::draw(artwork::Kind::kBrowse, row.item, gfx, thumb_x,
                      thumb_y)) {
      const int used = thumb_x + thumb + theme::kListThumbGap - text_x;
      text_x += used;
      text_w -= used;
    }
  }
  if (text_w <= 0) {
    return;
  }

  displayFontApplyHeight(gfx, theme::kListRowTextPx);
  gfx.setTextDatum(textdatum_t::middle_left);
  gfx.setTextColor(theme::kTextPrimary);

  char label[text::kMaxLineLen];
  text::ellipsize(gfx, row.title, text_w, label, sizeof(label));
  gfx.drawString(label, text_x, y + theme::kListRowHeight / 2);
}

/** Hand the shape a position and a length and let it decide what an
 *  indicator looks like here: a bar down the edge, or an arc on the bezel. */
void drawScrollIndicator(lgfx::LovyanGFX& gfx) {
  const int limit = maxScrollPx();
  const int content = contentHeight();
  if (limit <= 0 || content <= 0) {
    return;  // it all fits, so there is no position worth reporting
  }
  theme::drawScrollIndicator(
      gfx, static_cast<float>(s_scroll_px) / static_cast<float>(limit),
      static_cast<float>(kViewHeight) / static_cast<float>(content));
}

/** Say which kind of empty this is. A blank list has four quite different
 *  causes and only one of them is a fault; "Nothing to show" covered all four
 *  and helped with none. */
void drawEmptyState(lgfx::LovyanGFX& gfx) {
  const char* title = "Nothing to show";
  const char* detail = "";

  switch (services::browse::problem()) {
    case services::browse::Problem::kNoConfigEntry:
      title = "Not set up";
      detail = "add the Music Assistant id in the portal";
      break;
    case services::browse::Problem::kRequestFailed:
      title = "Music Assistant";
      detail = services::ha::lastError();
      break;
    case services::browse::Problem::kLibraryEmpty:
      title = "Library empty";
      detail = "nothing matched";
      break;
    default:
      break;
  }

  displayFontApplyHeight(gfx, theme::kListTitleTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(theme::kTextPrimary);
  gfx.drawString(title, theme::kCenterX,
                 theme::kCenterY - theme::kListRowHeight / 2);

  if (detail[0] == '\0') {
    return;
  }
  displayFontApplyHeight(gfx, theme::kListHeaderTextPx);
  gfx.setTextColor(theme::kTextMuted);

  char lines[2][text::kMaxLineLen] = {};
  const int width = theme::usableWidthAt(theme::kCenterY, theme::kListRowInset);
  const int count = text::wrap(gfx, detail, width, 2, lines);
  for (int i = 0; i < count; ++i) {
    gfx.drawString(lines[i], theme::kCenterX,
                   theme::kCenterY + theme::kListRowGap +
                   i * theme::kListHeaderHeight);
  }
}

/** Paint the backdrop in slabs, pausing between them. One call would be a
 *  megabyte and a half issued flat out, which is most of a frame's worth of
 *  bus time and the single longest burst in a repaint. */
void fillBackdrop(lgfx::LovyanGFX& gfx, int top, int height) {
  if constexpr (board::kComposeSlabRows <= 0) {
    theme::fillListBackdrop(gfx, top, height);
    return;
  }
  const int end = top + height;
  for (int y = top; y < end; y += board::kComposeSlabRows) {
    if (y > top) {
      standOff();
    }
    const int slab = (end - y) < board::kComposeSlabRows ? (end - y)
                                                         : board::kComposeSlabRows;
    theme::fillListBackdrop(gfx, y, slab);
  }
}

/** Draw every row that crosses rows [y0, y1) of the panel, at the current
 *  scroll. The caller holds the list's ReadGuard and has set a clip no wider
 *  than the view: rows are drawn whole and left to the clip to trim. */
void drawRows(lgfx::LovyanGFX& gfx, int y0, int y1) {
  const int count = services::browse::rowCount();
  int top = kViewTop - s_scroll_px;
  for (int i = 0; i < count && top < y1; ++i) {
    const int pitch = pitchFor(i);
    if (top + pitch > y0) {
      services::browse::Row row;
      if (services::browse::rowAt(i, row)) {
        if (row.header) {
          drawHeader(gfx, top, row.title);
        } else {
          // A row is a rounded fill, a thumbnail blit and a line of
          // anti-aliased text -- comfortably more than a slab's worth of
          // bus time, so one pause each rather than a row counter.
          drawItem(gfx, top, row);
          standOff();
        }
      }
    }
    top += pitch;
  }
}

/** Draw the list into the frame buffer. `band_only` repaints just the part
 *  a scroll moves; see bandTop() for what that leaves alone and why. */
void compose(lgfx::LovyanGFX& gfx, bool band_only) {
  // One version of the list for the whole frame: a background load landing
  // mid-compose would otherwise change the row count and the row contents
  // between calls, and draw half of one list and half of another.
  services::browse::ReadGuard guard;
  displayFontEnsureLoaded(gfx);

  // Only over the rows that move, when that is all that moved. What the
  // backdrop looks like is the shape's business: a square panel's rows run
  // edge to edge and need a backdrop that does too.
  if (band_only) {
    fillBackdrop(gfx, bandTop(), bandHeight());
  } else {
    fillBackdrop(gfx, 0, theme::kSize);
  }

  const int count = services::browse::rowCount();
  if (count == 0) {
    gfx.clearClipRect();
    drawEmptyState(gfx);
    return;
  }

  // Rows scroll under the title strip rather than past it, so the strip stays
  // a reliable place to tap to leave.
  gfx.setClipRect(0, kViewTop, theme::kSize, kViewHeight);
  drawRows(gfx, kViewTop, theme::kSize);
  gfx.clearClipRect();
  drawScrollIndicator(gfx);
}

/**
 * What the panel holds of the list, so a scroll can build on it.
 *
 * `valid` means the band on the glass is this list composed at `scroll_px`,
 * and `writes` is canvasPanelWrites() just after it got there. Anything else
 * drawing on the panel since -- another screen, the "Starting..." card --
 * shows up as a different count, and the next repaint starts from scratch.
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

/** Compose the list inside one rectangle of the canvas and nothing outside
 *  it: the backdrop, whichever rows cross it, and the indicator where it
 *  falls inside. Caller holds the ReadGuard. Square panels only: the round
 *  backdrop sets a clip of its own. */
void composeClipped(lgfx::LovyanGFX& gfx, int x, int y, int w, int h) {
  s_small_compose = static_cast<long>(w) * h <=
                    static_cast<long>(theme::kSize) * board::kComposeSlabRows;
  gfx.setClipRect(x, y, w, h);
  theme::fillListBackdrop(gfx, y, h);

  // Rows only ever show inside the view, so the clip narrows to it for them.
  const int row_y0 = y > kViewTop ? y : kViewTop;
  if (y + h > row_y0) {
    gfx.setClipRect(x, row_y0, w, y + h - row_y0);
    drawRows(gfx, row_y0, y + h);
  }

  gfx.setClipRect(x, y, w, h);
  drawScrollIndicator(gfx);
  gfx.clearClipRect();
  s_small_compose = false;
}

/**
 * Repaint a scroll by moving what is already on the glass.
 *
 * Every row a scroll keeps is on the panel already, pixel for pixel, only
 * somewhere else -- on a square panel a row does not change shape with its
 * height. So rather than recompose 624 rows and copy them all across, this
 * moves the panel's own rows by the distance scrolled (canvasScrollPanel(),
 * a move within the one framebuffer), then composes and presents only what
 * the move cannot supply: the strip it uncovers, and the narrow column the
 * indicator runs down, since the old indicator moved with the rows.
 *
 * Measured before this, a band repaint was about 75 ms of compose and 60 of
 * copy. A move is one pass over the band in place -- the same bytes as the
 * copy, none of the compose -- plus a few rows of drawing.
 *
 * The canvas is left behind: outside the two strips it still holds the rows
 * where they were. That is fine for as long as the only thing presented over
 * the band is what this composes, which PanelState checks, and the next
 * whole repaint puts the canvas right again.
 *
 * False when it cannot -- nothing on the panel to build on, a jump too far
 * to be worth it, a panel that cannot move its frame -- and nothing has
 * reached the panel; the caller repaints the band instead.
 */
bool scrollInPlace() {
  if (!s_panel.valid || s_panel.writes != ui::canvasPanelWrites() ||
      !ui::canvasCanScroll()) {
    return false;
  }
  // How far the picture moves: scrolling further down the list moves it up.
  const int dy = s_panel.scroll_px - s_scroll_px;
  if (dy == 0) {
    return true;  // already what the panel shows
  }
  // Past half the view, most of it is new anyway and a whole repaint costs
  // about the same.
  if (dy > kViewHeight / 2 || -dy > kViewHeight / 2) {
    return false;
  }
  int column_x = 0;
  int column_w = 0;
  if (!theme::scrollIndicatorColumn(column_x, column_w)) {
    return false;
  }

  lgfx::LovyanGFX& gfx = ui::canvas();

  // The rows the move uncovers: at the bottom when the picture moves up.
  const int strip_y = dy < 0 ? theme::kSize + dy : kViewTop;
  const int strip_h = dy < 0 ? -dy : dy;
  {
    services::browse::ReadGuard guard;
    displayFontEnsureLoaded(gfx);
    composeClipped(gfx, 0, strip_y, theme::kSize, strip_h);
    composeClipped(gfx, column_x, bandTop(), column_w, bandHeight());
  }

  // The bar's column stays out of the move. Its thumb travels the opposite
  // way to the rows, so carrying it with them showed it jumping the wrong
  // way before the column present put it right -- visibly, against the
  // scroll, on every step.
  if (!ui::canvasScrollPanel(kViewTop, kViewHeight, dy, column_x,
                             column_w)) {
    return false;  // nothing moved; the canvas strips are harmless
  }

  ui::canvasPresentRegion(0, strip_y, theme::kSize, strip_h);
  ui::canvasPresentRegion(column_x, bandTop(), column_w, bandHeight());
  notePanelMatches();
  return true;
}

void paint(bool band_only) {
  compose(ui::canvas(), band_only);
  if (band_only) {
    ui::canvasPresentRegion(0, bandTop(), theme::kSize, bandHeight());
  } else {
    ui::canvasPresent();
  }
  notePanelMatches();
}

}  // namespace

void draw() { paint(false); }

void redrawRows() {
  if (!scrollInPlace()) {
    paint(true);
  }
}

void repaintRows() { paint(true); }

void resetScroll() { s_scroll_px = 0; }

void opened() {
  s_scroll_px = 0;
  s_panel.valid = false;  // a fresh open is composed whole
  artwork::forget(artwork::Kind::kBrowse);
}

uint32_t updateThumbs() {
  return artwork::update(artwork::Kind::kBrowse, services::browse::itemCount(),
                         services::browse::imageUrlAt);
}

bool anyItemVisible(uint32_t items) {
  if (items == 0) {
    return false;
  }
  services::browse::ReadGuard guard;
  const int count = services::browse::rowCount();
  // The same walk compose() does, and the same test for a row being in view.
  int top = kViewTop - s_scroll_px;
  for (int i = 0; i < count && top < theme::kSize; ++i) {
    const int pitch = pitchFor(i);
    if (top + pitch > kViewTop) {
      services::browse::Row row;
      if (services::browse::rowAt(i, row) && !row.header && row.item >= 0 &&
          row.item < 32 && (items & (1u << row.item)) != 0) {
        return true;
      }
    }
    top += pitch;
  }
  return false;
}

bool scrollByPx(int delta) {
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

void showStarting(int row_index) {
  services::browse::Row row;
  if (!services::browse::rowAt(row_index, row)) {
    return;
  }

  // The card sits a little above centre so the label below it is inside the
  // circle rather than running into the curve.
  const int card_top =
      theme::kCenterY - theme::kListRowHeight / 2 - theme::px(8);

  lgfx::LovyanGFX& gfx = ui::canvas();
  displayFontEnsureLoaded(gfx);
  theme::fillListBackdrop(gfx, 0, theme::kSize);
  // The same card the list drew, in the same shape, so it reads as the row
  // that was touched rather than as a new screen.
  drawItem(gfx, card_top, row);
  displayFontApplyHeight(gfx, theme::kListHeaderTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(theme::kTextMuted);
  gfx.drawString("Starting...", theme::kCenterX,
                 card_top + theme::kListRowHeight + theme::px(16));
  ui::canvasPresent();
  s_panel.valid = false;  // the card, not the list, is on the glass now
}

int itemForRow(int row_index) {
  services::browse::Row row;
  if (!services::browse::rowAt(row_index, row) || row.header) {
    return -1;
  }
  return row.item;
}

bool playRow(int row_index) {
  services::browse::Row row;
  if (!services::browse::rowAt(row_index, row) || row.header) {
    return false;
  }
  return services::browse::play(row.item);
}

Result handleTap(int x, int y, int& row_out) {
  row_out = -1;
  // Above the list is the title strip; tapping it backs out.
  if (y < kViewTop) {
    return Result::kDismissed;
  }

  // Same reason as compose(): the row the finger landed on has to be the row
  // that was drawn, not whatever a load has since put in its place.
  services::browse::ReadGuard guard;
  const int count = services::browse::rowCount();
  int top = kViewTop - s_scroll_px;
  for (int i = 0; i < count; ++i) {
    services::browse::Row row;
    if (!services::browse::rowAt(i, row)) {
      break;
    }
    const int height =
        row.header ? theme::kListHeaderHeight : theme::kListRowHeight;
    const int pitch = pitchFor(i);

    if (y >= top && y < top + height) {
      if (row.header) {
        return Result::kNone;  // a label, not a target
      }
      const int half = rowHalfWidth(top, height);
      if (x < theme::kCenterX - half || x > theme::kCenterX + half) {
        return Result::kNone;
      }
      // Report the hit and stop. Playing it here would block the caller
      // before it had a chance to put anything on screen.
      row_out = i;
      return Result::kSelected;
    }
    top += pitch;
    if (top >= theme::kSize) {
      break;
    }
  }

  return Result::kNone;
}

}  // namespace ui::browse_list
