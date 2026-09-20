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
 * Loading blocks on the network, a TLS handshake per image, so callers take
 * one per loop pass rather than a batch: the names are on screen immediately
 * and the pictures arrive under them with touch still answering in between.
 */
namespace ui::artwork {

enum class Kind : uint8_t { kBrowse, kSearch };

/** Fills `out` with the image URL for `index`, or leaves it empty when that
 *  entry has no artwork. False when `index` is not a real entry. */
using UrlFn = bool (*)(int index, char* out, size_t out_len);

/** Claim the sprite pool. After the display is up -- the sprites take its
 *  colour depth -- and before Wi-Fi, while the heap is unfragmented. */
void init();

/** Thumbnail edge in display px, or 0 when there was no room for the pool, in
 *  which case the lists draw names only. */
int size();

/** Start the walk over again for `kind`. Pixels already decoded are kept: a
 *  refresh that returns the same items compares URLs and skips the refetch. */
void forget(Kind kind);

/** Load the next entry not tried since forget(). Blocks on the network when
 *  it does work. False once every entry has been tried, which is how a caller
 *  knows to stop asking. */
bool loadNext(Kind kind, int count, UrlFn url_for);

/** Push the slot's picture at (x, y). False when it has none -- no URL, a
 *  decode that failed, no pool, or the pool belongs to the other screen --
 *  and the caller should use the space for something else. */
bool draw(Kind kind, int index, lgfx::LovyanGFX& gfx, int x, int y);

}  // namespace ui::artwork
