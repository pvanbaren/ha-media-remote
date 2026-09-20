#include "ui/ui.h"
#include "log.h"

#include <Arduino.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

/**
 * ui/ui.h with no panel behind it.
 *
 * The device is perfectly capable of running without a display: it joins a
 * network, serves the settings portal and answers the BOOT button. All that
 * is missing is somewhere to put the picture. This narrates to the serial
 * console instead.
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

}  // namespace

bool init() {
  LOG_INFO("UI: headless -- no panel, serial only");
  return true;
}

void showPortal() { say("setup portal"); }
void showConnecting(const char* ssid) { say("connecting to %s", ssid); }
void tickConnecting() {}
void showConnectFailed() { say("no Wi-Fi"); }
void showSettingsCleared() { say("settings cleared"); }

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

}  // namespace ui
