#pragma once

#include <cstdint>

#include "services/wifi_setup.h"

/**
 * Choosing a Wi-Fi network on the device: a list of what a scan found, and a
 * keyboard for the password.
 *
 * Two pages. The list shows each network once, strongest first, with its
 * signal and a lock where it wants a password; tapping an open one joins it,
 * tapping a locked one opens the password page. That page is a phone's
 * keyboard -- letters with a shift, then pages of digits and symbols -- since
 * a password can hold any printable character, and JOIN where the search
 * screen has SEARCH.
 *
 * Nothing here scans or joins: the app asks services/wifi_setup for both and
 * hands the results in, so the screen keeps answering while the radio works.
 */
namespace ui::wifi_join {

enum class Result : uint8_t {
  kNone,     // the tap landed on nothing
  kChanged,  // something on screen changed; draw() it
  kJoin,     // join chosenSsid() with password()
  kRescan,   // scan again
};

/** Open on the list, with `note` (say, why the last join failed) in place of
 *  the title when it is not empty, and the network named `current` marked
 *  as the one in use. Shows "Scanning" until setNetworks(). */
void open(const char* note, const char* current);

/** What a scan found, strongest first. Copied. */
void setNetworks(const WifiNetwork* networks, int count);

/** True on the list, false on the password page. */
bool onList();

/** Back one page: password to list. False when already on the list. */
bool back();

/** Compose and present whichever page is showing. */
void draw();

/** Scroll the list by pixels; positive moves further down. False when the
 *  clamp meant nothing moved. */
bool scrollByPx(int delta);
/** Whether the list is as far as it goes; `direction` negative for the top. */
bool atScrollLimit(int direction);

Result handleTap(int x, int y);

/** The network chosen, and what was typed for it -- empty for an open one. */
const char* chosenSsid();
const char* password();

}  // namespace ui::wifi_join
