#include "hardware/touch.h"

// Gesture resolution, shared by every panel. What the board supplies is in
// hardware/touch_raw.h and is exactly two functions.

#include <Arduino.h>

#include <cstdlib>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "board/board.h"
#include "config.h"
#include "log.h"
#include "hardware/display.h"
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
/** When the consumer last saw the finger go down, move or lift. */
unsigned long s_last_activity_ms = 0;

/**
 * What the controller reported, in order, stamped with when.
 *
 * Touch used to be read once per loop pass and only the current state was
 * kept, so a pass longer than a tap -- a thumbnail fetch, a list repaint --
 * never saw the finger at all. Measured at boot, the loop went unread for
 * 560 to 860 ms at a time, and about one tap in six landed. Samples now go
 * into a queue as they are taken, and touchPoll() replays them through the
 * same state machine as before, at the times they happened. A slow pass
 * delays a tap; it no longer loses one.
 *
 * Only changes are queued -- down, up, or a move -- so a finger resting on
 * the glass costs nothing and 32 entries is a lot of gesture.
 */
struct Sample {
  unsigned long ms;
  int16_t x;
  int16_t y;
  bool down;
  /** Longest gap between two reads so far in this press. */
  uint16_t unwatched_ms;
};
constexpr int kQueueDepth = 32;
QueueHandle_t s_samples = nullptr;
TaskHandle_t s_sampler = nullptr;

/** Producer state: what the last sample said. Owned by whoever samples --
 *  the sampler task, or the loop on a board without one. */
bool s_raw_down = false;
/** When the controller was last read, and the longest the press in progress
 *  has gone between two reads. */
unsigned long s_last_read_ms = 0;
unsigned long s_press_unwatched_ms = 0;
int s_raw_x = 0;
int s_raw_y = 0;
unsigned long s_last_sample_ms = 0;

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

  // Where the panel cannot turn itself the canvas is drawn turned, so a
  // touch -- which arrives in the glass's own coordinates -- is turned back
  // into the canvas's. The inverse of LovyanGFX's sprite rotation; the panel
  // is square, so width and height are the same number throughout.
  if (!displayRotatesItself()) {
    const int last = board::kDisplayWidth - 1;
    const int px = x;
    const int py = y;
    switch (displayRotation()) {
      case 1:
        x = py;
        y = last - px;
        break;
      case 2:
        x = last - px;
        y = last - py;
        break;
      case 3:
        x = last - py;
        y = px;
        break;
      default:
        break;
    }
  }
}

/** Resolve the finished press into a gesture. `release_ms` is when the
 *  finger lifted, which is not now when samples queued up behind a slow
 *  loop pass. */
TouchEvent classifyRelease(unsigned long release_ms) {
  const unsigned long held = release_ms - s_down_ms;
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

/** Queue a sample, dropping the oldest if the queue is full: the newest is
 *  the one that says where the finger is. */
void enqueue(const Sample& sample) {
  if (xQueueSend(s_samples, &sample, 0) == pdTRUE) {
    return;
  }
  Sample oldest;
  xQueueReceive(s_samples, &oldest, 0);
  xQueueSend(s_samples, &sample, 0);
}

/** Read the controller once and queue it if anything changed. */
void sampleOnce() {
  int x = 0;
  int y = 0;
  const unsigned long now = millis();
  const unsigned long gap = s_last_read_ms == 0 ? 0 : now - s_last_read_ms;
  s_last_read_ms = now;
  const bool down = touchReadRaw(x, y);
  if (down) {
    mapPoint(x, y);
  }
  // Time the finger was down and nobody looked. Counted before the check for
  // an unchanged reading below, since a still finger is not queued but its
  // gaps still count; and counted on the read that finds it lifted, since the
  // finger may have moved anywhere in that gap before it left.
  if (down && !s_raw_down) {
    s_press_unwatched_ms = 0;
  } else if (s_raw_down && gap > s_press_unwatched_ms) {
    s_press_unwatched_ms = gap;
  }
  if (down == s_raw_down && (!down || (x == s_raw_x && y == s_raw_y))) {
    return;
  }
  s_raw_down = down;
  s_raw_x = x;
  s_raw_y = y;
  enqueue(Sample{now, static_cast<int16_t>(x), static_cast<int16_t>(y), down,
                 static_cast<uint16_t>(s_press_unwatched_ms < 0xFFFF
                                           ? s_press_unwatched_ms
                                           : 0xFFFF)});
}

/** Sample on a fixed period, whatever the loop is doing.
 *
 *  Above the loop's priority and on its core, so it preempts a repaint or a
 *  blocking call for the fraction of a millisecond a read takes, and core 0
 *  is left to the network. */
void samplerTask(void*) {
  TickType_t wake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(board::kTouchPollIntervalMs) > 0
                                ? pdMS_TO_TICKS(board::kTouchPollIntervalMs)
                                : 1;
  for (;;) {
    sampleOnce();
    vTaskDelayUntil(&wake, period);
  }
}

/** Run one sample through the press state machine. True, with `out` filled,
 *  when it completed a gesture; `released` says the finger lifted. */
bool apply(const Sample& sample, TouchReport& out, bool& released) {
  s_last_activity_ms = sample.ms;
  if (sample.down) {
    if (!s_down) {
      s_down = true;
      s_cancelled = false;
      s_down_ms = sample.ms;
      s_down_x = sample.x;
      s_down_y = sample.y;
      s_max_travel = 0;
    } else {
      const int travel = abs(sample.x - s_down_x) + abs(sample.y - s_down_y);
      if (travel > s_max_travel) {
        s_max_travel = travel;
      }
    }
    s_x = sample.x;
    s_y = sample.y;
    return false;
  }

  if (!s_down) {
    return false;
  }

  // Finger lifted. The release point is the last place it was down, as it
  // always was: the controller reports no coordinate for a lift.
  released = true;
  s_down = false;
  if (s_cancelled) {
    s_cancelled = false;
    return false;
  }

  const TouchEvent event = classifyRelease(sample.ms);
  if (event == TouchEvent::kNone) {
    return false;
  }
  out.event = event;
  out.x = s_x;
  out.y = s_y;
  out.down_ms = s_down_ms;
  out.unwatched_ms = sample.unwatched_ms;
  return true;
}

// --- Boot-time diagnostics --------------------------------------------------
//
// Talking to the bus through lgfx::i2c rather than Wire matters: LovyanGFX
// owns this port now, and bringing Wire up on the same pins would install a
// second driver over the top of it.



}  // namespace

bool touchInit() {
  s_available = touchRawInit();
  if (!s_available) {
    return false;
  }
  s_samples = xQueueCreate(kQueueDepth, sizeof(Sample));
  if (s_samples == nullptr) {
    s_available = false;
    return false;
  }
  if constexpr (board::kTouchSampleTask) {
    // Priority 5: above the loop's 1, below everything the network stack
    // runs at. 4 KB covers a Wire transaction with room to spare.
    if (xTaskCreatePinnedToCore(samplerTask, "touch", 4096, nullptr, 5,
                                &s_sampler, 1) != pdPASS) {
      s_sampler = nullptr;
      LOG_WARN("Touch: no sampler task, sampling from the loop");
    }
  }
  return true;
}

bool touchAvailable() { return s_available; }

bool touchIsDown() { return s_down; }

unsigned long touchLastActivityMs() {
  // A finger resting still queues nothing, but it is someone using the panel.
  return s_down ? millis() : s_last_activity_ms;
}

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

  if (s_sampler == nullptr) {
    const unsigned long now = millis();
    if (now - s_last_sample_ms >= board::kTouchPollIntervalMs) {
      s_last_sample_ms = now;
      sampleOnce();
    }
  }

  // Replay what has queued up. Stop at a gesture, so the caller acts on it
  // before anything after it; and stop at a lift, so the loop gets one pass
  // with the finger up -- a drag that ended and a new press that began inside
  // one slow pass must not read as a single press that jumped.
  Sample sample;
  while (xQueueReceive(s_samples, &sample, 0) == pdTRUE) {
    bool released = false;
    if (apply(sample, out, released)) {
      return true;
    }
    if (released) {
      break;
    }
  }
  return false;
}

}  // namespace hw
