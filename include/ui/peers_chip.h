#pragma once

#include <LovyanGFX.hpp>

#include "services/ha_client.h"

/**
 * The "+2 rooms" chip on now playing: shown while other zones of the volume
 * device's integration are on the same input as this room's, and a tap on it
 * offers to isolate this room (Intent::kIsolate). Drawn at the top, between
 * the volume and the artist line, by both now playing layouts.
 *
 * In its place, in amber, "Off in this room" while the player is playing but
 * this room's separate volume device is off -- heard elsewhere, or by no one
 * -- so now playing does not look as it does when the room is listening.
 * That one is not a hit target: a tap there is the top half's, which
 * switches the volume device on.
 */
namespace ui::peers_chip {

/** Draw the chip if `state` has other rooms on its input, or "Off in this
 *  room" if its volume device is off while it plays; remember where. */
void draw(lgfx::LovyanGFX& gfx, const services::ha::PlayerState& state);

/** Whether (x, y) is on the rooms chip as last drawn -- false when none
 *  was, or when the chip says "Off in this room". */
bool hit(int x, int y);

}  // namespace ui::peers_chip
