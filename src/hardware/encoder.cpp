#include "hardware/encoder.h"

#include "board/board.h"

#if defined(BOARD_HAS_ENCODER)

#include <Arduino.h>
#include <driver/pulse_cnt.h>
#include <hal/gpio_ll.h>

#include "log.h"

/**
 * The knob, counted by the S3's pulse counter rather than by interrupts: every
 * edge of either pin moves the count up or down by the other pin's level, so
 * nothing is lost to a slow loop pass or a busy core, and contact bounce
 * cancels itself out -- a bounce is an edge forward and the same edge back.
 *
 * The push switch is caught on an interrupt instead, since a press shorter
 * than a loop pass would otherwise go unseen; bounce is filtered by time.
 */
namespace hw {
namespace {

/** The counter's hardware range. It accumulates past it in software, so
 *  this only sets how often that happens. */
constexpr int kCountLimit = 10000;
/** Pulses narrower than this are noise on the line, not a click. */
constexpr uint32_t kGlitchNs = 1000;
/** A switch change sooner than this after the last is its contacts bouncing. */
constexpr unsigned long kPushDebounceMs = 30;

bool s_tried = false;
bool s_ready = false;
pcnt_unit_handle_t s_unit = nullptr;
/** Counts already handed out as clicks. */
int s_taken = 0;
int s_last_count = 0;
unsigned long s_activity_ms = 0;

/** The switch, as the interrupt last settled it. Shared with it. */
portMUX_TYPE s_push_lock = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_push_down = false;
volatile bool s_push_pending = false;
volatile unsigned long s_push_changed_ms = 0;

bool pushPinLow() {
  return gpio_ll_get_level(&GPIO, board::kEncoderPinPush) == 0;
}

void IRAM_ATTR onPushEdge() {
  const bool down = gpio_ll_get_level(&GPIO, board::kEncoderPinPush) == 0;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_push_lock);
  if (down != s_push_down && now - s_push_changed_ms >= kPushDebounceMs) {
    s_push_down = down;
    s_push_changed_ms = now;
    if (down) {
      s_push_pending = true;
    }
  }
  portEXIT_CRITICAL_ISR(&s_push_lock);
}

bool startCounter() {
  pcnt_unit_config_t unit = {};
  unit.low_limit = -kCountLimit;
  unit.high_limit = kCountLimit;
  unit.flags.accum_count = 1;
  if (pcnt_new_unit(&unit, &s_unit) != ESP_OK) {
    return false;
  }
  pcnt_glitch_filter_config_t filter = {};
  filter.max_glitch_ns = kGlitchNs;
  pcnt_unit_set_glitch_filter(s_unit, &filter);

  // Full quadrature: each pin's edges, read against the other's level.
  pcnt_chan_config_t a = {};
  a.edge_gpio_num = board::kEncoderPinA;
  a.level_gpio_num = board::kEncoderPinB;
  pcnt_chan_config_t b = {};
  b.edge_gpio_num = board::kEncoderPinB;
  b.level_gpio_num = board::kEncoderPinA;
  pcnt_channel_handle_t chan_a = nullptr;
  pcnt_channel_handle_t chan_b = nullptr;
  if (pcnt_new_channel(s_unit, &a, &chan_a) != ESP_OK ||
      pcnt_new_channel(s_unit, &b, &chan_b) != ESP_OK) {
    return false;
  }
  pcnt_channel_set_edge_action(chan_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                               PCNT_CHANNEL_EDGE_ACTION_INCREASE);
  pcnt_channel_set_level_action(chan_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
  pcnt_channel_set_edge_action(chan_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                               PCNT_CHANNEL_EDGE_ACTION_DECREASE);
  pcnt_channel_set_level_action(chan_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

  // Accumulating needs the limits watched, so the overflow is noticed.
  pcnt_unit_add_watch_point(s_unit, -kCountLimit);
  pcnt_unit_add_watch_point(s_unit, kCountLimit);
  return pcnt_unit_enable(s_unit) == ESP_OK &&
         pcnt_unit_clear_count(s_unit) == ESP_OK &&
         pcnt_unit_start(s_unit) == ESP_OK;
}

}  // namespace

bool encoderInit() {
  if (s_tried) {
    return s_ready;
  }
  s_tried = true;
  if (!startCounter()) {
    LOG_WARN("Encoder: no pulse counter, the knob will not turn anything");
    return false;
  }
  pinMode(board::kEncoderPinPush, INPUT_PULLUP);
  s_push_down = pushPinLow();
  attachInterrupt(board::kEncoderPinPush, onPushEdge, CHANGE);
  s_ready = true;
  LOG_INFO("Encoder: on GPIO %d/%d, push on %d",
           static_cast<int>(board::kEncoderPinA),
           static_cast<int>(board::kEncoderPinB),
           static_cast<int>(board::kEncoderPinPush));
  return true;
}

int encoderTakeDetents() {
  if (!s_ready) {
    return 0;
  }
  int count = 0;
  if (pcnt_unit_get_count(s_unit, &count) != ESP_OK) {
    return 0;
  }
  if (count != s_last_count) {
    s_last_count = count;
    s_activity_ms = millis();
  }
  // Toward zero, so a part-turn either way stays part of the next click.
  const int detents = (count - s_taken) / board::kEncoderCountsPerDetent;
  s_taken += detents * board::kEncoderCountsPerDetent;
  return board::kEncoderReverse ? -detents : detents;
}

bool encoderTakePress() {
  if (!s_ready) {
    return false;
  }
  const bool low = pushPinLow();
  const unsigned long now = millis();
  bool pressed = false;
  portENTER_CRITICAL(&s_push_lock);
  // An edge thrown away as bounce can be the last one, leaving the settled
  // state wrong until the next; the pin, held long enough, says otherwise.
  if (low != s_push_down && now - s_push_changed_ms >= kPushDebounceMs) {
    s_push_down = low;
    s_push_changed_ms = now;
    if (low) {
      s_push_pending = true;
    }
  }
  pressed = s_push_pending;
  s_push_pending = false;
  portEXIT_CRITICAL(&s_push_lock);
  if (pressed) {
    s_activity_ms = now;
  }
  return pressed;
}

unsigned long encoderLastActivityMs() { return s_activity_ms; }

}  // namespace hw

#else  // no knob on this board

namespace hw {

bool encoderInit() { return false; }
int encoderTakeDetents() { return 0; }
bool encoderTakePress() { return false; }
unsigned long encoderLastActivityMs() { return 0; }

}  // namespace hw

#endif
