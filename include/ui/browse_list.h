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

/** Repaint what a scroll moved. Where it can, by moving the rows already on
 *  the panel and drawing only the strip that uncovers; otherwise the rows and
 *  the indicator composed afresh, leaving the title strip alone. Only for a
 *  change of scroll: the moved rows are the old rows. */
void redrawRows();

/** Compose and present the rows and indicator afresh, whatever is on the
 *  panel. For when what the rows show has changed -- a thumbnail arriving --
 *  rather than only where they are. */
void repaintRows();

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

/** Queue the list's thumbnails and report, as a bit per item index, which
 *  have finished since the last call. Never blocks on the network. Safe to
 *  call whatever is on screen -- the list is usually filled in from the
 *  now-playing screen, before anyone has asked to see it. */
uint32_t updateThumbs();

/** Whether any item in `items` (a bit per item index, as updateThumbs()
 *  returns) is at least partly on screen at the current scroll. */
bool anyItemVisible(uint32_t items);

}  // namespace ui::browse_list
