#pragma once

/**
 * The one part of touch that depends on the board.
 *
 * Everything else -- telling a tap from a swipe, tracking a drag before the
 * gesture resolves, the slop and timing thresholds -- is the same on any
 * panel, and lives in hardware/touch_gestures.cpp. All a board has to supply
 * is whether a finger is on the glass and where.
 */
namespace hw {

/** Bring up whatever reads the controller. Called once by touchInit(), after
 *  the display is up. False on a board built without touch. */
bool touchRawInit();

/** Current contact, in display pixels before the orientation fixes. False
 *  when nothing is touching. */
bool touchReadRaw(int& x, int& y);

}  // namespace hw
