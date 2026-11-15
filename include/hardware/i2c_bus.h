#pragma once

#include <cstddef>
#include <cstdint>

/**
 * The board's I2C bus, for a board whose drivers share it: on the AMOLED the
 * touch controller, both audio codecs, the power manager, the motion sensor
 * and the clock all sit on one pair of pins.
 *
 * One owner, Arduino's `Wire` (port 0), started once here. Nothing else may
 * call Wire.begin(), install the ESP-IDF's legacy I2C driver, or hand the
 * pins to LovyanGFX: Arduino 3.x refuses two drivers on a port, and a library
 * that restarts Wire with its own speed changes it for everyone. A board
 * whose touch LovyanGFX reads (lgfx::i2c) does not use this at all.
 *
 * Each call below is one transaction, which Wire already serialises between
 * tasks. A sequence that must not be interleaved -- a read-modify-write, a
 * codec's setup -- holds a Lock across it; the calls take it too, and it is
 * recursive, so they nest.
 *
 * Measured on the AMOLED (2026-10-14): at 400 kHz, 59,920 register reads
 * from one task while touch was read from another, 0 failures. Two quirks of
 * Arduino-ESP32 3.x's driver shape the rules:
 *  - A zero-length write -- the classic "is anybody there" probe -- reports
 *    an answer about half the time from an empty address. There is no probe
 *    here; a device is found by reading its id register.
 *  - A read from an address with nothing on it leaves the driver in a state
 *    where the next operation fails (ESP_ERR_INVALID_STATE). So no address
 *    that is not fitted is ever spoken to, and every call retries once.
 */
namespace hw::i2c {

/** Start the bus on these pins at `hz`. Idempotent for the same arguments;
 *  false, with a log line, if it is already running on different ones. */
bool begin(int sda, int scl, uint32_t hz);

/** True once begin() has succeeded. Every call below fails until then. */
bool ready();

/** Hold the bus across a sequence of calls. Recursive; cheap when nothing
 *  else wants the bus. */
class Lock {
 public:
  Lock();
  ~Lock();
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

/** `len` bytes to `addr`, with a stop. */
bool write(uint8_t addr, const uint8_t* data, size_t len);

/** `wlen` bytes to `addr`, then -- after a repeated start -- `rlen` bytes
 *  back into `out`: the usual "set the register pointer, read" exchange. */
bool writeRead(uint8_t addr, const uint8_t* w, size_t wlen, uint8_t* out,
               size_t rlen);

/** The common 8-bit register forms. */
bool writeReg8(uint8_t addr, uint8_t reg, uint8_t value);
bool readReg8(uint8_t addr, uint8_t reg, uint8_t& value);
/** Read `reg`, replace the bits in `mask` with `value`'s, write it back --
 *  under one Lock, so nothing lands between the two. */
bool updateReg8(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t value);

/** Transactions that failed even after their retry, since begin(). */
uint32_t failures();

}  // namespace hw::i2c
