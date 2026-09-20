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

}  // namespace ui::theme
