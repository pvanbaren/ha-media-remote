#pragma once

/**
 * Everything the application asks of a display, and nothing about how any of
 * it looks.
 *
 * This is the seam. Above it, src/app/ runs the remote and decides what
 * should be on screen. Below it, one directory per panel does the drawing.
 * Neither knows the other's pixels.
 *
 * Only one implementation exists so far: src/ui/headless/ draws nothing and
 * narrates to the serial console. A real panel is a sibling directory and a
 * stanza in platformio.ini, with nothing above this file to change.
 *
 * It starts small because the device does: at this point the remote can join
 * a network and run a settings portal, so all a display has to say is which
 * of those is happening.
 */
namespace ui {

/** Bring the panel up. Call once in setup(), before Wi-Fi, while the heap is
 *  unfragmented. False when there is no usable display, in which case
 *  everything below is a no-op and the device still runs. */
bool init();

// --- Status cards ----------------------------------------------------------

/** The captive portal is up; say how to reach it. */
void showPortal();
void showConnecting(const char* ssid);
/** Called repeatedly during a connection attempt, for a progress indicator. */
void tickConnecting();
void showConnectFailed();
void showSettingsCleared();

// --- Panel power -----------------------------------------------------------

bool isBlanked();
void blank();
/** Bring the panel back and force a fresh frame. */
void wake();

}  // namespace ui
