#pragma once

#include <cstddef>

/**
 * Linking the remote to Home Assistant without typing a URL or a token.
 *
 * Home Assistant announces itself on the LAN over mDNS, so the URL can be
 * found. A token cannot be had without someone signing in, and Home Assistant
 * signs people in on its own page, so the remote hands that to a phone: it
 * shows a QR code for a page of its own, `/ha`, which sends the browser on to
 * Home Assistant's sign-in page with the remote as the OAuth client. Home
 * Assistant then sends the browser back to `/ha_auth` with a one-time code,
 * and the remote takes it from there -- the code for a short session, the
 * session for a long-lived token named after the remote, and the short
 * session revoked so it does not linger in the user's profile. Along the way
 * it looks up the Music Assistant config entry, if one is not stored.
 *
 * Nothing here draws or blocks the display for long: discover() and
 * complete() each take a few seconds and are called from the loop, with a
 * card up first.
 */
namespace services::ha_link {

/** Serve `/ha` and `/ha_auth` from the portal's web server. Call once in
 *  setup(), before Wi-Fi comes up. */
void init();

/** Find Home Assistant: the URL already stored if there is one, else an mDNS
 *  query, which takes up to about three seconds. True when a URL is known --
 *  and then ready() too. */
bool discover();

/** True once a URL is known and the QR code can go up. */
bool ready();

/** This remote's own sign-in page, `/ha`, to type into a browser. Empty
 *  until ready(). */
const char* pageUrl();
/** The same page as the QR code says it: in capitals, which a QR code holds
 *  in its alphanumeric mode -- the smallest code there is for an address
 *  this long, so the coarsest on a small panel. Browsers lower-case the
 *  scheme and host, and `/HA` is served too. */
const char* qrText();
/** Home Assistant's name for itself (its location name), or its URL. */
const char* serverName();

/** True when a browser has come back with a code that complete() has yet to
 *  use. */
bool codeArrived();

/** Trade the code for a long-lived token and store it with the URL. Blocks
 *  for the round trips. False with the reason in lastError(). */
bool complete();

const char* lastError();

/** Forget the URL found, so the next discover() looks again. */
void reset();

}  // namespace services::ha_link
