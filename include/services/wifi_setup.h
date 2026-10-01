#pragma once

#include <cstdint>

class WebServer;

/**
 * Wi-Fi bring-up and the LAN settings portal.
 *
 * Nothing here draws. The portal takes minutes and has plenty to say while it
 * works, but what any of it *looks* like is a display decision, so it reports
 * events to an Observer the app layer installs and the app turns those into
 * screens. That keeps this file honest on a panel of any shape, and on none.
 */
struct WifiObserver {
  /** The captive portal is up; show how to reach it. */
  void (*portalStarted)() = nullptr;
  /** A connection attempt to `ssid` has begun. */
  void (*connectingBegan)(const char* ssid) = nullptr;
  /** Called repeatedly while that attempt runs, for a progress indicator. */
  void (*connectingTick)() = nullptr;
  /** The attempt gave up. */
  void (*connectFailed)() = nullptr;
  /** Settings were cleared and a reboot follows. */
  void (*settingsCleared)() = nullptr;
  /** A different media_player was chosen in the portal. */
  void (*playerChanged)() = nullptr;
  /** Called over and over while the setup portal waits for someone, so the
   *  display can keep answering the finger: the network list and password
   *  page run inside it. A join asked for there with wifiRequestJoin() is
   *  attempted as soon as this returns. */
  void (*portalIdle)() = nullptr;
  /** A join asked for on the device did not connect; the setup portal is up
   *  again behind it. */
  void (*joinFailed)(const char* ssid) = nullptr;
};

/** Install the observer. Call once in setup(), before wifiSetupConnect().
 *  Every member is optional; a null one is simply not called. */
void wifiSetObserver(const WifiObserver& observer);

/** True when the next boot should show the setup screen first (after a reset). */
bool wifiShowsSetupScreenOnBoot();
void wifiResetCredentialsAndReboot();
/** Boot flow: connect with UI, open portal only if saved creds fail. */
bool wifiSetupConnect();
/** Reconnect using saved creds; never opens the captive portal. */
bool wifiReconnect();
/** Keeps the LAN config portal alive; call every loop() iteration. */
void wifiLoop();

/** One network a scan found: the strongest access point of each name. */
struct WifiNetwork {
  char ssid[33] = {};
  /** What this device hears from it, dBm. */
  int rssi = 0;
  /** False for an open network, which joins without a password. */
  bool secure = false;
};

/** Start a scan of every channel, in the background. False when the radio
 *  would not start one. */
bool wifiScanStart();
/** -1 while the scan runs; then the networks it found, strongest first, one
 *  per name, hidden ones left out, at most `capacity` of them -- and the
 *  scan's own results are freed. -2 when no scan is running or it failed. */
int wifiScanPoll(WifiNetwork* out, int capacity);

/** True while the setup access point is up and wifiSetupConnect() waits in
 *  it -- which is when a join goes through wifiRequestJoin(). */
bool wifiPortalActive();
/** Ask the setup portal to join `ssid` with `password` (empty for an open
 *  network) once portalIdle() returns. Saved only if it connects. */
void wifiRequestJoin(const char* ssid, const char* password);
/** Join `ssid` now, from the running remote: blocks through the connecting
 *  screens. Saved only if it connects; otherwise the network that was saved
 *  before is rejoined, and false comes back. */
bool wifiJoinNow(const char* ssid, const char* password);

/** Serve `handler` at `path` on the portal's web server, beside its own
 *  pages, whenever that server starts. Call before Wi-Fi comes up. A few
 *  slots; false when they are full. */
bool wifiAddWebPage(const char* path, void (*handler)(WebServer& server));

/** The station link as it is now, for the status page. */
struct WifiLinkStatus {
  bool connected = false;
  char ssid[33] = {};
  char ip[16] = {};
  int channel = 0;
  /** What this device hears from the access point, dBm. */
  int rssi = 0;
  /** What it transmits with, dBm. */
  float tx_dbm = 0.0f;
};
void wifiLinkStatus(WifiLinkStatus& out);

// --- Settings the device's own settings page changes as well ---------------

/** One Wi-Fi transmit power on offer, in the quarter-dBm units
 *  esp_wifi_set_max_tx_power() takes, and its name. */
struct WifiTxPowerStep {
  int8_t quarter_dbm;
  const char* label;
};
/** The steps on offer, strongest first, as `*steps`; returns how many. */
int wifiTxPowerSteps(const WifiTxPowerStep** steps);
/** The transmit power stored, quarter dBm. */
int8_t wifiTxPower();
/** Store a step from wifiTxPowerSteps() and apply it at once, as a portal
 *  save does. False, and nothing stored, for any other value. */
bool wifiSetTxPower(int8_t quarter_dbm);
/** Store `entity_id` as the Volume & power device, "" for the media player
 *  itself, as a portal save does: the player's input on it is kept when the
 *  new device lists an input by that name, and cleared when it does not.
 *  Blocks on a round trip to Home Assistant to ask. */
void wifiSelectControl(const char* entity_id);
/** A setting was changed on the device: bring the portal's fields up to
 *  date, so a page opened from now on shows it -- and a save from it does
 *  not put the old value back. May block on Home Assistant for the lists. */
void wifiSettingsChanged();

/** GPIO setup; call once early in setup(). Touch is the primary input, so
 *  BOOT exists only as the escape hatch that clears Wi-Fi and HA settings. */
void bootButtonInit();
bool wifiBootButtonPressed();
/** Call each loop iteration; triggers the settings reset on a long hold. */
void bootButtonPollLongPress();
