#pragma once

#include <cstdint>

/**
 * The RGB output stage on the Adafruit Qualia: an esp_lcd panel streaming the
 * S3's LCD peripheral over 16 data lines.
 *
 * **One framebuffer, no bounce buffer**, and both are choices.
 *
 * One framebuffer because it always holds the frame on the glass, which is
 * what lets rgbScroll() move a scrolled list's rows in place. A second
 * buffer would always be a flip behind, so every step of a list scroll would
 * be a recompose and a 624-row copy -- and bandwidth is what this panel is
 * short of.
 *
 * So every repaint writes the buffer being scanned, and what is left is
 * damage control, all of it in rgb.cpp: copy only the rectangle that changed
 * rather than the frame, stand off the bus between slabs of a large copy or
 * move so the scan-out can refill its FIFO, and let the UI ration how often
 * it asks. The every-VSYNC restart pulls the picture back if a burst still
 * gets through.
 *
 * No bounce buffer, because it is insurance against a problem this panel
 * does not have, and it creates one it does. It puts the CPU in the scan-out
 * path on a hard deadline, so a stall shows up as a glitch.
 *
 * Call expanderInit() first: the panel wants its reset released before pixels
 * start arriving.
 */
namespace hw::qualia {

/** Start the RGB panel and claim its framebuffer. False on failure, in which
 *  case rgbPresent() does nothing and the remote runs blind. */
bool rgbInit();

/**
 * Copy a rectangle of a composed frame into the framebuffer being scanned.
 *
 * `src` points at the whole 720x720 frame, not at the rectangle, so the copy
 * walks it a row at a time with the full width as its stride. That is what
 * lets a caller repaint a 120-pixel-tall volume bar for about a millisecond
 * instead of pushing a megabyte -- which matters more here than it would with
 * a spare buffer to hide in, because every one of these rows is being read by
 * the DMA while it is written.
 *
 * Returns once the rows are copied and their cache lines flushed. The panel
 * shows them whenever the scan reaches them, which may already have happened.
 */
void rgbPresent(const uint16_t* src, int x, int y, int w, int h);

/** Flood the screen with one colour. Blanking needs this: there is no
 *  controller to put to sleep, so a dark screen means a dark framebuffer.
 *  Separate from rgbPresent() because that one reads `src` as a full frame and
 *  there is no frame to read here. */
void rgbFill(uint16_t colour);

/**
 * Move rows [y, y + h) of the framebuffer by `dy` pixels -- positive is down
 * -- in place, without reading the composed frame at all.
 *
 * For a list that has scrolled and otherwise not changed: the rows already on
 * the glass are the rows it wants, only elsewhere. The |dy| rows the move
 * uncovers keep whatever they held; the caller presents what belongs there.
 * Rows outside the range are untouched, and so are columns
 * [keep_x, keep_x + keep_w): something drawn there that does not move with
 * the rows -- a scroll bar, whose thumb goes the other way -- stays put until
 * the caller presents its new state, rather than being carried off in the
 * wrong direction first.
 *
 * False, doing nothing, when there is no framebuffer or nothing would survive
 * the move.
 */
bool rgbScroll(int y, int h, int dy, int keep_x = 0, int keep_w = 0);

}  // namespace hw::qualia
