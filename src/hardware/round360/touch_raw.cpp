#include "hardware/touch_raw.h"

#include <Arduino.h>
#include <Wire.h>

#include "board/board.h"
#include "hardware/display.h"
#include "log.h"

namespace hw {
namespace {

/** What answered on the bus at boot, for the log. A CST816S ignores I2C while
 *  it is in its power-saving state, so silence here is the normal case for an
 *  untouched panel rather than evidence of anything. */
void scanI2cBus() {
  int found = 0;
  for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      LOG_DEBUG("Touch: I2C device at 0x%02X%s", addr,
                    addr == board::kTouchI2cAddress ? " (expected CST816S)"
                                                    : "");
      ++found;
    }
  }
  if (found == 0) {
    LOG_DEBUG(
        "Touch: nothing answered on I2C -- normal for an idle CST816S");
  }
}

}  // namespace

bool touchRawInit() {
  // displayInit() already ran, and with it the controller's own init: the
  // CST816S is attached to the panel in lgfx_config.hpp, so tft.init() brought
  // up I2C and reset it.
  scanI2cBus();
  return true;
}

bool touchReadRaw(int& x, int& y) {
  uint16_t raw_x = 0;
  uint16_t raw_y = 0;
  if (!tft.getTouch(&raw_x, &raw_y)) {
    return false;
  }
  // LovyanGFX has already rescaled the controller's raster to panel pixels.
  x = static_cast<int>(raw_x);
  y = static_cast<int>(raw_y);
  return true;
}

}  // namespace hw
