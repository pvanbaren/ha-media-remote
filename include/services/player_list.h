#pragma once

#include <cstdint>

#include "services/ha_client.h"

/**
 * Cached list of the instance's media_player entities.
 *
 * Two readers: the setup portal's dropdowns, and the lists the device shows
 * -- the one when no player has been chosen yet (ui/player_pick.h) and the
 * settings page's. Cached so a page reload does not re-ask Home Assistant
 * for something that changes on the order of never.
 *
 * The media player offers every player. Volume & power offers the devices
 * whose volume can be set, those with inputs first, and the one already
 * chosen (see ha::fetchPlayers()). That depends on the device chosen, so a
 * list fetched for another device than the one stored now counts as
 * stale.
 */
namespace services::players {

/** Which list is asking. */
enum class Role : uint8_t {
  /** The media player. */
  kPlayer,
  /** Volume & power: the devices whose volume can be set
   *  (ha::kVolumeDeviceFeatures), those with inputs first, and the one
   *  already chosen. */
  kControl,
};

/** Refetch from HA. Blocks on the network. False when the request failed --
 *  any list from a previous refresh is kept, so a blip does not empty the
 *  dropdown. */
bool refresh();

/** Whether the cached list is older than config::kHaPlayerListTtlMs, or was
 *  fetched for another Volume & power device than the one stored now. */
bool stale();

/** Refresh only if stale, then report whether anything is cached. */
bool ensureEntries();

/** How many entries `role`'s list offers. */
int entryCount(Role role);
/** Entry `index` of `role`'s list; nullptr when out of range. Indices are
 *  stable for as long as the cache and the stored volume device are. */
const services::ha::PlayerEntry* entryAt(Role role, int index);

}  // namespace services::players
