#include "hardware/touch.h"

// Gesture resolution, shared by every panel. What the board supplies is in
// hardware/touch_raw.h and is exactly two functions.

#include <Arduino.h>

#include <cstdlib>

#include "board/board.h"
#include "config.h"
#include "hardware/touch_raw.h"

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



}  // namespace

bool touchInit() {
  s_available = touchRawInit();
  return s_available;
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
  int raw_x = 0;
  int raw_y = 0;
  if (touchReadRaw(raw_x, raw_y)) {
    int x = raw_x;
    int y = raw_y;
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
