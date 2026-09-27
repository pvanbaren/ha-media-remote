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
/**
 * The frame is always drawn upright, so drawing coordinates are the buffer's
 * own everywhere in this file.
 *
 * A panel that cannot turn the picture itself -- the Qualia's RGB scan-out --
 * turns it as the frame is copied to the glass, in displayPresentFrame(),
 * rather than by drawing into a turned sprite. It used to be the sprite, and
 * LovyanGFX's turned sprites get anti-aliased text wrong: drawn upside down
 * on the Qualia, every other row of every letter came out black, which read
 * as grey text with the gaps inside letters filled in.
 */

/** Whether the panel can move rows it already shows. Not on a quarter turn
 *  the copy makes itself: the list's rows are then the glass's columns. */
bool s_rows_move = true;

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

/** Clip a rectangle to the panel. False when nothing is left of it. */
bool clipRect(int& x, int& y, int& w, int& h) {
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
  return w > 0 && h > 0;
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
  s_frame.setRotation(0);
  s_rows_move = displayRotatesItself() || (displayRotation() & 1) == 0;
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
  if (!clipRect(x, y, w, h)) {
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
  if (!s_rows_move) {
    return false;  // the caller repaints instead
  }
  if (!displayScrollFrame(y, h, dy, keep_x, keep_w)) {
    return false;
  }
  ++s_panel_writes;
  return true;
}

uint32_t canvasPanelWrites() { return s_panel_writes; }

bool canvasCanScroll() { return s_ready && s_rows_move; }

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
  if (!s_ready || alpha == 0 || !clipRect(x, y, w, h)) {
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

void dimMask(int x, int y, int w, int h, const uint8_t* mask) {
  if (!s_ready || mask == nullptr || w <= 0 || h <= 0) {
    return;
  }
  uint16_t* buffer = frameBuffer();
  if (buffer == nullptr) {
    return;
  }
  for (int my = 0; my < h; ++my) {
    const int py = y + my;
    if (py < 0 || py >= kSide) {
      continue;
    }
    const uint8_t* row = mask + static_cast<size_t>(my) * w;
    uint16_t* line = buffer + static_cast<size_t>(py) * kSide;
    for (int mx = 0; mx < w; ++mx) {
      const int px = x + mx;
      if (px >= 0 && px < kSide) {
        dimPixel(line[px], row[mx]);
      }
    }
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

  for (int row = first; row < last; ++row) {
    dimRow(buffer + static_cast<size_t>(row) * kSide, 0, kSide, alphaAt(row));
  }
}

}  // namespace ui
