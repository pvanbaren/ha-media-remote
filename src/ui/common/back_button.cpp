#include "ui/back_button.h"

#include <cstdlib>

#include "ui/theme.h"

namespace ui::back_button {
namespace {

/** The title row the lists and the keyboard share. */
constexpr int kY = theme::kSearchQueryY;
constexpr int kRadius = theme::px(11);
/** Generous, as a tap target on glass should be, but short of the first
 *  row of a list below it. */
constexpr int kHitRadius = theme::px(22);
/** Between the bezel, or the panel's edge, and the button. */
constexpr int kEdgeGap = theme::px(6);
/** Between the button and a line beside it. */
constexpr int kTextGap = theme::px(4);

int centerX() {
  return theme::kCenterX - theme::chordHalfWidth(kY) + kEdgeGap + kRadius;
}

}  // namespace

void draw(lgfx::LovyanGFX& gfx) {
  const int cx = centerX();
  gfx.fillSmoothCircle(cx, kY, kRadius, theme::kSurfaceRaised);
  // A chevron pointing left, a little left of centre so it looks centred.
  const int arm = kRadius / 2;
  const int tip = cx - arm / 2 - 1;
  const float half_width = theme::px(2) * 0.75f;
  gfx.drawWideLine(tip + arm, kY - arm, tip, kY, half_width,
                   theme::kTextPrimary);
  gfx.drawWideLine(tip, kY, tip + arm, kY + arm, half_width,
                   theme::kTextPrimary);
}

bool hit(int x, int y) {
  const int dx = x - centerX();
  const int dy = y - kY;
  return dx * dx + dy * dy <= kHitRadius * kHitRadius;
}

void lineSpan(int y, int text_h, int& center_x, int& width) {
  const int half = theme::usableWidthAt(y, theme::kTextEdgeInset) / 2;
  int left = theme::kCenterX - half;
  const int right = theme::kCenterX + half;
  if (abs(y - kY) < text_h / 2 + kRadius) {
    // Beside the button: what is left of the row to its right, centred
    // there -- off the panel's centre a little, but given all the room.
    const int clear = centerX() + kRadius + kTextGap;
    left = clear > left ? clear : left;
  }
  center_x = (left + right) / 2;
  width = right > left ? right - left : 0;
}

}  // namespace ui::back_button
