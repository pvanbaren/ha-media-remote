#include "hardware/qualia_expander.h"

#include <Arduino.h>
#include <Wire.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "board/board.h"
#include "log.h"

namespace hw::qualia {
namespace {

/** TCA9554 registers. */
constexpr uint8_t kRegInput = 0x00;
constexpr uint8_t kRegOutput = 0x01;
constexpr uint8_t kRegConfig = 0x03;

/** Shadow of the output port. The expander has no read-modify-write, so the
 *  only way to change one bit without disturbing the others is to remember
 *  what was last written. */
uint8_t s_output = board::kExpanderInitialOutput;
bool s_present = false;

SemaphoreHandle_t s_bus = nullptr;

bool write(uint8_t reg, uint8_t value) {
  BusGuard bus;
  Wire.beginTransmission(board::kExpanderI2cAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool writeOutput(uint8_t value) {
  s_output = value;
  return write(kRegOutput, value);
}

void setBit(uint8_t bit, bool high) {
  const uint8_t mask = static_cast<uint8_t>(1u << bit);
  writeOutput(high ? static_cast<uint8_t>(s_output | mask)
                   : static_cast<uint8_t>(s_output & ~mask));
}

}  // namespace

BusGuard::BusGuard() {
  if (s_bus != nullptr) {
    xSemaphoreTake(s_bus, portMAX_DELAY);
  }
}

BusGuard::~BusGuard() {
  if (s_bus != nullptr) {
    xSemaphoreGive(s_bus);
  }
}

bool expanderInit() {
  if (s_bus == nullptr) {
    s_bus = xSemaphoreCreateMutex();
  }
  Wire.begin(static_cast<int>(board::kExpanderPinSda),
             static_cast<int>(board::kExpanderPinScl),
             board::kExpanderI2cHz);

  Wire.beginTransmission(board::kExpanderI2cAddress);
  if (Wire.endTransmission() != 0) {
    LOG_ERROR("Qualia: TCA9554 not found at 0x%02X -- no panel reset",
                  board::kExpanderI2cAddress);
    s_present = false;
    return false;
  }
  s_present = true;

  // Outputs first, then a known output state, so no line is briefly driven to
  // whatever the port happened to hold.
  writeOutput(board::kExpanderInitialOutput);
  write(kRegConfig, board::kExpanderConfig);

  // Reset pulse. The TL040HDS20 wants nothing else -- no register init -- so
  // this is the whole of the panel's power-on sequence. Held low well past the
  // datasheet's minimum because the cost of being generous here is 20 ms once
  // at boot, and the cost of being mean is a panel that sometimes does not
  // come up.
  //
  // CS is parked high alongside it: the config SPI is unused on this panel,
  // and a floating chip select next to a switching reset line is the kind of
  // thing that works on the bench and not in a case.
  setBit(board::kExpanderBitTftCs, true);
  setBit(board::kExpanderBitTftReset, true);
  delay(10);
  setBit(board::kExpanderBitTftReset, false);
  delay(20);
  setBit(board::kExpanderBitTftReset, true);
  delay(120);

  LOG_INFO("Qualia: TCA9554 up, panel reset released");
  return true;
}

void expanderBacklight(bool on) {
  if (!s_present || !board::kBacklightOnExpander) {
    return;
  }
  setBit(board::kExpanderBitBacklight, on);
}

uint8_t buttonMask() {
  if (!s_present) {
    return 0;
  }
  BusGuard bus;
  Wire.beginTransmission(board::kExpanderI2cAddress);
  Wire.write(kRegInput);
  if (Wire.endTransmission(false) != 0) {
    return 0;
  }
  if (Wire.requestFrom(static_cast<int>(board::kExpanderI2cAddress), 1) != 1) {
    return 0;
  }
  const uint8_t port = static_cast<uint8_t>(Wire.read());

  uint8_t mask = 0;
  if ((port & (1u << board::kExpanderBitButtonUp)) == 0) {
    mask |= kButtonUp;  // active low
  }
  if ((port & (1u << board::kExpanderBitButtonDown)) == 0) {
    mask |= kButtonDown;
  }
  return mask;
}

}  // namespace hw::qualia
