#pragma once

#include <ctime>

#include "services/ha_client.h"

/**
 * The dimmed clock face: the time, large and dim, and what is playing under
 * it, on black. What now playing rests as once nobody has touched it for
 * config::kDimAfterMs -- mostly unlit pixels, which on an AMOLED is most of
 * the saving. Shown and dismissed through ui::dim() and ui::poll().
 */
namespace ui {
namespace clock_face {

/** The local time, once SNTP has set the clock; false before then. */
bool now(struct tm& out);

/** Compose the face for `local` and present it. `state` may be null, and
 *  without a title only the time is drawn. */
void draw(const services::ha::PlayerState* state, const struct tm& local);

}  // namespace clock_face
}  // namespace ui
