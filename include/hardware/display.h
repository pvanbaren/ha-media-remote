#pragma once

#include "hardware/lgfx_config.hpp"

extern LGFX tft;

/** Bring the panel up turned `rotation` quarter turns clockwise (0..3). */
void displayInit(uint8_t rotation);

/** The rotation displayInit() was given. */
uint8_t displayRotation();

/** Whether the panel turns the picture itself.
 *
 *  An SPI controller does, in its address mode, and LovyanGFX turns its
 *  touch readings to match; everything above then works in turned
 *  coordinates without knowing. An RGB panel scans a framebuffer out in one
 *  fixed order, so there the upright frame is turned as it is copied into
 *  that buffer (displayPresentFrame), and a touch is turned back -- see
 *  hw::mapPoint(). Either way the canvas itself is always drawn upright. */
bool displayRotatesItself();

/** Clear the panel, cut the backlight and put the controller to sleep.
 *
 *  Without a backlight pin wired the panel still goes black and stops
 *  scanning, which is most of the saving; the lamp itself stays lit. Nothing
 *  should be drawn while blanked -- a sleeping controller ignores it. */
void displayBlank();

/** Wake the controller and restore the backlight. The caller has to redraw:
 *  panel RAM is not trustworthy across a sleep. */
void displayWake();

bool displayIsBlanked();

/**
 * Put a rectangle of a composed frame on the glass.
 *
 * The frame is upright and the rectangle is in its coordinates; a panel that
 * does not turn the picture itself turns it here, on the way through.
 *
 * `frame` points at the whole frame, not at the rectangle, so implementations
 * walk it with the panel width as the stride. That is what lets a caller
 * repaint a volume bar without pushing the entire screen -- which matters a
 * great deal more at 720x720 than at 240x240.
 *
 * This is the one place the bus shows through. An SPI panel is written over
 * the wire; an RGB panel is a framebuffer that is already being scanned out,
 * so the same call is a memcpy.
 */
void displayPresentFrame(const uint16_t* frame, int x, int y, int w, int h);

/**
 * Move rows [y, y + h) of what is on the glass by `dy` pixels, positive
 * down, without being handed a frame: the pixels are the panel's own. In the
 * upright frame's coordinates, like displayPresentFrame().
 *
 * Only a panel that keeps its frame somewhere the CPU can reach can do this,
 * which here means the RGB panel and its framebuffer. The SPI panels hold
 * theirs in the controller, behind a bus that only writes, and return false;
 * so does any panel that is not up. The rows the move uncovers are the
 * caller's to present. Columns [keep_x, keep_x + keep_w) are left where they
 * are. False, too, where the picture is turned a quarter by the copy: the
 * rows to move are then the glass's columns.
 */
bool displayScrollFrame(int y, int h, int dy, int keep_x = 0, int keep_w = 0);
