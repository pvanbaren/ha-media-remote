#pragma once

/**
 * The battery, as a board's AXP2101 power manager reports it, on a board that
 * has one (BOARD_HAS_AXP2101 in its header).
 *
 * Like encoder.h this is readings, not meaning. On a board without a power
 * manager every call is a harmless no-op: batteryInit() says false, there is
 * never a battery, and nothing ever charges.
 */
namespace hw {

/** Idempotent; after touchInit(), which starts the bus the power manager is
 *  on. Turns the fuel gauge on if it is off and logs the first reading. False
 *  on a board without a power manager. */
bool batteryInit();

/** Charge in percent, 0 to 100, from the power manager's fuel gauge; -1 with
 *  no power manager, no battery, or no answer. Reads the bus each call, so
 *  call it from the loop, as touch is read. */
int batteryPercent();

/** True while the battery is charging. */
bool batteryCharging();

}  // namespace hw
