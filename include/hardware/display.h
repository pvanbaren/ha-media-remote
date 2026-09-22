#pragma once

#include "hardware/lgfx_config.hpp"

extern LGFX tft;

void displayInit();

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
