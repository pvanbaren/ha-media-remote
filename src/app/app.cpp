/**
 * The remote itself: what it decides and what it does.
 *
 * Nothing in this file draws, and nothing in it knows what kind of panel is
 * attached -- or whether one is. It asks ui/ui.h to put a screen up and lets
 * the display work out what that means.
 *
 * At this point the remote joins a network, serves the settings portal and
 * watches the BOOT button. Everything it will eventually control arrives
 * above this line.
 */
#include "app/app.h"

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "log.h"
#include "services/wifi_setup.h"
#include "ui/ui.h"

namespace app {
namespace {

/** millis() when the link dropped, or 0 while it is up. */
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;

/** Reconnect in the background rather than falling back into the portal.
 *
 *  A brief drop is far more common than a changed password, and opening the
 *  portal on one would take the device off the network it is trying to
 *  rejoin. The grace period is what tells the two apart. */
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
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  LOG_INFO("HA Media Remote");

  // The portal reports what it is doing; this is where those events become
  // screens. Everything below the app layer is written against no display.
  wifiSetObserver(WifiObserver{
      .portalStarted = ui::showPortal,
      .connectingBegan = ui::showConnecting,
      .connectingTick = ui::tickConnecting,
      .connectFailed = ui::showConnectFailed,
      .settingsCleared = ui::showSettingsCleared,
  });

  ui::init();
  bootButtonInit();

  if (wifiShowsSetupScreenOnBoot()) {
    ui::showPortal();
  }
  wifiSetupConnect();
}

void loop() {
  bootButtonPollLongPress();
  wifiLoop();
  handleWifiState();
  delay(5);
}

}  // namespace app
