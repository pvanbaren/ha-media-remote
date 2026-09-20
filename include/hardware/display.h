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
