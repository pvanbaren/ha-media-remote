#pragma once

/**
 * The remote, as a pair of calls.
 *
 * main.cpp is an Arduino shim over these two. Everything the device actually
 * does lives in src/app/, which is written against ui/ui.h and knows nothing
 * about the panel it is drawing on.
 */
namespace app {

/** Bring up the display, settings, Wi-Fi and the poll task, in that order. */
void setup();

/** One pass of the input, idle and repaint loop. */
void loop();

}  // namespace app
