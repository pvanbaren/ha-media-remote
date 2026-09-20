#include "hardware/touch_raw.h"

#include <Arduino.h>
#include <Wire.h>

#include "board/board.h"
#include "hardware/qualia_expander.h"
#include "log.h"

/**
 * FT6336 capacitive touch on the Qualia's shared I2C bus.
 *
 * Driven by hand rather than through LovyanGFX, because on this board
 * LovyanGFX is not driving a panel at all -- there is no LGFX_Device to hang a
 * touch controller off. That is no loss: FocalTech's register map is four
 * bytes of coordinate behind a count.
 *
 * The address is the thing to remember. FocalTech parts are almost always at
 * 0x38, and Adafruit's CircuitPython driver overrides this panel to **0x48**.
 * If touch is dead, check that before anything else.
 */
namespace hw {
namespace {

/** Number of active contacts, 0-2. */
constexpr uint8_t kRegTouchCount = 0x02;
/** First contact: XH, XL, YH, YL. The high bytes carry flags in their top
 *  bits, so both are masked to 4 bits of coordinate. */
constexpr uint8_t kRegTouch1 = 0x03;
constexpr uint8_t kRegChipId = 0xA3;
constexpr uint8_t kRegFirmware = 0xA6;

bool s_present = false;

bool readRegs(uint8_t reg, uint8_t* out, size_t len) {
  // The whole transaction, reply bytes included: see BusGuard.
  qualia::BusGuard bus;
  Wire.beginTransmission(board::kTouchI2cAddress);
  Wire.write(reg);
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

void scanI2cBus() {
  for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() != 0) {
      continue;
    }
    const char* note = "";
    if (addr == board::kTouchI2cAddress) {
      note = " (expected FT6336)";
    } else if (addr == board::kExpanderI2cAddress) {
      note = " (TCA9554 expander)";
    }
    LOG_DEBUG("Touch: I2C device at 0x%02X%s", addr, note);
  }
}

}  // namespace

bool touchRawInit() {
  // The expander already brought Wire up on its way to resetting the panel,
  // and the two share the bus. Re-begin at the faster touch rate: 100 kHz is
  // plenty for a handful of expander bytes, but a coordinate read happens
  // every 16 ms and wants to be brief.
  Wire.begin(static_cast<int>(board::kTouchPinSda),
             static_cast<int>(board::kTouchPinScl), board::kTouchI2cHz);

  // FocalTech parts hold reset for a moment after power-up and answer nothing
  // until they are ready.
  delay(100);

  scanI2cBus();

  uint8_t id[1] = {};
  uint8_t fw[1] = {};
  s_present = readRegs(kRegChipId, id, 1);
  if (!s_present) {
    LOG_ERROR("Touch: no FT6336 at 0x%02X -- panel will not answer taps",
                  board::kTouchI2cAddress);
    return false;
  }
  readRegs(kRegFirmware, fw, 1);
  LOG_INFO("Touch: FT6336 at 0x%02X, chip id 0x%02X, firmware 0x%02X",
                board::kTouchI2cAddress, id[0], fw[0]);
  return true;
}

bool touchReadRaw(int& x, int& y) {
  if (!s_present) {
    return false;
  }

  uint8_t count = 0;
  if (!readRegs(kRegTouchCount, &count, 1) || (count & 0x0F) == 0) {
    return false;
  }

  uint8_t p[4] = {};
  if (!readRegs(kRegTouch1, p, sizeof(p))) {
    return false;
  }
  // Twelve bits of coordinate per axis; the top bits of each high byte are an
  // event flag and a touch id, not position.
  x = ((p[0] & 0x0F) << 8) | p[1];
  y = ((p[2] & 0x0F) << 8) | p[3];

  // The controller reports against its own raster, which on this panel is the
  // panel's own size -- so nothing to rescale, unlike a board where LovyanGFX
  // does it on the way past.
  if (x < 0 || y < 0 || x >= board::kTouchNativeSize ||
      y >= board::kTouchNativeSize) {
    return false;  // a stale or half-written register pair
  }
  return true;
}

}  // namespace hw
