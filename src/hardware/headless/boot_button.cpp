#include "hardware/boot_button.h"

/**
 * No board, so no button.
 *
 * services/wifi_setup.cpp polls this every loop to notice a settings-reset
 * hold, and it is written against no particular hardware -- so rather than
 * teach it that a button might not exist, the headless build supplies one that
 * is never pressed. Clearing the settings on a headless target means erasing
 * NVS over serial, which is the right amount of ceremony for a build with no
 * screen to confirm on.
 */
namespace hw {

void bootButtonInit() {}

bool bootButtonPressed() { return false; }

}  // namespace hw
