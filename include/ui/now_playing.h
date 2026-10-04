#pragma once

#include "services/ha_client.h"

namespace ui::now_playing {

enum class Action : uint8_t {
  kNone,
  kPrevious,
  kPlayPause,
  kNext,
};

/** Compose and present the whole screen: cover art backdrop, scrim, track
 *  text, progress arc and transport row. Call ui::cover::prepare()
 *  first; art small enough to cache repaints from RAM, art too large for the
 *  cache is re-streamed here and makes this block.
 *
 *  The player's *name* is deliberately absent. It was a header line whose only
 *  other job was to be the tap target for the on-device picker, and with the
 *  choice moved to the setup portal it was a permanent reminder of something
 *  that changes about once a year, spending the widest part of the circle to
 *  say it. */
void draw(const services::ha::PlayerState& state);

/** Repaint just the elapsed/total chip straight to the panel. It sits on an
 *  opaque pill for exactly this reason -- no recompose, so the cover art
 *  underneath is never decoded again. */
void refreshElapsed(const services::ha::PlayerState& state);

/** Repaint just the volume arc at `level`, straight to the panel. The arc is
 *  opaque over the track it draws, so a drag can follow the finger at no
 *  cost. */
void refreshVolume(const services::ha::PlayerState& state, float level);

/** True when the player exposes a volume the slider can drive. */
bool volumeAvailable(const services::ha::PlayerState& state);

/** Whether a press starting at (x, y) may become a volume swipe, or a tap
 *  there toggle the volume device's power.
 *
 *  The whole top half of the panel, rather than the arc itself. Landing a
 *  fingertip on a 7 px track drawn 111 px out from the centre is a precision
 *  task on a 1.28" circle, and it is the one control here that gets used
 *  without looking. Nothing else lives up there -- the transport row is at
 *  y=181 and the title text is not a target -- so the space is free, and the
 *  volume's own half is where switching what carries it on and off belongs.
 *
 *  Vertical swipes are left alone by the caller, since swipe-up opens the
 *  browse list. */
bool volumeSwipeRegion(int x, int y);

/** Level change per pixel of horizontal finger travel.
 *
 *  Scaled so the knob moves the same distance along the arc that the finger
 *  moved across the glass: the arc is ~504 px long at a 240 px diameter, so a
 *  full-width swipe is a little under half the range and the whole span takes
 *  about two and a half of them. Relative, not absolute -- the swipe adjusts
 *  the volume from wherever it already was rather than jumping it to whatever
 *  the finger happens to be pointing at. */
float volumeLevelPerPx();

/** Which control is under a tap, if any. */
Action hitTest(int x, int y);

/** Repaint one transport button pressed or released, straight to the panel,
 *  so a press acknowledges before the HA round trip returns. */
void showButtonPressed(Action action, bool pressed);

}  // namespace ui::now_playing
