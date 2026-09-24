#include "ui/canvas.h"

#include <Arduino.h>

#include <esp_heap_caps.h>

#include <utility>

#include "board/board.h"
#include "config.h"
#include "log.h"
#include "hardware/display.h"
#include "hardware/display_font.h"

namespace ui {
namespace {

LGFX_Sprite s_frame(&tft);
bool s_ready = false;
/** Quarter turns the sprite itself is drawn turned by: the display's rotation
 *  where the panel cannot turn the picture, and 0 where it can. Everything
 *  that touches the buffer directly -- a present, a scroll, a scrim -- maps
 *  its rectangle through this, because LovyanGFX only turns what it draws. */
uint8_t s_turn = 0;

// Turning maps rows onto columns, which only keeps a frame's shape when it is
// square. Every panel here is.
static_assert(board::kDisplayWidth == board::kDisplayHeight,
              "rotation assumes a square panel");
constexpr int kSide = board::kDisplayWidth;
/** See canvasPanelWrites(). */
uint32_t s_panel_writes = 0;
/** True when the buffer stores RGB565 byte-swapped relative to the host. */
bool s_swapped = false;

/** Ask the sprite how it lays a colour out rather than assuming an endianness:
 *  LovyanGFX takes byte order from the panel, which differs by controller. */
void probeByteOrder() {
  auto* buffer = static_cast<uint16_t*>(s_frame.getBuffer());
  if (buffer == nullptr) {
    return;
  }
  constexpr uint16_t kProbe = 0xF800;  // pure red in RGB565
  s_frame.drawPixel(0, 0, kProbe);
  s_swapped = buffer[0] != kProbe;
  s_frame.fillSprite(0);
}

inline uint16_t hostOrder(uint16_t raw) {
  return s_swapped ? static_cast<uint16_t>((raw >> 8) | (raw << 8)) : raw;
}

inline uint16_t bufferOrder(uint16_t rgb565) {
  return s_swapped ? static_cast<uint16_t>((rgb565 >> 8) | (rgb565 << 8))
                   : rgb565;
}

/** Multiply a pixel towards black by `keep`/256. */
inline uint16_t blendToBlack(uint16_t rgb565, uint16_t keep) {
  const uint16_t r = (rgb565 >> 11) & 0x1F;
  const uint16_t g = (rgb565 >> 5) & 0x3F;
  const uint16_t b = rgb565 & 0x1F;
  return static_cast<uint16_t>((((r * keep) >> 8) << 11) |
                               (((g * keep) >> 8) << 5) | ((b * keep) >> 8));
}

inline void dimPixel(uint16_t& pixel, uint8_t alpha) {
  if (alpha != 0) {
    pixel = bufferOrder(blendToBlack(hostOrder(pixel),
                                     static_cast<uint16_t>(256 - alpha)));
  }
}

void dimRow(uint16_t* row, int x, int w, uint8_t alpha) {
  if (alpha == 0) {
    return;
  }
  for (int i = x; i < x + w; ++i) {
    dimPixel(row[i], alpha);
  }
}

/** Clip a rectangle in drawing coordinates to the panel, and map it into the
 *  framebuffer's own. False when nothing is left of it. The forward map of
 *  LovyanGFX's sprite rotation, applied to a rectangle rather than a pixel. */
bool toBuffer(int& x, int& y, int& w, int& h) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > kSide) {
    w = kSide - x;
  }
  if (y + h > kSide) {
    h = kSide - y;
  }
  if (w <= 0 || h <= 0) {
    return false;
  }
  switch (s_turn) {
    case 1: {
      const int bx = kSide - (y + h);
      y = x;
      x = bx;
      std::swap(w, h);
      break;
    }
    case 2:
      x = kSide - (x + w);
      y = kSide - (y + h);
      break;
    case 3: {
      const int by = kSide - (x + w);
      x = y;
      y = by;
      std::swap(w, h);
      break;
    }
    default:
      break;
  }
  return true;
}

uint16_t* frameBuffer() {
  return static_cast<uint16_t*>(s_frame.getBuffer());
}

/** Clamp a row range to the panel. */
bool clipY(int y, int h, int& first, int& last) {
  first = y > 0 ? y : 0;
  last = y + h;
  if (last > board::kDisplayHeight) {
    last = board::kDisplayHeight;
  }
  return last > first;
}

}  // namespace

bool canvasInit() {
  const int width = board::kDisplayWidth;
  const int height = board::kDisplayHeight;

  // Byte order is the board's call, not a default: where the frame is copied
  // straight into a scan-out buffer there is nothing left to convert it.
  if constexpr (board::kFrameNativeByteOrder) {
    s_frame.setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
  } else {
    s_frame.setColorDepth(lgfx::color_depth_t::rgb565_2Byte);
  }
  s_frame.setPsram(true);

  if (s_frame.createSprite(width, height) == nullptr) {
    // 115 KB is nothing against 2 MB of PSRAM, so this means the PSRAM is not
    // there or not working -- a board fault rather than a budget.
    LOG_ERROR("Canvas: no frame buffer, drawing direct (check PSRAM)");
    s_ready = false;
    return false;
  }

  // Probed unturned, so pixel (0, 0) is buffer[0].
  probeByteOrder();
  s_turn = displayRotatesItself() ? 0 : displayRotation();
  s_frame.setRotation(s_turn);
  s_frame.setTextWrap(false);
  displayFontEnsureLoaded(s_frame);
  s_ready = true;

  LOG_INFO("Canvas: %dx%d frame in PSRAM (%u bytes)", width, height,
                static_cast<unsigned>(width * height * 2));
  return true;
}

bool canvasReady() { return s_ready; }

lgfx::LovyanGFX& canvas() {
  // Falling back to the panel keeps every screen drawable when the buffer
  // could not be had; repaints tear, and the scrim helpers below go quiet.
  return s_ready ? static_cast<lgfx::LovyanGFX&>(s_frame)
                 : static_cast<lgfx::LovyanGFX&>(tft);
}

void canvasPresent() {
  canvasPresentRegion(0, 0, board::kDisplayWidth, board::kDisplayHeight);
}

void canvasPresentRegion(int x, int y, int w, int h) {
  if (!s_ready) {
    return;  // drawing went straight to the panel; there is nothing to push
  }
  if (!toBuffer(x, y, w, h)) {
    return;
  }
  ++s_panel_writes;
  displayPresentFrame(static_cast<const uint16_t*>(s_frame.getBuffer()), x, y,
                      w, h);
}

bool canvasScrollPanel(int y, int h, int dy, int keep_x, int keep_w) {
  if (!s_ready) {
    return false;  // drawing goes straight to the panel; there is no frame
  }
  if (s_turn & 1) {
    // A quarter turn puts the list's rows down the framebuffer's columns,
    // and the move shifts rows. The caller repaints instead.
    return false;
  }
  if (s_turn == 2) {
    // Upside down: the same rows from the other end, moving the other way,
    // and the kept columns mirrored.
    y = kSide - (y + h);
    dy = -dy;
    if (keep_w > 0) {
      keep_x = kSide - (keep_x + keep_w);
    }
  }
  if (!displayScrollFrame(y, h, dy, keep_x, keep_w)) {
    return false;
  }
  ++s_panel_writes;
  return true;
}

uint32_t canvasPanelWrites() { return s_panel_writes; }

bool canvasCanScroll() { return s_ready && (s_turn & 1) == 0; }

lgfx::LovyanGFX& panel() {
  // Where a small repaint goes. On a panel with a command channel that is the
  // panel itself, and the frame is left alone. On an RGB panel there is no
  // such channel, so it is the frame -- and the caller follows the draw with
  // canvasPresentRegion() to push just what it touched.
  if (board::kPanelWritesDirect) {
    return tft;
  }
  return canvas();
}

void dim(int x, int y, int w, int h, uint8_t alpha) {
  if (!s_ready || alpha == 0 || !toBuffer(x, y, w, h)) {
    return;
  }
  uint16_t* buffer = frameBuffer();
  if (buffer == nullptr) {
    return;
  }
  for (int row = y; row < y + h; ++row) {
    dimRow(buffer + static_cast<size_t>(row) * kSide, x, w, alpha);
  }
}

void dimGradient(int y, int h, uint8_t alpha_top, uint8_t alpha_bottom) {
  if (!s_ready || h <= 0) {
    return;
  }
  uint16_t* buffer = frameBuffer();
  if (buffer == nullptr) {
    return;
  }
  int first = 0;
  int last = 0;
  if (!clipY(y, h, first, last)) {
    return;
  }

  const int span = h > 1 ? h - 1 : 1;
  auto alphaAt = [&](int row) {
    return static_cast<uint8_t>(alpha_top +
                                ((alpha_bottom - alpha_top) * (row - y)) /
                                    span);
  };

  if ((s_turn & 1) == 0) {
    // The ramp runs down the framebuffer's rows: from the top unturned, from
    // the bottom upside down.
    for (int row = first; row < last; ++row) {
      const int at = s_turn == 2 ? kSide - 1 - row : row;
      dimRow(buffer + static_cast<size_t>(at) * kSide, 0, kSide, alphaAt(row));
    }
    return;
  }

  // A quarter turn: the drawing's rows are the buffer's columns, so the ramp
  // runs across each buffer row. Walked row by row all the same, which is
  // the order the memory is in; alphas are worked out once per column.
  uint8_t alphas[kSide];
  int col_first = 0;
  int col_last = 0;
  for (int row = first; row < last; ++row) {
    const int col = s_turn == 1 ? kSide - 1 - row : row;
    alphas[col] = alphaAt(row);
  }
  if (s_turn == 1) {
    col_first = kSide - last;
    col_last = kSide - first;
  } else {
    col_first = first;
    col_last = last;
  }
  for (int row = 0; row < kSide; ++row) {
    uint16_t* line = buffer + static_cast<size_t>(row) * kSide;
    for (int col = col_first; col < col_last; ++col) {
      dimPixel(line[col], alphas[col]);
    }
  }
}

}  // namespace ui
