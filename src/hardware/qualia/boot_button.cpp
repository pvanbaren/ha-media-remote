#include "hardware/boot_button.h"

#include "hardware/qualia_expander.h"

/**
 * The Qualia has no BOOT button to offer.
 *
 * GPIO 0 -- the S3's strapping pin, and the button every other board in this
 * tree reads -- is an RGB data line here, carrying blue bit 3 to the panel.
 * Reading it would be reading pixels.
 *
 * The board does have two user buttons, on the same I2C expander as the
 * panel's reset line. DOWN stands in for BOOT: holding it clears the Wi-Fi and
 * Home Assistant settings exactly as a BOOT hold does elsewhere, so
 * services/wifi_setup.cpp needs no notion of which board it is on.
 */
namespace hw {

void bootButtonInit() {
  // Nothing to do: the expander is already up, brought there by displayInit()
  // on its way to resetting the panel.
}

bool bootButtonPressed() {
  return (qualia::buttonMask() & qualia::kButtonDown) != 0;
}

}  // namespace hw
