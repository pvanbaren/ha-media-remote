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

/** Queue the results' thumbnails and report, as a bit per result index,
 *  which have finished since the last call. Never blocks on the network. */
uint32_t updateThumbs();

/** Whether any result in `results` is at least partly on screen. */
bool anyResultVisible(uint32_t results);

/** True once a search has run, so the screen is showing results rather than
 *  the keyboard. */
bool showingResults();

/** Go back to the keyboard with the query intact, so a near miss can be
 *  edited rather than retyped. False when already there. */
bool backToKeyboard();

/** Compose and present whichever half is showing. */
void draw();

/** Repaint what a scroll of the results moved. Where it can, by moving the
 *  rows already on the panel and drawing only the strip that uncovers;
 *  otherwise draw(). Only for a change of scroll: the moved rows are the old
 *  rows, so anything that changes what a row shows wants draw(). */
void redrawResults();

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
