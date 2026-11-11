#pragma once

#include <LovyanGFX.hpp>

namespace ui::toast {

/** The toast ui::showToast() asked for, if it is still showing: a pill
 *  over now playing -- in the middle of a round panel, up to two lines, and
 *  at the rooms chip's height on a square one.
 *  Drawn last in a compose, so it is in every frame of now playing for as
 *  long as it shows. */
void draw(lgfx::LovyanGFX& gfx);

}  // namespace ui::toast
