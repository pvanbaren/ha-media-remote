#include "hardware/touch_raw.h"

#include <Arduino.h>
#include <Wire.h>

#include "board/board.h"
#include "hardware/lcd28c.h"
#include "log.h"

/**
 * GT911 capacitive touch on the Waveshare 2.8C's shared I2C bus, driven by
 * hand as the Qualia's FT6336 is: there is no LovyanGFX panel here to hang a
 * touch driver off.
 *
 * Goodix registers are sixteen bits wide, big-endian on the wire. The status
 * byte at 0x814E says whether a fresh report is waiting and how many points
 * it holds; the first point follows from 0x8150, little-endian, and the
 * status has to be written back to zero or the controller never offers
 * another.
 */
namespace hw {
namespace {

constexpr uint16_t kRegProductId = 0x8140;
constexpr uint16_t kRegStatus = 0x814E;
constexpr uint16_t kRegPoint1 = 0x8150;
/** Status: a report is ready. The low nibble is the point count. */
constexpr uint8_t kStatusReady = 0x80;
/** A finger on the glass is reported every scan, about every 10 ms; this
 *  long without one and it is taken to have lifted, so that a lift report
 *  missed on the bus cannot leave a press held down for good. */
constexpr unsigned long kReportStaleMs = 150;

bool s_present = false;
/** The last contact, reported again while the controller has nothing newer:
 *  it offers a report only when something changed, and a finger held still
 *  is still a finger. */
bool s_down = false;
int s_x = 0;
int s_y = 0;
unsigned long s_report_ms = 0;

bool readRegs(uint16_t reg, uint8_t* out, size_t len) {
  lcd28c::BusGuard bus;
  Wire.beginTransmission(board::kTouchI2cAddress);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(static_cast<int>(board::kTouchI2cAddress),
                       static_cast<int>(len)) != static_cast<int>(len)) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    out[i] = static_cast<uint8_t>(Wire.read());
  }
  return true;
}

bool writeReg(uint16_t reg, uint8_t value) {
  lcd28c::BusGuard bus;
  Wire.beginTransmission(board::kTouchI2cAddress);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

}  // namespace

bool touchRawInit() {
  // The bus is up already -- the expander brought it there for the panel --
  // and the reset is the expander's too.
  lcd28c::touchReset();

  uint8_t id[4] = {};
  s_present = readRegs(kRegProductId, id, sizeof(id));
  if (!s_present) {
    LOG_ERROR("Touch: no GT911 at 0x%02X -- panel will not answer taps",
              board::kTouchI2cAddress);
    return false;
  }
  // The product id is ASCII, "911" and a NUL.
  LOG_INFO("Touch: GT%c%c%c%c at 0x%02X", id[0], id[1], id[2],
           id[3] != 0 ? id[3] : ' ', board::kTouchI2cAddress);
  writeReg(kRegStatus, 0);
  return true;
}

bool touchReadRaw(int& x, int& y) {
  if (!s_present) {
    return false;
  }
  uint8_t status = 0;
  if (!readRegs(kRegStatus, &status, 1)) {
    return false;
  }
  if ((status & kStatusReady) != 0) {
    s_report_ms = millis();
    const uint8_t points = status & 0x0F;
    uint8_t p[4] = {};
    if (points > 0 && points <= 5 && readRegs(kRegPoint1, p, sizeof(p))) {
      s_x = p[0] | (p[1] << 8);
      s_y = p[2] | (p[3] << 8);
      s_down = s_x < board::kTouchNativeSize && s_y < board::kTouchNativeSize;
    } else {
      s_down = false;  // a report of no points: the finger lifted
    }
    writeReg(kRegStatus, 0);
  } else if (s_down && millis() - s_report_ms > kReportStaleMs) {
    s_down = false;
  }
  if (!s_down) {
    return false;
  }
  x = s_x;
  y = s_y;
  return true;
}

}  // namespace hw
