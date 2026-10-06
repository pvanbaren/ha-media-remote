#include "hardware/display.h"

#include <Arduino.h>

#include "board/board.h"
#include "config.h"
#include "log.h"
#include "hardware/display_font.h"
#include "hardware/qualia_expander.h"
#include "hardware/rgb_panel.h"

/**
 * The display, as the Qualia sees it.
 *
 * `tft` is declared by hardware/display.h and named by shared UI code, but on
 * this board it is a never-created sprite: an RGB panel has no command channel
 * for LovyanGFX to drive. All of the output happens through esp_lcd, and the
 * composed frame reaches the glass in displayPresentFrame().
 */
LGFX tft;

namespace {

bool s_blanked = false;
bool s_up = false;
uint8_t s_rotation = 0;

}  // namespace

void displayInit(uint8_t rotation) {
  s_rotation = rotation & 3;
  LOG_INFO("Board: %s, %dx%d square, turned %u quarter%s",
                board::kName, board::kDisplayWidth, board::kDisplayHeight,
                static_cast<unsigned>(s_rotation), s_rotation == 1 ? "" : "s");

  // Order matters: the panel wants its reset released before the peripheral
  // starts clocking pixels at it.
  hw::qualia::expanderInit();
  s_up = hw::rgb::rgbInit();
  hw::qualia::expanderBacklight(true);

  displayFontInit();
}

void displayBlank() {
  if (s_blanked) {
    return;
  }
  // There is no controller to put to sleep -- the peripheral streams whatever
  // is in the framebuffer, forever. Blanking is therefore literally blanking:
  // fill the buffer with black and, where the board lets us, cut the lamp.
  //
  // The scan-out keeps running, so this saves the backlight rather than the
  // LCD peripheral. On a mains-powered wall remote that is the part that
  // matters, and it is what makes the room go dark at night.
  if (s_up) {
    hw::rgb::rgbFill(0);
  }
  hw::qualia::expanderBacklight(false);
  s_blanked = true;
  LOG_INFO("Display: blanked (player idle)");
}

void displayWake() {
  if (!s_blanked) {
    return;
  }
  hw::qualia::expanderBacklight(true);
  s_blanked = false;
  LOG_INFO("Display: woken");
}

bool displayIsBlanked() { return s_blanked; }

uint8_t displayRotation() { return s_rotation; }

// The scan-out order is fixed, so the frame is turned as it is copied in.
bool displayRotatesItself() { return false; }

void displayPresentFrame(const uint16_t* frame, int x, int y, int w, int h) {
  if (!s_up) {
    return;
  }
  hw::rgb::rgbPresent(frame, x, y, w, h, s_rotation);
}

bool displayScrollFrame(int y, int h, int dy, int keep_x, int keep_w) {
  // Nothing to move while blanked: the buffer holds black, not the frame.
  if (!s_up || s_blanked) {
    return false;
  }
  switch (s_rotation) {
    case 0:
      break;
    case 2:
      // Upside down: the same rows from the other end, moving the other way,
      // and the kept columns mirrored.
      y = board::kDisplayHeight - (y + h);
      dy = -dy;
      if (keep_w > 0) {
        keep_x = board::kDisplayWidth - (keep_x + keep_w);
      }
      break;
    default:
      return false;  // a quarter turn: the rows are the glass's columns
  }
  return hw::rgb::rgbScroll(y, h, dy, keep_x, keep_w);
}
