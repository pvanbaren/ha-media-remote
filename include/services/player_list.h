#pragma once

#include "services/ha_client.h"

/**
 * Cached list of the instance's media_player entities.
 *
 * Two readers: the setup portal's dropdown, and the list the device shows
 * when no player has been chosen yet (ui/player_pick.h). Cached so a page
 * reload does not re-ask Home Assistant for something that changes on the
 * order of never.
 */
namespace services::players {

/** Refetch from HA. Blocks on the network. False when the request failed --
 *  any list from a previous refresh is kept, so a blip does not empty the
 *  dropdown. */
bool refresh();

/** Whether the cached list is older than config::kHaPlayerListTtlMs. */
bool stale();

/** Refresh only if stale, then report whether anything is cached. */
bool ensureEntries();

int entryCount();
/** nullptr when `index` is out of range. */
const services::ha::PlayerEntry* entryAt(int index);

}  // namespace services::players
