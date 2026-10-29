#include "hardware/boot_button.h"

#include <Arduino.h>

#include "board/board.h"

/**
 * The 4B's BOOT button, on the P4's strapping pin, GPIO 35 -- the S3's
 * GPIO 0 under another number. Active low.
 */
namespace hw {
namespace {

bool s_initialised = false;

}  // namespace

void bootButtonInit() {
  if (s_initialised) {
    return;
  }
  pinMode(board::kBootButtonPin, INPUT_PULLUP);
  s_initialised = true;
}

bool bootButtonPressed() {
  return digitalRead(board::kBootButtonPin) == LOW;
}

}  // namespace hw
