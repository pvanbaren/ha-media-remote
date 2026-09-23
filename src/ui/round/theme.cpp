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

namespace {

// 0 degrees is 3 o'clock in LovyanGFX, so this spans the right-hand
// quadrant either side of it.
constexpr float kTrackStartDeg = -55.0f;
constexpr float kTrackEndDeg = 55.0f;
constexpr float kTrackSpan = kTrackEndDeg - kTrackStartDeg;

}  // namespace

void fillListBackdrop(lgfx::LovyanGFX& gfx, int top, int height) {
  // Black first, then the circle over it. Rows are clipped to the chord
  // here, so nothing ever draws in the corners -- but they are part of the
  // sprite and something has to be in them.
  gfx.setClipRect(0, top, kSize, height);
  gfx.fillRect(0, top, kSize, height, kBackground);
  gfx.fillCircle(kCenterX, kCenterY, kRadius, kListBackdrop);
  gfx.clearClipRect();
}

void drawScrollIndicator(lgfx::LovyanGFX& gfx, float offset, float visible) {
  // A short arc on the bezel. There is no straight edge on this panel to
  // hang a bar from, and a bar inset far enough to stay on the glass would
  // cut across the rows instead of bounding them.
  gfx.fillArc(kCenterX, kCenterY, kListScrollInnerRadius,
              kListScrollOuterRadius, kTrackStartDeg, kTrackEndDeg,
              kArcTrack);

  const float span = kTrackSpan * visible;
  const float start = kTrackStartDeg + (kTrackSpan - span) * offset;
  gfx.fillArc(kCenterX, kCenterY, kListScrollInnerRadius,
              kListScrollOuterRadius, start, start + span, kAccent);
}

int scrollIndicatorTopY() {
  // The arc's highest point: sin(55 deg) of the outer radius above centre.
  return kCenterY - static_cast<int>(kListScrollOuterRadius *
                                     sinf(kTrackEndDeg * 3.14159265f / 180.0f));
}

bool scrollIndicatorColumn(int&, int&) {
  // An arc round the bezel, whose bounding box is most of the panel's right
  // half. And the rows change width with height here, so a moved row is not
  // the row that belongs there anyway.
  return false;
}

}  // namespace ui::theme
