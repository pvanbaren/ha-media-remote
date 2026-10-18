#include "ui/cover_colour.h"

#include <cstddef>

namespace ui::cover_colour {
namespace {

/** Samples a side: 1024 in all, whatever the cover's size. */
constexpr int kSampleSide = 32;
/** Hues, 15 degrees each. */
constexpr int kHues = 24;
/** Below this chroma a pixel is grey, white or black, and has no hue. */
constexpr int kMinChroma = 48;
/** Darker than this and a pixel's hue is noise. */
constexpr int kMinBrightest = 56;
/** Per mille of the samples that must be coloured for the cover to have a
 *  colour at all. */
constexpr int kMinColouredPermille = 40;
/** The least saturation a highlight is given, as chroma over brightest: a
 *  pale cover's colour would otherwise come out next to white. */
constexpr int kMinSaturationPercent = 50;

/** 8-bit channels out of a stored RGB565 pixel. */
void channels(uint16_t stored, bool swapped, int& r, int& g, int& b) {
  const uint16_t v =
      swapped ? static_cast<uint16_t>((stored >> 8) | (stored << 8)) : stored;
  r = ((v >> 11) * 255 + 15) / 31;
  g = (((v >> 5) & 0x3F) * 255 + 31) / 63;
  b = ((v & 0x1F) * 255 + 15) / 31;
}

/** Which of the kHues a coloured pixel's hue falls in. */
int hueOf(int r, int g, int b, int brightest, int chroma) {
  int hue;
  if (brightest == r) {
    hue = 60 * (g - b) / chroma;
  } else if (brightest == g) {
    hue = 120 + 60 * (b - r) / chroma;
  } else {
    hue = 240 + 60 * (r - g) / chroma;
  }
  if (hue < 0) {
    hue += 360;
  }
  return hue * kHues / 360 % kHues;
}

/** The averaged colour as a highlight: saturated to at least
 *  kMinSaturationPercent, then scaled so its brightest channel is full. The
 *  hue stays what the cover's was. */
uint16_t asHighlight(int r, int g, int b) {
  const int brightest = r > g ? (r > b ? r : b) : (g > b ? g : b);
  const int darkest = r < g ? (r < b ? r : b) : (g < b ? g : b);
  if (brightest <= 0) {
    return 0;
  }
  const int chroma = brightest - darkest;
  const int wanted = brightest * kMinSaturationPercent / 100;
  if (chroma > 0 && chroma < wanted) {
    // Each channel's distance below the brightest, stretched until the
    // darkest is `wanted` below it.
    r = brightest - (brightest - r) * wanted / chroma;
    g = brightest - (brightest - g) * wanted / chroma;
    b = brightest - (brightest - b) * wanted / chroma;
  }
  r = r * 255 / brightest;
  g = g * 255 / brightest;
  b = b * 255 / brightest;
  return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

}  // namespace

bool pick(const uint16_t* pixels, int stride, int x, int y, int w, int h,
          bool swapped, uint16_t& colour) {
  if (pixels == nullptr || w <= 0 || h <= 0) {
    return false;
  }
  uint32_t weight[kHues] = {};
  uint32_t sum_r[kHues] = {};
  uint32_t sum_g[kHues] = {};
  uint32_t sum_b[kHues] = {};
  const int columns = w < kSampleSide ? w : kSampleSide;
  const int rows = h < kSampleSide ? h : kSampleSide;
  int coloured = 0;
  for (int j = 0; j < rows; ++j) {
    // The middle of each of `rows` equal bands, and of each column's.
    const int sy = y + (j * 2 + 1) * h / (rows * 2);
    const uint16_t* line = pixels + static_cast<size_t>(sy) * stride;
    for (int i = 0; i < columns; ++i) {
      const int sx = x + (i * 2 + 1) * w / (columns * 2);
      int r, g, b;
      channels(line[sx], swapped, r, g, b);
      const int brightest = r > g ? (r > b ? r : b) : (g > b ? g : b);
      const int darkest = r < g ? (r < b ? r : b) : (g < b ? g : b);
      const int chroma = brightest - darkest;
      if (chroma < kMinChroma || brightest < kMinBrightest) {
        continue;
      }
      ++coloured;
      // Weighed by chroma, so large and saturated areas win.
      const int hue = hueOf(r, g, b, brightest, chroma);
      weight[hue] += static_cast<uint32_t>(chroma);
      sum_r[hue] += static_cast<uint32_t>(r * chroma);
      sum_g[hue] += static_cast<uint32_t>(g * chroma);
      sum_b[hue] += static_cast<uint32_t>(b * chroma);
    }
  }
  if (coloured * 1000 < rows * columns * kMinColouredPermille ||
      coloured == 0) {
    return false;
  }

  // The hue with the most weight counting its two neighbours, since a colour
  // can straddle a border between two.
  int best = 0;
  uint32_t best_weight = 0;
  for (int k = 0; k < kHues; ++k) {
    const uint32_t around = weight[(k + kHues - 1) % kHues] + weight[k] +
                            weight[(k + 1) % kHues];
    if (around > best_weight) {
      best_weight = around;
      best = k;
    }
  }
  uint64_t total = 0, r = 0, g = 0, b = 0;
  for (int d = -1; d <= 1; ++d) {
    const int k = (best + kHues + d) % kHues;
    total += weight[k];
    r += sum_r[k];
    g += sum_g[k];
    b += sum_b[k];
  }
  if (total == 0) {
    return false;
  }
  colour = asHighlight(static_cast<int>(r / total), static_cast<int>(g / total),
                       static_cast<int>(b / total));
  return true;
}

}  // namespace ui::cover_colour
