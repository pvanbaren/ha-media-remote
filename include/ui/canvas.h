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

/** The scrim over cover art: the whole frame dimmed by `base_alpha`, and from
 *  row `ramp_top` down a ramp from nothing to `ramp_alpha` on top of it --
 *  dim() then dimGradient(), in one pass over the frame rather than two, and
 *  only across the glass: on a round panel the corners are skipped. */
void scrim(uint8_t base_alpha, int ramp_top, uint8_t ramp_alpha);

/** The frames a now playing compose keeps for its next one: the cover art
 *  under its scrim, which changes with the cover, and that with the track's
 *  title and its glow drawn on, which changes with the text too. Between
 *  them, a repaint for a play, a pause or a volume step starts from a copy
 *  rather than a decode and a blur, and a radio title changing over the same
 *  cover redraws only the text. */
enum class Backdrop : uint8_t {
  kArt,
  kArtAndText,
};
/** Keep a copy of the frame as it stands, as `which`, showing what `key`
 *  names. In PSRAM, a frame apiece, claimed on first use and kept. */
void canvasSaveBackdrop(Backdrop which, uint32_t key);
/** Put `which` back into the frame, if it is the one `key` names. False,
 *  leaving the frame alone, when what is kept is some other one or nothing. */
bool canvasRestoreBackdrop(Backdrop which, uint32_t key);

/** The frame's pixels, when `gfx` is the canvas, laid out as
 *  canvasColorDepth() says; nullptr for anything else. For filling the frame
 *  faster than drawing calls can -- the cover art's upscale. */
uint16_t* canvasPixels(const lgfx::LGFXBase& gfx);
/** How the frame stores a pixel, so that a sprite set up the same way can be
 *  copied into it a pixel at a time without converting anything. */
lgfx::color_depth_t canvasColorDepth();

/** As dim(), with each pixel's alpha from `mask`: `w` x `h` bytes, row-major,
 *  laid over the frame at (x, y) in drawing coordinates. What a soft shadow
 *  under text is made of -- the text's own shape, blurred. */
void dimMask(int x, int y, int w, int h, const uint8_t* mask);

}  // namespace ui
