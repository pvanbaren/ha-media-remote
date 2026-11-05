#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

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
/** How long the portal's "saved" page stays before going back to the form.
 *  Long enough for a device that restarts to apply a setting -- rotation, or
 *  its name -- to be answering again when the form reloads. */
constexpr unsigned long kPortalSavedReturnMs = 15000;

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
/** While the setup portal is up because the saved network did not answer at
 *  boot -- the router down with the power, say -- how often to try it again.
 *  Only while nobody is on the portal's access point or the device's own
 *  network screens: a try takes the radio off the access point's channel. */
constexpr unsigned long kPortalSavedRetryMs = 60000;
/** Wi-Fi transmit power until one is chosen in the portal, in the quarter-dBm
 *  steps esp_wifi uses: 44 is 11 dBm. Well below the 19.5 dBm the ESP32-S3
 *  can do; see prepareSta() in wifi_setup.cpp for why, and the portal's
 *  "Wi-Fi transmit power" for a remote that is far from its access point. */
constexpr int8_t kWifiTxPowerQuarterDbm = 44;
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;
/** Once a background reconnect has failed and the No Wi-Fi card is up, how
 *  often to nudge the stack to try again. A nudge does not block: the core's
 *  own auto-reconnect is trying meanwhile, and the card stays answerable. */
constexpr unsigned long kWifiLostNudgeMs = 60000;

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

/** How long after this remote switches the volume device on or off that
 *  "Off in this room" is held back. Play from the remote switches the device
 *  on and starts the player together, and for a second or two Home Assistant
 *  has the player playing while the device still reports off; switching it
 *  off, the device reports off a moment before the player stops. Either way
 *  the chip flashed up. A device still off, with the player still playing,
 *  when this runs out shows it. */
constexpr unsigned long kOffHereHoldMs = 8000;

/** How long a track change's repaint waits for its cover. The cover is
 *  fetched on the artwork worker, off the task that services the state
 *  stream; a quick one lands in time and the title and the art appear
 *  together, as they did when the fetch came first, and a slow one no longer
 *  holds up the title -- it shows over the plain backdrop, and the cover drops
 *  in when it arrives. */
constexpr unsigned long kCoverArtHoldMs = 1000;

/** The room's history is written to NVS at once when an artist new to it is
 *  added, and otherwise -- an artist moving back to the front -- at most this
 *  often: a reordering lost to a power cut is worth less than the flash. */
constexpr unsigned long kHistorySaveIntervalMs = 60UL * 60 * 1000;
/** Pictures are not stored with the history; after a restart they are found
 *  again, one artist every this often, by the search that found them first. */
constexpr unsigned long kHistoryPictureLookupMs = 5000;
/** One input name on the volume/power entity ("AirPlay", "HDMI4"). */
constexpr size_t kSourceNameMaxLen = 64;
/** Inputs offered in the portal's dropdown. A Yamaha receiver lists 27. */
constexpr int kMaxSources = 40;
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
/** Where a section's items come from. Each room source has the section's
 *  library query to fall back on. */
enum class BrowseSource : uint8_t {
  /** The Music Assistant library query in the section. */
  kLibrary,
  /** The artists played in this room (services::history), most recent first,
   *  while the Recent list setting is Room only
   *  (services::browse::recentHere()). The library query stands in, under
   *  `fallback_title`, while it is Music Assistant or there are none yet. */
  kRoomArtists,
  /** Artists like the first few of the section above (services::recommend),
   *  with a Music Assistant token (services::ma). The library query stands in
   *  without one, or until there are any. */
  kRoomSimilar,
};

/** One section of the list: a heading and what fills it. */
struct BrowseSection {
  const char* title;
  /** artist, album, playlist, radio or track. */
  const char* media_type;
  /** An integration sort key: last_played_desc, play_count_desc, random,
   *  timestamp_added_desc, name... or nullptr for its default. */
  const char* order_by;
  int limit;
  BrowseSource source = BrowseSource::kLibrary;
  /** The heading while the library query stands in for the room, or nullptr
   *  to keep `title`. */
  const char* fallback_title = nullptr;
};

/** How many of the recent artists "Recommended" is built from: the first of
 *  the list's top section, this room's or the server's.
 *  Each is one or two Music Assistant calls, made in the background whenever
 *  they change -- not per list load. */
constexpr int kRecommendSeedArtists = 3;
/** Similar artists asked for per seed, and similar tracks read per seed
 *  (twice this: a track list repeats its artists). */
constexpr int kRecommendPerSeed = 12;
/** Artists kept to draw from. */
constexpr int kRecommendPoolMax = 48;
/** How long the seeds must hold still before the pool is rebuilt, so artist
 *  radio moving through a run of names costs one rebuild rather than one each. */
constexpr unsigned long kRecommendSettleMs = 10000;
/** A pool is rebuilt this often even when nothing changed: the answers do. */
constexpr unsigned long kRecommendRefreshMs = 6UL * 60 * 60 * 1000;
/** After Music Assistant could not be reached. */
constexpr unsigned long kRecommendRetryMs = 5UL * 60 * 1000;

/** Music Assistant's own API (services::ma). The add-on's port, on Home
 *  Assistant's host, is what an empty address in the portal means. */
constexpr uint16_t kMaDefaultPort = 8095;
constexpr size_t kMaUrlMaxLen = 96;
constexpr size_t kMaTokenMaxLen = 512;
/** Similar artists take about three seconds as the library merges its
 *  providers' answers; a provider's own lookup can take longer. */
constexpr uint32_t kMaCallTimeoutMs = 20000;
/** Largest answer read, in PSRAM. similar_tracks ignores its limit and sends
 *  fifty tracks, about 107 KB; top_tracks about 65 KB. */
constexpr size_t kMaResponseMaxBytes = 192 * 1024;

/** What the swipe-up list shows, top to bottom.
 *
 *  The top section is this room's while the Recent list setting is Room
 *  only, and the server's while it is Music Assistant. Music Assistant's own
 *  "recently played" is
 *  one timestamp per library item shared by every player, so two remotes in
 *  two rooms see the same list; what played on this remote's player is what
 *  played in this room (services::history).
 *
 *  "Recommended" is artists like the first three of the top section, from
 *  Music Assistant's own API (services::recommend), which needs its token in
 *  the portal. Built in the background and drawn from at random, so a load
 *  costs nothing. Without a token, or until there are any, it is artists
 *  from the library at random -- artists, so that a tap starts radio, as it
 *  does on the rest of the list.
 *
 *  A library query costs a request when the list loads, and leaves out
 *  whatever an earlier section already shows. */
constexpr BrowseSection kBrowseSections[] = {
    {"Recent here", "artist", "last_played_desc", 8,
     BrowseSource::kRoomArtists, "Recent artists"},
    {"Recommended", "artist", "random", 8, BrowseSource::kRoomSimilar},
};
/** The Recent list setting on a remote that has never stored one: Music
 *  Assistant's recent artists, the same on every remote. */
constexpr bool kRecentHereByDefault = false;
constexpr int kBrowseSectionCount =
    static_cast<int>(sizeof(kBrowseSections) / sizeof(kBrowseSections[0]));

/** Items held across all sections. Each costs a name, a URI, an image URL and
 *  a decoded thumbnail, so this is the real cost knob here. It caps the total,
 *  not the per-section limits above -- keep it at their sum, or the later
 *  sections are what get cut.
 *
 *  At 16: two ~5 KB item arrays in internal RAM (the list and the one being
 *  fetched into), and sixteen thumbnail sprites in PSRAM -- 46 KB on the
 *  240 px panel, 185 KB on the Qualia. */
constexpr int kMaxBrowseItems = 16;

// =====================================================================
// Search — the swipe-left screen
// =====================================================================
/** Longest name that can be typed. Music Assistant's search is forgiving
 *  about partial names -- "radioh" finds Radiohead -- so this is generous
 *  rather than load-bearing. */
constexpr size_t kSearchQueryMaxLen = 24;

/** Results held. More than fits on screen at once, so the list scrolls. */
constexpr int kMaxSearchResults = 8;

/** Play a chosen artist -- from the search, or from Recent here or
 *  Recommended on the swipe-up list -- as *radio*, an endless queue seeded
 *  from them, rather than playing their tracks straight through. Artist radio
 *  is the reason to pick an artist from a wall remote; playing a discography
 *  in order is something you would reach for a phone to do. */
constexpr bool kArtistsPlayRadio = true;

/** How long the list stays fresh before a reopen refetches it. Short enough
 *  that "recent artists" means it, and that a random "Recommended" draw is
 *  actually a different draw next time. */
constexpr unsigned long kBrowseTtlMs = 300000;
/** After a load that failed, or could not be made for want of a Music
 *  Assistant config entry, how long the background preload waits before
 *  trying again. It is asked on every pass of the poll task, a few times a
 *  second, and without this tried -- and logged, or sent the request -- on
 *  every one. Opening the list still tries at once, and so does saving a
 *  Music Assistant token. */
constexpr unsigned long kBrowseRetryMs = 60000;

/** Most media_player entities a picker will hold. Entities past this are
 *  dropped from the list and become unselectable, so it has to clear the real
 *  count: a house with Chromecasts, a receiver's zones and a Music Assistant
 *  bridge runs to dozens. ~105 bytes each, so this array is ~10 KB, in PSRAM. */
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

/** Internal RAM a TLS handshake to Home Assistant wants free before it is
 *  tried again: mbedTLS's two record buffers, ~17 KB each, and the
 *  handshake's own working memory. A request whose TLS connection fails at
 *  once with less than this free waits for it -- up to kHaTlsRoomWaitMs --
 *  and tries once more, rather than failing outright. The shortfall comes
 *  and goes with whatever else holds internal RAM at that moment. */
constexpr size_t kHaTlsRoomBytes = 56 * 1024;
constexpr unsigned long kHaTlsRoomWaitMs = 1000;

/** Plain mallocs at least this big go to PSRAM first, internal RAM only
 *  when PSRAM is out. The SDK's own rule keeps everything under 4 KB
 *  internal (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) -- every String, every
 *  JSON node, every list -- in the same RAM mbedTLS needs for its record
 *  buffers, which it asks for as internal and always gets there or nowhere.
 *  What must be internal says so by its caps: task stacks, Wi-Fi, DMA and
 *  the drivers' buffers are unaffected.
 *
 *  Measured on all six boards, a few minutes after boot, internal RAM free
 *  over leaving the SDK's rule alone: +21 KB from 32 bytes, +20 from 64,
 *  +16.5 from 128, +13.5 from 256, +11 from 512, +6.3 from 1024, +4.8 from
 *  2048, nothing from 4096 -- the same on every board, to half a KB.
 *
 *  The smaller the size, the more of the firmware's constant small traffic
 *  lands in PSRAM, and a panel scanned out of a PSRAM framebuffer has to
 *  win every read of it against that traffic. At 32 bytes the 2.8C and the
 *  Qualia blinked and jumped sideways while sitting idle. So 256 where the
 *  panel keeps its own picture, and kPsramMallocFromBytesRgb where it is
 *  scanned out of PSRAM; ui::psramMallocFromBytes() chooses. */
constexpr size_t kPsramMallocFromBytes = 256;
constexpr size_t kPsramMallocFromBytesRgb = 2048;

/** Volume to set on the *media player* when the remote switches the
 *  amplifier on -- play pressed, or something picked from the list, while it
 *  was off -- and only when a separate entity carries volume and power. Never
 *  when the player starts by itself.
 *
 *  When the two are the same thing this is meaningless and is skipped: the
 *  volume the user last chose is the volume they want back.
 *
 *  When they differ, the player's own volume_level is not a volume at all --
 *  it is the gain on the signal being handed to an amplifier that has its own
 *  knob. Whatever that gain happens to be when a session starts is arbitrary,
 *  and if it drifted low once, every later adjustment fights it: the amplifier
 *  gets turned up to compensate for a quiet source, and the next track through
 *  a correctly-set player is deafening. Pinning it to one known value as the
 *  amplifier comes on makes the amplifier's setting mean something from one
 *  session to the next.
 *
 *  Off by default: whether a player's gain wants pinning, and where, depends
 *  on the player and the amplifier, and a volume changed on its own when
 *  playback starts is a surprise to anyone who did not ask for it. The
 *  portal's "Player volume at switch-on" field, shown when the volume device is a
 *  separate one, turns it on at a level of its own -- 70% is a sensible one:
 *  a little headroom costs nothing audible and leaves room for a player that
 *  applies replaygain above unity. A reset of the Home Assistant settings
 *  comes back here. Negative is off; 0..1 would pin every device that has not
 *  been through the portal. */
constexpr float kPlayerWakeVolume = -1.0f;

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
/** How long Home Assistant may go unheard -- no stream, every poll failing --
 *  before now playing says it is offline. Longer than a poll or two, so one
 *  dropped request does not flash it up. */
constexpr unsigned long kHaOfflineMarkMs = 10000;
/** How often a token Home Assistant refused is tried again, in case the
 *  refusal was the server's mistake. Every try is a failed sign-in in its
 *  log, and enough of them get the remote's address banned where IP banning
 *  is on; a new token is tried at once. */
constexpr unsigned long kHaTokenRetryMs = 600000;

/** Take state from Home Assistant's WebSocket API rather than polling for it
 *  (see services::ha::streamOpen). Polling above is what runs while the
 *  stream is down, and all that runs with this false. */
constexpr bool kHaStreamEnabled = true;
/** First wait before reopening a stream that failed to open, doubled on each
 *  further failure up to the max. A stream that had been working for a
 *  minute is reopened at once when it drops; one that dies sooner -- a
 *  subscription refused, say -- counts as a failure to open. */
constexpr unsigned long kHaStreamRetryMinMs = 5000;
constexpr unsigned long kHaStreamRetryMaxMs = 60000;
/** Longest the stream's TLS handshake may take. More than the REST timeout:
 *  on this board's noisier links a handshake has measured over 4 s. */
constexpr unsigned long kHaStreamHandshakeMs = 10000;
/** Send Home Assistant a ping after this long without one... */
constexpr unsigned long kHaStreamPingMs = 30000;
/** ...and give the connection up after this long without hearing anything at
 *  all. The template re-renders every minute on its own, so a live
 *  connection is never quiet for long. */
constexpr unsigned long kHaStreamSilentMs = 75000;
/** After a state message, wait this long for the next before handing it on.
 *  A track change arrives as a burst -- title, then art, then duration -- and
 *  each would otherwise be its own recompose. */
constexpr unsigned long kHaStreamSettleMs = 150;
/** How long another task waits for the stream's owner to take a service call
 *  before sending it over REST instead. The owner can be busy for seconds on
 *  a library load; a button press should not wait on that. */
constexpr unsigned long kHaStreamPickupMs = 300;
/** Largest message the stream will hold; anything bigger is read off the
 *  wire and dropped. State is a few hundred bytes, but Music Assistant's
 *  library and search answers come this way too while the stream is open --
 *  sixteen albums run to about 8 KB. A message this size is one allocation,
 *  which lands in PSRAM. */
constexpr size_t kHaStreamMaxMessage = 32768;
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
/** Now playing rests as the dimmed clock face after this long untouched; 0
 *  never does. A touch brings now playing back and does nothing else, as on
 *  a blanked panel. Blanking still follows kIdleTimeoutMs, from the clock as
 *  from now playing. */
constexpr unsigned long kDimAfterMs = 15000;
/** The clock face's zone, as a POSIX TZ string.
 *  shortcut: Berlin only, right for this fork; a build for anyone else wants
 *  it a setting, or taken from Home Assistant's time_zone. */
constexpr char kTimeZone[] = "CET-1CEST,M3.5.0,M10.5.0/3";
constexpr char kNtpServer[] = "pool.ntp.org";
/** Earlier than this, the clock has not been set: the board has no RTC and
 *  starts at 1970 until SNTP answers. */
constexpr time_t kClockSetAfter = 1700000000;
/** Whether paused counts as idle for standing down.
 *
 *  On, which it was not originally. A paused player is often one you are about
 *  to resume, and the screen saying what it is holding has its own value -- but
 *  leaving this false would mean a pause never leads to the amplifier
 *  switching off, which is the case that most wants it to. */
constexpr bool kBlankWhenPaused = true;

/** Switch the volume/power entity off when the panel blanks -- the default
 *  for the Auto-off setting, which the portal and the settings page change
 *  (services::ha::autoOffControl()).
 *
 *  The counterweight to play and the list switching it on. Narrow by
 *  construction: standing
 *  down already requires the player to be paused, idle, off or unavailable for
 *  kIdleTimeoutMs, and the panel wakes the instant that stops being true -- so
 *  the timer cannot run while anything is playing, and any touch or resumed
 *  playback cancels it.
 *
 *  Only sent once per blanking, and only to an entity whose supported_features
 *  claim TURN_OFF. A source on the volume device's input with a power switch
 *  of its own holds the blank off until it is switched off (app.cpp,
 *  sourcePowersItself()), so this never cuts a room that source still has. */
constexpr bool kTurnOffControlOnBlank = true;

/** How long the browse list or the search screen stays up with nobody
 *  touching it before the device goes back to now playing. 0 never does.
 *
 *  Those screens are only ever up because someone was using them, and one
 *  walked away from leaves a remote that no longer says what is playing --
 *  and keeps the panel lit, since only now playing ever blanks. Counted from
 *  the last touch of any kind, or from opening the screen if that is later,
 *  so a list being read slowly is not pulled away mid-scroll. */
constexpr unsigned long kListIdleReturnMs = 60000;
/** How long the Starting... card waits for a pick to start playing before
 *  giving way to now playing whatever it shows. Its back button leaves
 *  sooner. */
constexpr unsigned long kStartingWaitMs = 60000;

/** A tap on a scrolling list is refused if it began while the list was
 *  moving, or this soon after it stopped. Flicking again to keep a list going
 *  is a press that lands mid-glide, and taking it as a tap started whatever
 *  row happened to be under the finger. */
constexpr unsigned long kListTapGuardMs = 250;
/** ...and refused, too, if the finger went unread for longer than this at any
 *  point while it was down. Where touch is read from the loop, a flick can
 *  fall between two reads of a busy pass -- down, then gone, never seen to
 *  move -- and a press nobody watched cannot be told from a tap. */
constexpr unsigned long kTouchWatchGapMs = 60;

/** Taps on now playing are refused for this long after Isolate's card gives
 *  way to it. The finger that pressed Isolate, Cancel or OK may come down
 *  again on now playing -- where the same spot toggles the room's power. */
constexpr unsigned long kIsolateTapGuardMs = 1000;

/** Shortest gap between two repaints of a list for thumbnails arriving.
 *
 *  The worker can land several a second once its connection is warm, and
 *  each repaint is the better part of 150 ms of compose and copy on the
 *  Qualia. Gathering what arrived in the meantime into one repaint keeps
 *  the list from spending most of a second redrawing itself. */
constexpr unsigned long kThumbRepaintMinMs = 300;


/** Give up on art beyond this size outright; something is wrong upstream.
 *  The cache size itself is board::kCoverArtBufferBytes -- how much art is
 *  worth holding depends on how many pixels the panel asked for. */
constexpr size_t kCoverArtMaxBytes = 2u * 1024u * 1024u;

/** Rewrite an existing `size=` query parameter to board::kCoverArtRequestPx,
 *  to fetch art near the panel's size instead of the integration's default.
 *
 *  Servers offer a ladder of sizes rather than any value -- Music Assistant's
 *  image proxy accepts 0, 80, 160, 256, 512, 1024 and answers 400 otherwise --
 *  so the request rounds up to a power of two, which lands on that ladder
 *  without upscaling. A 240 px panel asks for 256: on one album that took the
 *  JPEG from 88 KB to 28 KB, the difference between caching it and re-fetching
 *  it on every compose pass.
 *
 *  Safe to leave on: a refusal is retried with the untouched URL, and
 *  rewriting is then dropped for the session. */
constexpr bool kCoverArtRequestPanelSize = true;

// --- UI colours (RGB565) ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorWhite = 0xFFFF;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = kColorWhite;

}  // namespace config
