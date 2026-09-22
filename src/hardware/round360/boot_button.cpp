#include "hardware/boot_button.h"

#include <Arduino.h>
#include <driver/gpio.h>

namespace hw {
namespace {

/** Not a panel detail, so not in board/: GPIO 0 is the ESP32-S3's strapping
 *  pin and carries the BOOT button on every module that exposes one, with an
 *  onboard pull-up and active LOW. */
constexpr gpio_num_t kBootPin = GPIO_NUM_0;

bool s_initialised = false;

}  // namespace

void bootButtonInit() {
  if (s_initialised) {
    return;
  }
  pinMode(kBootPin, INPUT_PULLUP);
  s_initialised = true;
}

bool bootButtonPressed() { return digitalRead(kBootPin) == LOW; }

}  // namespace hw
