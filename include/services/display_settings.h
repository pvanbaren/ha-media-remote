#pragma once

#include <cstdint>

namespace services::display {

/** How far the picture is turned, in quarter turns clockwise: 0 upright,
 *  1 for +90 degrees, 2 for 180, 3 for -90. Read from NVS on first use, so it
 *  is ready before the display comes up. */
uint8_t rotation();

/** Store a new rotation. It takes effect at the next boot: the framebuffers,
 *  the touch mapping and every screen are set up against it once. */
void saveRotation(uint8_t quarter_turns);

/** "0", "+90", "180", "-90": what the portal and the log call it. */
const char* rotationLabel(uint8_t quarter_turns);

}  // namespace services::display
