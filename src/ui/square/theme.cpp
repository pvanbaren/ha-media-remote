#include "ui/theme.h"

#include "board/board.h"

/**
 * Square geometry: the whole of what this panel's shape means.
 *
 * On a circle a row has to narrow as it approaches the top or bottom, or its
 * corners fall off the glass. A square has no such constraint, so every row
 * gets the full width and the screens that ask the question -- the browse
 * list, the search results, the status cards -- widen to fill it without
 * knowing anything has changed.
 *
 * That is why those screens are shared with the round build rather than
 * written twice.
 */
namespace ui::theme {

int chordHalfWidth(int y) {
  // The answer does not depend on y, but the signature does: this is the
  // question every layout asks, and a square simply always gives the same
  // reply. Bounds are still honoured so a caller drawing off the panel gets
  // nothing rather than a negative width.
  if (y < 0 || y >= board::kDisplayHeight) {
    return 0;
  }
  return kSize / 2;
}

int usableWidthAt(int y, int inset) {
  const int half = chordHalfWidth(y) - inset;
  return half > 0 ? half * 2 : 0;
}

namespace {

/** The bar's track: down the right-hand edge, level with the rows. */
constexpr int kTrackTop = kListTopY + kListRowGap;
constexpr int kTrackBottom = kSize - kListScrollBarInset;
constexpr int kTrackHeight = kTrackBottom - kTrackTop;
/** Flush against the right-hand edge, with no inset on that side. A list
 *  scroll moves the rows already on the panel but leaves the bar's columns
 *  where they are (its thumb goes the other way); with the bar at the edge
 *  those columns are the end of every row, so each row moves as one run
 *  rather than two either side of it. The vertical inset stays. */
constexpr int kTrackX = kSize - kListScrollBarWidth;
constexpr int kTrackRadius = kListScrollBarWidth / 2;

}  // namespace

void fillListBackdrop(lgfx::LovyanGFX& gfx, int top, int height) {
  // One fill, edge to edge. A square panel has no corners to leave black:
  // rows here are full width, so anything the backdrop does not cover is
  // somewhere a row has already drawn.
  gfx.fillRect(0, top, kSize, height, kListBackdrop);
}

void drawScrollIndicator(lgfx::LovyanGFX& gfx, float offset, float visible) {
  // A straight bar, because there is a straight edge to put it against.
  // The round build curves this around the bezel for want of one.
  gfx.fillRoundRect(kTrackX, kTrackTop, kListScrollBarWidth, kTrackHeight,
                    kTrackRadius, kArcTrack);

  int thumb = static_cast<int>(kTrackHeight * visible);
  if (thumb < kListScrollBarMinLength) {
    thumb = kListScrollBarMinLength;
  }
  if (thumb > kTrackHeight) {
    thumb = kTrackHeight;
  }
  // Clamped after the minimum is applied, so a very long list still puts the
  // thumb against the bottom of the track at the end rather than past it.
  const int travel = kTrackHeight - thumb;
  int top = kTrackTop + static_cast<int>(travel * offset);
  if (top < kTrackTop) {
    top = kTrackTop;
  }
  if (top > kTrackTop + travel) {
    top = kTrackTop + travel;
  }

  gfx.fillRoundRect(kTrackX, top, kListScrollBarWidth, thumb, kTrackRadius,
                    kAccent);
}

int scrollIndicatorTopY() { return kTrackTop; }

bool scrollIndicatorColumn(int& x, int& w) {
  x = kTrackX;
  w = kListScrollBarWidth;
  return true;
}

}  // namespace ui::theme
