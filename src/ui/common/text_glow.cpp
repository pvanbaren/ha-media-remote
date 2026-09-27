#include "ui/text_glow.h"

#include <algorithm>
#include <cstdint>

#include "board/board.h"
#include "ui/canvas.h"

namespace ui::glow {
namespace {

/** Blurred coverage is multiplied by this before use, so the glow stays dark
 *  close to a stroke and fades only towards its edge. */
constexpr int kGain = 3;
/** The darkest the glow gets, out of 255. */
constexpr int kAlpha = 170;

LGFX_Sprite s_mask;
int s_top = 0;
int s_height = 0;

/** A box blur along one line of `n` bytes, `step` apart, in place. */
void blurLine(uint8_t* p, int n, int step, int r, uint8_t* tmp) {
  int sum = 0;
  for (int i = -r; i <= r; ++i) {
    sum += (i >= 0 && i < n) ? p[i * step] : 0;
  }
  const int span = 2 * r + 1;
  for (int i = 0; i < n; ++i) {
    tmp[i] = static_cast<uint8_t>(sum / span);
    const int out = i - r;
    const int in = i + r + 1;
    sum -= (out >= 0) ? p[out * step] : 0;
    sum += (in < n) ? p[in * step] : 0;
  }
  for (int i = 0; i < n; ++i) {
    p[i * step] = tmp[i];
  }
}

/** Two box passes each way, which is close enough to a Gaussian for a halo,
 *  then the gain and the ceiling. */
void blurMask(uint8_t* mask, int w, int h) {
  static uint8_t tmp[std::max(board::kDisplayWidth, board::kDisplayHeight)];
  for (int pass = 0; pass < 2; ++pass) {
    for (int y = 0; y < h; ++y) {
      blurLine(mask + static_cast<size_t>(y) * w, w, 1, kRadius, tmp);
    }
    for (int x = 0; x < w; ++x) {
      blurLine(mask + x, h, w, kRadius, tmp);
    }
  }
  const size_t n = static_cast<size_t>(w) * h;
  for (size_t i = 0; i < n; ++i) {
    const int boosted = std::min(255, mask[i] * kGain);
    mask[i] = static_cast<uint8_t>(boosted * kAlpha / 255);
  }
}

}  // namespace

lgfx::LGFXBase* begin(int top, int height) {
  if (height <= 0) {
    return nullptr;
  }
  // Each screen asks for the same band every time, so this allocates once;
  // a taller band than before is the only thing that makes it again.
  if (s_mask.getBuffer() == nullptr || s_mask.height() < height) {
    s_mask.deleteSprite();
    s_mask.setPsram(true);
    s_mask.setColorDepth(lgfx::color_depth_t::grayscale_8bit);
    if (s_mask.createSprite(theme::kSize, height) == nullptr) {
      return nullptr;
    }
  }
  s_top = top;
  s_height = height;
  s_mask.fillSprite(0);
  return &s_mask;
}

void apply() {
  auto* mask = static_cast<uint8_t*>(s_mask.getBuffer());
  if (mask == nullptr || s_height <= 0) {
    return;
  }
  const int w = s_mask.width();
  blurMask(mask, w, s_height);
  ui::dimMask(0, s_top, w, s_height, mask);
  s_height = 0;
}

}  // namespace ui::glow
