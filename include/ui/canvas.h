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

/** Push just the rectangle a partial repaint touched. Cheap where a full
 *  present is not: at 720x720 the whole frame is a megabyte. */
void canvasPresentRegion(int x, int y, int w, int h);

/** Move rows [y, y + h) of the panel by `dy`, positive down, leaving the
 *  canvas alone. False when the panel cannot -- see displayScrollFrame() --
 *  in which case nothing moved and the caller repaints instead.
 *
 *  Afterwards the panel and the canvas disagree over those rows: the panel
 *  holds the moved picture, the canvas the old one. A caller that uses this
 *  owns putting that right, by presenting only what it has composed at the
 *  new positions until it next composes and presents the rows whole. */
bool canvasScrollPanel(int y, int h, int dy, int keep_x = 0, int keep_w = 0);

/** How many times anything has been put on the panel: every present, every
 *  scroll. A caller that wants to build on what it last drew there -- a
 *  scroll moves the pixels already on the glass -- compares this with what it
 *  was after its own last write, and a difference means someone else has
 *  drawn over them since. */
uint32_t canvasPanelWrites();

/** False when a move is ruled out before it is tried -- no frame, or a quarter
 *  turn, which puts a list's rows down the framebuffer's columns -- so a
 *  caller can find out before composing the strips the move would need. A
 *  panel that cannot move its frame at all still only says so from
 *  canvasScrollPanel(). */
bool canvasCanScroll();

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
