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
  const int a = theme::chordHalfWidth(top);
  const int b = theme::chordHalfWidth(top + height);
  const int half = (a < b ? a : b) - theme::kListRowInset;
  return half > 0 ? half : 0;
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

void drawScrollIndicator(lgfx::LovyanGFX& gfx) {
  const int limit = maxScrollPx();
  const int content = contentHeight();
  if (limit <= 0 || content <= 0) {
    return;
  }

  // A short arc on the right bezel: 0 degrees is 3 o'clock in LovyanGFX, so
  // this spans the right-hand quadrant either side of it.
  constexpr float kTrackStartDeg = -55.0f;
  constexpr float kTrackEndDeg = 55.0f;
  constexpr float kTrackSpan = kTrackEndDeg - kTrackStartDeg;

  const int cy = theme::kCenterY;

  gfx.fillArc(theme::kCenterX, cy, theme::kListScrollInnerRadius,
              theme::kListScrollOuterRadius, kTrackStartDeg, kTrackEndDeg,
              theme::kArcTrack);

  const float visible_fraction =
      static_cast<float>(kViewHeight) / static_cast<float>(content);
  const float thumb_span = kTrackSpan * visible_fraction;
  const float offset_fraction = static_cast<float>(s_scroll_px) / limit;
  const float thumb_start =
      kTrackStartDeg + (kTrackSpan - thumb_span) * offset_fraction;

  gfx.fillArc(theme::kCenterX, cy, theme::kListScrollInnerRadius,
              theme::kListScrollOuterRadius, thumb_start,
              thumb_start + thumb_span, theme::kAccent);
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

/** Draw the whole list into the frame buffer. */
void compose(lgfx::LovyanGFX& gfx) {
  // One version of the list for the whole frame: a background load landing
  // mid-compose would otherwise change the row count and the row contents
  // between calls, and draw half of one list and half of another.
  services::browse::ReadGuard guard;
  displayFontEnsureLoaded(gfx);

  // What the backdrop looks like is the shape's business: a square panel's
  // rows run edge to edge and need a backdrop that does too.
  theme::fillListBackdrop(gfx, 0, theme::kSize);

  const int count = services::browse::rowCount();
  if (count == 0) {
    drawEmptyState(gfx);
    return;
  }

  // Rows scroll under the title strip rather than past it, so the strip stays
  // a reliable place to tap to leave.
  gfx.setClipRect(0, kViewTop, theme::kSize, kViewHeight);

  int top = kViewTop - s_scroll_px;
  for (int i = 0; i < count; ++i) {
    const int pitch = pitchFor(i);
    if (top >= theme::kSize) {
      break;
    }
    if (top + pitch > kViewTop - pitch) {
      services::browse::Row row;
      if (services::browse::rowAt(i, row)) {
        if (row.header) {
          drawHeader(gfx, top, row.title);
        } else {
          drawItem(gfx, top, row);
        }
      }
    }
    top += pitch;
  }

  gfx.clearClipRect();
  drawScrollIndicator(gfx);
}

}  // namespace

void draw() {
  compose(ui::canvas());
  ui::canvasPresent();
}

void resetScroll() { s_scroll_px = 0; }

void opened() {
  s_scroll_px = 0;
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
