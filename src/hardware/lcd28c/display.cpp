#include "hardware/display.h"

#include <Arduino.h>

#include "board/board.h"
#include "config.h"
#include "hardware/display_font.h"
#include "hardware/lcd28c.h"
#include "hardware/rgb_panel.h"
#include "log.h"

/**
 * The display, as the Waveshare 2.8C sees it: the Qualia's arrangement --
 * esp_lcd scanning a framebuffer out, `tft` a sprite that is never drawn
 * to -- with a round panel that has to be told its registers first, and a
 * backlight on a PWM pin rather than the expander.
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

  // Dark until there is a picture: the lamp lit over a panel still being
  // set up shows whatever its RAM powered up holding.
  ledcAttach(static_cast<uint8_t>(board::kDisplayPinBacklight),
             board::kBacklightPwmHz, board::kBacklightPwmBits);
  backlight(false);

  // The panel's registers before its pixels: it ignores the RGB stream until
  // it has been told how to take it.
  if (hw::lcd28c::expanderInit() && hw::lcd28c::panelInit()) {
    s_up = hw::rgb::rgbInit();
  }
  backlight(s_up);

  displayFontInit();
}

void displayBlank() {
  if (s_blanked) {
    return;
  }
  // As on the Qualia: no controller to put to sleep, so a black frame and the
  // lamp cut.
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
