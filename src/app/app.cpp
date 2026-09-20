/**
 * The remote itself: what it polls, what it decides, what it sends.
 *
 * Nothing in this file draws, and nothing in it knows the panel is round. It
 * asks ui/ui.h to put a screen up and hands it back intents -- "play/pause",
 * "set the volume to 0.4", "play browse row 3" -- without ever learning where
 * the finger landed.
 *
 * Network work all happens on haPollTask; setup() and loop() own everything
 * else. The two meet at g_state (behind g_state_mutex) and at the artwork
 * cache, which serialises itself.
 */

#include <Arduino.h>
#include <WiFi.h>

#include <atomic>
#include <cmath>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "app/app.h"
#include "config.h"
#include "log.h"
#include "services/browse.h"
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
/** A tap woke the panel and the volume/power entity should be switched on.
 *  Handed to the poll task rather than called from the touch path: turn_on is
 *  a network round trip, and holding the screen black for it is exactly the
 *  wrong moment to block. */
std::atomic<bool> g_turn_on_control{false};
/** ...and the other way, once the panel has been dark long enough. */
std::atomic<bool> g_turn_off_control{false};

unsigned long g_last_elapsed_ms = 0;

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

/** Playback state the previous poll saw, for spotting the edge into activity.
 *  kUnknown until the first poll lands: the very first observation is not a
 *  transition, and treating it as one would reset the volume of a system that
 *  was happily playing before the remote was even switched on. */
PlaybackState g_last_playback = PlaybackState::kUnknown;
bool g_have_last_playback = false;

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

void showNowPlaying() {
  if (!services::ha::configured()) {
    ui::showNeedsHaSetup();
    showMessageScreen();
    return;
  }
  if (services::ha::selectedEntity()[0] == '\0') {
    ui::showNoPlayer();
    showMessageScreen();
    return;
  }

  PlayerState snapshot;
  if (!snapshotState(snapshot)) {
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

/** A tap landed on a dark panel. Reaching for a dark screen is already the
 *  gesture for "I want this", and on a separate amplifier there is nothing to
 *  hear until it is switched on -- so unlike waking because playback resumed,
 *  this powers the control entity. Whatever started playing by itself clearly
 *  did not need the help. */
void requestControlWake() {
  g_idle_since = 0;
  g_control_off_sent = false;
  g_state_dirty = true;
  if (!config::kTurnOnControlOnWake || !services::ha::configured()) {
    return;
  }
  if (services::ha::controlEntity()[0] == '\0') {
    return;
  }
  g_turn_on_control = true;
  g_poll_now = true;
}

/** Point the volume/power entity at this player, where it is a zone
 *  amplifier that lists the player as one of its inputs.
 *
 *  Switching a zone on does not route anything to it: a Triad output comes up
 *  with no input, stays silent, and its integration drops it again a minute
 *  later unless the linked player starts playing first. So a power-on the
 *  remote asks for selects the player's input as well -- straight after the
 *  turn_on, unless the zone is already on it.
 *
 *  For a zone that was already on it depends who is asking. `claim` -- play
 *  pressed on this remote -- takes the zone over whatever it is on: that is
 *  someone in this room asking to hear this player. Anything else only fills
 *  a zone with no input at all, so one deliberately switched to another source
 *  is left alone. */
void selectPlayerSource(const PlayerState& snapshot, bool just_turned_on,
                        bool claim) {
  if (!config::kSelectControlSource || !services::ha::controlIsSeparate()) {
    return;
  }
  if (snapshot.player_source[0] == '\0') {
    return;  // no input by this player's name: not a zone amplifier
  }
  if (snapshot.control_features != 0 &&
      (snapshot.control_features & services::ha::kFeatureSelectSource) == 0) {
    return;
  }
  if (strcmp(snapshot.control_source, snapshot.player_source) == 0) {
    return;  // already on it
  }
  if (!just_turned_on && !claim && snapshot.control_source[0] != '\0') {
    return;  // on something else, and nobody here asked to change that
  }
  services::ha::selectSource(services::ha::controlEntity(),
                             snapshot.player_source);
}

/** Switch the volume/power entity on, unless it is plainly unnecessary, and
 *  route this player to it where it switches inputs (selectPlayerSource();
 *  `claim_source` is that function's `claim`).
 *
 *  Skipped when the last snapshot says it is already on, and when that
 *  snapshot says it has no TURN_ON feature -- calling turn_on on an entity
 *  that does not support it just fills the Home Assistant log. A player
 *  reporting no features at all is treated as supporting everything, as
 *  everywhere else here: that is an integration that never set the attribute.
 *
 *  Blocks on the round trip, so callers on the touch path have to want the
 *  wait. Returns true if a call was actually made. */
bool powerOnControl(bool claim_source = false) {
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
      selectPlayerSource(snapshot, false, claim_source);
      return false;
    }
    if (snapshot.control_features != 0 &&
        (snapshot.control_features & services::ha::kFeatureTurnOn) == 0) {
      return false;
    }
  }

  const bool ok = services::ha::callService("turn_on", control);
  if (ok && have_state) {
    selectPlayerSource(snapshot, true, claim_source);
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

/** Whether the player is doing something, as opposed to sitting in standby.
 *
 *  Paused counts. A paused player has already been through the wake below;
 *  resuming it is not a fresh start. */
bool playerIsActive(PlaybackState playback) {
  return playback == PlaybackState::kPlaying ||
         playback == PlaybackState::kPaused;
}

/** Handle the player coming out of standby, when a separate entity carries
 *  volume and power.
 *
 *  Two things, and they belong together because they are two halves of the
 *  same handoff. The amplifier is switched on, because nothing the player does
 *  is audible until it is. And the *player's* own volume is pinned to
 *  config::kPlayerWakeVolume, because with the real knob elsewhere that value
 *  is a source gain rather than a volume, and leaving it wherever it drifted
 *  means the amplifier's setting stops meaning the same thing session to
 *  session.
 *
 *  Note which entity each targets: turn_on goes to controlEntity(), the
 *  amplifier, and volume_set to selectedEntity(), the thing that plays. Power
 *  first, so the amplifier has the length of the second round trip to wake up
 *  before there is anything to hear.
 *
 *  Does nothing when the two are the same entity: the volume last chosen there
 *  is the volume wanted back, and powering it on is already handled by the
 *  touch and play paths. */
void handlePlayerWake(PlaybackState before, PlaybackState now) {
  if (!services::ha::controlIsSeparate()) {
    return;
  }
  if (!g_have_last_playback || playerIsActive(before) ||
      !playerIsActive(now)) {
    return;
  }

  const char* player = services::ha::selectedEntity();
  if (player[0] == '\0') {
    return;
  }

  LOG_INFO("HA: %s woke from standby", player);
  powerOnControl();

  if (config::kPlayerWakeVolume >= 0.0f) {
    LOG_INFO("HA: pinning %s to %.2f", player,
                  config::kPlayerWakeVolume);
    services::ha::setVolume(player, config::kPlayerWakeVolume);
  }
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
}

/** No player is selected, and the device cannot choose one: the picker lives
 *  in the setup portal now. Say where to go and keep rechecking -- the portal
 *  stores the choice in NVS, and handleMessageRecheck() notices within a
 *  couple of seconds without a reboot. */
void showNeedsPlayer() {
  if (!services::ha::configured()) {
    ui::showNeedsHaSetup();
  } else {
    ui::showNoPlayer();
  }
  showMessageScreen();
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
  const char* entity = services::ha::selectedEntity();
  if (entity[0] == '\0') {
    showNeedsPlayer();
    return;
  }

  // Light the button for the duration of the round trip: the call itself is
  // the press feedback, so nothing extra has to be timed.
  // An amplifier that is off will not make a sound whatever the player does,
  // so play powers it first and then plays -- in that order, so the amp is
  // awake before the audio starts rather than a second into the track. Only
  // play: skip-next on a dark room is a mis-tap, not a request for music.
  ui::showCommandPending(intent, true);
  if (intent == Intent::kPlayPause) {
    // A press that will start playback also takes the zone over, whatever
    // input it is on; one that will pause leaves it be.
    PlayerState before;
    const bool starting =
        !snapshotState(before) || before.playback != PlaybackState::kPlaying;
    powerOnControl(starting);
  }
  const bool ok = services::ha::callService(service, entity);
  ui::showCommandPending(intent, false);

  if (!ok) {
    return;
  }

  // Flip play/pause locally so the glyph answers immediately; the poll that
  // follows replaces this with whatever HA actually did.
  if (intent == Intent::kPlayPause && g_state_mutex != nullptr &&
      xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (g_state.playback == PlaybackState::kPlaying) {
      g_state.playback = PlaybackState::kPaused;
    } else if (g_state.playback == PlaybackState::kPaused ||
               g_state.playback == PlaybackState::kIdle) {
      g_state.playback = PlaybackState::kPlaying;
    }
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
  if (g_screen != Screen::kMessage ||
      millis() - g_message_recheck_ms < 2000) {
    return;
  }
  g_message_recheck_ms = millis();

  if (!services::ha::configured() || WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (services::ha::selectedEntity()[0] == '\0') {
    return;
  }
  g_poll_now = true;
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

/** One pass of the input loop: ask the display what the finger asked for, and
 *  do it. Everything arriving here is an intent -- never a coordinate, never
 *  a pixel -- which is what lets a different panel answer the same calls. */
void handleInput() {
  PlayerState snapshot;
  const bool have_state = snapshotState(snapshot);
  const ui::Input in = ui::poll(g_screen, have_state ? &snapshot : nullptr);

  switch (in.intent) {
    case Intent::kNone:
      break;

    case Intent::kWokeFromTouch:
      requestControlWake();
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

    case Intent::kOpenBrowse:
      openBrowse();
      break;

    case Intent::kOpenSearch:
      openSearch();
      break;

    case Intent::kBackToNowPlaying:
      showNowPlaying();
      break;

    case Intent::kPlayBrowseRow:
      // The item has its own art and title, and Music Assistant takes a
      // moment to switch; drop the cached cover so the old one cannot sit
      // behind the new name. Power first, so the amplifier has the length of
      // the play_media round trip -- seconds, for an artist -- to wake up.
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

/** All HTTP lives here so a blocking request never stalls the touch loop. */
void haPollTask(void*) {
  for (;;) {
    unsigned long interval = config::kHaPollIdleMs;

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
      if (g_turn_on_control) {
        g_turn_on_control = false;
        g_turn_off_control = false;  // a wake outranks a pending power-off
        powerOnControl();
      }
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

      PlayerState fresh;
      if (services::ha::fetchState(entity, fresh)) {
        // Cache the art before publishing, so the repaint the main loop does
        // in response already has something to draw.
        ui::prepareArtwork(fresh.picture);
        publishState(fresh);

        handlePlayerWake(g_last_playback, fresh.playback);
        g_last_playback = fresh.playback;
        g_have_last_playback = true;

        interval = fresh.playback == PlaybackState::kPlaying
                       ? config::kHaPollPlayingMs
                       : config::kHaPollIdleMs;
      } else if (!g_state_valid) {
        // Only worth a repaint when there is nothing already on screen; a
        // single dropped poll should not replace a good frame with an error
        // card.
        g_state_dirty = true;
      }
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
      .portalStarted = ui::showPortal,
      .connectingBegan = ui::showConnecting,
      .connectingTick = ui::tickConnecting,
      .connectFailed = ui::showConnectFailed,
      .settingsCleared = ui::showSettingsCleared,
      .playerChanged = ui::clearArtwork,
  });

  bootButtonInit();
  ui::init();
  // Claimed before Wi-Fi, while the heap is still unfragmented, and never
  // freed -- so it cannot fail later and cannot leave a hole.
  services::browse::init();

  if (wifiShowsSetupScreenOnBoot()) {
    ui::showPortal();
  }
  services::ha::init();

  if (!wifiSetupConnect()) {
    showMessageScreen();
  } else if (!services::ha::configured()) {
    ui::showNeedsHaSetup();
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
  xTaskCreate(haPollTask, "ha_poll", 12288, nullptr, 1, nullptr);
}

void loop() {
  bootButtonPollLongPress();
  wifiLoop();
  handleWifiState();

  handleIdleBlanking();

  // The display reports a tap on a dark panel as kWokeFromTouch and nothing
  // else, so the same press cannot also press a button.
  handleInput();

  // Artwork arrives one image per pass rather than in a batch, so names are
  // on screen immediately and the panel still answers taps between fetches.
  ui::idleWork(g_screen);

  handleMessageRecheck();

  if (g_state_dirty && !ui::isBlanked()) {
    g_state_dirty = false;
    // Neither the list nor the search screen may be replaced by a poll
    // landing underneath someone who is reading or typing.
    if (g_screen != Screen::kBrowse && g_screen != Screen::kSearch) {
      showNowPlaying();
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
