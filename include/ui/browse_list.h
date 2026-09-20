#pragma once

#include <cstdint>

namespace ui::browse_list {

enum class Result : uint8_t {
  kNone,       // tap landed on nothing, or on a section heading
  kSelected,   // an item was chosen; `row` says which. Nothing has played yet
  kDismissed,  // leave the list, back to now playing
};

/** Compose and present the list. */
void draw();

/** Handle a tap at display coordinates.
 *
 *  Deliberately does **not** play anything: starting an artist takes Music
 *  Assistant several seconds to answer, and a screen that freezes for that
 *  long with nothing on it reads as a device that missed the tap. On
 *  kSelected, `row` is the row that was hit -- draw showStarting(row) first,
 *  then call playRow(row). */
Result handleTap(int x, int y, int& row);

/** Show just the chosen item, centred, with a line saying it is starting.
 *  The acknowledgement that handleTap() deliberately leaves to the caller. */
void showStarting(int row);

/** Play the item on `row`. Blocks on the service call -- up to
 *  config::kHaServiceTimeoutMs for an artist. */
bool playRow(int row);

/** The services::browse item a row stands for, or -1 for a section heading.
 *  Rows are a layout idea -- headings take one -- so the display resolves
 *  them before handing an index to anything that does not draw. */
int itemForRow(int row);

/** Scroll by pixels rather than rows: positive moves further down the list.
 *  Clamped at both ends, and false when the clamp meant nothing moved -- which
 *  is what a caller following a finger uses to skip a redraw. */
bool scrollByPx(int delta);

/** Whether the list is already as far as it goes. `direction` is negative for
 *  the top, positive for the bottom. Lets a glide stop rather than spin. */
bool atScrollLimit(int direction);

/** Put the list back at the top, for a fresh open. */
/** Called when the list is opened: back to the top, and start the artwork
 *  walk over so anything that changed since last time is refetched. */
void opened();

void resetScroll();

/** Fetch one thumbnail that has not been tried yet. Blocks on the network for
 *  the length of one image, so the caller does this between touch polls and
 *  stops when it returns false. Safe to call whatever is on screen -- the
 *  list is usually filled in from the now-playing screen, before anyone has
 *  asked to see it. */
bool loadNextThumb();

}  // namespace ui::browse_list
