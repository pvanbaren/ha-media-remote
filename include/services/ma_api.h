#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

/**
 * Music Assistant's own API, for the one thing Home Assistant does not pass
 * through: which artists are like which. Its services cover the library,
 * search and playback, but similarity lives only on the server's API --
 * POST <server>/api {"command": ..., "args": {...}}, with a bearer token made
 * in Music Assistant's own profile settings.
 *
 * The server is usually plain HTTP on the LAN (the add-on listens on port
 * 8095 of the Home Assistant host), so these calls cost no TLS session beside
 * the Home Assistant stream's. Left blank in the portal, the address is
 * derived from the Home Assistant URL: same host, port 8095.
 *
 * The settings are written from the portal on the Arduino loop and read by
 * the recommendation task, so every read copies under a lock.
 */
namespace services::ma {

/** Load the address and token from NVS. Once, in setup(). */
void init();

/** True once a token is stored and there is an address to use it on. */
bool configured();

/** The address as stored: empty means "derived from Home Assistant's". */
const char* storedUrl();
/** The address that will actually be used, into `out`. */
void effectiveUrl(char* out, size_t out_len);
bool hasStoredToken();

/** Persist portal input. An empty url stores "derive it"; an empty token
 *  keeps the stored one. */
void saveSettings(const char* url, const char* token);
/** Forget the token, keeping the address: the list goes back to the
 *  server's sections. */
void clearToken();
void clear();

/** Changes whenever the address or token does. */
uint32_t generation();

/** Run `command` with `args` (a JSON object, "{}" for none). The response
 *  body -- the command's result, directly -- goes into `out`, NUL-terminated.
 *  Returns its length, or -1 on any failure, with the reason in lastError().
 *  Blocks for the round trip: for one background task, not the touch loop. */
int call(const char* command, const String& args, char* out, size_t out_len,
         uint32_t timeout_ms);

const char* lastError();

}  // namespace services::ma
