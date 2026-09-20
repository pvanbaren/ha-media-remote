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
#include "services/ha_client.h"
#include "services/player_list.h"

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

constexpr int kUrlParamLen = static_cast<int>(config::kHaBaseUrlMaxLen);
constexpr int kTokenParamLen = static_cast<int>(config::kHaTokenMaxLen);

constexpr char kUrlInputAttrs[] =
    " type=\"url\" placeholder=\"http://homeassistant.local:8123\"";

constexpr int kPlayerParamLen = static_cast<int>(config::kEntityIdMaxLen) - 1;

char s_token_attrs[96] = " type=\"password\"";

/** One entity dropdown: the markup it is rendered from, and the parameter
 *  built against that markup.
 *
 *  WiFiManagerParameter stores the custom-HTML pointer it is handed rather
 *  than copying, and offers no setter for it, so `attrs` has to outlive the
 *  parameter AND never move. Capacity is reserved once, before the parameter
 *  is constructed against c_str(), and later rebuilds reuse it -- assigning ""
 *  to an Arduino String keeps its buffer. Rebuilds truncate rather than grow,
 *  since growing would reallocate and leave the parameter pointing at freed
 *  memory. */
struct EntitySelect {
  const char* id;
  const char* label;
  /** Extra option offered above the entities, or nullptr for none. */
  const char* empty_option;
  /** Shown first, selected and not choosable, while nothing is stored --
   *  for a dropdown with no empty option. Without it the browser shows the
   *  first entity as if it were chosen, and saving the page unchanged stores
   *  nothing, since the hidden value only follows a change. */
  const char* unset_option = nullptr;
  String attrs;
  size_t capacity = 0;
  /** Constructed on first build, once `attrs` has its final buffer. */
  WiFiManagerParameter* param = nullptr;
};

EntitySelect s_player_select{"ha_player", "Media player", nullptr,
                             "Choose a player&hellip;"};
/** Volume and power. Separate from the player for the setup this exists to
 *  serve: something streaming into an amplifier, where the amplifier owns the
 *  knob and the power. Defaults to following the player. */
EntitySelect s_control_select{"ha_control", "Volume &amp; power",
                              "Same as the media player"};

WiFiManagerParameter s_param_ha_url("ha_url", "Home Assistant URL", "",
                                    kUrlParamLen, kUrlInputAttrs);
WiFiManagerParameter s_param_ha_token("ha_token", "Long-lived access token", "",
                                      kTokenParamLen, s_token_attrs);
/** What the device is called on the network -- its hostname, so DHCP and
 *  mDNS both carry it, and the portal is at http://<name>.local. A text field
 *  the name is cleaned from on save (see services::device::cleanName()). */
char s_name_attrs[160] = {};
WiFiManagerParameter s_param_device_name(
    "dev_name", "Device name (restarts to apply)", "",
    static_cast<int>(services::device::kNameMaxLen), s_name_attrs);

/** A name change restarts the device, but not from inside the save
 *  callback: WiFiManager sends its response page after the callback
 *  returns, and restarting first would leave the browser hanging on a dead
 *  request. */
bool s_restart_pending = false;
unsigned long s_restart_requested_ms = 0;
constexpr unsigned long kRestartAfterSaveMs = 1500;

/** Rough worst case per option: entity_id, escaped name and the markup. */
constexpr size_t kPlayerOptionBytes = 160;
constexpr size_t kPlayerHtmlOverhead = 320;

void appendHtmlEscaped(String& out, const char* text) {
  for (const char* p = text; *p != '\0'; ++p) {
    switch (*p) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out += *p;
        break;
    }
  }
}

/** Build one entity dropdown.
 *
 *  WiFiManager only renders `<input id='{i}' name='{n}' ... {c}>`, so the
 *  select is stuffed through the custom-attribute slot: {c} closes the input
 *  as hidden and opens a select, and the template's own closing bracket
 *  finishes the `</select`. The select carries no name, so only the hidden
 *  input is submitted and doParamSave reads it back by id as usual -- which
 *  also means the stored value survives if the dropdown never fires. */
void buildEntitySelect(EntitySelect& sel, const char* selected) {
  const bool have_list = services::players::ensureEntries();
  const int count = services::players::entryCount();

  // Nothing to offer yet -- no Wi-Fi or no HA settings at first boot. Leave
  // the parameter uncreated so the buffer is sized against a real list when
  // one arrives, rather than against kMaxPlayers.
  if (!have_list && sel.param == nullptr) {
    return;
  }

  if (sel.capacity == 0) {
    // Sized for the list we have plus a little headroom, not for the maximum:
    // this buffer is permanent, and every kilobyte here is a kilobyte the TLS
    // handshake and the cover art cache do not get.
    size_t slots = static_cast<size_t>(count) + 8;
    if (slots > config::kMaxPlayers) {
      slots = config::kMaxPlayers;
    }
    const size_t wanted = kPlayerHtmlOverhead + slots * kPlayerOptionBytes;
    if (!sel.attrs.reserve(wanted)) {
      // Out of heap. Leave the capacity at zero and try again next time the
      // portal is built, rather than recording a reservation that did not
      // happen: the truncation guard below stops appending at sel.capacity,
      // and if it trusted this number the String would reallocate instead --
      // moving the buffer out from under the raw c_str() that
      // WiFiManagerParameter holds, so the next page render reads freed heap.
      LOG_WARN("Portal: no %u byte buffer for %s, list skipped",
                    static_cast<unsigned>(wanted), sel.id);
      return;
    }
    sel.capacity = wanted;
    LOG_DEBUG("Portal: %u byte buffer for %s (%d players)",
                  static_cast<unsigned>(sel.capacity), sel.id, count);
  }

  sel.attrs = "";  // keeps the reserved buffer
  sel.attrs += " type=\"hidden\"><select id=\"";
  sel.attrs += sel.id;
  sel.attrs += "_sel\" onchange='document.getElementById(\"";
  sel.attrs += sel.id;
  sel.attrs += "\").value=this.value'>";

  if (!have_list) {
    sel.attrs +=
        "<option value=\"\">no players found - save the URL and token "
        "first</option>";
  } else if (sel.empty_option != nullptr) {
    sel.attrs += "<option value=\"\"";
    if (selected[0] == '\0') {
      sel.attrs += " selected";
    }
    sel.attrs += '>';
    sel.attrs += sel.empty_option;
    sel.attrs += "</option>";
  } else if (sel.unset_option != nullptr && selected[0] == '\0') {
    sel.attrs += "<option value=\"\" selected disabled>";
    sel.attrs += sel.unset_option;
    sel.attrs += "</option>";
  }

  for (int i = 0; i < count; ++i) {
    const services::ha::PlayerEntry* entry = services::players::entryAt(i);
    if (entry == nullptr) {
      continue;
    }
    // Never outgrow the reserved buffer: reallocating would move it out from
    // under the parameter.
    if (sel.attrs.length() + kPlayerOptionBytes > sel.capacity) {
      LOG_WARN("Portal: %s list truncated to fit the page buffer",
                    sel.id);
      break;
    }
    sel.attrs += "<option value=\"";
    sel.attrs += entry->entity_id;
    sel.attrs += '"';
    if (strcmp(entry->entity_id, selected) == 0) {
      sel.attrs += " selected";
    }
    sel.attrs += '>';
    appendHtmlEscaped(sel.attrs, entry->name);
    if (!entry->available) {
      sel.attrs += " (unavailable)";
    }
    sel.attrs += "</option>";
  }

  sel.attrs += "</select";

  if (sel.param != nullptr) {
    sel.param->setValue(selected, kPlayerParamLen);
    return;
  }

  // Built now rather than at static-init, so the custom-HTML pointer it keeps
  // is the reserved buffer rather than an empty literal. Registered here too,
  // since the list usually only arrives after attachPortalParams has run.
  sel.param = new WiFiManagerParameter(sel.id, sel.label, selected,
                                       kPlayerParamLen, sel.attrs.c_str());
  s_wm.addParameter(sel.param);
}

void buildPlayerSelects() {
  buildEntitySelect(s_player_select, services::ha::selectedEntity());
  // The *stored* value, not the resolved one: an empty string means "follow
  // the player", and offering it back as the player's own entity_id would
  // quietly turn a default into a pin.
  buildEntitySelect(s_control_select, services::ha::storedControlEntity());
}

void refreshPortalParamDefaults() {
  s_param_ha_url.setValue(services::ha::storedBaseUrl(), kUrlParamLen);
  // Never echo the token back into the page. An empty field on save means
  // "keep the stored one"; the placeholder says which of the two it is.
  snprintf(s_token_attrs, sizeof(s_token_attrs),
           " type=\"password\" placeholder=\"%s\"",
           services::ha::hasStoredToken() ? "stored - leave blank to keep"
                                          : "paste token here");
  s_param_ha_token.setValue("", kTokenParamLen);
  snprintf(s_name_attrs, sizeof(s_name_attrs),
           " placeholder=\"%s\" autocapitalize=\"none\" spellcheck=\"false\"",
           config::kPortalHostname);
  s_param_device_name.setValue(services::device::name(),
                               static_cast<int>(services::device::kNameMaxLen));
  buildPlayerSelects();
}

/** Whether the form just saved carried the field `id`.
 *
 *  WiFiManager stores an empty string for a parameter the form did not
 *  include, which reads exactly like a deliberate blank. The dropdowns are
 *  registered only once the player list has loaded, so a page opened before
 *  that -- straight after a boot -- has none of them, and saving it would
 *  clear Volume & power, where blank is a real choice. A field that was not
 *  on the page is left as it is instead. */
bool submitted(const char* id) {
  return s_wm.server != nullptr && s_wm.server->hasArg(id);
}

void onPortalParamsSaved() {
  char old_url[config::kHaBaseUrlMaxLen + 1];
  snprintf(old_url, sizeof(old_url), "%s", services::ha::storedBaseUrl());
  const char* token = s_param_ha_token.getValue();
  const bool token_entered = token != nullptr && token[0] != '\0';
  services::ha::saveCredentials(s_param_ha_url.getValue(), token);
  const bool server_changed =
      token_entered || strcmp(old_url, services::ha::storedBaseUrl()) != 0;

  const char* player =
      s_player_select.param != nullptr && submitted(s_player_select.id)
          ? s_player_select.param->getValue()
          : nullptr;
  if (player != nullptr && player[0] != '\0' &&
      strcmp(player, services::ha::selectedEntity()) != 0) {
    services::ha::selectEntity(player);
    // The new player has its own art, and whatever is on screen belongs to
    // the old one. What to do about that is the app layer's call.
    notify(s_observer.playerChanged);
    LOG_INFO("Player selected from portal: %s", player);
  }

  // Empty is meaningful here -- it is the "same as the media player" option --
  // so unlike the fields above, a blank value is stored rather than skipped.
  const char* control =
      s_control_select.param != nullptr && submitted(s_control_select.id)
          ? s_control_select.param->getValue()
          : nullptr;
  if (control != nullptr &&
      strcmp(control, services::ha::storedControlEntity()) != 0) {
    services::ha::selectControlEntity(control);
  }

  // WiFiManager blanked a missing field's value all the same, and the page
  // is rendered from those values: put the stored ones back, or the next
  // page would carry the blank.
  if (s_player_select.param != nullptr && !submitted(s_player_select.id)) {
    s_player_select.param->setValue(services::ha::selectedEntity(),
                                    kPlayerParamLen);
  }
  if (s_control_select.param != nullptr && !submitted(s_control_select.id)) {
    s_control_select.param->setValue(services::ha::storedControlEntity(),
                                     kPlayerParamLen);
  }

  // Blank keeps the current name rather than clearing it; the placeholder
  // shows the one it would fall back to.
  const char* requested = s_param_device_name.getValue();
  char cleaned[services::device::kNameMaxLen + 1];
  services::device::cleanName(requested, cleaned, sizeof(cleaned));
  if (cleaned[0] != '\0' && strcmp(cleaned, services::device::name()) != 0 &&
      services::device::saveName(cleaned)) {
    s_restart_pending = true;
    s_restart_requested_ms = millis();
  }

  LOG_INFO("HA settings saved: %s", services::ha::storedBaseUrl());

  // The two dropdowns are built from the player list, and the list needs the
  // server and the token. Until both were saved there was nothing to build
  // them from, and they were only built again when the portal restarted --
  // which in practice meant a reboot. Built now instead, so the page after
  // the save offers them; against the new server's list when it changed.
  if (services::ha::configured() &&
      (server_changed || s_player_select.param == nullptr)) {
    if (server_changed) {
      services::players::refresh();
    }
    buildPlayerSelects();
  }
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamDefaults();
  // The two entity dropdowns register themselves from buildEntitySelect(),
  // whenever the list first becomes available. The page lists fields in the
  // order they are added: the device's own name first, then what it talks to.
  wm.addParameter(&s_param_device_name);
  wm.addParameter(&s_param_ha_url);
  wm.addParameter(&s_param_ha_token);
  wm.setSaveParamsCallback(onPortalParamsSaved);
}

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
  services::ha::clearCredentials();
  LOG_INFO("WiFi credentials and Home Assistant settings cleared");
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
  attachPortalParams(s_wm);
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
  refreshPortalParamDefaults();
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

  if (s_restart_pending &&
      millis() - s_restart_requested_ms >= kRestartAfterSaveMs) {
    LOG_INFO("Portal: restarting to apply the new settings");
    Serial.flush();
    ESP.restart();
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
