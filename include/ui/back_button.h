#pragma once

#include <LovyanGFX.hpp>

/**
 * The back button, top left on the status page, the settings and the pages
 * under them: a tap on it does what a swipe right does.
 *
 * On the title row, at its left end. A square panel has its corner there; a
 * round one has no corner, and the left end of the title row is as near to
 * one as the glass goes while still leaving the button whole.
 */
namespace ui::back_button {

void draw(lgfx::LovyanGFX& gfx);

/** Whether (x, y) is on the button, generously. */
bool hit(int x, int y);

/** Where a line `text_h` tall at `y` can go without running into the button:
 *  the centre to draw it on, and how wide it may be. The panel's centre and
 *  the full usable width where the line is clear of it. */
void lineSpan(int y, int text_h, int& center_x, int& width);

}  // namespace ui::back_button
