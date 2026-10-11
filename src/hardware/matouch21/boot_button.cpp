#include "hardware/boot_button.h"

#include <Arduino.h>
#include <driver/gpio.h>

namespace hw {
namespace {

/** GPIO 0, the S3's strapping pin and the board's flash button: active low
 *  with a pull-up. It is also the ST7701 init's data line, which hands it
 *  back as an input once the panel is set up (hw::matouch21::panelInit()). */
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
