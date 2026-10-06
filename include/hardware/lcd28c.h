#pragma once

#include <cstdint>

/**
 * What the Waveshare 2.8C's panel and touch controller need before they
 * work, beyond the shared RGB output stage: the TCA9554 expander that holds
 * their resets and the panel's chip select, and the ST7701's register init.
 *
 * Order matters and displayInit() keeps it: expanderInit(), panelInit(), then
 * the RGB output stage, then touchReset() before the GT911 is read.
 */
namespace hw::lcd28c {

/** Bring up the I2C bus the expander and the GT911 share, and put the
 *  expander's port in a known state -- the buzzer off above all. False when
 *  the expander does not acknowledge, in which case nothing below works. */
bool expanderInit();

/** Pulse the panel's reset and send the ST7701 its register init over
 *  3-wire SPI, then release the SPI pins. False when the SPI bus could not
 *  be set up. */
bool panelInit();

/** Reset the GT911 with INT held low, which is what puts it at
 *  board::kTouchI2cAddress, then let INT go back to being an input. */
void touchReset();

/**
 * Hold the I2C bus for one whole transaction, reply bytes included, as
 * hw::qualia::BusGuard does on the Qualia and for the same reason: the touch
 * controller is read from its own task and the expander from the loop, and
 * Wire's own lock is let go before the reply is read out of a buffer the
 * next transaction reuses. Made in expanderInit(); does nothing before.
 */
class BusGuard {
 public:
  BusGuard();
  ~BusGuard();
  BusGuard(const BusGuard&) = delete;
  BusGuard& operator=(const BusGuard&) = delete;
};

}  // namespace hw::lcd28c
