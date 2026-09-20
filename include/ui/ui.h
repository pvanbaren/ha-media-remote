#pragma once

#include <cstdint>

#include "services/ha_client.h"

/**
 * Everything the application asks of a display, and nothing about how any of
 * it looks.
 *
 * This is the seam. Above it, src/app/ runs the remote: it polls Home
 * Assistant, decides what should be on screen, sends the service calls and
 * owns the idle timer. Below it, one directory per panel does the drawing and
 * turns finger movements into the intents below. Neither knows the other's
 * pixels or its entity ids.
 *
 * Only src/ui/headless/ implements it so far -- it draws nothing and narrates
 * to the serial console. A real panel is a sibling directory and a stanza in
 * platformio.ini, with nothing above this file to change.
 *
 * The input half is the part that earns its keep. A display does its own hit
 * testing, because only it knows where it drew the buttons, and hands back
 * what the gesture *meant*. A round panel might read a swipe round its bezel
 * as a volume change where a square one uses a strip down the side; both
 * return Intent::kSetVolume, and src/app/ is none the wiser.
 */
namespace ui {

/** Which screen is showing. The app decides and the display draws. */
enum class Screen : uint8_t {
  kMessage,  // a status card: needs setup, no player, HA unreachable
  kNowPlaying,
  kBrowse,   // the library list
  kSearch,   // artist search, reached from the list
};

/** What a gesture meant, as opposed to where it landed. */
enum class Intent : uint8_t {
  kNone,
  kPrevious,
  kPlayPause,
  kNext,
  kSetVolume,        // `level` carries it, 0.0-1.0
  kOpenBrowse,
  kOpenSearch,
  kBackToNowPlaying,
  kPlayBrowseRow,     // `index` is a services::browse item
  kRunSearch,         // the typed query is in services::search
  kPlaySearchResult,  // `index` is a result
  kWokeFromTouch,     // a tap landed on a blanked panel and only woke it
};

struct Input {
  Intent intent = Intent::kNone;
  float level = 0.0f;
  int index = -1;
};

/** Bring the panel up. Call once in setup(), before Wi-Fi, while the heap is
 *  unfragmented. False when there is no usable display, in which case
 *  everything below is a no-op and the remote still runs. */
bool init();

/** Service touch and drags, and report what the finger asked for. Called
 *  every loop iteration. `state` is the current snapshot, or nullptr when
 *  there is none yet -- a volume gesture needs to know where the level
 *  started. */
Input poll(Screen screen, const services::ha::PlayerState* state);

// --- What to show ----------------------------------------------------------

void showNowPlaying(const services::ha::PlayerState& state);
/** The library list. The app has already made sure it is loaded. */
void showBrowse();
/** The artist search, on an empty query. */
void showSearch();
/** Results for whatever services::search now holds. */
void showSearchResults();
/** A frame before a blocking call, so the panel does not simply stop. */
void showSearching();
void showLoadingList();
/** The boot card, naming whatever is being waited on. */
void showLoading(const char* what);

/** Light whichever control corresponds to `intent` for the duration of a
 *  round trip. The call itself is the press feedback, so nothing has to be
 *  timed; the app brackets its service call with pending true then false. */
void showCommandPending(Intent intent, bool pending);

/** Status cards. */
void showNeedsHaSetup();
void showNoPlayer();
void showHaUnreachable(const char* detail);
void showPortal();
void showConnecting(const char* ssid);
void tickConnecting();
void showConnectFailed();
void showSettingsCleared();

/** A cheap partial repaint that skips a full compose. */
void refreshElapsed(const services::ha::PlayerState& state);

// --- Panel power -----------------------------------------------------------

bool isBlanked();
void blank();
/** Bring the panel back and force a fresh frame. */
void wake();

// --- Artwork ---------------------------------------------------------------

/** Fetch and cache the art at `picture` -- an entity_picture path or an
 *  absolute URL. Called from the poll task, off the drawing path. */
void prepareArtwork(const char* picture);
/** Drop it, when what is playing is about to change. */
void clearArtwork();

/** Work the display would like to do when nothing else is happening: one
 *  thumbnail fetch per call. True when it did something, so the caller knows
 *  there may be more. Never called while a finger is down. */
bool idleWork(Screen screen);

}  // namespace ui
