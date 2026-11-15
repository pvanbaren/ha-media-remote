#pragma once

#include <LovyanGFX.hpp>

namespace ui::toast {

/** The toast ui::showToast() asked for, if it is still showing: a pill
 *  over now playing, in the middle of the panel, up to two lines.
 *  Drawn last in a compose, so it is in every frame of now playing for as
 *  long as it shows. */
void draw(lgfx::LovyanGFX& gfx);

}  // namespace ui::toast
