#include "hardware/display.h"

#include <Arduino.h>

#include "board/board.h"
#include "config.h"
#include "hardware/display_font.h"
#include "hardware/matouch21.h"
#include "hardware/rgb_panel.h"
#include "log.h"

/**
 * The display, as the Makerfabs MaTouch 2.1" sees it: the 2.8C's arrangement
 * -- esp_lcd scanning a framebuffer out, `tft` a sprite that is never drawn
 * to, the panel's registers set first, a backlight on a PWM pin -- with no
 * expander in the way.
 */
LGFX tft;

namespace {

bool s_blanked = false;
bool s_up = false;
uint8_t s_rotation = 0;

void backlight(bool on) {
  ledcWrite(static_cast<uint8_t>(board::kDisplayPinBacklight),
            on ? board::kBacklightDuty : 0);
}

}  // namespace

void displayInit(uint8_t rotation) {
  s_rotation = rotation & 3;
  LOG_INFO("Board: %s, %d px round, turned %u quarter%s", board::kName,
           board::kDisplayDiameter, static_cast<unsigned>(s_rotation),
           s_rotation == 1 ? "" : "s");

  // Dark until there is a picture.
  ledcAttach(static_cast<uint8_t>(board::kDisplayPinBacklight),
             board::kBacklightPwmHz, board::kBacklightPwmBits);
  backlight(false);

  if (hw::matouch21::panelInit()) {
    s_up = hw::rgb::rgbInit();
  }
  backlight(s_up);

  displayFontInit();
}

void displayBlank() {
  if (s_blanked) {
    return;
  }
  if (s_up) {
    hw::rgb::rgbFill(0);
  }
  backlight(false);
  s_blanked = true;
  LOG_INFO("Display: blanked (player idle)");
}

void displayWake() {
  if (!s_blanked) {
    return;
  }
  backlight(s_up);
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
