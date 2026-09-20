#include "ui/ui.h"
#include "log.h"

#include <Arduino.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

/**
 * ui/ui.h with no panel behind it.
 *
 * The remote is perfectly capable of running without a display: it polls Home
 * Assistant, tracks the player, honours the idle timer and sends every
 * service call it would otherwise send. All that is missing is somewhere to
 * put the picture. This narrates to the serial console instead.
 *
 * It reports no input, so a headless build is a remote that watches rather
 * than one that is used.
 *
 * It exists for two reasons. It is what the firmware links against while a
 * panel is being brought up -- everything above ui/ui.h can be built and
 * exercised before the first pixel -- and it is the proof that the seam is
 * real. Anything src/app/ needs that cannot be answered here is something
 * that leaked.
 */
namespace ui {
namespace {

/** One line per change rather than per call: several of these land on every
 *  loop pass, and a console scrolling at that rate is no more readable than
 *  no console at all. */
char s_last[96] = {};
bool s_blanked = false;

void say(const char* fmt, ...) {
  char line[sizeof(s_last)];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  if (strcmp(line, s_last) == 0) {
    return;
  }
  snprintf(s_last, sizeof(s_last), "%s", line);
  LOG_INFO("UI: %s", line);
}

const char* playbackName(services::ha::PlaybackState state) {
  switch (state) {
    case services::ha::PlaybackState::kPlaying:  return "playing";
    case services::ha::PlaybackState::kPaused:   return "paused";
    case services::ha::PlaybackState::kIdle:     return "idle";
    case services::ha::PlaybackState::kOff:      return "off";
    default:                                     return "unavailable";
  }
}

}  // namespace

bool init() {
  LOG_INFO("UI: headless -- no panel, serial only");
  return true;
}

Input poll(Screen, const services::ha::PlayerState*) { return Input{}; }

void showNowPlaying(const services::ha::PlayerState& state) {
  say("%s | %s - %s | vol %.2f", playbackName(state.playback),
      state.subtitle, state.title, state.volume);
}

void showLoading(const char* what) { say("loading %s", what); }

void showCommandPending(Intent, bool) {}

void showNeedsHaSetup() { say("needs Home Assistant setup"); }
void showNoPlayer() { say("no player selected"); }
void showHaUnreachable(const char* detail) { say("HA unreachable: %s", detail); }
void showPortal() { say("setup portal"); }
void showConnecting(const char* ssid) { say("connecting to %s", ssid); }
void tickConnecting() {}
void showConnectFailed() { say("no Wi-Fi"); }
void showSettingsCleared() { say("settings cleared"); }

void refreshElapsed(const services::ha::PlayerState&) {}

bool isBlanked() { return s_blanked; }

void blank() {
  if (!s_blanked) {
    s_blanked = true;
    LOG_INFO("UI: stood down");
  }
}

void wake() {
  if (s_blanked) {
    s_blanked = false;
    s_last[0] = '\0';  // say the screen again once there is one
    LOG_INFO("UI: awake");
  }
}

// Artwork is pixels, and there are none. The app still calls these on every
// track change, which costs nothing here and keeps the call sites honest.
void prepareArtwork(const char*) {}
void clearArtwork() {}

}  // namespace ui
