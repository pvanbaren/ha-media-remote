#pragma once

#include <LovyanGFX.hpp>

namespace ui {

/**
 * The off-screen frame every screen draws into, and the push that puts it on
 * the panel.
 *
 * One whole 240x240 16bpp sprite in PSRAM -- 115 KB -- composed in a single
 * pass and pushed over SPI. Screen coordinates throughout: nothing here takes
 * a y offset, and no drawing code converts between coordinate spaces.
 */

/** Allocate the frame buffer. False if it could not be had, in which case
 *  drawing goes straight to the panel and repaints tear. */
bool canvasInit();

/** False when no buffer could be had at all. */
bool canvasReady();

/** The buffer to draw into. Screen coordinates throughout. */
lgfx::LovyanGFX& canvas();

/** Push the composed frame to the panel. */
void canvasPresent();

/** The panel itself, for the small direct repaints that skip compositing --
 *  a pressed transport button, the elapsed chip. Both draw over an opaque
 *  shape, so they need nothing from the frame underneath. */
lgfx::LovyanGFX& panel();

/** Multiply a rectangle of the composed frame towards black. `alpha` is 0-255
 *  of darkening; 0 is a no-op. Reads and writes the buffer directly, so it
 *  only does anything between drawing and canvasPresent(). */
void dim(int x, int y, int w, int h, uint8_t alpha);

/** As dim(), full width, with alpha ramped from `alpha_top` at row `y` to
 *  `alpha_bottom` at row `y + h - 1`. */
void dimGradient(int y, int h, uint8_t alpha_top, uint8_t alpha_bottom);

}  // namespace ui
