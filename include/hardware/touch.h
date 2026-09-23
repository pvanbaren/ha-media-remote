#pragma once

#include <cstdint>

namespace hw {

/** One resolved gesture, consumed by whichever screen is on top. */
enum class TouchEvent : uint8_t {
  kNone,
  kTap,        // press and release in place
  kSwipeUp,
  kSwipeDown,
  kSwipeLeft,
  kSwipeRight,
};

struct TouchReport {
  TouchEvent event = TouchEvent::kNone;
  /** Tap point, or the release point of a swipe. Display pixels. */
  int x = 0;
  int y = 0;
  /** millis() when the finger went down. */
  unsigned long down_ms = 0;
  /** Longest the press went unread while the finger was down, in ms. Small
   *  wherever touch is sampled on its own task; a busy loop pass can make it
   *  long on a board that samples from the loop. */
  unsigned long unwatched_ms = 0;
};

/** Arms the touch path and logs what is on the I2C bus. Call after
 *  displayInit(): the controller is the panel's touch device, so LovyanGFX
 *  has already brought up I2C and reset it by then.
 *
 *  False only on a board built without touch. It cannot report a missing
 *  controller, because a CST816S ignores I2C while it is in its power-saving
 *  state -- "no answer" at boot is the normal case for an untouched panel, not
 *  evidence of anything. The boot log says what did and did not answer. */
bool touchInit();
/** True when this board has a touch panel at all. */
bool touchAvailable();

/** Sample the panel. Call every loop iteration — it rate-limits itself.
 *  Returns true and fills `out` on the frame a gesture completes. */
bool touchPoll(TouchReport& out);

/** True while a finger is physically on the panel. Reports the contact itself,
 *  so it stays true after touchCancel() -- cancelling suppresses the gesture a
 *  release would produce, it does not lift the finger. A drag handler needs
 *  exactly this: grab, cancel the gesture, then keep tracking until lift. */
bool touchIsDown();

/** millis() of the last touch the gesture layer processed -- down, move or
 *  release -- or now while a finger is down; 0 before the first. */
unsigned long touchLastActivityMs();
/** Live finger position; only meaningful while touchIsDown(). */
void touchPosition(int& x, int& y);

/** Live travel of the press in progress, in display pixels (positive = right
 *  and down). Zero when nothing is down. Lets a list track the finger before
 *  the gesture resolves, and lets a caller tell a vertical press from a
 *  horizontal one before deciding whether to claim it. */
int touchDragDy();
int touchDragDx();

/** Drop any press in progress so it cannot land on a screen that just
 *  replaced the one the finger started on. */
void touchCancel();

}  // namespace hw
