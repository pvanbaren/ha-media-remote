#pragma once

#include <cstdint>

#include "services/ha_client.h"
#include "services/wifi_setup.h"

/**
 * Everything the application asks of a display, and nothing about how any of
 * it looks.
 *
 * This is the seam. Above it, src/app/ runs the remote: it polls Home
 * Assistant, decides what should be on screen, sends the service calls and
 * owns the idle timer. Below it, one directory per panel does the drawing and
 * turns finger movements into the intents above. Neither knows the other's
 * pixels or its entity ids.
 *
 * Two implementations exist. src/ui/round/ drives the 1.28" GC9A01 circle;
 * src/ui/headless/ draws nothing and narrates to the serial console, which is
 * what the firmware links against before any panel is wired up. A third --
 * a 720x720 square, say -- is a sibling directory and a line in
 * platformio.ini, with nothing above this file to change.
 *
 * The input half is the part that earns its keep. A display does its own hit
 * testing, because only it knows where it drew the buttons, and hands back
 * what the gesture *meant*. A round panel reads a swipe round its bezel as a
 * volume change; a square one might use a vertical strip down the side. Both
 * return Intent::kSetVolume, and src/app/ is none the wiser.
 */
namespace ui {

/** Which screen is showing. The app decides, the display draws, and the poll
 *  task reads it to keep a background refresh from landing under someone who
 *  is reading a list. */
enum class Screen : uint8_t {
  kMessage,     // a status card: needs setup, no player, HA unreachable
  kNowPlaying,
  kBrowse,      // the library list
  kSearch,      // artist search, reached from the list
  kStatus,      // the device's own status, a swipe down from now playing
  kWifi,        // choosing a Wi-Fi network and typing its password
  kPlayers,     // choosing the media player, when none is chosen
};

/** What a gesture meant, as opposed to where it landed. */
enum class Intent : uint8_t {
  kNone,
  kPrevious,
  kPlayPause,
  kNext,
  kSetVolume,         // `level` carries it, 0.0-1.0
  kOpenBrowse,
  kOpenSearch,
  kBackToNowPlaying,
  kPlayBrowseRow,     // `index` is a browse row
  kRunSearch,         // the typed query is in services::search
  kPlaySearchResult,  // `index` is a result
  kWokeFromTouch,     // a tap landed on a blanked panel and only woke it
  kOpenStatus,
  kTapCard,           // a tap on a status card, which the app may act on
  kOpenWifi,          // the network list, a swipe left from the status page
  kWifiRescan,        // scan again
  kWifiJoin,          // join wifiChoice()'s network
  kWifiLeave,         // back out of the network list
  kChoosePlayer,      // store playerChosen() as the media player
  kPlayersRefresh,    // fetch the player list again
};

struct Input {
  Intent intent = Intent::kNone;
  float level = 0.0f;
  int index = -1;
};

/** Claim buffers and bring the panel up, turned `rotation` quarter turns
 *  clockwise (0..3). Call once in setup(), before Wi-Fi, while the heap is
 *  unfragmented. False when there is no usable display, in which case
 *  everything below is a no-op and the remote still runs.
 *
 *  Every screen is laid out the same way at any rotation -- the panels are
 *  square -- so the turn is entirely below this line. */
bool init(uint8_t rotation);

/** Service touch, drags and glides, and report what the finger asked for.
 *  Called every loop iteration. `state` is the current snapshot, or nullptr
 *  when there is none yet -- a volume gesture needs to know where the level
 *  started. */
Input poll(Screen screen, const services::ha::PlayerState* state);

// --- What to show ----------------------------------------------------------

void showNowPlaying(const services::ha::PlayerState& state);
/** The library list. The app has already made sure it is loaded. */
void showBrowse();
/** The artist search, on an empty query. */
void showSearch();

/** What the status page shows. The app gathers it, the display lays it out. */
struct DeviceStatus {
  const char* hostname = "";
  bool connected = false;
  const char* ip = "";
  unsigned long uptime_s = 0;
  const char* ssid = "";
  int channel = 0;
  int rssi = 0;
  float tx_dbm = 0.0f;
  /** The media player's name; empty when none is chosen. */
  const char* player = "";
  /** The Volume & power device's name, empty when that is the player. */
  const char* control = "";
  /** The player's input on that device as chosen in the portal; empty when it
   *  is the input named after the player. Shown in the player's place. */
  const char* input = "";
};
/** The status page, or its next refresh: it is redrawn whole each time. */
void showStatus(const DeviceStatus& status);
/** The network list, scanning, with `note` in place of its title when not
 *  empty and `current` marked as the network in use. */
void showWifi(const char* note, const char* current);
/** What the scan found, onto the list. */
void showWifiNetworks(const WifiNetwork* networks, int count);
/** The network chosen and the password typed for it ("" for an open one). */
const char* wifiChosenSsid();
const char* wifiPassword();

/** The media players services::players holds, to choose one from, with
 *  `note` in place of the title when it is not empty. */
void showPlayers(const char* note);
/** The entity id of the player tapped. */
const char* playerChosen();

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
/** The boot card, naming whatever is being waited on. */
void showLoading(const char* what);

/** Light whichever control corresponds to `intent` for the duration of a
 *  round trip. The call itself is the press feedback, so nothing has to be
 *  timed; the app brackets its service call with pending true then false. */
void showCommandPending(Intent intent, bool pending);

/** Status cards. */
void showNeedsHaSetup();
/** Home Assistant found and waiting for a sign-in: a QR code saying
 *  `qr_text`, the remote's sign-in page, with `url` to type instead and the
 *  server's `name`. */
void showHaSignIn(const char* qr_text, const char* url, const char* name);
void showNoPlayer();
void showHaUnreachable(const char* detail);
void showPortal();
void showConnecting(const char* ssid);
void tickConnecting();
void showConnectFailed();
void showSettingsCleared();

/** Cheap partial repaints that skip a full compose. */
void refreshElapsed(const services::ha::PlayerState& state);

// --- Panel power -----------------------------------------------------------

bool isBlanked();
void blank();
/** Bring the panel back and force a fresh frame. */
void wake();

// --- Artwork ---------------------------------------------------------------

/** Ask for the art at `picture` -- an entity_picture path or an absolute
 *  URL -- to be fetched and cached on the artwork worker. Returns at once, so
 *  the poll task can go straight back to the state stream; cheap to repeat. */
void requestArtwork(const char* picture);
/** True while that picture is still being fetched: a track change's repaint
 *  waits on it, up to config::kCoverArtHoldMs. */
bool artworkPending(const char* picture);
/** True once after a fetch finishes, for the repaint that puts it on screen. */
bool takeArtworkFinished();
/** Drop it, when what is playing is about to change. */
void clearArtwork();

/** Housekeeping for the list screens, once per loop pass: queue the
 *  thumbnails a screen wants (fetched on a worker, so this never blocks on
 *  the network) and repaint when some that are on screen have arrived.
 *  Returns true when it repainted. */
bool idleWork(Screen screen);

/** millis() of the last touch the panel saw -- down, move or release -- or 0
 *  if there has been none. For timeouts that should wait on a person rather
 *  than on the clock. */
unsigned long lastInteractionMs();

/** True while a finger is dragging the volume. The level on screen is then the
 *  finger's, and a state update must not move it or repaint over it. */
bool volumeDragging();

}  // namespace ui
