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

void fillListBackdrop(lgfx::LovyanGFX& gfx, int top, int height) {
  // One fill, edge to edge. A square panel has no corners to leave black:
  // rows here are full width, so anything the backdrop does not cover is
  // somewhere a row has already drawn.
  gfx.fillRect(0, top, kSize, height, kListBackdrop);
}

}  // namespace ui::theme
