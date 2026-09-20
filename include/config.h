#pragma once

#include <cstddef>
#include <cstdint>

/**
 * How the remote behaves, not what it runs on.
 *
 * Nothing here knows the panel size, the pins or the touch controller -- those
 * are in board/, included only by src/ui/ and src/hardware/. Everything in
 * this file is equally true of a round 240 px panel and a square 720 px one.
 */
namespace config {

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "MediaRemote-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** Device name until one is set in the portal: the hostname, and the mDNS
 *  host (no ".local" suffix) -- http://media-remote.local. See
 *  services::device::name(). */
constexpr char kPortalHostname[] = "media-remote";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// Touch is the primary input; BOOT only exists as the Wi-Fi/HA escape hatch,
// so it has no short-tap action. The pin itself is in board/.
constexpr unsigned long kBootResetHoldMs = 3000UL;

// =====================================================================
// Home Assistant
// =====================================================================
/** Base URL and long-lived token are entered in the Wi-Fi portal and kept in
 *  NVS; these are only the field limits. */
constexpr size_t kHaBaseUrlMaxLen = 96;
constexpr size_t kHaTokenMaxLen = 220;
constexpr size_t kEntityIdMaxLen = 64;

/** How long a player may report no title before the screen says so --
 *  "Nothing playing", "Idle". A track change often passes through a second
 *  or so with no title at all, and a label flashing up for that gap reads as
 *  the music stopping; until this has passed the title area is left blank. */
constexpr unsigned long kUntitledLabelDelayMs = 3000;
constexpr size_t kFriendlyNameMaxLen = 40;

/** Music Assistant config entry id, entered in the portal. `get_library` is
 *  addressed by config entry rather than by entity -- it asks the *server*
 *  what is in the library, not a player what it is doing -- so without this
 *  there are no stations to show. Settings -> Devices & Services -> Music
 *  Assistant; it is the long id in the URL. */
constexpr size_t kMaConfigEntryIdMaxLen = 40;

// =====================================================================
// The swipe-up list
// =====================================================================
/** One section of the list: a heading and the Music Assistant library query
 *  that fills it. */
struct BrowseSection {
  const char* title;
  /** artist, album, playlist, radio or track. */
  const char* media_type;
  /** An integration sort key: last_played_desc, play_count_desc, random,
   *  timestamp_added_desc, name... or nullptr for its default. */
  const char* order_by;
  int limit;
};

/** What the swipe-up list shows, top to bottom.
 *
 *  A note on "Recommended", since the name is a promise this cannot quite
 *  keep. Music Assistant does have real recommendations -- the rows on its own
 *  Home page -- but they live behind its websocket API on port 8095 and are
 *  not exposed as a Home Assistant service, so nothing reachable over the REST
 *  API can ask for them. What `get_library` does offer is `order_by`, and a
 *  random draw from the albums is a decent stand-in: it surfaces things the
 *  library has and the last few weeks did not. `play_count_desc` is the other
 *  honest option, if "what we actually play" suits better than "something
 *  else for a change".
 *
 *  "Recent artists" needs no such apology: last_played_desc over artists is
 *  exactly what it says.
 *
 *  Sections cost one request each when the list is opened, so this table is
 *  the network cost as well as the layout. */
constexpr BrowseSection kBrowseSections[] = {
    {"Recent artists", "artist", "last_played_desc", 5},
    {"Recommended", "album", "random", 5},
};
constexpr int kBrowseSectionCount =
    static_cast<int>(sizeof(kBrowseSections) / sizeof(kBrowseSections[0]));

/** Items held across all sections. Each costs a name, a URI, an image URL and
 *  a decoded thumbnail, so this is the real cost knob here. It caps the total,
 *  not the per-section limits above. */
constexpr int kMaxBrowseItems = 10;




/** How long the list stays fresh before a reopen refetches it. Short enough
 *  that "recent artists" means it, and that a random "Recommended" draw is
 *  actually a different draw next time. */
constexpr unsigned long kBrowseTtlMs = 300000;

/** Most media_player entities a picker will hold. Entities past this are
 *  dropped from the list and become unselectable, so it has to clear the real
 *  count: a house with Chromecasts, a receiver's zones and a Music Assistant
 *  bridge runs to dozens. ~110 bytes each, so this array is ~10 KB. */
constexpr size_t kMaxPlayers = 96;

/** Player controlled until one is chosen in the portal. Whatever the portal
 *  stores in NVS wins from then on; clearing settings (BOOT held 3 s) falls
 *  back here. Empty = no default: the No player card, pointing at the portal,
 *  until one is chosen. A build for one particular house can name its player
 *  here to land on now playing straight after setup. */
constexpr char kDefaultPlayerEntityId[] = "";

/** Entity carrying volume and power until one is chosen in the portal. Empty
 *  means the player above does both, which is the ordinary case. */
constexpr char kDefaultControlEntityId[] = "";

/** Power the volume/power entity on when a tap wakes a blanked panel.
 *
 *  The gesture that means "I want this" already exists, and on a separate
 *  amplifier or receiver there is nothing to hear until it is switched on, so
 *  reaching for the screen may as well do it. Only a *tap* on a blanked panel
 *  does this -- waking because playback resumed does not, since whatever
 *  started it clearly did not need the help.
 *
 *  The off half lives in kTurnOffControlOnBlank, on the same idle timer. */
constexpr bool kTurnOnControlOnWake = true;

/** When powering on a separate volume/power entity that lists the player as
 *  one of its sources -- a zone amplifier such as a Triad, by the player's
 *  own name -- select that source too. Turning a zone on routes nothing to
 *  it; without this it stays silent until the player happens to play. Play
 *  pressed on the remote selects it even over another source. */
constexpr bool kSelectControlSource = true;

/** How much the serial console says, from least to most. Each level includes
 *  the ones before it. See log.h. */
enum class LogLevel : uint8_t {
  kNone = 0,
  /** Something is broken: a panel that did not come up, a template Home
   *  Assistant rejected, memory that could not be had. */
  kError,
  /** Something failed that the firmware works around: a dropped request, a
   *  stream that closed early, a cover too big to cache. */
  kWarn,
  /** What the device is doing, once per thing worth knowing: boot, settings,
   *  the stream opening, a command, an artist noted. */
  kInfo,
  /** Every request, every state change, every repaint decision: for chasing
   *  something down. A playing player makes this a line a second or more. */
  kDebug,
  /** Anything finer still. */
  kVerbose,
};

/** The console's level, fixed at build time: everything below it is compiled
 *  out, arguments and all, so a quiet build pays nothing for the lines it
 *  does not print. kDebug adds a line per HTTP request and per state change. */
constexpr LogLevel kLogLevel = LogLevel::kInfo;

/** Hold one TLS connection to Home Assistant open across requests instead of
 *  handshaking per call.
 *
 *  A fresh WiFiClientSecure per request means a full handshake every poll --
 *  every 2 s while playing -- costing hundreds of milliseconds of CPU and
 *  churning mbedTLS's 16 KB in and 16 KB out buffers through the heap each
 *  time. Keeping the connection means those are allocated once, early, and
 *  never returned to fragment anything.
 *
 *  It holds ~35 KB permanently rather than in bursts. That is the point: the
 *  failures on this board come from contiguity, and memory claimed once at the
 *  start is memory that cannot fragment later. */
constexpr bool kHaKeepAlive = true;

/** Volume to set on the *media player* when it wakes up, and only when a
 *  separate entity carries volume and power.
 *
 *  When the two are the same thing this is meaningless and is skipped: the
 *  volume the user last chose is the volume they want back.
 *
 *  When they differ, the player's own volume_level is not a volume at all --
 *  it is the gain on the signal being handed to an amplifier that has its own
 *  knob. Whatever that gain happens to be when the player wakes is arbitrary,
 *  and if it drifted low once, every later adjustment fights it: the amplifier
 *  gets turned up to compensate for a quiet source, and the next track through
 *  a correctly-set player is deafening. Pinning it to one known value on the
 *  way up makes the amplifier's setting mean something from one session to the
 *  next.
 *
 *  0.70 rather than 1.00 because a little headroom costs nothing audible and
 *  leaves room for a player that applies replaygain above unity. Negative
 *  disables it. */
constexpr float kPlayerWakeVolume = 0.70f;

constexpr uint16_t kHaHttpTimeoutMs = 6000;

/** Longer limit for the calls that do real work on the server rather than
 *  just reading state.
 *
 *  music_assistant.play_media on an *artist* has to resolve that artist's
 *  tracks and build a queue before it answers, and it answers only when that
 *  is done. Measured on a live instance: 4.3 s for one artist and past 6 s for
 *  another, so the ordinary timeout turned a working request into a failure
 *  the device then retried -- doubling the load on a server that was already
 *  busy doing the thing it had been asked to do. */
constexpr uint16_t kHaServiceTimeoutMs = 25000;
/** State poll cadence while the selected player is playing. */
constexpr unsigned long kHaPollPlayingMs = 2000;
/** ...and while it is paused, idle, or off. */
constexpr unsigned long kHaPollIdleMs = 8000;
/** Re-poll this soon after a transport press so the UI catches up. */
constexpr unsigned long kHaPollAfterCommandMs = 400;
/** Slowest rate volume_set is sent while the finger is dragging. The arc
 *  follows the finger locally regardless, so this only paces the audio. */
constexpr unsigned long kVolumeSendIntervalMs = 250;
/** How long the entity list stays fresh before the picker refetches it. */
constexpr unsigned long kHaPlayerListTtlMs = 300000;

/** How long the player has to stay idle before the device stands down: the
 *  panel blanks and, if kTurnOffControlOnBlank is set, the volume/power entity
 *  is switched off at the same moment. 0 never stands down.
 *
 *  One timer for both, because they are one decision. A screen could go dark
 *  sooner -- the only thing a short timer has to clear is the gap between two
 *  tracks -- but an amplifier wants a claim that has held for a while, so the
 *  longer of the two sets the pace and the screen waits with it. Any touch
 *  wakes it, as does playback resuming; polling carries on while blanked,
 *  which is what notices. */
constexpr unsigned long kIdleTimeoutMs = 300000;
/** Whether paused counts as idle for standing down.
 *
 *  On, which it was not originally. A paused player is often one you are about
 *  to resume, and the screen saying what it is holding has its own value -- but
 *  leaving this false would mean a pause never leads to the amplifier
 *  switching off, which is the case that most wants it to. */
constexpr bool kBlankWhenPaused = true;

/** Switch the volume/power entity off when the panel blanks.
 *
 *  The counterweight to kTurnOnControlOnWake. Narrow by construction: standing
 *  down already requires the player to be paused, idle, off or unavailable for
 *  kIdleTimeoutMs, and the panel wakes the instant that stops being true -- so
 *  the timer cannot run while anything is playing, and any touch or resumed
 *  playback cancels it.
 *
 *  Only sent once per blanking, and only to an entity whose supported_features
 *  claim TURN_OFF. */
constexpr bool kTurnOffControlOnBlank = true;

}  // namespace config
