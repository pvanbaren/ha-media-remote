/**
 * The remote itself: what it hears, what it decides, what it sends.
 *
 * Nothing in this file draws, and nothing in it knows the panel is round. It
 * asks ui/ui.h to put a screen up and hands it back intents -- "play/pause",
 * "set the volume to 0.4", "play browse row 3" -- without ever learning where
 * the finger landed.
 *
 * Network work all happens on haPollTask, which also owns the state stream;
 * setup() and loop() own everything else. The two meet at g_state (behind g_state_mutex) and at the artwork
 * cache, which serialises itself.
 */

#include <Arduino.h>
#include <WiFi.h>

#include <atomic>
#include <cmath>
#include <cstring>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "app/app.h"
#include "config.h"
#include "log.h"
#include "services/browse.h"
#include "services/device_name.h"
#include "services/display_settings.h"
#include "services/ha_link.h"
#include "services/history.h"
#include "services/ma_api.h"
#include "services/player_list.h"
#include "services/recommend.h"
#include "services/ha_client.h"
#include "services/search.h"
#include "services/wifi_setup.h"
#include "ui/ui.h"

namespace app {
namespace {

using services::ha::PlaybackState;
using services::ha::PlayerState;
using ui::Intent;
using ui::Screen;

/** The flags the two tasks pass each other are std::atomic, not volatile.
 *
 *  volatile only promises the compiler will not invent or elide the access.
 *  It promises nothing about ordering against ordinary memory, and nothing
 *  about the other core seeing the write -- which matters here, because the
 *  S3 is dual-core and haPollTask is left unpinned. std::atomic says what is
 *  actually meant, and says it to the hardware: __GCC_ATOMIC_BOOL_LOCK_FREE
 *  and __GCC_ATOMIC_INT_LOCK_FREE are both 2 on xtensa-esp32s3-elf (the core
 *  has S32C1I), so everything below is lock-free and a sequentially
 *  consistent load or store is a plain load or store plus a memw. Nothing
 *  here is 64-bit, which is the size that would fall back to a lock.
 *
 *  Which screen is up: written by the Arduino loop, read by the poll task to
 *  decide whether refreshing the browse list would pull the rug out from
 *  under someone reading it. */
std::atomic<Screen> g_screen{Screen::kMessage};

SemaphoreHandle_t g_state_mutex = nullptr;
PlayerState g_state;
bool g_state_valid = false;

std::atomic<bool> g_state_dirty{false};
/** Set to cut the poll task's wait short after a transport command. */
std::atomic<bool> g_poll_now{false};
/** The panel has been dark long enough and the volume/power entity should be
 *  switched off. Handed to the poll task rather than called from the loop:
 *  turn_off is a network round trip. */
std::atomic<bool> g_turn_off_control{false};

unsigned long g_last_elapsed_ms = 0;
/** When the browse list or search screen was last opened, for
 *  kListIdleReturnMs. Loop only. */
unsigned long g_list_opened_ms = 0;

/** A volume gesture is in progress somewhere on the panel, so a poll must not
 *  contradict it. The display owns the gesture; this is a copy of
 *  ui::volumeDragging() taken after every input pass on the Arduino loop, for
 *  publishState() on the poll task to read. */
std::atomic<bool> g_volume_dragging{false};
/** A state change arrived during a drag and its repaint was held back, so it
 *  is owed once the finger lifts. Without this a new title that landed
 *  mid-drag stayed off the screen until something else changed. */
std::atomic<bool> g_repaint_after_drag{false};

/** The level last commanded on the control entity, and when -- or -1 when
 *  nothing is outstanding. Held behind g_state_mutex with g_state.
 *
 *  This exists because a poll can contradict a volume the device just set, in
 *  two different ways. A fetch already in flight when the finger lifts was
 *  issued before the new level and returns the old one. And even a fetch begun
 *  afterwards can return the old one, because Home Assistant's state for a
 *  receiver lags the service call that changed it.
 *
 *  Either way the arc snapped back, and -- worse -- the next swipe read that
 *  stale level as its starting point and re-applied the same increase, so the
 *  volume ratcheted instead of rising. */
float g_volume_commanded = -1.0f;
unsigned long g_volume_commanded_ms = 0;
/** How long a commanded level is believed over a poll that disagrees. */
constexpr unsigned long kVolumeConfirmTimeoutMs = 4000;
/** Close enough to count as Home Assistant having caught up. Receivers
 *  quantise, so an exact match is not on offer. */
constexpr float kVolumeConfirmEpsilon = 0.02f;
unsigned long g_message_recheck_ms = 0;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;

/** millis() when the player last went idle, or 0 while it is doing something. */
unsigned long g_idle_since = 0;
/** Whether this stretch of darkness has already asked for the power-off. */
bool g_control_off_sent = false;

/** Set the control entity's volume and remember that we did, so a poll
 *  carrying the pre-change level cannot undo it. Blocks on the round trip. */
void commandVolume(const char* entity, float level) {
  if (g_state_mutex != nullptr &&
      xSemaphoreTake(g_state_mutex, portMAX_DELAY) == pdTRUE) {
    g_volume_commanded = level;
    g_volume_commanded_ms = millis();
    // And the level on record is this one from now on, not the server's
    // last word. That is what the screen shows, what an update arriving
    // mid-drag is held to, and where the next swipe starts. Left at the
    // server's level, a second swipe started from the pre-drag volume, and the
    // screen only caught up when some later update happened to differ from
    // that stale number -- which with the stream could be a minute away.
    if (g_state_valid) {
      g_state.volume = level;
    }
    xSemaphoreGive(g_state_mutex);
  }
  services::ha::setVolume(entity, level);
}

/** Copy the shared snapshot out from under the poll task. */
bool snapshotState(PlayerState& out) {
  if (g_state_mutex == nullptr) {
    return false;
  }
  bool valid = false;
  if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    out = g_state;
    valid = g_state_valid;
    xSemaphoreGive(g_state_mutex);
  }
  return valid;
}

/** Whether two snapshots would compose to a different frame.
 *
 *  Deliberately ignores position: it moves on every poll, and a recompose
 *  means decoding the cover art again -- or re-fetching it, on the streaming
 *  path. The elapsed chip handles its own repaint instead. */
bool visuallyDiffers(const PlayerState& a, const PlayerState& b) {
  return a.playback != b.playback || a.muted != b.muted ||
         a.control_off != b.control_off || a.peer_count != b.peer_count ||
         a.supported_features != b.supported_features ||
         a.control_features != b.control_features ||
         a.duration_s != b.duration_s || a.volume != b.volume ||
         strcmp(a.name, b.name) != 0 || strcmp(a.title, b.title) != 0 ||
         strcmp(a.subtitle, b.subtitle) != 0 ||
         strcmp(a.picture, b.picture) != 0;
}

/** When the player last stopped reporting a title, for the untitled label
 *  (config::kUntitledLabelDelayMs). Written under g_state_mutex. */
bool g_untitled = false;
unsigned long g_untitled_since_ms = 0;
/** A frame went up with the label held back; one more is owed when the
 *  delay runs out, if the title is still missing then. Loop task only. */
bool g_untitled_repaint_owed = false;

void publishState(const PlayerState& incoming) {
  PlayerState fresh = incoming;
  bool changed = true;
  if (g_state_mutex != nullptr &&
      xSemaphoreTake(g_state_mutex, portMAX_DELAY) == pdTRUE) {
    if (fresh.title[0] != '\0') {
      g_untitled = false;
    } else if (!g_untitled) {
      g_untitled = true;
      g_untitled_since_ms = millis();
    }

    // Decide what this poll is allowed to say about the volume before
    // comparing it to what is on screen, so a level we are holding does not
    // also count as a change worth repainting.
    if (g_volume_dragging) {
      // The finger owns it until it lifts; letting a poll overwrite it
      // mid-gesture makes the knob jump back under the user's thumb.
      fresh.volume = g_state.volume;
    } else if (g_volume_commanded >= 0.0f) {
      if (fabsf(fresh.volume - g_volume_commanded) <= kVolumeConfirmEpsilon) {
        g_volume_commanded = -1.0f;  // Home Assistant has caught up
      } else if (millis() - g_volume_commanded_ms < kVolumeConfirmTimeoutMs) {
        fresh.volume = g_volume_commanded;  // still ours; ignore the old level
      } else {
        // Long enough. Either the command did not take or something else moved
        // it, and continuing to assert a level nobody agrees with is worse
        // than accepting the one the server reports.
        g_volume_commanded = -1.0f;
      }
    }

    changed = !g_state_valid || visuallyDiffers(g_state, fresh);
    g_state = fresh;
    g_state_valid = true;
    xSemaphoreGive(g_state_mutex);
  }
  if (changed) {
    // One line per visible change, so what the firmware actually parsed can be
    // checked against Home Assistant without guessing.
    LOG_DEBUG("HA: state=%d vol=%.3f muted=%d pos=%.1f/%.1f feat=%u \"%s\"",
                  static_cast<int>(fresh.playback), fresh.volume,
                  fresh.muted ? 1 : 0, fresh.position_s, fresh.duration_s,
                  static_cast<unsigned>(fresh.supported_features), fresh.title);
  }
  if (changed) {
    if (g_volume_dragging) {
      g_repaint_after_drag = true;
    } else {
      g_state_dirty = true;
    }
  }
}

void showMessageScreen() {
  g_screen = Screen::kMessage;
  g_message_recheck_ms = millis();
}

/** The picture a repaint last waited for, and since when. A picture is
 *  waited for once: a repaint after the wait gave up, or after the cover
 *  arrived, goes ahead. Loop only. */
char g_cover_hold_picture[sizeof(PlayerState::picture)] = {};
unsigned long g_cover_hold_since_ms = 0;

/** Whether the pending repaint should wait for the cover still loading for
 *  what is now playing -- up to config::kCoverArtHoldMs, and only on the now
 *  playing screen, where the cover is drawn. */
bool holdForCover() {
  if (g_screen != Screen::kNowPlaying && g_screen != Screen::kMessage) {
    return false;
  }
  PlayerState snapshot;
  if (!snapshotState(snapshot) || !ui::artworkPending(snapshot.picture)) {
    return false;
  }
  if (strcmp(g_cover_hold_picture, snapshot.picture) != 0) {
    snprintf(g_cover_hold_picture, sizeof(g_cover_hold_picture), "%s",
             snapshot.picture);
    g_cover_hold_since_ms = millis();
  }
  return millis() - g_cover_hold_since_ms < config::kCoverArtHoldMs;
}

/** True while the player has reported no title for less than
 *  config::kUntitledLabelDelayMs. */
bool untitledBriefly() {
  bool brief = false;
  if (g_state_mutex != nullptr &&
      xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    brief = g_untitled &&
            millis() - g_untitled_since_ms < config::kUntitledLabelDelayMs;
    xSemaphoreGive(g_state_mutex);
  }
  return brief;
}

/** Not linked to Home Assistant yet: the QR code to sign in with when Home
 *  Assistant has been found, the card pointing at the portal otherwise. */
void showNeedsHa() {
  if (services::ha_link::ready()) {
    ui::showHaSignIn(services::ha_link::qrText(),
                     services::ha_link::pageUrl(),
                     services::ha_link::serverName());
  } else {
    ui::showNeedsHaSetup();
  }
}

void openPlayers();
void leaveIsolate();

void showNowPlaying() {
  if (!services::ha::configured()) {
    LOG_INFO("UI: now playing skipped, Home Assistant not configured");
    showNeedsHa();
    showMessageScreen();
    return;
  }
  if (services::ha::selectedEntity()[0] == '\0') {
    LOG_INFO("UI: now playing skipped, no player selected");
    if (g_screen != Screen::kPlayers) {
      openPlayers();
    }
    return;
  }

  PlayerState snapshot;
  if (!snapshotState(snapshot)) {
    // Either no poll has ever succeeded, or the 50 ms wait for the state
    // mutex expired -- which reads the same from here and does not from
    // the console, so say which.
    LOG_WARN("UI: now playing skipped, no state snapshot (%s)",
                  services::ha::lastError());
    ui::showHaUnreachable(services::ha::lastError());
    showMessageScreen();
    return;
  }

  // No title yet, and not for long: leave the title area blank for now and
  // owe a repaint for when the label is due.
  if (snapshot.title[0] == '\0' && untitledBriefly()) {
    snapshot.hold_label = true;
    g_untitled_repaint_owed = true;
  }

  LOG_DEBUG("UI: now playing \"%s\"", snapshot.title);
  ui::showNowPlaying(snapshot);
  g_screen = Screen::kNowPlaying;
  g_last_elapsed_ms = millis();
}

/** Whether the player is doing nothing worth lighting the panel for. */
bool playerIsIdle(const PlayerState& state) {
  switch (state.playback) {
    case PlaybackState::kIdle:
    case PlaybackState::kOff:
    case PlaybackState::kUnavailable:
      return true;
    case PlaybackState::kPaused:
      return config::kBlankWhenPaused;
    default:
      return false;
  }
}

/** Bring the panel back and force a fresh frame. Panel RAM is not trustworthy
 *  across a sleep, so the screen has to be composed again rather than assumed
 *  to still be there. */
void wakeDisplay() {
  if (!ui::isBlanked()) {
    return;
  }
  ui::wake();
  g_control_off_sent = false;
  g_state_dirty = true;
}

/** A tap landed on a dark panel: the idle clock starts again. It powers
 *  nothing -- a glance at the remote is not a request for music, and the
 *  amplifier is switched on by play or a pick from the list instead (see
 *  powerOnControl()) -- but a power-off the blanking asked for and the poll
 *  task has not yet sent is called off, since someone is here after all. */
void noteTouchWake() {
  g_idle_since = 0;
  g_control_off_sent = false;
  g_turn_off_control = false;
  g_state_dirty = true;
}

/** Point the volume/power entity at this player, where it has an input for
 *  it: the one chosen in the portal, or one named after the player, which is
 *  how a zone amplifier lists its players.
 *
 *  Switching a zone on does not route anything to it: a Triad output comes up
 *  with no input, stays silent, and its integration drops it again a minute
 *  later unless the linked player starts playing first. So a power-on selects
 *  the player's input as well -- straight after the turn_on, unless the zone
 *  is already on it. A zone that was already on is taken over whatever it is
 *  on too: the only power-ons are play pressed on this remote and a pick from
 *  its list, someone in this room asking to hear this player. */
void selectPlayerSource(const PlayerState& snapshot) {
  if (!config::kSelectControlSource || !services::ha::controlIsSeparate()) {
    return;
  }
  if (snapshot.player_source[0] == '\0') {
    return;  // no input for this player, chosen or by its name
  }
  if (snapshot.control_features != 0 &&
      (snapshot.control_features & services::ha::kFeatureSelectSource) == 0) {
    return;
  }
  if (strcmp(snapshot.control_source, snapshot.player_source) == 0) {
    return;  // already on it
  }
  services::ha::selectSource(services::ha::controlEntity(),
                             snapshot.player_source);
}

/** Set the *player's* own volume to the portal's Player volume at switch-on,
 *  services::ha::playerWakeVolume() -- or config::kPlayerWakeVolume until one
 *  is saved -- when a separate entity carries volume and power and a level is
 *  set. With the real knob on the amplifier the player's volume is the gain
 *  on the signal it hands over, and leaving it wherever it drifted means the
 *  amplifier's setting stops meaning the same thing session to session.
 *
 *  Only from powerOnControl(), once it has switched the amplifier on: the
 *  start of a session this remote began. Never because the player started by
 *  itself -- a cast from a phone keeps the volume it was cast at. volume_set
 *  goes to selectedEntity(), the thing that plays; where the two are the same
 *  entity, the volume last chosen there is the one wanted back. */
void pinPlayerVolume() {
  if (!services::ha::controlIsSeparate()) {
    return;
  }
  const float level = services::ha::playerWakeVolume();
  if (level < 0.0f) {
    return;
  }
  char player[config::kEntityIdMaxLen];
  services::ha::copySelectedEntity(player, sizeof(player));
  if (player[0] == '\0') {
    return;
  }
  LOG_INFO("HA: amplifier switched on, pinning %s to %.2f", player, level);
  services::ha::setVolume(player, level);
}

/** Switch the volume/power entity on, unless it is plainly unnecessary, and
 *  route this player to it where it switches inputs (selectPlayerSource()),
 *  setting the player's own volume once it has switched it on
 *  (pinPlayerVolume()). Power, input and volume all come before the play
 *  they are for.
 *
 *  Only for playback started from this remote -- play pressed, or something
 *  picked from the list or the search. Never for playback that started on
 *  its own, nor for a tap that only wakes the screen: a player can feed
 *  several rooms -- a Chromecast every zone can be switched to, say -- and
 *  a cast from a phone to it must not switch on every room's amplifier.
 *
 *  Skipped when the last snapshot says it is already on, and when that
 *  snapshot says it has no TURN_ON feature -- calling turn_on on an entity
 *  that does not support it just fills the Home Assistant log. A player
 *  reporting no features at all is treated as supporting everything, as
 *  everywhere else here: that is an integration that never set the attribute.
 *
 *  Blocks on the round trip, so callers on the touch path have to want the
 *  wait. Returns true if a call was actually made. */
bool powerOnControl() {
  if (!services::ha::configured()) {
    return false;
  }
  const char* control = services::ha::controlEntity();
  if (control[0] == '\0') {
    return false;
  }

  PlayerState snapshot;
  const bool have_state = snapshotState(snapshot);
  if (have_state) {
    if (!snapshot.control_off) {
      selectPlayerSource(snapshot);
      return false;
    }
    if (snapshot.control_features != 0 &&
        (snapshot.control_features & services::ha::kFeatureTurnOn) == 0) {
      return false;
    }
  }

  const bool ok = services::ha::callService("turn_on", control);
  if (ok) {
    if (have_state) {
      selectPlayerSource(snapshot);
    }
    pinPlayerVolume();
  }
  return ok;
}

/** Switch the volume/power entity off, if it is on and can be. */
bool powerOffControl() {
  if (!services::ha::configured()) {
    return false;
  }
  const char* control = services::ha::controlEntity();
  if (control[0] == '\0') {
    return false;
  }

  PlayerState snapshot;
  if (snapshotState(snapshot)) {
    if (snapshot.control_off) {
      return false;  // already off
    }
    if (snapshot.control_features != 0 &&
        (snapshot.control_features & services::ha::kFeatureTurnOff) == 0) {
      return false;
    }
  }

  return services::ha::callService("turn_off", control);
}


/** Stand down once the player has been idle for kIdleTimeoutMs -- blank the
 *  panel and let the amplifier go with it -- and wake again the moment it is
 *  not. Polling continues while blanked, which is what notices playback
 *  resuming. */
void handleIdleBlanking() {
  if (config::kIdleTimeoutMs == 0) {
    return;
  }

  PlayerState snapshot;
  const bool have_state = snapshotState(snapshot);

  // Only the now-playing screen blanks. The browse list and the status cards
  // are there to be read, and are only ever up because someone is looking.
  if (!have_state || g_screen != Screen::kNowPlaying ||
      !playerIsIdle(snapshot)) {
    g_idle_since = 0;
    wakeDisplay();
    return;
  }

  if (g_idle_since == 0) {
    g_idle_since = millis();
    return;
  }
  if (ui::isBlanked() ||
      millis() - g_idle_since < config::kIdleTimeoutMs) {
    return;
  }

  ui::blank();

  // The amplifier goes with the screen. Getting here at all means the player
  // has been paused, idle, off or unavailable for the whole timeout, since
  // anything else would have woken the panel several branches ago.
  if (config::kTurnOffControlOnBlank && !g_control_off_sent) {
    g_control_off_sent = true;
    g_turn_off_control = true;
    g_poll_now = true;
  }
}



/** Open the artist search, a swipe left from the browse list.
 *
 *  Starts on an empty query every time rather than keeping the last one. A
 *  search is a thing you do once and then forget; coming back to somebody
 *  else's half-typed name is only ever an obstacle. */
void openSearch() {
  if (!services::ha::configured() ||
      services::ha::selectedEntity()[0] == '\0') {
    return;
  }
  g_screen = Screen::kSearch;
  ui::showSearch();
  g_list_opened_ms = millis();
}

/** Open the browse list.
 *
 *  Usually instant, because the poll task has already loaded it in the
 *  background -- see the preload in haPollTask. The loading card is for the
 *  two cases that are left: the first swipe after a boot that beat the
 *  preload, and a swipe that lands while a background load is mid-flight, in
 *  which case ensureLoaded() waits for that one rather than starting another.
 */
void openBrowse() {
  if (!services::ha::configured() ||
      services::ha::selectedEntity()[0] == '\0') {
    return;
  }

  if (services::browse::busy() || services::browse::stale()) {
    ui::showLoadingList();
  }
  services::browse::ensureLoaded();
  g_screen = Screen::kBrowse;
  ui::showBrowse();
  g_list_opened_ms = millis();
}

/** When the status page was last drawn, for its once-a-second refresh.
 *  Loop only. */
unsigned long g_status_drawn_ms = 0;

/** Draw the status page from what the device and its link say now. */
void drawStatus() {
  WifiLinkStatus link;
  wifiLinkStatus(link);
  ui::DeviceStatus status;
  status.hostname = services::device::name();
  status.connected = link.connected;
  status.ip = link.ip;
  status.uptime_s = millis() / 1000UL;
  status.ssid = link.ssid;
  status.channel = link.channel;
  status.rssi = link.rssi;
  status.tx_dbm = link.tx_dbm;
  // The media player, and where they differ the Volume & power device and
  // the player's input on it as chosen in the portal -- empty for the input
  // named after the player. By their names in Home Assistant, from the last
  // state; the entity ids until there is one.
  char player[config::kEntityIdMaxLen];
  char control[config::kEntityIdMaxLen];
  services::ha::copySelectedEntity(player, sizeof(player));
  services::ha::copyControlEntity(control, sizeof(control));
  PlayerState state;
  const bool named = snapshotState(state);
  status.player = named && state.player_name[0] != '\0'
                      ? state.player_name
                      : services::ha::entityLabel(player);
  if (services::ha::controlIsSeparate()) {
    status.control = named && state.control_name[0] != '\0'
                         ? state.control_name
                         : services::ha::entityLabel(control);
    status.input = services::ha::storedControlInput();
  }
  ui::showStatus(status);
  g_status_drawn_ms = millis();
}

/** Open the status page, a swipe down from now playing: the device's name,
 *  address, uptime and Wi-Fi link, on the device itself. */
void openStatus() {
  g_screen = Screen::kStatus;
  drawStatus();
  g_list_opened_ms = millis();
}

/** Once a second while it is showing: uptime and signal both move. */
void handleStatusRefresh() {
  if (g_screen == Screen::kStatus && !ui::isBlanked() &&
      millis() - g_status_drawn_ms >= 1000UL) {
    drawStatus();
  }
}

/** Go back to now playing from a list or search screen nobody is using.
 *
 *  Measured from the later of the last touch and the screen opening, so the
 *  clock starts fresh on arrival -- a swipe up after a minute of listening
 *  must not be sent straight back. */
void handleListTimeout() {
  if (config::kListIdleReturnMs == 0 ||
      (g_screen != Screen::kBrowse && g_screen != Screen::kSearch &&
       g_screen != Screen::kStatus && g_screen != Screen::kWifi &&
       g_screen != Screen::kSettings && g_screen != Screen::kIsolate)) {
    return;
  }
  unsigned long since = ui::lastInteractionMs();
  if (static_cast<long>(g_list_opened_ms - since) > 0) {
    since = g_list_opened_ms;  // the later of the two, wrap-safe
  }
  if (millis() - since < config::kListIdleReturnMs) {
    return;
  }
  LOG_DEBUG("UI: %s untouched for %lu s, back to now playing",
                g_screen == Screen::kBrowse     ? "list"
                : g_screen == Screen::kSearch   ? "search"
                : g_screen == Screen::kWifi     ? "wifi"
                : g_screen == Screen::kSettings ? "settings"
                : g_screen == Screen::kIsolate  ? "isolate"
                                                : "status",
                config::kListIdleReturnMs / 1000);
  if (g_screen == Screen::kIsolate) {
    leaveIsolate();
  } else {
    showNowPlaying();
  }
}

/** No player is selected: the list of players to choose one from, once Home
 *  Assistant is linked, or the card that says how to link it. The portal can
 *  still choose one too; handleMessageRecheck() notices that within a couple
 *  of seconds. */
void showNeedsPlayer() {
  if (!services::ha::configured()) {
    showNeedsHa();
    showMessageScreen();
  } else {
    openPlayers();
  }
}

/** Fetch Home Assistant's media players and show them to choose from. The
 *  fetch blocks for a round trip, so a card goes up first. */
void openPlayers() {
  ui::showLoading("players");
  const bool fetched = services::players::refresh();
  g_screen = Screen::kPlayers;
  ui::showPlayers(fetched || services::players::entryCount() > 0
                      ? ""
                      : "Could not load players");
  g_list_opened_ms = millis();
}

/** A player tapped on the list: store it, as the portal would, and start
 *  following it. */
void choosePlayer() {
  char entity[config::kEntityIdMaxLen];
  snprintf(entity, sizeof(entity), "%s", ui::playerChosen());
  if (entity[0] == '\0') {
    return;
  }
  services::ha::selectEntity(entity);
  ui::clearArtwork();
  LOG_INFO("Player selected on the device: %s", entity);
  ui::showLoading(services::ha::entityLabel(entity));
  showMessageScreen();
  g_poll_now = true;
}


/** A tap in the volume's half of now playing: toggle the volume device's
 *  power with media_player.toggle, which switches it off if Home Assistant
 *  has it on and on if it has it off. The decision is Home Assistant's, on
 *  its own state at the moment of the call, rather than the remote's copy of
 *  it -- which this tap must not depend on.
 *
 *  Then the state is asked for afresh, and if the device came on it gets
 *  what play's power-on gives it -- the player's input and the player volume
 *  at switch-on -- since this is someone here asking to hear it. A zone
 *  amplifier needs the input: a Triad output switched off is disconnected
 *  from its input, and one switched back on with none is silent. That state
 *  is also put on screen at once, the volume grey while the device is off,
 *  rather than waiting on the stream. */
void togglePower() {
  if (!services::ha::configured()) {
    return;
  }
  char control[config::kEntityIdMaxLen];
  char player[config::kEntityIdMaxLen];
  services::ha::copyControlEntity(control, sizeof(control));
  services::ha::copySelectedEntity(player, sizeof(player));
  if (control[0] == '\0' || player[0] == '\0') {
    return;
  }

  LOG_INFO("UI: tap toggles %s", control);
  if (!services::ha::callService("toggle", control)) {
    LOG_WARN("UI: toggle of %s refused", control);
    return;
  }

  PlayerState now;
  if (!services::ha::fetchState(player, now)) {
    g_poll_now = true;  // the stream will say, in its own time
    return;
  }
  LOG_INFO("UI: %s is now %s", control, now.control_off ? "off" : "on");
  if (!now.control_off) {
    selectPlayerSource(now);
    pinPlayerVolume();
  }
  publishState(now);
  g_poll_now = true;
}

/** The most rooms Isolate deals with at once: more than any zone amplifier
 *  here has outputs. */
constexpr int kMaxPeers = 16;
/** When Isolate's card last gave way to now playing, or 0. Loop only. */
unsigned long g_isolate_left_ms = 0;

/** Split `list` in place at each `sep`, into at most `capacity` pieces.
 *  Returns how many; none for an empty list. */
int splitList(char* list, char sep, const char** out, int capacity) {
  int count = 0;
  char* p = list;
  while (*p != '\0' && count < capacity) {
    out[count++] = p;
    char* end = strchr(p, sep);
    if (end == nullptr) {
      break;
    }
    *end = '\0';
    p = end + 1;
  }
  return count;
}

/** The "+N rooms" chip: ask before turning the other rooms off, naming them.
 *  Isolate and Cancel on the card; a swipe right cancels too. */
void openIsolateConfirm() {
  PlayerState snapshot;
  if (!snapshotState(snapshot) || snapshot.peer_count <= 0) {
    return;
  }
  const char* rooms[kMaxPeers];
  ui::IsolateCard card;
  card.title = "Isolate this room?";
  card.note = "Turns off";
  card.rooms = rooms;
  card.room_count = splitList(snapshot.peer_names, services::ha::kPeerNameSep,
                              rooms, kMaxPeers);
  card.buttons = ui::IsolateCard::Buttons::kAsk;
  g_screen = Screen::kIsolate;
  ui::showIsolate(card);
  g_list_opened_ms = millis();
}

/** Isolate confirmed: turn off every other zone on this room's input,
 *  leaving this room playing, and go back to now playing -- where the chip
 *  going is what says it worked. Only a failure stays on the card, naming the
 *  rooms still on. The zones are the ones
 *  the state names now, not when the card went up -- one may have changed
 *  input since -- and the state is the subscription's, as fresh as the rest
 *  of it and with no request of its own: a second TLS session for one is
 *  more internal RAM than a board with the stream open can be sure of.
 *  Zone Source Auto-Shutoff, which stops a source no zone is using any
 *  more, leaves it alone -- this room still is. */
void isolateRoom() {
  ui::IsolateCard card;
  card.buttons = ui::IsolateCard::Buttons::kOk;
  PlayerState snapshot;
  if (!snapshotState(snapshot)) {
    card.title = "Couldn't isolate";
    card.note = "No word from Home Assistant";
    card.warning = true;
    ui::showIsolate(card);
    return;
  }
  const char* names[kMaxPeers];
  const char* ids[kMaxPeers];
  const int named = splitList(snapshot.peer_names, services::ha::kPeerNameSep,
                              names, kMaxPeers);
  const int count = splitList(snapshot.peer_ids, ',', ids, kMaxPeers);
  if (count == 0) {
    LOG_INFO("UI: isolate: no other room on this input now");
    leaveIsolate();
    return;
  }

  // Each room is a round trip; the card says what is happening meanwhile.
  card.title = "Isolating";
  card.note = "Turning off";
  card.rooms = names;
  card.room_count = named;
  card.buttons = ui::IsolateCard::Buttons::kNone;
  ui::showIsolate(card);

  const char* still_on[kMaxPeers];
  int still_count = 0;
  for (int i = 0; i < count; ++i) {
    LOG_INFO("UI: isolate: turning off %s", ids[i]);
    if (!services::ha::callService("turn_off", ids[i])) {
      LOG_WARN("UI: isolate: %s not turned off (%s)", ids[i],
               services::ha::lastError());
      still_on[still_count++] = i < named ? names[i] : ids[i];
    }
  }
  g_poll_now = true;

  if (still_count == 0) {
    leaveIsolate();
    return;
  }
  card.title = "Couldn't isolate";
  card.note = "Still on";
  card.rooms = still_on;
  card.room_count = still_count;
  card.warning = true;
  card.buttons = ui::IsolateCard::Buttons::kOk;
  ui::showIsolate(card);
}

/** Isolate's card, cancelled or done with: back to now playing, and the
 *  clock started on the taps it refuses for a moment. */
void leaveIsolate() {
  g_isolate_left_ms = millis();
  showNowPlaying();
}

/** Whether `intent` is a tap on now playing that has just been refused,
 *  because Isolate's card gave way to it under the finger. */
bool tapGuardedAfterIsolate(Intent intent) {
  if (g_isolate_left_ms == 0 || g_screen != Screen::kNowPlaying ||
      millis() - g_isolate_left_ms >= config::kIsolateTapGuardMs) {
    return false;
  }
  switch (intent) {
    case Intent::kPrevious:
    case Intent::kPlayPause:
    case Intent::kNext:
    case Intent::kTogglePower:
    case Intent::kIsolate:
      LOG_DEBUG("UI: tap refused, Isolate's card has only just gone");
      return true;
    default:
      return false;
  }
}

/** The media_player service an intent maps to, or nullptr when it is not a
 *  transport command at all. */
const char* serviceFor(Intent intent) {
  switch (intent) {
    case Intent::kPrevious:
      return "media_previous_track";
    case Intent::kNext:
      return "media_next_track";
    case Intent::kPlayPause:
      return "media_play_pause";
    default:
      return nullptr;
  }
}

void sendCommand(Intent intent) {
  const char* service = serviceFor(intent);
  if (service == nullptr) {
    return;
  }
  if (services::ha::selectedEntity()[0] == '\0') {
    showNeedsPlayer();
    return;
  }
  // To whatever the room is hearing: when the volume device is on another
  // input, the player that input carries or the device itself, whatever it is
  // doing -- play on a zone switched to another input is play on that input,
  // idle or not, and never takes the zone back for this remote's player; the
  // list and the search are for that. The player otherwise.
  PlayerState before;
  const bool have_before = snapshotState(before);
  const bool elsewhere = have_before && before.media_entity[0] != '\0';
  const char* entity =
      elsewhere ? before.media_entity : services::ha::selectedEntity();
  // An outside app casting to a Music Assistant player -- a phone's YouTube
  // Music -- answers to the native entity behind it, which the state names.
  if (have_before && before.transport_entity[0] != '\0') {
    entity = before.transport_entity;
  }

  // Light the button for the duration of the round trip: the call itself is
  // the press feedback, so nothing extra has to be timed.
  // An amplifier that is off will not make a sound whatever the player does,
  // so play powers it first and then plays -- in that order, so the amp is
  // awake before the audio starts rather than a second into the track. Only
  // a press that starts playback: one that pauses is not asking to hear
  // anything, and skip-next on a dark room is a mis-tap. Not while the room
  // is hearing another input either: then play is that input's.
  //
  // Play and pause are sent as what they are, media_play or media_pause, not
  // as the toggle: whether this press starts or pauses is decided here from
  // the state of the entity it goes to, and a toggle would undo that on one
  // whose state the snapshot did not have -- a player playing unheard behind
  // another input was paused by the press meant to start it.
  ui::showCommandPending(intent, true);
  const bool starting =
      !have_before || before.playback != PlaybackState::kPlaying;
  if (intent == Intent::kPlayPause) {
    service = starting ? "media_play" : "media_pause";
  }
  if (intent == Intent::kPlayPause && starting && !elsewhere) {
    powerOnControl();
  }
  const bool ok = services::ha::callService(service, entity);
  ui::showCommandPending(intent, false);

  if (!ok) {
    return;
  }

  // Show what was asked for at once, so the glyph answers immediately; the
  // poll that follows replaces this with whatever HA actually did.
  if (intent == Intent::kPlayPause && g_state_mutex != nullptr &&
      xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    g_state.playback =
        starting ? PlaybackState::kPlaying : PlaybackState::kPaused;
    xSemaphoreGive(g_state_mutex);
    g_state_dirty = true;
  }

  g_poll_now = true;
}




void handleWifiState() {
  if (WiFi.status() == WL_CONNECTED) {
    g_wifi_down_since = 0;
    return;
  }
  if (g_screen == Screen::kWifi) {
    return;  // someone is choosing a network; a reconnect would take the screen
  }

  if (g_wifi_down_since == 0) {
    g_wifi_down_since = millis();
    LOG_WARN("WiFi lost - will reconnect");
  }

  const unsigned long down_ms = millis() - g_wifi_down_since;
  if (down_ms < config::kWifiDownGraceMs ||
      millis() - g_last_reconnect_ms < config::kWifiReconnectIntervalMs) {
    return;
  }

  g_last_reconnect_ms = millis();
  if (wifiReconnect()) {
    g_wifi_down_since = 0;
    showNowPlaying();
  }
}

/** Re-evaluate a status card periodically: the settings it complains about can
 *  be fixed from the LAN portal while it is on screen. */
void handleMessageRecheck() {
  if ((g_screen != Screen::kMessage && g_screen != Screen::kPlayers) ||
      millis() - g_message_recheck_ms < 2000) {
    return;
  }
  g_message_recheck_ms = millis();

  // The player list is up and the portal has chosen one meanwhile: on to it,
  // as a tap on the list would have.
  if (g_screen == Screen::kPlayers) {
    if (services::ha::selectedEntity()[0] != '\0') {
      ui::showLoading(
          services::ha::entityLabel(services::ha::selectedEntity()));
      showMessageScreen();
      g_poll_now = true;
    }
    return;
  }

  if (!services::ha::configured() || WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (services::ha::selectedEntity()[0] == '\0') {
    return;
  }
  g_poll_now = true;
}

/** Linking to Home Assistant, from the loop: look for it once Wi-Fi is up
 *  and nothing is linked, put the QR code up when it is found, and finish
 *  the sign-in once a browser comes back with a code. */
unsigned long g_ha_discover_ms = 0;
bool g_ha_discover_tried = false;

void handleHaLink() {
  if (services::ha::configured() || WiFi.status() != WL_CONNECTED ||
      g_screen != Screen::kMessage) {
    return;
  }

  if (services::ha_link::codeArrived()) {
    ui::showLoading("Home Assistant link");
    if (services::ha_link::complete()) {
      // Linked. The player is the one thing left to choose.
      if (services::ha::selectedEntity()[0] == '\0') {
        showNeedsPlayer();
      } else {
        ui::showLoading(
            services::ha::entityLabel(services::ha::selectedEntity()));
        showMessageScreen();
        g_poll_now = true;
      }
    } else {
      ui::showHaUnreachable(services::ha_link::lastError());
      showMessageScreen();
      delay(3000);
      showNeedsHa();
    }
    return;
  }

  // Not found yet: once at the start, then every half minute, since Home
  // Assistant may still be starting, or on the network a little later.
  if (services::ha_link::ready()) {
    return;
  }
  const unsigned long now = millis();
  if (g_ha_discover_tried && now - g_ha_discover_ms < 30000UL) {
    return;
  }
  g_ha_discover_tried = true;
  g_ha_discover_ms = now;
  if (services::ha_link::discover()) {
    showNeedsHa();
  }
}

/** Start whatever the finger picked out of a list, and acknowledge it the
 *  same way in both cases: the display has already put a "Starting..." frame
 *  up, and no card is drawn here on purpose so the screen carries straight on
 *  from the tap to the track with nothing flashing in between. */
void startPlaying(bool started) {
  showMessageScreen();
  g_poll_now = true;
  (void)started;
}

// --- Choosing a network on the device ------------------------------------------

constexpr int kMaxWifiNetworks = 24;
WifiNetwork g_wifi_networks[kMaxWifiNetworks];
/** A scan has been started for the network list and not yet collected. */
bool g_wifi_scanning = false;

/** Open the network list and start the scan that fills it. `note`, when not
 *  empty, takes the title's place: why the last join failed. */
void openWifi(const char* note) {
  WifiLinkStatus link;
  wifiLinkStatus(link);
  g_screen = Screen::kWifi;
  ui::showWifi(note != nullptr ? note : "", link.connected ? link.ssid : "");
  g_wifi_scanning = wifiScanStart();
  if (!g_wifi_scanning) {
    ui::showWifiNetworks(g_wifi_networks, 0);
  }
  g_list_opened_ms = millis();
}

/** Each pass while the list is up: hand it the scan once that is done. */
void collectWifiScan() {
  if (!g_wifi_scanning || g_screen != Screen::kWifi) {
    return;
  }
  const int found = wifiScanPoll(g_wifi_networks, kMaxWifiNetworks);
  if (found == -1) {
    return;  // still scanning
  }
  g_wifi_scanning = false;
  ui::showWifiNetworks(g_wifi_networks, found > 0 ? found : 0);
}

/** A join on the device failed, from either path: back to the list, saying so. */
void onJoinFailed(const char* ssid) {
  char note[64];
  snprintf(note, sizeof(note), "Could not join %s", ssid);
  openWifi(note);
}

void onPortalStarted() {
  ui::showPortal();
  g_screen = Screen::kMessage;
}

// --- Settings on the device -----------------------------------------------------

/** The settings page's rows, in the order they may be shown. The input and
 *  the player volume at switch-on only while a separate device carries
 *  volume and power, as in the portal. */
enum class Setting : uint8_t {
  kWifi,
  kPlayer,
  kControl,
  kInput,
  kWakeVolume,
  kTxPower,
  kKeyboard,
  kRotation,
  kRestart,
  kPortal,  // where the rest is set: a hint, and a tap does nothing
};

constexpr int kMaxSettingRows = 10;
/** What each row of the page on screen is. Loop only, as is all of this. */
Setting g_setting_rows[kMaxSettingRows];
int g_setting_row_count = 0;
/** True while one setting's choices are up, rather than the page. */
bool g_settings_choosing = false;
/** Which setting they are, and its row on the page, to come back to. */
Setting g_setting_open = Setting::kWifi;
int g_setting_open_row = -1;
/** The network list was opened from the settings page, and goes back to it. */
bool g_wifi_from_settings = false;

/** The values behind a short list of choices, by row. The player and input
 *  lists are read by row from services::players and g_sources instead. */
constexpr int kMaxChoiceValues = 16;
int g_choice_values[kMaxChoiceValues];
int g_choice_count = 0;

/** The volume device's inputs, for its list of choices: PSRAM, claimed on
 *  first use and kept. */
char (*g_sources)[config::kSourceNameMaxLen] = nullptr;
int g_source_count = 0;

const char* const kRotationNames[4] = {"Upright", "+90 (clockwise)", "180",
                                       "-90 (counter-clockwise)"};

/** The player's name from the last state, as the status page shows it, or
 *  its entity id until there is one. */
const char* playerName(char* out, size_t out_len, bool control) {
  char entity[config::kEntityIdMaxLen];
  if (control) {
    services::ha::copyControlEntity(entity, sizeof(entity));
  } else {
    services::ha::copySelectedEntity(entity, sizeof(entity));
  }
  PlayerState state;
  const char* name = snapshotState(state)
                         ? (control ? state.control_name : state.player_name)
                         : "";
  snprintf(out, out_len, "%s",
           name[0] != '\0' ? name : services::ha::entityLabel(entity));
  return out;
}

void formatTxPower(char* out, size_t out_len, int quarter_dbm) {
  snprintf(out, out_len, "%g dBm%s", quarter_dbm / 4.0,
           quarter_dbm == config::kWifiTxPowerQuarterDbm ? " (default)" : "");
}

void formatWakeVolume(char* out, size_t out_len, int percent) {
  if (percent < 0) {
    snprintf(out, out_len, "Off");
  } else {
    snprintf(out, out_len, "%d%%", percent);
  }
}

void addSettingRow(Setting setting, const char* label, const char* value) {
  if (g_setting_row_count >= kMaxSettingRows) {
    return;
  }
  g_setting_rows[g_setting_row_count++] = setting;
  ui::settingsAdd(label, value);
}

/** The settings page, from what is stored now, with row `focus` in view. */
void showSettingsPage(int focus) {
  g_screen = Screen::kSettings;
  g_settings_choosing = false;
  g_setting_row_count = 0;
  ui::settingsBegin("Settings", "");

  WifiLinkStatus link;
  wifiLinkStatus(link);
  addSettingRow(Setting::kWifi, "Wi-Fi network",
                link.connected ? link.ssid : "Not connected");

  char value[config::kSourceNameMaxLen];
  addSettingRow(Setting::kPlayer, "Media player",
                services::ha::selectedEntity()[0] != '\0'
                    ? playerName(value, sizeof(value), false)
                    : "None");
  const bool separate = services::ha::controlIsSeparate();
  addSettingRow(Setting::kControl, "Volume & power",
                separate ? playerName(value, sizeof(value), true)
                         : "The media player");
  if (separate) {
    const char* input = services::ha::storedControlInput();
    addSettingRow(Setting::kInput, "Player's input",
                  input[0] != '\0' ? input : "Named after the player");
    formatWakeVolume(value, sizeof(value),
                     services::ha::playerWakeVolumePercent());
    addSettingRow(Setting::kWakeVolume, "Player volume at switch-on", value);
  }
  formatTxPower(value, sizeof(value), wifiTxPower());
  addSettingRow(Setting::kTxPower, "Wi-Fi transmit power", value);
  addSettingRow(Setting::kKeyboard, "Search keyboard",
                services::display::keyboardLayout() ==
                        services::display::KeyboardLayout::kQwerty
                    ? "QWERTY"
                    : "Alphabetical");
  addSettingRow(Setting::kRotation, "Screen rotation",
                kRotationNames[services::display::rotation() & 3]);
  addSettingRow(Setting::kRestart, "Restart", "");
  // The name, the Home Assistant link and Music Assistant stay in the
  // portal: typed text, which the portal does better.
  snprintf(value, sizeof(value), "%s.local", services::device::name());
  addSettingRow(Setting::kPortal, "More at", value);

  ui::showSettings(focus);
  g_list_opened_ms = millis();
}

/** Open the settings page, the gear on the status page. */
void openSettings() { showSettingsPage(-1); }

void addChoice(const char* text, int value, bool current) {
  if (g_choice_count >= kMaxChoiceValues) {
    return;
  }
  g_choice_values[g_choice_count++] = value;
  ui::settingsAdd(text, "",
                  current ? ui::SettingStyle::kCurrent
                          : ui::SettingStyle::kNormal);
}

/** The list of choices behind one row of the page. */
void openSettingChoices(Setting setting, int row) {
  g_setting_open = setting;
  g_setting_open_row = row;
  g_choice_count = 0;
  char text[48];

  switch (setting) {
    case Setting::kWifi:
      openWifi(nullptr);
      g_wifi_from_settings = true;
      return;

    case Setting::kPlayer:
    case Setting::kControl: {
      if (services::players::stale()) {
        ui::showLoading("players");
      }
      const bool have = services::players::ensureEntries();
      const bool control = setting == Setting::kControl;
      ui::settingsBegin(control ? "Volume & power" : "Media player",
                        have ? "" : "Could not load players");
      const char* current = control ? services::ha::storedControlEntity()
                                    : services::ha::selectedEntity();
      if (control) {
        // First, as in the portal: the player carrying its own volume.
        ui::settingsAdd("The media player", "",
                        current[0] == '\0' ? ui::SettingStyle::kCurrent
                                           : ui::SettingStyle::kNormal);
      }
      for (int i = 0; i < services::players::entryCount(); ++i) {
        const services::ha::PlayerEntry* entry = services::players::entryAt(i);
        if (entry == nullptr) {
          continue;
        }
        // Unavailable ones are still offered -- one that is off now may well
        // be on later -- but greyed, so the live ones stand out.
        ui::settingsAdd(entry->name[0] != '\0' ? entry->name : entry->entity_id,
                        "",
                        strcmp(entry->entity_id, current) == 0
                            ? ui::SettingStyle::kCurrent
                        : entry->available ? ui::SettingStyle::kNormal
                                           : ui::SettingStyle::kMuted);
      }
      break;
    }

    case Setting::kInput: {
      ui::showLoading("inputs");
      if (g_sources == nullptr) {
        g_sources = static_cast<char (*)[config::kSourceNameMaxLen]>(
            heap_caps_calloc(config::kMaxSources, config::kSourceNameMaxLen,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      }
      const int n = g_sources != nullptr
                        ? services::ha::fetchSources(
                              services::ha::controlEntity(), g_sources,
                              config::kMaxSources)
                        : -1;
      g_source_count = n > 0 ? n : 0;
      const char* current = services::ha::storedControlInput();
      ui::settingsBegin("Player's input",
                        n < 0 ? "Could not load inputs" : "");
      ui::settingsAdd("Named after the player", "",
                      current[0] == '\0' ? ui::SettingStyle::kCurrent
                                         : ui::SettingStyle::kNormal);
      for (int i = 0; i < g_source_count; ++i) {
        ui::settingsAdd(g_sources[i], "",
                        strcmp(g_sources[i], current) == 0
                            ? ui::SettingStyle::kCurrent
                            : ui::SettingStyle::kNormal);
      }
      break;
    }

    case Setting::kWakeVolume: {
      ui::settingsBegin("Switch-on volume", "");
      const int current = services::ha::playerWakeVolumePercent();
      addChoice("Off - leave it alone", -1, current < 0);
      for (int pct = 10; pct <= 100; pct += 10) {
        // One set in the portal off the steps is kept on offer, where it
        // would come in order.
        if (current >= pct - 10 && current < pct &&
            (current % 10 != 0 || current == 0)) {
          formatWakeVolume(text, sizeof(text), current);
          addChoice(text, current, true);
        }
        formatWakeVolume(text, sizeof(text), pct);
        addChoice(text, pct, current == pct);
      }
      break;
    }

    case Setting::kTxPower: {
      ui::settingsBegin("Transmit power", "");
      const WifiTxPowerStep* steps = nullptr;
      const int n = wifiTxPowerSteps(&steps);
      const int8_t current = wifiTxPower();
      for (int i = 0; i < n; ++i) {
        formatTxPower(text, sizeof(text), steps[i].quarter_dbm);
        addChoice(text, steps[i].quarter_dbm,
                  steps[i].quarter_dbm == current);
      }
      break;
    }

    case Setting::kKeyboard: {
      ui::settingsBegin("Search keyboard", "");
      const bool qwerty = services::display::keyboardLayout() ==
                          services::display::KeyboardLayout::kQwerty;
      addChoice("Alphabetical", 0, !qwerty);
      addChoice("QWERTY", 1, qwerty);
      break;
    }

    case Setting::kRotation: {
      ui::settingsBegin("Rotation", "");
      for (int r = 0; r < 4; ++r) {
        addChoice(kRotationNames[r], r, r == services::display::rotation());
      }
      break;
    }

    case Setting::kRestart:
      ui::settingsBegin("Restart?", "");
      addChoice("Restart now", 1, false);
      addChoice("Cancel", 0, false);
      break;

    case Setting::kPortal:
      return;
  }

  g_screen = Screen::kSettings;
  g_settings_choosing = true;
  ui::showSettings();
  g_list_opened_ms = millis();
}

void restartNow(const char* why) {
  ui::showRestarting(why);
  LOG_WARN("Restarting from the settings page: %s", why);
  delay(800);  // long enough to read the card
  ESP.restart();
}

/** Store the choice tapped on row `row` of the list that is up. */
void applySettingChoice(int row) {
  const int value = row >= 0 && row < g_choice_count ? g_choice_values[row] : 0;
  bool changed = false;
  switch (g_setting_open) {
    case Setting::kPlayer: {
      const services::ha::PlayerEntry* entry = services::players::entryAt(row);
      if (entry == nullptr) {
        return;
      }
      if (strcmp(entry->entity_id, services::ha::selectedEntity()) != 0) {
        services::ha::selectEntity(entry->entity_id);
        // The new player has its own art, and the old one's is on screen.
        ui::clearArtwork();
        LOG_INFO("Player selected on the device: %s", entry->entity_id);
        g_poll_now = true;
        changed = true;
      }
      break;
    }
    case Setting::kControl: {
      // Row 0 is the player itself; the players follow it.
      const services::ha::PlayerEntry* entry =
          row > 0 ? services::players::entryAt(row - 1) : nullptr;
      if (row > 0 && entry == nullptr) {
        return;
      }
      const char* chosen = entry != nullptr ? entry->entity_id : "";
      if (strcmp(chosen, services::ha::storedControlEntity()) != 0) {
        ui::showLoading("Volume & power");
        wifiSelectControl(chosen);
        LOG_INFO("Volume & power selected on the device: %s",
                 chosen[0] != '\0' ? chosen : "the media player");
        g_poll_now = true;
        changed = true;
      }
      break;
    }
    case Setting::kInput: {
      if (row < 0 || row > g_source_count) {
        return;
      }
      const char* chosen = row == 0 ? "" : g_sources[row - 1];
      if (strcmp(chosen, services::ha::storedControlInput()) != 0) {
        services::ha::selectControlInput(chosen);
        changed = true;
      }
      break;
    }
    case Setting::kWakeVolume:
      if (value != services::ha::playerWakeVolumePercent()) {
        services::ha::savePlayerWakeVolumePercent(value);
        changed = true;
      }
      break;
    case Setting::kTxPower:
      changed = value != wifiTxPower() &&
                wifiSetTxPower(static_cast<int8_t>(value));
      break;
    case Setting::kKeyboard: {
      const auto chosen = value == 1
                              ? services::display::KeyboardLayout::kQwerty
                              : services::display::KeyboardLayout::kAlphabetical;
      if (chosen != services::display::keyboardLayout()) {
        services::display::saveKeyboardLayout(chosen);
        changed = true;
      }
      break;
    }
    case Setting::kRotation:
      if (value != services::display::rotation()) {
        services::display::saveRotation(static_cast<uint8_t>(value));
        restartNow("Turning the screen");
      }
      break;
    case Setting::kRestart:
      if (value == 1) {
        restartNow("");
      }
      break;
    case Setting::kWifi:
    case Setting::kPortal:
      break;
  }
  if (changed) {
    wifiSettingsChanged();
  }
  showSettingsPage(g_setting_open_row);
}

void handleSettingsTap(int row) {
  if (g_settings_choosing) {
    applySettingChoice(row);
  } else if (row >= 0 && row < g_setting_row_count) {
    openSettingChoices(g_setting_rows[row], row);
  }
}

/** A swipe right: from a list of choices to the page, unchanged, and from
 *  the page back to the status page it was opened from. */
void handleSettingsBack() {
  if (g_settings_choosing) {
    showSettingsPage(g_setting_open_row);
  } else {
    openStatus();
  }
}

/** What the Wi-Fi screens and the cards that lead to them ask for. The same
 *  from the setup portal's loop and from the running remote; only the join
 *  differs, since in the portal it is the portal's loop that makes it. */
void handleWifiIntent(const ui::Input& in) {
  switch (in.intent) {
    case Intent::kTapCard:
      // The Setup card, or No Wi-Fi: a tap opens the network list. Any other
      // card is about Home Assistant, and a tap there does nothing.
      if (wifiPortalActive() || WiFi.status() != WL_CONNECTED) {
        g_wifi_from_settings = false;  // from a card
        openWifi(nullptr);
      } else if (services::ha::configured() &&
                 services::ha::selectedEntity()[0] == '\0') {
        openPlayers();  // a card left up while there is no player
      }
      break;
    case Intent::kOpenWifi:
      g_wifi_from_settings = false;  // from the status page
      openWifi(nullptr);
      break;
    case Intent::kWifiRescan:
      openWifi(nullptr);
      break;
    case Intent::kWifiJoin: {
      char ssid[33];
      char pass[65];
      snprintf(ssid, sizeof(ssid), "%s", ui::wifiChosenSsid());
      snprintf(pass, sizeof(pass), "%s", ui::wifiPassword());
      if (ssid[0] == '\0') {
        break;
      }
      if (wifiPortalActive()) {
        wifiRequestJoin(ssid, pass);  // the portal's loop joins next
        break;
      }
      g_screen = Screen::kMessage;
      if (wifiJoinNow(ssid, pass)) {
        g_wifi_down_since = 0;
        showNowPlaying();
      } else {
        onJoinFailed(ssid);
      }
      break;
    }
    case Intent::kWifiLeave:
      if (wifiPortalActive()) {
        onPortalStarted();
      } else if (g_wifi_from_settings) {
        g_wifi_from_settings = false;
        showSettingsPage(g_setting_open_row);
      } else if (WiFi.status() == WL_CONNECTED) {
        openStatus();
      } else {
        ui::showConnectFailed();
        showMessageScreen();
      }
      break;
    default:
      break;
  }
}

/** The setup portal's turn for the display: while wifiSetupConnect() waits
 *  in the portal, nothing else reads the touch screen. */
void portalIdle() {
  collectWifiScan();
  const ui::Input in = ui::poll(g_screen, nullptr);
  handleWifiIntent(in);
}

/** One pass of the input loop: ask the display what the finger asked for, and
 *  do it. Everything arriving here is an intent -- never a coordinate, never
 *  a pixel -- which is what lets a different panel answer the same calls. */
void handleInput() {
  PlayerState snapshot;
  const bool have_state = snapshotState(snapshot);
  ui::Input in = ui::poll(g_screen, have_state ? &snapshot : nullptr);
  if (tapGuardedAfterIsolate(in.intent)) {
    in.intent = Intent::kNone;
  }

  switch (in.intent) {
    case Intent::kNone:
      break;

    case Intent::kWokeFromTouch:
      noteTouchWake();
      break;

    case Intent::kPrevious:
    case Intent::kPlayPause:
    case Intent::kNext:
      sendCommand(in.intent);
      break;

    case Intent::kSetVolume:
      // The control entity, not the player: where they differ it is the
      // receiver that owns the knob, and volume_set on the player would
      // either fail or move a software gain nobody can hear.
      commandVolume(services::ha::controlEntity(), in.level);
      g_poll_now = true;
      break;

    case Intent::kTogglePower:
      togglePower();
      break;

    case Intent::kIsolate:
      openIsolateConfirm();
      break;

    case Intent::kIsolateConfirm:
      isolateRoom();
      break;

    case Intent::kIsolateCancel:
      leaveIsolate();
      break;

    case Intent::kOpenBrowse:
      openBrowse();
      break;

    case Intent::kOpenSearch:
      openSearch();
      break;

    case Intent::kOpenStatus:
      openStatus();
      break;

    case Intent::kBackToNowPlaying:
      showNowPlaying();
      break;

    case Intent::kPlayBrowseRow:
      // The item has its own art and title, and Music Assistant takes a
      // moment to switch; drop the cached cover so the old one cannot sit
      // behind the new name. Power first, so the amplifier has the length of
      // the play_media round trip -- seconds, for an artist -- to wake up;
      // and it is switched to the player's input whatever it was on, since
      // picking something to play here is asking to hear it.
      ui::clearArtwork();
      powerOnControl();
      startPlaying(services::browse::play(in.index));
      break;

    case Intent::kRunSearch:
      services::search::run();
      ui::showSearchResults();
      break;

    case Intent::kPlaySearchResult:
      ui::clearArtwork();
      powerOnControl();
      startPlaying(services::search::play(in.index));
      break;

    case Intent::kChoosePlayer:
      choosePlayer();
      break;

    case Intent::kPlayersRefresh:
      openPlayers();
      break;

    case Intent::kOpenSettings:
      openSettings();
      break;

    case Intent::kSettingsTap:
      handleSettingsTap(in.index);
      break;

    case Intent::kSettingsBack:
      handleSettingsBack();
      break;

    case Intent::kTapCard:
    case Intent::kOpenWifi:
    case Intent::kWifiRescan:
    case Intent::kWifiJoin:
    case Intent::kWifiLeave:
      handleWifiIntent(in);
      break;
  }

  // After the intent, not before: the release of a drag comes back as its
  // final kSetVolume, and commandVolume() has to have recorded that level
  // before the flag drops, or a state update landing in between carries the
  // old level and nothing holds it back.
  const bool dragging = ui::volumeDragging();
  g_volume_dragging = dragging;
  if (!dragging && g_repaint_after_drag.exchange(false)) {
    g_state_dirty = true;
  }
}

/** How long streamService() may wait for state before the task goes round
 *  again for the power flags and the preload. */
constexpr uint32_t kStreamSliceMs = 250;
/** A stream that lasted this long was a working one, and is reopened at once
 *  when it drops. One that died sooner -- a subscription the server refused,
 *  a template it rejected -- counts as a failure to open, and backs off. */
constexpr unsigned long kStreamHealthyMs = 60000;

/** All network work lives here so a blocking request never stalls the touch
 *  loop.
 *
 *  State comes from Home Assistant's WebSocket stream while one is open --
 *  pushed the moment anything changes -- and from polling while it is not:
 *  before it first opens, after it drops, or when the server will not give
 *  one. A stream that fails to open, or dies soon after it does, is retried
 *  on a doubling backoff; one that had been working is reopened at once. */
void haPollTask(void*) {
  unsigned long stream_retry_at = 0;
  unsigned long stream_backoff = config::kHaStreamRetryMinMs;
  unsigned long stream_opened_ms = 0;
  auto stream_failed = [&](const char* how) {
    LOG_WARN("HA: stream %s, polling; next try in %lu s", how,
                  stream_backoff / 1000);
    stream_retry_at = millis() + stream_backoff;
    stream_backoff = stream_backoff * 2 < config::kHaStreamRetryMaxMs
                         ? stream_backoff * 2
                         : config::kHaStreamRetryMaxMs;
  };
  for (;;) {
    unsigned long interval = config::kHaPollIdleMs;
    bool streaming = false;

    // A pick made from the touch loop changed the room's history; the flash
    // write is done here rather than there.
    services::history::persist();

    // A stream over a dead link would sit there until its silence timeout,
    // taking service calls it cannot deliver.
    if (services::ha::streamIsOpen() &&
        (WiFi.status() != WL_CONNECTED || !services::ha::configured())) {
      services::ha::streamClose();
    }

    // A copy, not the pointer. selectedEntity() hands back the live buffer,
    // and the portal rewrites that buffer from the Arduino task when someone
    // saves a different player -- so holding the pointer across a fetch means
    // the entity_id can change, or be half-written, while it is being read
    // into a request. Taken under the lock the portal writes with, so the
    // copy cannot catch it half-written either.
    char entity[config::kEntityIdMaxLen] = {};
    services::ha::copySelectedEntity(entity, sizeof(entity));

    if (WiFi.status() == WL_CONNECTED && services::ha::configured() &&
        entity[0] != '\0') {
      if (g_turn_off_control) {
        g_turn_off_control = false;
        powerOffControl();
      }

      // Load the browse list ahead of anyone asking for it, so a swipe up
      // lands on a list rather than on a loading card. Cheap to call: it only
      // does work when the list is missing or past its TTL, which is once
      // every few minutes at most.
      //
      // Skipped while the list is actually on screen. ensureLoaded() holds a
      // lock that the drawing side also takes, so doing it anyway would be
      // correct -- but it would mean scrolling stuttering against a load, and
      // there is no reason to refresh a list someone is reading.
      if (g_screen != Screen::kBrowse && g_screen != Screen::kSearch) {
        services::browse::preload();
      }

      if (config::kHaStreamEnabled && !services::ha::streamIsOpen() &&
          static_cast<long>(millis() - stream_retry_at) >= 0) {
        // The backoff is not reset here: a server can take the connection
        // and then refuse the subscription, and reopening at once each time
        // that happens is a reconnect loop that never gets any state.
        if (services::ha::streamOpen(entity)) {
          stream_opened_ms = millis();
        } else {
          stream_failed("unavailable");
        }
      }

      PlayerState fresh;
      bool got = false;
      streaming = services::ha::streamIsOpen();
      if (streaming) {
        got = services::ha::streamService(fresh, kStreamSliceMs);
        if (!services::ha::streamIsOpen()) {
          if (millis() - stream_opened_ms >= kStreamHealthyMs) {
            stream_backoff = config::kHaStreamRetryMinMs;
            stream_retry_at = millis();  // it was working: straight back
          } else {
            stream_failed("closed soon after opening");
          }
          // And poll this pass rather than leave it with nothing.
          streaming = false;
        }
      }
      if (!streaming && !got) {
        got = services::ha::fetchState(entity, fresh);
      }

      if (got) {
        // Asked for, not fetched: the artwork worker downloads it while this
        // task goes back to the stream. The loop holds the repaint a moment
        // for it (see kCoverArtHoldMs).
        ui::requestArtwork(fresh.picture);
        publishState(fresh);

        // What plays on this player is what plays in this room: the history
        // the swipe-up list's first two sections are built from. A new
        // artist costs a search, here on the network task.
        if (fresh.playback == PlaybackState::kPlaying) {
          services::history::notePlaying(fresh.artist, fresh.track);
        }

        interval = fresh.playback == PlaybackState::kPlaying
                       ? config::kHaPollPlayingMs
                       : config::kHaPollIdleMs;
      } else if (!streaming && !g_state_valid) {
        // Only worth a repaint when there is nothing already on screen; a
        // single dropped poll should not replace a good frame with an error
        // card.
        g_state_dirty = true;
      }

      // After a restart the history has names but no pictures; one is found
      // again every few seconds until the list is whole.
      services::history::refreshPictures();
    }

    if (streaming) {
      // streamService() has already waited, and a command's effect arrives
      // as an event rather than needing a poll to fetch it.
      g_poll_now = false;
      continue;
    }

    // Sleep in slices so a transport command can cut the wait short.
    constexpr unsigned long kSlice = 50;
    for (unsigned long waited = 0; waited < interval; waited += kSlice) {
      if (g_poll_now) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(kSlice));
    }
    if (g_poll_now) {
      g_poll_now = false;
      vTaskDelay(pdMS_TO_TICKS(config::kHaPollAfterCommandMs));
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  LOG_INFO("HA Media Remote");

  g_state_mutex = xSemaphoreCreateMutex();

  // The portal reports what it is doing; this is where those events become
  // screens. Everything below the app layer is written against no display.
  wifiSetObserver(WifiObserver{
      .portalStarted = onPortalStarted,
      .connectingBegan = ui::showConnecting,
      .connectingTick = ui::tickConnecting,
      .connectFailed = ui::showConnectFailed,
      .settingsCleared = ui::showSettingsCleared,
      .playerChanged = ui::clearArtwork,
      .portalIdle = portalIdle,
      .joinFailed = onJoinFailed,
  });

  bootButtonInit();
  // Stored in its own namespace, so it is there before anything else is
  // loaded -- the first screen already has to be the right way up.
  ui::init(services::display::rotation());
  // Claimed before Wi-Fi, while the heap is still unfragmented, and never
  // freed -- so it cannot fail later and cannot leave a hole.
  services::browse::init();
  services::history::init();

  if (wifiShowsSetupScreenOnBoot()) {
    ui::showPortal();
  }
  services::ha::init();
  // Before Wi-Fi, so its pages are there when the portal's server starts.
  services::ha_link::init();
  // After ha::init(): an unset Music Assistant address is derived from Home
  // Assistant's. The recommendations wait for Wi-Fi on their own task.
  services::ma::init();
  services::recommend::init();

  if (!wifiSetupConnect()) {
    showMessageScreen();
  } else if (!services::ha::configured()) {
    showNeedsHa();
    showMessageScreen();
  } else if (services::ha::selectedEntity()[0] == '\0') {
    showNeedsPlayer();
  } else {
    ui::showLoading(
        services::ha::entityLabel(services::ha::selectedEntity()));
    showMessageScreen();
  }

  // 12 KB covers TLS plus the JSON body; the touch loop stays on the Arduino
  // task. Left unpinned: letting the scheduler place it across the S3's two
  // cores is no worse than guessing which one it wants.
  //
  // Priority 0, the idle task's own, so that on core 1 it never takes time
  // from the loop.
  //
  // That is not what keeps it from starving the idle task on core 0 while it
  // waits on a slow response, and cannot be: the loop sends commands over
  // the same connection and blocks on s_http_mutex while this task holds it,
  // and priority inheritance then lifts this task to the loop's priority for
  // as long as it holds the lock. What does is the client it waits through,
  // which sleeps instead of spinning -- see services::Yielding.
  xTaskCreate(haPollTask, "ha_poll", 12288, nullptr, tskIDLE_PRIORITY,
              nullptr);
}

void loop() {
  bootButtonPollLongPress();
  wifiLoop();
  handleWifiState();

  handleIdleBlanking();

  // The display reports a tap on a dark panel as kWokeFromTouch and nothing
  // else, so the same press cannot also press a button.
  handleInput();
  collectWifiScan();
  handleListTimeout();

  handleMessageRecheck();
  handleHaLink();

  // Before the artwork, and that ordering is the whole point. What is playing
  // is why the device exists; browse thumbnails are a preload for a list
  // nobody has opened yet. Behind them, this check ran once per fetch -- and a
  // fetch is a TLS connection, which on a slow or contended link is seconds
  // each and ten of them before the first frame. The boot symptom was the
  // Loading card staying up long after Home Assistant had answered.
  // A cover the worker has finished was not in the frame on screen: repaint
  // it in. Checked first, so a cover landing while the repaint below is held
  // for it costs one frame, not two.
  if (ui::takeArtworkFinished() && g_screen == Screen::kNowPlaying) {
    g_state_dirty = true;
  }

  if (g_state_dirty && !ui::isBlanked() && holdForCover()) {
    // Waiting on this track's cover, briefly: nothing drawn this pass.
  } else if (g_state_dirty && !ui::isBlanked()) {
    g_state_dirty = false;
    // Neither the list, the search nor the status page may be replaced by a
    // poll landing underneath someone who is reading or typing.
    if (g_screen != Screen::kBrowse && g_screen != Screen::kSearch &&
        g_screen != Screen::kStatus && g_screen != Screen::kWifi &&
        g_screen != Screen::kPlayers && g_screen != Screen::kSettings &&
        g_screen != Screen::kIsolate) {
      showNowPlaying();
    } else {
      LOG_DEBUG("UI: repaint held back, a list is on screen");
    }
  }

  // The untitled label's delay has run out with the title still missing:
  // the frame that held it back is redrawn with it. A title that arrived in
  // the meantime has already repainted, and cancels this.
  if (g_untitled_repaint_owed && !untitledBriefly()) {
    g_untitled_repaint_owed = false;
    PlayerState snapshot;
    if (g_screen == Screen::kNowPlaying && snapshotState(snapshot) &&
        snapshot.title[0] == '\0') {
      g_state_dirty = true;
    }
  }

  // Thumbnails are fetched on artwork's own worker; this only queues what the
  // screen wants and repaints when some of it has arrived.
  ui::idleWork(g_screen);
  handleStatusRefresh();

  if (!ui::isBlanked() && g_screen == Screen::kNowPlaying &&
      !g_volume_dragging && millis() - g_last_elapsed_ms >= 1000) {
    g_last_elapsed_ms = millis();
    PlayerState snapshot;
    if (snapshotState(snapshot) &&
        snapshot.playback == PlaybackState::kPlaying) {
      ui::refreshElapsed(snapshot);
    }
  }

  delay(5);
}

}  // namespace app
