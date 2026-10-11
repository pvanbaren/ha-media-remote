#include "hardware/touch_raw.h"

#include <Arduino.h>
#include <Wire.h>

#include "board/board.h"
#include "log.h"

/**
 * CST826 capacitive touch on the MaTouch 2.1"'s own I2C bus, read by hand as
 * Makerfabs' factory test reads it: the Hynitron layout the CST816 uses,
 * from register 0x02 -- the number of fingers, then the first point's
 * coordinates, twelve bits each, the top two bits of the first byte the
 * event: 0 down, 1 lifted, 2 still in contact.
 *
 * Neither its interrupt nor its reset reaches the S3, so it is polled, and
 * there is no way to wake it or start it over from here.
 *
 * On the glass it reports the edge as 480, one past the last pixel. Idle, it
 * now and then answers a read with noise -- all 0xFF, or a stray byte where
 * the finger count goes -- and one such read once woke a blanked screen. In
 * the middle of a press it sometimes says no finger for a read or two. Taken
 * as they came, a swipe to the edge or a press that hiccuped turned into
 * several short taps; so the edge is clamped, a reading that is not one or
 * two fingers on the glass is ignored, and a finger is held down through
 * kReleaseMs of reads that say it lifted.
 */
namespace hw {
namespace {

constexpr uint8_t kRegFingers = 0x02;
constexpr uint8_t kEventLifted = 1;
/** How long reads must say "no finger" before a press is let go. A few
 *  polls: shorter than any real gap between two taps. */
constexpr unsigned long kReleaseMs = 60;

bool s_present = false;
bool s_down = false;
int s_x = 0;
int s_y = 0;
unsigned long s_last_seen_ms = 0;

bool readRegs(uint8_t reg, uint8_t* out, size_t len) {
  Wire.beginTransmission(board::kTouchI2cAddress);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(static_cast<int>(board::kTouchI2cAddress),
                       static_cast<int>(len)) != static_cast<int>(len)) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    out[i] = static_cast<uint8_t>(Wire.read());
  }
  return true;
}

int clampToPanel(int v) {
  return v < 0 ? 0 : v >= board::kTouchNativeSize ? board::kTouchNativeSize - 1
                                                  : v;
}

}  // namespace

bool touchRawInit() {
  Wire.begin(static_cast<int>(board::kTouchPinSda),
             static_cast<int>(board::kTouchPinScl), board::kTouchI2cHz);
  // Present if it answers at all; a CST8xx may be dozing and say nothing
  // until touched, so silence is logged but not taken as absence.
  Wire.beginTransmission(board::kTouchI2cAddress);
  const bool answered = Wire.endTransmission() == 0;
  s_present = true;
  if (answered) {
    LOG_INFO("Touch: CST826 at 0x%02X", board::kTouchI2cAddress);
  } else {
    LOG_WARN("Touch: nothing answered at 0x%02X yet -- a dozing CST826, or "
             "none",
             board::kTouchI2cAddress);
  }
  return true;
}

bool touchReadRaw(int& x, int& y) {
  if (!s_present) {
    return false;
  }
  uint8_t r[5] = {};
  const bool read = readRegs(kRegFingers, r, sizeof(r));
  if (read) {
    const uint8_t fingers = r[0];
    const uint8_t event = r[1] >> 6;
    const int raw_x = ((r[1] & 0x0F) << 8) | r[2];
    const int raw_y = ((r[3] & 0x0F) << 8) | r[4];
    // Only a reading that makes sense is a finger: one or two of them, and a
    // point on the glass or its edge. Anything else is noise.
    const bool sane = fingers >= 1 && fingers <= 2 &&
                      raw_x <= board::kTouchNativeSize &&
                      raw_y <= board::kTouchNativeSize;
    if (sane && event != kEventLifted) {
      s_x = clampToPanel(raw_x);
      s_y = clampToPanel(raw_y);
      s_down = true;
      s_last_seen_ms = millis();
    }
  }
  // A miss, a noisy read or a "lifted" lets go only once it has lasted.
  if (s_down && millis() - s_last_seen_ms > kReleaseMs) {
    s_down = false;
  }
  if (!s_down) {
    return false;
  }
  x = s_x;
  y = s_y;
  return true;
}

}  // namespace hw
