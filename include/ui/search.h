#pragma once

#include <cstdint>

namespace ui::search {

enum class Result : uint8_t {
  kNone,      // tap landed on nothing
  kChanged,   // the query changed; redraw
  kSearch,    // SEARCH pressed; run the search, then redraw
  kSelected,  // a result was chosen; `index` says which. Nothing plays yet
  kBack,      // leave the results and go back to the keyboard
};

/** Start over: empty query, no results, keyboard showing. */
void reset();

/** Fetch one result thumbnail that has not been tried yet. One image per
 *  call: each is a TLS handshake and takes the better part of a second, so a
 *  batch would either delay the names by several seconds or freeze touch for
 *  as long. False once every result has been tried. */
bool loadNextThumb();

/** True once a search has run, so the screen is showing results rather than
 *  the keyboard. */
bool showingResults();

/** Go back to the keyboard with the query intact, so a near miss can be
 *  edited rather than retyped. False when already there. */
bool backToKeyboard();

/** Compose and present whichever half is showing. */
void draw();

/** Show just the chosen result, centred, while it starts. The same
 *  acknowledgement the browse list gives, and for the same reason: play_media
 *  on an artist takes seconds, and a screen that stops for that long reads as
 *  a device that missed the tap. */
void showStarting(int index);

/** Scroll the results by pixels; positive moves further down. Clamped, and
 *  false when the clamp meant nothing moved. No-op on the keyboard. */
bool scrollByPx(int delta);
/** Whether the results are as far as they go. `direction` is negative for the
 *  top, positive for the bottom. */
bool atScrollLimit(int direction);

/** Draw the query with a "Searching..." line, for the blocking call. */
void showSearching();

/** Handle a tap. Deliberately does not search or play: both block for a
 *  round trip, and the caller wants to put something on screen first. */
Result handleTap(int x, int y, int& index);

}  // namespace ui::search
