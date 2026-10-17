#include "hardware/boot_button.h"

#include <Arduino.h>

#include "board/board.h"
#include "hardware/guition4848.h"

/**
 * The 4848S040 has no BOOT button to offer: GPIO 0, the S3's strapping pin
 * and the button every other board here reads, is a data line to the panel,
 * red's top bit. Reading it would be reading pixels.
 *
 * So a finger on the glass stands in for it, within limits that keep an
 * ordinary long press well away from it: only a hold begun in the first
 * board::kResetHoldWindowMs after power-up counts, and it reads as pressed
 * only once it has lasted board::kResetHoldLeadMs. services/wifi_setup.cpp
 * then times its own hold on top, as for a button, and needs no notion of
 * which board it is on.
 */
namespace hw {
namespace {

/** millis() when the qualifying hold began; 0 while there is none. */
unsigned long s_held_since = 0;

}  // namespace

void bootButtonInit() {}

bool bootButtonPressed() {
  if (!guition4848::touchHeld()) {
    s_held_since = 0;
    return false;
  }
  const unsigned long now = millis();
  if (s_held_since == 0) {
    if (now > board::kResetHoldWindowMs) {
      return false;  // begun too late to be a reset
    }
    s_held_since = now | 1;  // never 0, which means no hold
  }
  return now - s_held_since >= board::kResetHoldLeadMs;
}

}  // namespace hw
