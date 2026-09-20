#include "services/wifi_setup.h"

#include <WiFi.h>
#include <WiFiManager.h>

#include <cstdio>
#include <cstring>

#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "hardware/boot_button.h"
#include "config.h"
#include "log.h"
#include "services/device_name.h"

bool s_long_press_handled = false;

namespace {

WifiObserver s_observer;

void notify(void (*fn)()) {
  if (fn != nullptr) {
    fn();
  }
}


/** Separate from the hamedia namespace so the two never contend for an NVS
 *  handle. */
constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";

bool s_force_config_portal = false;
WiFiManager s_wm;
bool s_wm_configured = false;

void ensureWifiManager();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();


void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

void eraseWifiCredentials() {
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  WiFi.persistent(false);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  LOG_INFO("WiFi credentials cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  notify(s_observer.portalStarted);
#ifdef WM_MDNS
  if (MDNS.begin(services::device::name())) {
    MDNS.addService("http", "tcp", 80);
    LOG_INFO("Setup portal: http://%s.local (or http://%s)",
                  services::device::name(), config::kPortalIp);
  } else {
    LOG_WARN("Setup portal: http://%s (mDNS unavailable)", config::kPortalIp);
  }
#else
  LOG_INFO("Setup portal: http://%s", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  // Before the station first starts: the network stack only takes the
  // hostname when it brings the interface up, and without it DHCP and the
  // portal's heading both say esp32s3-XXXXXX. WiFiManager would set it too,
  // but only on its own connect path, which this firmware does not take.
  WiFi.setHostname(services::device::name());
  s_wm.setHostname(services::device::name());
  s_wm.setAPCallback(onConfigPortalApStarted);
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
  WiFi.mode(WIFI_STA);
  s_wm.setConfigPortalBlocking(false);
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(services::device::name())) {
    MDNS.addService("http", "tcp", 80);
  }
#endif
  s_wm.startWebPortal();
  LOG_INFO("LAN config: http://%s.local or http://%s",
                services::device::name(), WiFi.localIP().toString().c_str());
}

void stopLanWebPortal() {
  if (!s_wm.getWebPortalActive()) {
    return;
  }
  s_wm.stopWebPortal();
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void prepareSta() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

void startStaConnect(const String& ssid, const String& pass) {
  prepareSta();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  // Elapsed rather than a deadline, which fails at once when millis() wraps.
  const unsigned long started = millis();
  while (millis() - started < attempt_ms) {
    if (wifiLinkUp()) {
      return true;
    }
    bootButtonPollLongPress();
    notify(s_observer.connectingTick);
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    if (s_observer.connectingBegan != nullptr) {
      s_observer.connectingBegan(ui_ssid);
    }
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      LOG_WARN("WiFi connect retry %u/%u", attempt,
                    config::kWifiConnectAttempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

    startStaConnect(ssid, pass);

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

bool connectSavedNetwork(bool show_ui) {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }

  if (conf.sta.ssid[0] == '\0') {
    return false;
  }

  // ESP-IDF stores the SSID in a fixed 32-byte field. A maximum-length
  // SSID has no room for a trailing NUL, so copy it to a larger buffer
  // and explicitly terminate it before constructing an Arduino String.
  char ssid_buf[sizeof(conf.sta.ssid) + 1] = {};
  memcpy(ssid_buf, conf.sta.ssid, sizeof(conf.sta.ssid));
  ssid_buf[sizeof(conf.sta.ssid)] = '\0';

  char pass_buf[sizeof(conf.sta.password) + 1] = {};
  memcpy(pass_buf, conf.sta.password, sizeof(conf.sta.password));
  pass_buf[sizeof(conf.sta.password)] = '\0';

  const String ssid(ssid_buf);
  const String pass(pass_buf);

  return tryConnectWithUi(ssid, pass, show_ui);
}

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  notify(s_observer.portalStarted);
  s_wm.setConfigPortalBlocking(false);
  s_wm.startConfigPortal(config::kPortalApName);
  while (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    if (s_wm.process()) {
      return true;
    }
    delay(10);
  }
  return wifiLinkUp();
}

}  // namespace

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() { return hw::bootButtonPressed(); }

void wifiSetObserver(const WifiObserver& observer) {
  s_observer = observer;
}

void bootButtonInit() { hw::bootButtonInit(); }

void bootButtonPollLongPress() {
  static unsigned long s_down_since = 0;

  if (!wifiBootButtonPressed()) {
    s_down_since = 0;
    s_long_press_handled = false;
    return;
  }

  if (s_down_since == 0) {
    s_down_since = millis();
  }
  if (!s_long_press_handled &&
      millis() - s_down_since >= config::kBootResetHoldMs) {
    s_long_press_handled = true;
    LOG_WARN("BOOT held - clearing WiFi and Home Assistant settings");
    wifiResetCredentialsAndReboot();
  }
}

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  notify(s_observer.settingsCleared);
  delay(800);
  esp_restart();
}

bool wifiReconnect() {
  hw::bootButtonInit();
  LOG_INFO("WiFi reconnecting...");
  return connectSavedNetwork(true);
}

void wifiLoop() {
  ensureWifiManager();
  if (wifiLinkUp()) {
    if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
      startLanWebPortal();
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      s_wm.process();
    }
  } else {
    stopLanWebPortal();
  }
}

bool wifiSetupConnect() {
  hw::bootButtonInit();
  ensureWifiManager();

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    LOG_INFO("Opening WiFi setup portal (after reset)");
    if (openConfigPortal() && wifiLinkUp()) {
      WiFi.setAutoReconnect(true);
      LOG_INFO("Connected: %s  IP %s", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
    LOG_WARN("WiFi connection failed");
    notify(s_observer.connectFailed);
    return false;
  }

  LOG_INFO("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    LOG_INFO("Connected: %s  IP %s", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials() && connectSavedNetwork(true)) {
    WiFi.setAutoReconnect(true);
    LOG_INFO("Connected: %s  IP %s", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials()) {
    LOG_WARN("Saved WiFi could not connect — opening setup portal");
  } else {
    LOG_INFO("No saved WiFi — opening setup portal");
  }

  if (openConfigPortal() && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    LOG_INFO("Connected: %s  IP %s", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  LOG_WARN("WiFi connection failed");
  notify(s_observer.connectFailed);
  return false;
}
