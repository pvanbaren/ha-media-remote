#pragma once

#include <cstddef>
#include <cstdint>

#include "services/ha_client.h"

/**
 * "Recommended": artists like the recent ones at the top of the list -- this
 * room's while Recent list is Room only (services::browse::recentHere()) and
 * it has any, and otherwise the server's, as the list last loaded them.
 *
 * Drawn from Music Assistant's own API (services::ma), since Home Assistant
 * does not pass similarity through. For each of the
 * config::kRecommendSeedArtists most recent artists it asks for similar
 * artists -- which the library answers, merging whatever its providers know --
 * and, when that comes back empty, as it does for YouTube Music's own
 * artists, for tracks similar to one of theirs that played here, whose
 * artists stand in. Each artist in the pool carries a weight: how many of the
 * seeds pointed at it.
 *
 * That takes seconds -- three seconds a call is ordinary -- so it runs on a
 * task of its own, when the seeds change, and keeps the result. A list load
 * only draws from what is kept, which costs nothing: draw() picks at random,
 * weighted, so the section changes from one load to the next and leans
 * toward the artists more than one seed agrees on.
 */
namespace services::recommend {

/** Start the task. Once, in setup(), after history::init(). */
void init();

/** Up to `want` artists from the pool into `out`, none already among the
 *  recent artists it was built from. 0 when the pool is empty -- no token,
 *  nothing played yet, or the calls failing -- and the caller falls back to
 *  something else. */
int draw(ha::LibraryItem* out, int want);

/** The server's recent artists, as the list just loaded them: the seeds, and
 *  the artists to leave out, whenever the room's own are not in use. */
void setServerRecent(const ha::LibraryItem* items, int count);

/** Changes whenever the pool does, so a list built from it knows to rebuild. */
uint32_t generation();

}  // namespace services::recommend
