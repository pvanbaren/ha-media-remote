#pragma once

/**
 * A rotary encoder and its push switch, on a board that has one
 * (BOARD_HAS_ENCODER in its header).
 *
 * Like touch_raw.h this is readings, not meaning: what a click or a press does
 * on each screen is the UI's business. On a board without a knob every call is
 * a harmless no-op -- encoderInit() says false, and nothing ever turns.
 */
namespace hw {

/** Idempotent. False on a board without a knob. */
bool encoderInit();

/** Clicks turned since the last call, positive clockwise. A part-turn that
 *  has not reached a click is kept for the next call. */
int encoderTakeDetents();

/** True once for each press of the knob, however long ago it happened: a
 *  press is caught as it happens, not when someone next asks. */
bool encoderTakePress();

/** millis() of the last turn or press, or 0 if there has been none. Updated
 *  by the two calls above, so as fresh as the last time they were made. */
unsigned long encoderLastActivityMs();

}  // namespace hw
