#include "hardware/battery.h"

#include "board/board.h"

#if defined(BOARD_HAS_AXP2101)

#include <Arduino.h>
#include <Wire.h>

#include "log.h"

/**
 * The AXP2101, read over the touch controller's I2C bus with the same Wire,
 * which touchRawInit() starts. Registers as XPowersLib (MIT) reads them, the
 * library Waveshare's examples drive this board's power manager with.
 */
namespace hw {
namespace {

/** Bit 3: a battery is present. */
constexpr uint8_t kRegStatus1 = 0x00;
/** Bits 6:5: 01 charging, 10 discharging, 00 standby. */
constexpr uint8_t kRegStatus2 = 0x01;
/** Bit 3: the fuel gauge, which fills kRegPercent, is on. */
constexpr uint8_t kRegGaugeCtrl = 0x18;
constexpr uint8_t kGaugeOn = 0x08;
constexpr uint8_t kRegPercent = 0xA4;

bool s_ready = false;

/** One register, or -1 when the power manager does not answer. */
int readReg(uint8_t reg) {
  Wire.beginTransmission(board::kPmuI2cAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(static_cast<int>(board::kPmuI2cAddress), 1) != 1) {
    return -1;
  }
  return Wire.read();
}

bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(board::kPmuI2cAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

}  // namespace

bool batteryInit() {
  if (s_ready) {
    return true;
  }
  const int gauge = readReg(kRegGaugeCtrl);
  if (gauge < 0) {
    LOG_WARN("Battery: no AXP2101 at 0x%02X", board::kPmuI2cAddress);
    return false;
  }
  if ((gauge & kGaugeOn) == 0 &&
      !writeReg(kRegGaugeCtrl, static_cast<uint8_t>(gauge | kGaugeOn))) {
    LOG_WARN("Battery: could not turn the fuel gauge on");
  }
  s_ready = true;
  const int pct = batteryPercent();
  if (pct < 0) {
    LOG_INFO("Battery: none reported (status 0x%02X)", readReg(kRegStatus1));
  } else {
    LOG_INFO("Battery: %d%%%s", pct, batteryCharging() ? ", charging" : "");
  }
  return true;
}

int batteryPercent() {
  if (!s_ready) {
    return -1;
  }
  const int status = readReg(kRegStatus1);
  if (status < 0 || (status & 0x08) == 0) {
    return -1;
  }
  const int pct = readReg(kRegPercent);
  return pct >= 0 && pct <= 100 ? pct : -1;
}

bool batteryCharging() {
  const int status = s_ready ? readReg(kRegStatus2) : -1;
  return status >= 0 && (status >> 5 & 0x03) == 0x01;
}

}  // namespace hw

#else  // no power manager on this board

namespace hw {

bool batteryInit() { return false; }
int batteryPercent() { return -1; }
bool batteryCharging() { return false; }

}  // namespace hw

#endif
