#include "hardware/display.h"

#include <Arduino.h>

#include "board/board.h"
#include "config.h"
#include "log.h"
#include "hardware/display_font.h"

LGFX tft;

namespace {
bool s_blanked = false;
}  // namespace

void displayInit() {
  LOG_INFO("Board: %s, %d px round", board::kName,
                board::kDisplayDiameter);
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

void displayPresentFrame(const uint16_t* frame, int x, int y, int w, int h) {
  if (frame == nullptr || w <= 0 || h <= 0) {
    return;
  }
  // Row at a time, because `frame` is the whole screen and pushImage wants the
  // rectangle packed. One startWrite around the lot keeps it to a single SPI
  // transaction rather than one per row.
  tft.startWrite();
  for (int row = 0; row < h; ++row) {
    tft.pushImage(x, y + row, w, 1,
                  frame + static_cast<size_t>(y + row) * board::kDisplayWidth +
                      x);
  }
  tft.endWrite();
}

bool displayScrollFrame(int, int, int, int, int) {
  // The frame lives in the controller's own RAM, on the far side of a bus
  // this firmware only writes. There is nothing here to move.
  return false;
}
