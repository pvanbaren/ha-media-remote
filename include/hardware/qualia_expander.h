#pragma once

#include <cstdint>

/**
 * The TCA9554 I2C expander on the Adafruit Qualia.
 *
 * An RGB-parallel panel has no command channel -- the LCD peripheral only
 * streams pixels -- so everything that would normally be a register write or a
 * GPIO lives here instead: the panel's reset line, its bit-banged config SPI,
 * the backlight, and the two user buttons.
 *
 * The 4" square TL040HDS20 needs no register programming at all, unlike the
 * round NV3052C on the same board: Adafruit's CircuitPython driver for it
 * ships an empty init sequence. So the only thing the panel wants from this
 * expander is a clean reset pulse before pixels start.
 */
namespace hw::qualia {

/** Bring up I2C, take the expander to a known state and pulse the panel's
 *  reset line. Call once, before the RGB output stage starts streaming.
 *  False when the expander does not acknowledge -- almost always a wiring or
 *  address problem rather than a dead board. */
bool expanderInit();

/** Drive the backlight bit, for panels that need it driven rather than
 *  relying on their own default-on pull. No-op unless the board header sets
 *  kBacklightOnExpander. */
void expanderBacklight(bool on);

enum : uint8_t {
  kButtonUp = 0x01,
  kButtonDown = 0x02,
};

/** Currently-pressed user buttons, as a bitmask. The Qualia has no BOOT button
 *  available -- GPIO 0 is an RGB data line on this board -- so these stand in
 *  for it. Active low; 0 when the expander does not answer. */
uint8_t buttonMask();

/**
 * Hold the I2C bus the expander shares with the touch controller, for one
 * whole register transaction.
 *
 * Wire's own lock is not enough, and not for want of trying: it is held
 * from beginTransmission() through requestFrom(), but the bytes are then
 * read out of a buffer the next transaction reuses, after the lock is let
 * go. The touch controller is read from its own task and the expander from
 * the loop, so without this one task can read the other's reply.
 *
 * The mutex is made in expanderInit(), before any second task exists; before
 * that this does nothing.
 */
class BusGuard {
 public:
  BusGuard();
  ~BusGuard();
  BusGuard(const BusGuard&) = delete;
  BusGuard& operator=(const BusGuard&) = delete;
};

}  // namespace hw::qualia
