#pragma once

#include <cstddef>
#include <cstdint>

#include <LovyanGFX.hpp>

/**
 * Decoded thumbnails for the list screens.
 *
 * This lives in ui/ rather than beside the services that name the images,
 * because a thumbnail is pixels at a size only the panel knows. The browse
 * list and the search results ask services::browse and services::search what
 * the pictures *are*; turning a URL into a sprite of the right size is a
 * display decision, and a 720 px panel would want three times the edge.
 *
 * One pool serves both screens. They are never on screen together -- search
 * is reached from the list and replaces it -- so the pool is tagged with
 * whichever screen last used it and switching screens invalidates the other.
 * That also halves the sprite count against a pool each.
 *
 * The fetching happens on a worker task of this module's own, so nothing
 * here blocks: the loop queues what a screen wants with update(), draws
 * whatever has arrived, and repaints when update() says more has. The names
 * are on screen immediately and the pictures arrive under them.
 */
namespace ui::artwork {

enum class Kind : uint8_t { kBrowse, kSearch };

/** Fills `out` with the image URL for `index`, or leaves it empty when that
 *  entry has no artwork. False when `index` is not a real entry. */
using UrlFn = bool (*)(int index, char* out, size_t out_len);

/** Claim the sprite pool and start the worker. After the display is up --
 *  the sprites take its colour depth -- and before Wi-Fi, while the heap is
 *  unfragmented. */
void init();

/** Thumbnail edge in display px, or 0 when there was no room for the pool, in
 *  which case the lists draw names only. */
int size();

/** Start the walk over again for `kind`. Pixels already decoded are kept: a
 *  refresh that returns the same items compares URLs and skips the refetch,
 *  and one that failed is tried again. */
void forget(Kind kind);

/** Queue every entry below `count` not walked since forget(), and report
 *  which slots have finished -- arrived or failed -- since the last call, as a
 *  bit per slot index. Never blocks on the network; cheap enough to call on
 *  every loop pass. Switching `kind` drops the other screen's pictures. */
uint32_t update(Kind kind, int count, UrlFn url_for);

/** Push the slot's picture at (x, y). False when it has none -- no URL, a
 *  decode that failed, no pool, or the pool belongs to the other screen --
 *  and the caller should use the space for something else. */
bool draw(Kind kind, int index, lgfx::LovyanGFX& gfx, int x, int y);

// --- The now-playing cover ----------------------------------------------------
// Fetched on the same worker, ahead of any thumbnail, so the task servicing
// the state stream never waits out a download. A one-deep slot: a request made
// while another is loading replaces whatever was waiting behind it.

/** Ask for the cover at `picture` (an entity_picture path or an absolute URL).
 *  From any task, and cheap to repeat: the same picture asked for again is
 *  ignored. Empty drops the cover at once. Without a worker, fetches here and
 *  now, as before. */
void requestCover(const char* picture);

/** True while `picture` has been asked for and the worker has not finished
 *  it -- what a repaint waiting for its cover checks. */
bool coverPending(const char* picture);

/** True once after the worker finishes a cover, arrived or failed: the frame
 *  on screen was drawn without it. */
bool takeCoverFinished();

/** Forget what has been asked for, alongside cover::clear(), so the same
 *  picture asked for again is fetched again. */
void forgetCover();

}  // namespace ui::artwork
