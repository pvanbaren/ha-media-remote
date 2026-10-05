#pragma once

#include <LovyanGFX.hpp>

#include "services/ha_client.h"

/**
 * The "+2 rooms" chip on now playing: shown while other zones of the volume
 * device's integration are on the same input as this room's, and a tap on it
 * offers to isolate this room (Intent::kIsolate). Drawn at the top, between
 * the volume and the artist line, by both now playing layouts.
 */
namespace ui::peers_chip {

/** Draw the chip if `state` has other rooms on its input; remember where. */
void draw(lgfx::LovyanGFX& gfx, const services::ha::PlayerState& state);

/** Whether (x, y) is on the chip as last drawn -- false when none was. */
bool hit(int x, int y);

}  // namespace ui::peers_chip
