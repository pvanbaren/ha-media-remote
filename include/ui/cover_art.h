#pragma once

#include <LovyanGFX.hpp>

namespace ui::cover {

/** Claim the reusable art buffer. Call once in setup(), BEFORE Wi-Fi comes up:
 *  taking it from a pristine heap is what guarantees it can be had at all, and
 *  it is never freed afterwards, so it cannot fragment anything. */
void init();

/** Fetch the art at `picture` if it is not already cached. `picture` is the
 *  entity_picture attribute: a HA-relative path, or an absolute URL from
 *  integrations that link art off-site. Pass an empty string to drop the
 *  cache. Does network work, so keep it out of the draw path. */
void prepare(const char* picture);

/** True when prepare() found art for the current picture, whether or not it
 *  fit in the cache. */
bool hasArt();

/** Paint the art centre-cropped to fill a `diameter` square whose TOP-LEFT
 *  corner is (x, y) -- not its centre. LovyanGFX treats the image position as
 *  the corner of the fit box and uses the datum only to place the image
 *  within that box, so passing a centre here puts the art off-screen.
 *
 *  Decodes from the cache when the image fit in RAM; re-streams it from HA
 *  when it did not. False when there is nothing to draw. */
bool draw(lgfx::LGFXBase& gfx, int x, int y, int diameter);

void clear();

/** Fetch the image at `url` and decode it into `gfx`, fitted inside a `size`
 *  square whose TOP-LEFT corner is (x, y). For images fetched once and kept
 *  as pixels afterwards -- the list thumbnails.
 *
 *  Keeps its connection open between calls, so a run of thumbnails from one
 *  host pays for one TLS handshake rather than one each; releaseThumbConnection()
 *  gives it back. The body is downloaded whole before decoding, so the lock
 *  shared with draw() is held for a decode from memory and never across the
 *  network. Blocks on the network, and is for one task only: the artwork
 *  worker. */
bool fetchThumb(lgfx::LGFXBase& gfx, const char* url, int x, int y, int size);

/** Close the connection fetchThumb() keeps. Called once a batch is done: an
 *  idle TLS connection is tens of KB of internal RAM held for nothing, beside
 *  the poll task's own. */
void releaseThumbConnection();

}  // namespace ui::cover
