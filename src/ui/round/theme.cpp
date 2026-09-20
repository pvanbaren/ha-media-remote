#include "ui/theme.h"

#include <cmath>

namespace ui::theme {

int chordHalfWidth(int y) {
  const float dy = static_cast<float>(y - kCenterY);
  const float r = static_cast<float>(kRadius);
  const float inside = r * r - dy * dy;
  if (inside <= 0.0f) {
    return 0;
  }
  return static_cast<int>(sqrtf(inside));
}

int usableWidthAt(int y, int inset) {
  const int half = chordHalfWidth(y) - inset;
  return half > 0 ? half * 2 : 0;
}

void fillListBackdrop(lgfx::LovyanGFX& gfx, int top, int height) {
  // Black first, then the circle over it. Rows are clipped to the chord
  // here, so nothing ever draws in the corners -- but they are part of the
  // sprite and something has to be in them.
  gfx.setClipRect(0, top, kSize, height);
  gfx.fillRect(0, top, kSize, height, kBackground);
  gfx.fillCircle(kCenterX, kCenterY, kRadius, kListBackdrop);
  gfx.clearClipRect();
}

}  // namespace ui::theme
