#include "hardware/touch_raw.h"

#include <Arduino.h>

#include "board/board.h"
#include "hardware/i2c_bus.h"
#include "log.h"

/**
 * CST9217 capacitive touch, read by hand over I2C.
 *
 * The Hynitron protocol LovyanGFX's Touch_CST226 speaks -- 16-bit register
 * addresses, a report at 0xD000 acknowledged by writing 0xAB back -- but that
 * driver accepts only the CST226's chip id, so this is the few bytes it would
 * have done, after SensorLib's TouchDrvCST92xx, which Waveshare's examples
 * use for this board.
 */
namespace hw {
namespace {

constexpr uint16_t kRegReport = 0xD000;
constexpr uint16_t kRegCommandMode = 0xD101;
constexpr uint16_t kRegNormalMode = 0xD109;
constexpr uint16_t kRegResolution = 0xD1F8;
constexpr uint16_t kRegChipType = 0xD204;
constexpr uint8_t kAck = 0xAB;
/** A report: the first point's five bytes, then a count and the 0xAB that
 *  says it is whole, then the second point's. One point is all a remote
 *  needs, but the report is read whole so the controller is done with it. */
constexpr size_t kReportBytes = 15;
/** A finger that is down, as opposed to one just lifted. */
constexpr uint8_t kEventDown = 0x06;

bool s_present = false;
int s_raster_x = board::kTouchNativeSize;
int s_raster_y = board::kTouchNativeSize;

/** Set by the controller's interrupt line, which falls when it has a report.
 *  Polled blind, the controller went unanswered about once a second -- busy
 *  or asleep between touches -- and the I2C driver logged every miss as an
 *  error. So it is read only after an edge, and then for as long as a finger
 *  stays down, so a held touch and its lifting are still seen whether or not
 *  the line pulses for every report. */
volatile bool s_irq = false;
bool s_down = false;
bool s_irq_logged = false;

void IRAM_ATTR onTouchIrq() { s_irq = true; }

/** Its registers have 16-bit addresses, high byte first. */
bool writeReg(uint16_t reg, const uint8_t* data = nullptr, size_t len = 0) {
  uint8_t buf[2 + 4] = {static_cast<uint8_t>(reg >> 8),
                        static_cast<uint8_t>(reg & 0xFF)};
  if (len > sizeof(buf) - 2) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    buf[2 + i] = data[i];
  }
  return i2c::write(board::kTouchI2cAddress, buf, 2 + len);
}

bool readReg(uint16_t reg, uint8_t* out, size_t len) {
  const uint8_t addr[2] = {static_cast<uint8_t>(reg >> 8),
                           static_cast<uint8_t>(reg & 0xFF)};
  return i2c::writeRead(board::kTouchI2cAddress, addr, sizeof(addr), out,
                        len);
}

void reset() {
  pinMode(board::kTouchPinRst, OUTPUT);
  digitalWrite(board::kTouchPinRst, LOW);
  delay(10);
  digitalWrite(board::kTouchPinRst, HIGH);
  // Out of its boot code and answering, per SensorLib.
  delay(50);
}

}  // namespace

bool touchRawInit() {
  pinMode(board::kTouchPinInt, INPUT_PULLUP);
  // The bus the codecs, the power manager and the rest share: started here,
  // by the first of them to come up, and by nothing else (hardware/i2c_bus.h).
  if (!i2c::begin(static_cast<int>(board::kTouchPinSda),
                  static_cast<int>(board::kTouchPinScl), board::kTouchI2cHz)) {
    return false;
  }
  reset();

  // Who it is and what raster it reports against, which it only says in
  // command mode.
  uint8_t info[4] = {};
  uint16_t chip = 0;
  {
    // One sequence: nothing else on the bus between entering command mode
    // and leaving it.
    i2c::Lock lock;
    if (writeReg(kRegCommandMode)) {
      delay(10);
      if (readReg(kRegChipType, info, sizeof(info))) {
        chip = static_cast<uint16_t>(info[3] << 8 | info[2]);
      }
      if (readReg(kRegResolution, info, sizeof(info))) {
        const int rx = info[1] << 8 | info[0];
        const int ry = info[3] << 8 | info[2];
        if (rx > 0 && ry > 0) {
          s_raster_x = rx;
          s_raster_y = ry;
        }
      }
      writeReg(kRegNormalMode);
    }
  }
  if (chip == 0) {
    LOG_ERROR("Touch: no CST92xx at 0x%02X -- panel will not answer taps",
              board::kTouchI2cAddress);
    return false;
  }
  // Out of command mode by a reset as well as by asking: SensorLib's own
  // examples reset after reading these.
  reset();
  s_present = true;
  attachInterrupt(digitalPinToInterrupt(board::kTouchPinInt), onTouchIrq,
                  FALLING);
  LOG_INFO("Touch: CST%04X at 0x%02X, %dx%d", chip, board::kTouchI2cAddress,
           s_raster_x, s_raster_y);
  return true;
}

bool touchReadRaw(int& x, int& y) {
  if (!s_present || (!s_irq && !s_down)) {
    return false;
  }
  if (s_irq && !s_irq_logged) {
    s_irq_logged = true;
    LOG_INFO("Touch: interrupt line answering");
  }
  s_irq = false;
  // Down until a report says otherwise; a miss or a half report reads as a
  // lift, as it always did, and the next edge starts over.
  s_down = false;
  uint8_t r[kReportBytes] = {};
  {
    // The report and its acknowledgement as one sequence.
    i2c::Lock lock;
    if (!readReg(kRegReport, r, sizeof(r))) {
      return false;
    }
    // Acknowledge, or the controller keeps this report and sends no more.
    writeReg(kRegReport, &kAck, 1);
  }

  if (r[0] == 0x00 || r[0] == kAck || r[6] != kAck) {
    return false;  // no report, or half of one
  }
  const uint8_t points = r[5] & 0x7F;
  if (points == 0 || (r[0] & 0x0F) != kEventDown) {
    return false;
  }
  s_down = true;
  int rx = (r[1] << 4) | (r[3] >> 4);
  int ry = (r[2] << 4) | (r[3] & 0x0F);
  if (s_raster_x != board::kDisplayWidth) {
    rx = rx * board::kDisplayWidth / s_raster_x;
  }
  if (s_raster_y != board::kDisplayHeight) {
    ry = ry * board::kDisplayHeight / s_raster_y;
  }
  if (rx < 0 || ry < 0 || rx >= board::kDisplayWidth ||
      ry >= board::kDisplayHeight) {
    return false;
  }
  x = rx;
  y = ry;
  return true;
}

}  // namespace hw
