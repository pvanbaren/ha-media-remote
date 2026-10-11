#pragma once

#include <cstdint>

#include <LovyanGFX.hpp>

#include "ui/ui.h"

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
/** Home Assistant answers but refuses the token; a tap links again. */
void statusScreenHaTokenRejected();
/** Home Assistant found: a QR code for `qr_text` (the remote's own sign-in
 *  page), with `name` -- which Home Assistant -- and `url`, the same page to
 *  type instead of scanning. */
void statusScreenHaSignIn(const char* qr_text, const char* url,
                          const char* name);

/** Connected and configured, but no media_player has been chosen. */
void statusScreenNoPlayer();

/** The status page: the device's name, what it controls, then its address,
 *  uptime and link. */
void statusScreenDevice(const ui::DeviceStatus& status);

/** The gear that opens the settings, top right on the title row, where the
 *  status page, the cards that offer it and the player list all put it. */
void statusScreenDrawGear(lgfx::LovyanGFX& gfx, uint16_t color,
                          uint16_t background);
/** Whether (x, y) is on that gear. */
bool statusScreenGearHit(int x, int y);
/** How far in from the chord's edge the gear reaches, with the gap a line
 *  beside it needs: what a centred title on its row gives up on each side. */
int statusScreenGearClearance();

/** What a card on screen offers besides its own tap: a swipe down to the
 *  status page, and the gear to the settings. The cards about something wrong
 *  offer them -- the way to see what, and to change it -- and the passing
 *  ones (loading, connecting, restarting) do not. */
constexpr uint8_t kCardSwipeToStatus = 1;
constexpr uint8_t kCardGearToSettings = 2;
/** The ways the card last drawn offers. */
uint8_t statusScreenCardWays();
/** Something else is on screen now: no card's ways apply. */
void statusScreenForgetCard();

/** Generic centred card, used by the screens above. `ways` says what it
 *  offers besides a tap, and draws the gear when that is among them. */
void statusScreenMessage(const char* title, const char* line1,
                         const char* line2, uint16_t background,
                         uint16_t foreground, uint8_t ways = 0);
