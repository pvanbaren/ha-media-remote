#pragma once

#include <cstddef>
#include <cstdint>

#include "services/ha_client.h"

/**
 * What the swipe-up list shows: a few sections of the Music Assistant library,
 * defined by config::kBrowseSections, flattened into one scrollable run of
 * rows with a heading above each.
 *
 * Artwork is named here and drawn elsewhere. This file deals in image URLs;
 * turning one into pixels is a size only the panel knows, so the sprites live
 * in ui::artwork. Nothing in this file includes a graphics library.
 *
 * Everything here is **loaded in the background**, by the poll task, so a
 * swipe up lands on a list that is already there rather than on a loading
 * card. That means two tasks touch this state -- the poll task writing, the
 * Arduino loop reading while it draws -- so every entry point below takes a
 * mutex and returns by value. Row carries a copy of the title rather than a
 * pointer into the item array for exactly that reason: a pointer would dangle
 * the moment the lock was released.
 */
namespace services::browse {

/** Why the list is empty, so the screen can say so rather than shrugging.
 *  An empty list has several quite different causes and only one of them is
 *  a fault, which is worth a sentence on a panel this small. */
enum class Problem : uint8_t {
  kNone,           // there are rows
  kNotLoaded,      // never refreshed
  kNoConfigEntry,  // no Music Assistant config entry id in the portal
  kRequestFailed,  // every section's request failed; ha::lastError() says why
  kLibraryEmpty,   // the requests worked and the library has nothing to show
};

/** One line of the list. A copy, not a view: see the note above. */
struct Row {
  /** A section heading: not selectable, and has no artwork. */
  bool header = false;
  char title[config::kFriendlyNameMaxLen] = {};
  /** Index into the item array, or -1 for a heading. */
  int item = -1;
};

/** Create the lock. Call once in setup(), before the poll task exists. */
void init();

/** Load the list if it is missing or stale, and do nothing if it is not.
 *
 *  Blocks on the network when it does work -- one request per section, plus
 *  one per image not already decoded -- so the poll task calls this on its own
 *  schedule and the list is usually ready before anyone asks for it. The
 *  screen calls it too, which then costs nothing, or waits out a load already
 *  in flight rather than starting a second one.
 *
 *  False when it tried and every section failed; whatever was there before is
 *  kept. True when there is a usable list, loaded now or earlier. */
bool ensureLoaded();

/** Whether the cached list is older than config::kBrowseTtlMs. */
bool stale();

/** Load in the background if stale, raising busy() while it does. What the
 *  poll task calls; the UI wants ensureLoaded(). */
void preload();

/** True while a background load is in flight. Worth checking before calling
 *  ensureLoaded() from the UI, since that call will wait for it. */
bool busy();

/** Why rowCount() is zero. Meaningless when it is not. */
Problem problem();

/** Hold the list still for a whole frame.
 *
 *  Every call below takes the lock on its own, which keeps each one safe but
 *  not a run of them: a background load landing between rowCount() and the
 *  last rowAt() changes the list out from under a half-drawn screen. The
 *  drawing and hit-testing code holds one of these across the whole pass so
 *  it sees a single version of the list. The lock is recursive, so the calls
 *  inside keep working unchanged. */
struct ReadGuard {
  ReadGuard();
  ~ReadGuard();
  ReadGuard(const ReadGuard&) = delete;
  ReadGuard& operator=(const ReadGuard&) = delete;
};

/** Items across all sections, which is what artwork is keyed by -- not
 *  rowCount(), which counts the section headings too. */
int itemCount();

int rowCount();
/** False when `index` is out of range. */
bool rowAt(int index, Row& out);

/** Play item `index` on the selected player. Blocks on the service call. */
bool play(int index);

/** Copy item `index`'s artwork URL into `out`, empty when it has none. False
 *  when `index` is not an item. Taken under the lock and returned by value,
 *  like everything else here. */
bool imageUrlAt(int index, char* out, size_t out_len);

}  // namespace services::browse
