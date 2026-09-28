#pragma once

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

/** GPIO setup; call once early in setup(). Touch is the primary input, so
 *  BOOT exists only as the escape hatch that clears Wi-Fi and HA settings. */
void bootButtonInit();
bool wifiBootButtonPressed();
/** Call each loop iteration; triggers the settings reset on a long hold. */
void bootButtonPollLongPress();
