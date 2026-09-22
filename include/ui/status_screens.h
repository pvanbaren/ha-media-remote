#pragma once

#include <cstdint>

/** Full-screen messages shown before -- or instead of -- the remote UI.
 *
 *  These compose into ui::canvas() and present, exactly like every other
 *  screen. Drawing straight to `tft` would be simpler and does work on a panel
 *  with a command channel, but an RGB panel has no such channel: there `tft`
 *  is an inert stand-in and the only route to the glass is the frame buffer.
 *  ui::init() creates the canvas before anything here can be called. */

/** Yellow setup card: AP name and the two URLs that reach the portal. */
void statusScreenPortal();
/** Saved network would not come up. */
void statusScreenConnectFailed();
/** Credentials cleared, about to reboot. */
void statusScreenWifiReset();

/** Connect animation. Call Tick repeatedly until the attempt resolves. */
void statusScreenConnectingBegin(const char* ssid);
void statusScreenConnectingTick();

/** Wi-Fi is up but no HA base URL / token is stored yet. */
void statusScreenNeedsHaSetup();
/** HA reachable check failed; `detail` is services::ha::lastError(). */
void statusScreenHaUnreachable(const char* detail);
/** Connected and configured, but no media_player has been chosen. */
void statusScreenNoPlayer();

/** Generic centred card, used by the screens above. */
void statusScreenMessage(const char* title, const char* line1,
                         const char* line2, uint16_t background,
                         uint16_t foreground);
