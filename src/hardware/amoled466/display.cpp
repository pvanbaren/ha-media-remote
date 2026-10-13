#include "hardware/display.h"

#include <Arduino.h>

#include "board/board.h"
#include "config.h"
#include "log.h"
#include "hardware/display_font.h"

LGFX tft;

namespace {

constexpr int kSide = board::kDisplayWidth;
static_assert(board::kDisplayWidth == board::kDisplayHeight,
              "turning assumes a square panel");
static_assert(kSide % 2 == 0, "the controller takes even windows only");

bool s_blanked = false;
uint8_t s_rotation = 0;

/** Two rows of the glass, built from the frame: the controller takes a window
 *  only at an even row and an even height as well as an even column and
 *  width, and a window one row tall was dropped -- every row of every
 *  present, which left the panel dark. Internal RAM, reused pair after pair. */
uint16_t s_rows[2 * kSide];

/** Down to the even number at or below `v`. */
int evenDown(int v) { return v & ~1; }
/** Up to the even number at or above `v`, no further than the panel. */
int evenUp(int v) {
  v = (v + 1) & ~1;
  return v < kSide ? v : kSide;
}

}  // namespace

void displayInit(uint8_t rotation) {
  s_rotation = rotation & 3;
  LOG_INFO("Board: %s, %d px round, turned %u quarter%s", board::kName,
           board::kDisplayDiameter, static_cast<unsigned>(s_rotation),
           s_rotation == 1 ? "" : "s");
  tft.init();
  // Never turned here: the frame is turned as it is copied out (see
  // displayPresentFrame), because LovyanGFX's AMOLED rotation leaves the
  // panel's column offset where it was and shifts the picture.
  tft.setRotation(0);
  tft.fillScreen(config::kColorBlack);
  tft.setBrightness(board::kDisplayBrightness);
  tft.setTextWrap(false);
  displayFontInit();
}

void displayBlank() {
  if (s_blanked) {
    return;
  }
  // An AMOLED lights only what it shows, so black is already most of the
  // saving; brightness 0 and sleep are the rest.
  tft.fillScreen(config::kColorBlack);
  tft.setBrightness(0);
  tft.sleep();
  s_blanked = true;
  LOG_INFO("Display: blanked (player idle)");
}

void displayWake() {
  if (!s_blanked) {
    return;
  }
  tft.wakeup();
  tft.setBrightness(board::kDisplayBrightness);
  s_blanked = false;
  LOG_INFO("Display: woken");
}

bool displayIsBlanked() { return s_blanked; }

uint8_t displayRotation() { return s_rotation; }

bool displayRotatesItself() { return false; }

void displayPresentFrame(const uint16_t* frame, int x, int y, int w, int h) {
  if (frame == nullptr || w <= 0 || h <= 0) {
    return;
  }
  // The rectangle as it lands on the glass: LovyanGFX's sprite rotation,
  // which is the map hw::mapPoint() undoes for a touch -- (x, y) goes to
  // (W-1-y, x) a quarter turn round, (W-1-x, H-1-y) upside down, and
  // (y, W-1-x) three quarters. Half-open, [x0, x1) by [y0, y1).
  int x0 = x;
  int y0 = y;
  int x1 = x + w;
  int y1 = y + h;
  switch (s_rotation) {
    case 1:
      x0 = kSide - (y + h);
      x1 = kSide - y;
      y0 = x;
      y1 = x + w;
      break;
    case 2:
      x0 = kSide - (x + w);
      x1 = kSide - x;
      y0 = kSide - (y + h);
      y1 = kSide - y;
      break;
    case 3:
      x0 = y;
      x1 = y + h;
      y0 = kSide - (x + w);
      y1 = kSide - x;
      break;
    default:
      break;
  }
  // Widened to even edges, or the controller ignores the window: columns,
  // which LovyanGFX checks, and rows, which it does not.
  x0 = evenDown(x0 < 0 ? 0 : x0);
  y0 = evenDown(y0 < 0 ? 0 : y0);
  x1 = evenUp(x1);
  y1 = evenUp(y1);
  const int gw = x1 - x0;
  if (gw <= 0 || y1 <= y0) {
    return;
  }

  const int last = kSide - 1;
  tft.startWrite();
  // Two rows a window (see s_rows); y0 and y1 are even, so they pair up.
  for (int gy = y0; gy < y1; gy += 2) {
    for (int r = 0; r < 2; ++r) {
      const int row = gy + r;
      uint16_t* out = s_rows + r * gw;
      switch (s_rotation) {
        case 1:
          // Glass (gx, gy) shows frame (gy, W-1-gx): one of the frame's
          // columns, read bottom up.
          for (int i = 0; i < gw; ++i) {
            out[i] = frame[static_cast<size_t>(last - (x0 + i)) * kSide + row];
          }
          break;
        case 2: {
          // Glass (gx, gy) shows frame (W-1-gx, H-1-gy): a row, reversed.
          const uint16_t* from =
              frame + static_cast<size_t>(last - row) * kSide;
          for (int i = 0; i < gw; ++i) {
            out[i] = from[last - (x0 + i)];
          }
          break;
        }
        case 3:
          // Glass (gx, gy) shows frame (W-1-gy, gx): a column, read top down.
          for (int i = 0; i < gw; ++i) {
            out[i] =
                frame[static_cast<size_t>(x0 + i) * kSide + (last - row)];
          }
          break;
        default:
          memcpy(out, frame + static_cast<size_t>(row) * kSide + x0,
                 static_cast<size_t>(gw) * sizeof(uint16_t));
          break;
      }
    }
    tft.pushImage(x0, gy, gw, 2, s_rows);
  }
  tft.endWrite();
}

bool displayScrollFrame(int, int, int, int, int) {
  // The frame lives in the controller's own RAM, behind a bus this firmware
  // only writes. There is nothing here to move.
  return false;
}
