#pragma once

#include <cstddef>
#include <cstdint>

#include "services/ha_client.h"

/**
 * What has played in this room: the artists seen playing on the selected
 * player, most recent first, kept on the device.
 *
 * Music Assistant's own "recently played" is one timestamp per library item,
 * shared by the whole server, so two remotes in two rooms see the same list --
 * whatever anyone played last. Each remote drives its own player, though, so
 * what played on *that* player is what played in that room, which in practice
 * is what one person listens to. This is that list: the swipe-up list's
 * "Recent here", and the seeds of its "Recommended" (services::recommend).
 *
 * Noted from the state the network task already receives, so anything that
 * plays counts -- started from this remote, a phone, or an automation. Each new
 * artist costs one Music Assistant artist search, to turn the name into
 * something playable with a picture; an artist already on the list only moves
 * to the front. Stored in its own NVS namespace, so a BOOT reset of the Home
 * Assistant settings keeps it, like the device's name and rotation -- names
 * and URIs only, about 2 KB; pictures are kept in RAM and found again after a
 * restart (refreshPictures()).
 *
 * The network task writes and the browse load reads, on whichever task asked
 * for the list, so every call takes a lock and copies.
 */
namespace services::history {

/** Artists kept. */
constexpr int kMaxArtists = 16;

/** Load the stored list and create the lock. Once, in setup(), before any
 *  task that notes or reads exists. */
void init();

/** The player is playing `track` (media_content_id) by `artist`
 *  (media_artist). Cheap unless the artist is new to the list, when it blocks
 *  on a Music Assistant search -- so it belongs on the network task. The
 *  track is kept with the artist, as a seed for recommendations. */
void notePlaying(const char* artist, const char* track);

/** An artist was picked from search or the list and sent to the player. It
 *  goes on the list at once, from the item itself -- no search, and no wait
 *  for the player to start and report it -- so the list shows it on the very
 *  next swipe up. Anything but an artist is ignored. Safe from any task. */
void notePicked(const ha::LibraryItem& item);

/** Write the list out if it needs it: at once when an artist new to it was
 *  added, otherwise at most once every config::kHistorySaveIntervalMs. From
 *  the network task, each pass. */
void persist();

/** Find one missing picture again -- after a restart none are stored -- by
 *  the artist search, keeping it only for the same artist. At most one every
 *  config::kHistoryPictureLookupMs, each entry once per boot unless the search
 *  went unanswered. Blocks on the search: the network task, each pass. */
void refreshPictures();

/** Artists on the list, most recent first. */
int count();

/** Copy artist `index` out as a playable library item, and with `track` a
 *  track of theirs that played here (empty when none has since boot). False
 *  when out of range. */
bool at(int index, ha::LibraryItem& out, char* track = nullptr,
        size_t track_len = 0);

/** Changes whenever the list does, so a list built from it knows to rebuild. */
uint32_t generation();

}  // namespace services::history
