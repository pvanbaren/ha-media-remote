#pragma once

#include "services/ha_client.h"

/**
 * Cached list of the instance's media_player entities.
 *
 * This is not a screen. The player is chosen in the **setup portal**, not on
 * the device: a dropdown in a browser is a better list than forty rows on a
 * 240 px circle, and it is reachable when the panel is not. The list lives
 * here so the portal can render it, and so a page reload does not re-ask Home
 * Assistant for something that changes on the order of never.
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
