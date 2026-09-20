#include "hardware/touch.h"

#include <Arduino.h>

#include <cstdlib>

#include "board/waveshare_s3.h"
#include "config.h"
#include "log.h"
#include "hardware/display.h"

namespace hw {
namespace {

/** Slop and swipe thresholds are authored at kUiBaseSize like the rest of the
 *  UI, so they stay proportionate on a larger panel. */
constexpr int scaledPx(int px240) {
  return static_cast<int>(px240 * board::kUiScale + 0.5f);
}

const int kTapSlopPx = scaledPx(board::kTouchTapSlopPx);
const int kSwipeMinPx = scaledPx(board::kTouchSwipeMinPx);

bool s_available = false;

bool s_down = false;
int s_x = 0;
int s_y = 0;
int s_down_x = 0;
int s_down_y = 0;
int s_max_travel = 0;
unsigned long s_down_ms = 0;
bool s_cancelled = false;
unsigned long s_last_poll_ms = 0;
/** Longest the press in progress has gone between two reads. */
unsigned long s_unwatched_ms = 0;

/** Apply the orientation fixes. LovyanGFX has already rescaled the
 *  controller's raster to panel pixels, so these work in display coordinates
 *  and the panel diameter stays the only porting knob. */
void mapPoint(int& x, int& y) {
  if constexpr (board::kTouchSwapXy) {
    const int t = x;
    x = y;
    y = t;
  }
  if constexpr (board::kTouchInvertX) {
    x = board::kDisplayWidth - 1 - x;
  }
  if constexpr (board::kTouchInvertY) {
    y = board::kDisplayHeight - 1 - y;
  }
  x = constrain(x, 0, board::kDisplayWidth - 1);
  y = constrain(y, 0, board::kDisplayHeight - 1);
}

/** Resolve the finished press into a gesture. */
TouchEvent classifyRelease() {
  const unsigned long held = millis() - s_down_ms;
  const int dx = s_x - s_down_x;
  const int dy = s_y - s_down_y;

  if (s_max_travel <= kTapSlopPx) {
    if (held >= board::kTouchTapMinMs && held <= board::kTouchTapMaxMs) {
      return TouchEvent::kTap;
    }
    return TouchEvent::kNone;
  }

  // Dominant axis wins, so a sloppy diagonal still scrolls the list.
  if (abs(dy) >= abs(dx)) {
    if (abs(dy) < kSwipeMinPx) {
      return TouchEvent::kNone;
    }
    return dy > 0 ? TouchEvent::kSwipeDown : TouchEvent::kSwipeUp;
  }
  if (abs(dx) < kSwipeMinPx) {
    return TouchEvent::kNone;
  }
  return dx > 0 ? TouchEvent::kSwipeRight : TouchEvent::kSwipeLeft;
}

// --- Boot-time diagnostics --------------------------------------------------
//
// Talking to the bus through lgfx::i2c rather than Wire matters: LovyanGFX
// owns this port now, and bringing Wire up on the same pins would install a
// second driver over the top of it.

/** Report every address that answers, so a silent controller separates
 *  "wired to the wrong pins" from "wired, but not what we expect". */
void scanI2cBus() {
  int found = 0;
  for (int address = 1; address < 127; ++address) {
    if (lgfx::i2c::transactionWrite(board::kTouchI2cPort, address, nullptr, 0,
                                    board::kTouchI2cHz)
            .has_value()) {
      LOG_INFO("Touch: a device answers at 0x%02X", address);
      ++found;
    }
  }
  if (found == 0) {
    LOG_ERROR(
        "Touch: nothing answers on SDA %d / SCL %d - check the wiring, the "
        "3.3V and GND rails, and that RST (%d) is not held low",
        static_cast<int>(board::kTouchPinSda),
        static_cast<int>(board::kTouchPinScl),
        static_cast<int>(board::kTouchPinRst));
  }
}

/** Read the chip id, for the log only.
 *
 *  A miss is not a failure. The CST816S does not answer I2C at all while it is
 *  in its power-saving state, which is most of the time nobody is touching it,
 *  so LovyanGFX deliberately treats its own init as unconditionally successful
 *  and probes again on the first read. The reset pulse below is an attempt to
 *  catch it awake; when it works the id is worth having, and when it does not
 *  the bus scan says whether anything is out there at all. */
void logController() {
  if (board::kTouchPinRst != GPIO_NUM_NC) {
    pinMode(board::kTouchPinRst, OUTPUT);
    digitalWrite(board::kTouchPinRst, LOW);
    delay(10);
    digitalWrite(board::kTouchPinRst, HIGH);
    // It NAKs for ~50 ms after reset while it loads firmware.
    delay(60);
  }

  constexpr uint8_t kRegChipId = 0xA7;
  const auto id = lgfx::i2c::readRegister8(board::kTouchI2cPort,
                                           board::kTouchI2cAddress,
                                           kRegChipId, board::kTouchI2cHz);
  if (id.has_value()) {
    // 0xB5 CST816S, 0xB6 CST816T, 0xB7 CST820 -- the register map is shared,
    // so an unlisted variant is reported and then used anyway.
    LOG_INFO("Touch: CST816S id 0x%02X at 0x%02X (SDA %d, SCL %d)",
                  id.value(), board::kTouchI2cAddress,
                  static_cast<int>(board::kTouchPinSda),
                  static_cast<int>(board::kTouchPinScl));
    return;
  }

  LOG_WARN(
      "Touch: no answer at 0x%02X - asleep, or not wired. Scanning:",
      board::kTouchI2cAddress);
  scanI2cBus();
}

}  // namespace

bool touchInit() {
  // displayInit() already ran, and with it the controller's own init: it is
  // the panel's touch device, so tft.init() brought up I2C and reset it.
  logController();
  s_available = true;
  return true;
}

bool touchAvailable() { return s_available; }

bool touchIsDown() { return s_down; }

void touchPosition(int& x, int& y) {
  x = s_x;
  y = s_y;
}

int touchDragDy() { return s_down ? s_y - s_down_y : 0; }
int touchDragDx() { return s_down ? s_x - s_down_x : 0; }

void touchCancel() {
  // Keep s_down as-is so the finger still has to lift before anything new can
  // start; the cancel flag is what suppresses the gesture.
  s_cancelled = s_down;
}

bool touchPoll(TouchReport& out) {
  if (!s_available) {
    return false;
  }

  const unsigned long now = millis();
  if (now - s_last_poll_ms < board::kTouchPollIntervalMs) {
    return false;
  }
  const unsigned long gap = s_last_poll_ms == 0 ? 0 : now - s_last_poll_ms;
  s_last_poll_ms = now;

  int32_t raw_x = 0;
  int32_t raw_y = 0;
  if (tft.getTouch(&raw_x, &raw_y)) {
    int x = static_cast<int>(raw_x);
    int y = static_cast<int>(raw_y);
    mapPoint(x, y);

    if (!s_down) {
      s_down = true;
      s_cancelled = false;
      s_down_ms = now;
      s_down_x = x;
      s_down_y = y;
      s_max_travel = 0;
      s_unwatched_ms = 0;
    } else {
      if (gap > s_unwatched_ms) {
        s_unwatched_ms = gap;
      }
      const int travel = abs(x - s_down_x) + abs(y - s_down_y);
      if (travel > s_max_travel) {
        s_max_travel = travel;
      }
    }
    s_x = x;
    s_y = y;
    return false;
  }

  if (!s_down) {
    return false;
  }

  // The gap before the read that found it gone counts too: the finger may
  // have moved anywhere in it before it left.
  if (gap > s_unwatched_ms) {
    s_unwatched_ms = gap;
  }

  // Finger lifted.
  s_down = false;
  if (s_cancelled) {
    s_cancelled = false;
    return false;
  }

  const TouchEvent event = classifyRelease();
  if (event == TouchEvent::kNone) {
    return false;
  }

  out.event = event;
  out.x = s_x;
  out.y = s_y;
  out.down_ms = s_down_ms;
  out.unwatched_ms = s_unwatched_ms;
  return true;
}

}  // namespace hw
