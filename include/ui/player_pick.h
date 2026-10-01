#pragma once

#include <cstdint>

/**
 * Choosing the media player on the device, when none is chosen yet: a list
 * of Home Assistant's media players by name, from services::players.
 *
 * One row per player, unavailable ones greyed, and a last row to fetch the
 * list again. Tapping a player chooses it; the app stores it. The volume
 * device and the rest stay in the portal.
 */
namespace ui::player_pick {

enum class Result : uint8_t {
  kNone,     // the tap landed on nothing
  kChosen,   // chosenEntity() was tapped
  kRefresh,  // fetch the list again
};

/** Open on the list as services::players holds it, with `note` in place of
 *  the title when it is not empty -- say, that the list could not be
 *  fetched. */
void open(const char* note);

/** Compose and present the list. */
void draw();

/** Scroll by pixels; positive moves further down. False when the clamp
 *  meant nothing moved. */
bool scrollByPx(int delta);
/** Whether the list is as far as it goes; `direction` negative for the top. */
bool atScrollLimit(int direction);

Result handleTap(int x, int y);

/** The entity id tapped, or "" before one is. */
const char* chosenEntity();

}  // namespace ui::player_pick
