#include "hardware/display.h"

#include <Arduino.h>

#include "board/waveshare_s3.h"
#include "config.h"
#include "log.h"
#include "hardware/display_font.h"

LGFX tft;

namespace {
bool s_blanked = false;
}  // namespace

void displayInit() {
  tft.init();
  tft.setRotation(0);
  tft.setBrightness(board::kDisplayBrightness);
  tft.setTextWrap(false);
  displayFontInit();
}

void displayBlank() {
  if (s_blanked) {
    return;
  }
  // Clear before sleeping: on a module whose BL is tied to 3V3 the lamp stays
  // on, and a sleeping controller holds whatever was last scanned out.
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
