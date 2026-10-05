#include "services/wifi_setup.h"

#include <WiFi.h>
#include <WiFiManager.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <Preferences.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "hardware/boot_button.h"
#include "config.h"
#include "log.h"
#include "services/device_name.h"
#include "services/display_settings.h"
#include "services/ha_client.h"
#include "services/ma_api.h"
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
/** The portal's Wi-Fi transmit power, quarter dBm. Absent until one is saved. */
constexpr char kPrefsTxPowerKey[] = "txpow";

bool s_force_config_portal = false;

/** Pages other services asked for with wifiAddWebPage(). */
struct WebPage {
  const char* path = nullptr;
  void (*handler)(WebServer& server) = nullptr;
};
WebPage s_web_pages[4];

/** True while wifiSetupConnect() waits in the setup portal. */
bool s_in_setup_portal = false;
/** A join asked for on the device, for the setup portal's loop to make. */
bool s_join_pending = false;
char s_join_ssid[33] = {};
char s_join_pass[65] = {};

/** WiFiManager with its two save handlers reachable, so the routes can be
 *  claimed ahead of it and handed on (see claimSaveRoutes()). */
class PortalWiFiManager : public WiFiManager {
 public:
  using WiFiManager::handleParamSave;
  using WiFiManager::handleWifiSave;
};

PortalWiFiManager s_wm;
bool s_wm_configured = false;

void ensureWifiManager();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();

// Names the hidden field that marks a request as carrying the portal's form.
// A macro so the rendered HTML and the lookup cannot drift apart; undefined
// again once both have used it.
#define PORTAL_FORM_MARKER "mr_form"

constexpr char kPortalFormMarker[] = PORTAL_FORM_MARKER;

/** Hidden field proving a save request came from the portal's form, so a
 *  request without one is turned away rather than read as every field
 *  blank. On both forms: WiFiManager puts the parameters on the Wi-Fi page
 *  too, and saves them from /wifisave as well as /paramsave. */
WiFiManagerParameter s_param_form_marker(
    "<input type=\"hidden\" name=\"" PORTAL_FORM_MARKER "\" value=\"1\">");

#undef PORTAL_FORM_MARKER

constexpr int kUrlParamLen = static_cast<int>(config::kHaBaseUrlMaxLen);
constexpr int kTokenParamLen = static_cast<int>(config::kHaTokenMaxLen);

constexpr char kUrlInputAttrs[] =
    " type=\"url\" placeholder=\"http://homeassistant.local:8123\"";

constexpr int kPlayerParamLen = static_cast<int>(config::kEntityIdMaxLen) - 1;
constexpr int kMaEntryParamLen =
    static_cast<int>(config::kMaConfigEntryIdMaxLen);

constexpr char kMaEntryAttrs[] =
    " placeholder=\"none - no station list\"";

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
/** Which input on the volume/power entity carries the player, for a device
 *  whose inputs are not named after it -- a receiver's "AirPlay" or "HDMI4".
 *  Playing from the list or search switches the device to it, and while the
 *  device is on it the player is what the room hears. A dropdown of that
 *  device's own inputs, over a hidden input like the entity pickers, in a
 *  buffer that never moves for the same reason: WiFiManager keeps the
 *  pointer. In PSRAM, and so made -- with the parameter pointing at it -- on
 *  first build rather than at static init: three kilobytes of markup the
 *  internal heap and its TLS sessions have better uses for. Blank keeps the
 *  old rule, an input named after the player, which is what a Triad zone
 *  amplifier offers. */
constexpr size_t kInputAttrsBytes = 3072;
char* s_input_attrs = nullptr;
/** The input dropdown was last built without the stored input among the
 *  device's inputs -- it showed "(not listed now)" -- so the next page that
 *  shows it asks again first. See startLanWebPortal(). */
bool s_input_unlisted = false;
WiFiManagerParameter* s_param_ctl_input = nullptr;

/** Music Assistant config entry id. Only the station list needs it, and only
 *  because get_library is addressed by config entry rather than by entity --
 *  it asks the server what is in the library, not a player what it is doing.
 *  Settings -> Devices & Services -> Music Assistant; it is the long id in
 *  the browser's URL. Typed rather than picked: there is no REST endpoint
 *  that lists config entries for a long-lived token. */
WiFiManagerParameter s_param_ma_entry("ma_entry",
                                      "Music Assistant config entry id", "",
                                      kMaEntryParamLen, kMaEntryAttrs);
/** Clearing the id is this box's job, not a blank field's: a blank field
 *  keeps the stored id, as a blank URL or token does. A form that arrived
 *  with the field empty used to switch the station list off without a word,
 *  and one did -- the id went missing from a device whose settings were only
 *  being saved for something else. Same checkbox pattern as ma_forget. */
WiFiManagerParameter s_param_ma_entry_forget(
    "ma_entry_forget", "Forget the stored config entry id<br/>", "1", 1,
    " type=\"checkbox\" style=\"width:auto\"", WFM_LABEL_AFTER);

/** Music Assistant's own API, for "Recommended": the one thing Home Assistant
 *  does not pass through is which artists are like which. The address may be
 *  left blank -- the add-on is on Home Assistant's host, port 8095 -- and the
 *  placeholder shows what blank means. The token comes from Music Assistant's
 *  own profile settings, and like the Home Assistant one is never echoed. */
char s_ma_url_attrs[160] = " type=\"url\"";
WiFiManagerParameter s_param_ma_url("ma_url", "Music Assistant URL", "",
                                    static_cast<int>(config::kMaUrlMaxLen),
                                    s_ma_url_attrs);
char s_ma_token_attrs[96] = " type=\"password\"";
WiFiManagerParameter s_param_ma_token(
    "ma_token", "Music Assistant token (for Recommended)", "",
    static_cast<int>(config::kMaTokenMaxLen), s_ma_token_attrs);
/** The one way to take the token away again, since a blank field keeps it.
 *  A checkbox through the custom-attribute slot, label after the box: a
 *  ticked box submits its value, "1", and an unticked one submits nothing,
 *  which WiFiManager stores as empty -- so the value is put back after every
 *  save. Ticked together with a new token, the new one is what is kept.
 *
 *  The label ends in a line break of its own. WiFiManager puts the label in
 *  as it is, and every other field's input is full width, which is what
 *  starts the next field on a new line; a checkbox is not, so without it the
 *  next field's label runs on beside this one. */
WiFiManagerParameter s_param_ma_forget(
    "ma_forget", "Forget the stored Music Assistant token<br/>", "1", 1,
    " type=\"checkbox\" style=\"width:auto\"", WFM_LABEL_AFTER);

/** The token field's hint says whether one is stored, so it is redrawn
 *  whenever that can have changed. */
void refreshMaTokenField() {
  snprintf(s_ma_token_attrs, sizeof(s_ma_token_attrs),
           " type=\"password\" placeholder=\"%s\"",
           services::ma::hasStoredToken() ? "stored - leave blank to keep"
                                          : "blank = server's lists");
  s_param_ma_token.setValue("", static_cast<int>(config::kMaTokenMaxLen));
  s_param_ma_forget.setValue("1", 1);
}

/** Screen rotation, a dropdown over a hidden input like the entity pickers.
 *  The markup lives in a fixed array for the reason EntitySelect's does:
 *  WiFiManagerParameter keeps the pointer, so the buffer must never move.
 *  Rebuilt in place with the current choice marked. */
char s_rotation_attrs[480] = " type=\"hidden\"";
WiFiManagerParameter s_param_rotation("rotation",
                                      "Screen rotation (restarts to apply)",
                                      "0", 2, s_rotation_attrs);
/** What the device is called on the network -- its hostname, so DHCP and
 *  mDNS both carry it, and the portal is at http://<name>.local. A text field
 *  the name is cleaned from on save (see services::device::cleanName()). */
char s_name_attrs[160] = {};
WiFiManagerParameter s_param_device_name(
    "dev_name", "Device name (restarts to apply)", "",
    static_cast<int>(services::device::kNameMaxLen), s_name_attrs);

/** A rotation or name change restarts the device, but not from inside the
 *  save callback: WiFiManager sends its response page after the callback
 *  returns, and restarting first would leave the browser hanging on a dead
 *  request. */
bool s_restart_pending = false;
unsigned long s_restart_requested_ms = 0;
constexpr unsigned long kRestartAfterSaveMs = 1500;

void buildRotationSelect() {
  const uint8_t current = services::display::rotation();
  static const char* const kLabels[4] = {
      "Upright", "+90&deg; (clockwise)", "180&deg;",
      "-90&deg; (counter-clockwise)"};
  int n = snprintf(s_rotation_attrs, sizeof(s_rotation_attrs),
                   " type=\"hidden\"><select id=\"rotation_sel\" "
                   "onchange='document.getElementById(\"rotation\")"
                   ".value=this.value'>");
  for (uint8_t r = 0; r < 4 && n > 0 &&
                      static_cast<size_t>(n) < sizeof(s_rotation_attrs);
       ++r) {
    n += snprintf(s_rotation_attrs + n, sizeof(s_rotation_attrs) - n,
                  "<option value=\"%u\"%s>%s</option>",
                  static_cast<unsigned>(r), r == current ? " selected" : "",
                  kLabels[r]);
  }
  if (n > 0 && static_cast<size_t>(n) < sizeof(s_rotation_attrs)) {
    snprintf(s_rotation_attrs + n, sizeof(s_rotation_attrs) - n, "</select");
  }
  char value[2] = {static_cast<char>('0' + current), '\0'};
  s_param_rotation.setValue(value, 2);
}

/** The search keyboard's layout, a dropdown over a hidden input like the
 *  rotation's, and rebuilt the same way with the current choice marked.
 *  Unlike the rotation it needs no restart: the search screen reads it each
 *  time it opens. */
char s_keyboard_attrs[320] = " type=\"hidden\"";
WiFiManagerParameter s_param_keyboard("keyboard", "Search keyboard", "0", 2,
                                      s_keyboard_attrs);

void buildKeyboardSelect() {
  const bool qwerty = services::display::keyboardLayout() ==
                      services::display::KeyboardLayout::kQwerty;
  snprintf(s_keyboard_attrs, sizeof(s_keyboard_attrs),
           " type=\"hidden\"><select id=\"keyboard_sel\" "
           "onchange='document.getElementById(\"keyboard\").value=this.value'>"
           "<option value=\"0\"%s>Alphabetical</option>"
           "<option value=\"1\"%s>QWERTY</option></select",
           qwerty ? "" : " selected", qwerty ? " selected" : "");
  s_param_keyboard.setValue(qwerty ? "1" : "0", 2);
}

/**
 * Wi-Fi transmit power, chosen in the portal: the steps offered, in the
 * quarter-dBm units esp_wifi_set_max_tx_power() takes.
 *
 * Low by default (config::kWifiTxPowerQuarterDbm, 11 dBm). Raising it is for a
 * remote far from its access point: there the device can hear the access
 * point well enough while the access point cannot hear the device -- pings
 * lost, updates stalling -- and /link shows both ends. Applied as soon as it
 * is saved, and cleared by a reset, so a setting too low to reconnect with
 * can always be undone.
 */
using TxPowerStep = WifiTxPowerStep;
constexpr TxPowerStep kTxPowerSteps[] = {
    {78, "19.5 dBm (the most)"}, {68, "17 dBm"}, {60, "15 dBm"},
    {52, "13 dBm"},              {44, "11 dBm"}, {34, "8.5 dBm"},
};

bool validTxPower(int quarter_dbm) {
  for (const TxPowerStep& step : kTxPowerSteps) {
    if (step.quarter_dbm == quarter_dbm) {
      return true;
    }
  }
  return false;
}

int8_t s_tx_power = config::kWifiTxPowerQuarterDbm;
bool s_tx_power_loaded = false;

int8_t txPower() {
  if (!s_tx_power_loaded) {
    s_tx_power_loaded = true;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, true)) {
      const int8_t stored =
          prefs.getChar(kPrefsTxPowerKey, config::kWifiTxPowerQuarterDbm);
      prefs.end();
      if (validTxPower(stored)) {
        s_tx_power = stored;
      }
    }
  }
  return s_tx_power;
}

/** Only takes once the station or the access point is running; before that
 *  the core refuses it. */
void applyTxPower() { WiFi.setTxPower(static_cast<wifi_power_t>(txPower())); }

void saveTxPower(int8_t quarter_dbm) {
  s_tx_power = quarter_dbm;
  s_tx_power_loaded = true;
  Preferences prefs;
  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.putChar(kPrefsTxPowerKey, quarter_dbm);
    prefs.end();
  }
  LOG_WARN("WiFi: transmit power %.1f dBm", quarter_dbm / 4.0f);
}

void clearTxPower() {
  s_tx_power = config::kWifiTxPowerQuarterDbm;
  s_tx_power_loaded = true;
  Preferences prefs;
  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsTxPowerKey);
    prefs.end();
  }
}

/** A dropdown over a hidden input, like the rotation's. */
char s_txpow_attrs[640] = " type=\"hidden\"";
WiFiManagerParameter s_param_txpow("txpow", "Wi-Fi transmit power", "44", 3,
                                   s_txpow_attrs);

void buildTxPowerSelect() {
  const int8_t current = txPower();
  int n = snprintf(s_txpow_attrs, sizeof(s_txpow_attrs),
                   " type=\"hidden\"><select id=\"txpow_sel\" "
                   "onchange='document.getElementById(\"txpow\")"
                   ".value=this.value'>");
  for (const TxPowerStep& step : kTxPowerSteps) {
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(s_txpow_attrs)) {
      break;
    }
    n += snprintf(s_txpow_attrs + n, sizeof(s_txpow_attrs) - n,
                  "<option value=\"%d\"%s>%s%s</option>",
                  static_cast<int>(step.quarter_dbm),
                  step.quarter_dbm == current ? " selected" : "", step.label,
                  step.quarter_dbm == config::kWifiTxPowerQuarterDbm
                      ? " - the default"
                      : "");
  }
  if (n > 0 && static_cast<size_t>(n) < sizeof(s_txpow_attrs)) {
    snprintf(s_txpow_attrs + n, sizeof(s_txpow_attrs) - n, "</select");
  }
  char value[4];
  snprintf(value, sizeof(value), "%d", static_cast<int>(current));
  s_param_txpow.setValue(value, 3);
}

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

  bool selected_listed = selected[0] == '\0';
  for (int i = 0; i < count; ++i) {
    const services::ha::PlayerEntry* entry = services::players::entryAt(i);
    if (entry == nullptr) {
      continue;
    }
    // Never outgrow the reserved buffer: reallocating would move it out from
    // under the parameter. One option's room is kept back for the stored
    // entity, should the list turn out not to have it.
    if (sel.attrs.length() + 2 * kPlayerOptionBytes > sel.capacity) {
      LOG_WARN("Portal: %s list truncated to fit the page buffer",
                    sel.id);
      break;
    }
    sel.attrs += "<option value=\"";
    sel.attrs += entry->entity_id;
    sel.attrs += '"';
    if (strcmp(entry->entity_id, selected) == 0) {
      sel.attrs += " selected";
      selected_listed = true;
    }
    sel.attrs += '>';
    appendHtmlEscaped(sel.attrs, entry->name);
    if (!entry->available) {
      sel.attrs += " (unavailable)";
    }
    sel.attrs += "</option>";
  }

  // Stored, and not among Home Assistant's players now -- renamed or removed.
  // Offered as itself and marked, as the input dropdown does: otherwise the
  // browser shows the first player as chosen while the form still carries
  // the stored id, and the page says something the save will not do.
  if (have_list && !selected_listed) {
    sel.attrs += "<option value=\"";
    appendHtmlEscaped(sel.attrs, selected);
    sel.attrs += "\" selected>";
    appendHtmlEscaped(sel.attrs, services::ha::entityLabel(selected));
    sel.attrs += " (not listed now)</option>";
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

/** The input dropdown, from the volume device's own source_list. Built after
 *  the two entity dropdowns and registered straight after them, so it sits
 *  under "Volume & power" on the page. */
/** Room for one entity's input list: PSRAM, claimed on first use and kept.
 *  Shared by the dropdown and the save that checks a choice against it. */
char (*sourceBuffer())[config::kSourceNameMaxLen] {
  static char (*sources)[config::kSourceNameMaxLen] = nullptr;
  if (sources == nullptr) {
    sources = static_cast<char (*)[config::kSourceNameMaxLen]>(
        heap_caps_calloc(config::kMaxSources, config::kSourceNameMaxLen,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  return sources;
}

/** Whether `entity_id` lists `input` among its sources. */
bool listsInput(const char* entity_id, const char* input) {
  char (*sources)[config::kSourceNameMaxLen] = sourceBuffer();
  if (sources == nullptr) {
    return false;
  }
  const int n =
      services::ha::fetchSources(entity_id, sources, config::kMaxSources);
  for (int i = 0; i < n; ++i) {
    if (strcmp(sources[i], input) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * "Player volume at switch-on": the level the player's own volume is set to
 * when the remote switches the amplifier on, in percent, blank for off (see
 * pinPlayerVolume() in app.cpp).
 *
 * Shown only while a separate entity carries volume and power. The pin is
 * for that setup -- the player's volume is then a source gain, and the knob
 * is the amplifier's -- and with one entity the level last chosen is the one
 * wanted back, so the pin does nothing and neither should the field. Hidden
 * rather than left out, label and line break too, because a field WiFiManager
 * was given cannot be taken back; it still submits the stored value, which
 * saves as no change.
 *
 * The label and the attributes are buffers WiFiManager keeps pointers to,
 * rewritten in place to show or hide it.
 */
char s_wake_label[112] = {};
char s_wake_attrs[112] = {};
WiFiManagerParameter* s_param_wake = nullptr;
constexpr int kWakeParamLen = 3;

void refreshWakeVolumeField() {
  if (services::ha::controlIsSeparate()) {
    snprintf(s_wake_label, sizeof(s_wake_label),
             "Player volume when the remote switches the amplifier on, %% "
             "(blank = leave it alone)");
    snprintf(s_wake_attrs, sizeof(s_wake_attrs),
             " type=\"number\" min=\"0\" max=\"100\" step=\"1\" "
             "placeholder=\"off\"");
  } else {
    snprintf(s_wake_label, sizeof(s_wake_label),
             "<style>label[for=wake_vol]+br{display:none}</style>");
    snprintf(s_wake_attrs, sizeof(s_wake_attrs), " type=\"hidden\"");
  }
  if (s_param_wake != nullptr) {
    char value[kWakeParamLen + 1] = {};
    const int pct = services::ha::playerWakeVolumePercent();
    if (pct >= 0) {
      snprintf(value, sizeof(value), "%d", pct);
    }
    s_param_wake->setValue(value, kWakeParamLen);
  }
}

void buildInputSelect() {
  const char* stored = services::ha::storedControlInput();
  String html;
  if (s_input_attrs == nullptr) {
    s_input_attrs = static_cast<char*>(heap_caps_calloc(
        1, kInputAttrsBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_input_attrs == nullptr) {
      return;  // no dropdown; the name-match rule still applies
    }
  }
  html.reserve(kInputAttrsBytes);
  html += " type=\"hidden\"><select id=\"ctl_input_sel\" "
          "onchange='document.getElementById(\"ctl_input\").value=this.value'>";
  html += "<option value=\"\"";
  html += stored[0] == '\0' ? " selected>" : ">";
  html += "The input named after the player</option>";

  if (services::ha::controlIsSeparate()) {
    char (*sources)[config::kSourceNameMaxLen] = sourceBuffer();
    const int n = sources != nullptr
                      ? services::ha::fetchSources(
                            services::ha::controlEntity(), sources,
                            config::kMaxSources)
                      : -1;
    bool stored_listed = false;
    for (int i = 0; i < n; ++i) {
      // Room for this option and the closing tags, or stop: the buffer the
      // parameter points at never grows.
      if (html.length() + 2 * config::kSourceNameMaxLen + 64 >
          kInputAttrsBytes) {
        break;
      }
      const bool selected = strcmp(sources[i], stored) == 0;
      stored_listed = stored_listed || selected;
      html += "<option value=\"";
      appendHtmlEscaped(html, sources[i]);
      html += selected ? "\" selected>" : "\">";
      appendHtmlEscaped(html, sources[i]);
      html += "</option>";
    }
    s_input_unlisted = stored[0] != '\0' && !stored_listed;
    if (s_input_unlisted) {
      // Chosen before, and the device does not list it now -- off, or
      // renamed. Kept, and said so, rather than silently dropped.
      html += "<option value=\"";
      appendHtmlEscaped(html, stored);
      html += "\" selected>";
      appendHtmlEscaped(html, stored);
      html += " (not listed now)</option>";
    }
  } else {
    s_input_unlisted = false;
    html += "<option value=\"\" disabled>not needed - no separate volume "
            "device</option>";
  }
  html += "</select";
  snprintf(s_input_attrs, kInputAttrsBytes, "%s", html.c_str());
  if (s_param_ctl_input != nullptr) {
    s_param_ctl_input->setValue(
        stored, static_cast<int>(config::kSourceNameMaxLen) - 1);
    refreshWakeVolumeField();
    return;
  }
  s_param_ctl_input = new WiFiManagerParameter(
      "ctl_input", "Player's input on the volume device", stored,
      static_cast<int>(config::kSourceNameMaxLen) - 1, s_input_attrs);
  s_wm.addParameter(s_param_ctl_input);
  // Registered here, the first time the input dropdown is, so it sits just
  // under it on the page.
  refreshWakeVolumeField();
  s_param_wake = new WiFiManagerParameter("wake_vol", s_wake_label, "",
                                          kWakeParamLen, s_wake_attrs);
  s_wm.addParameter(s_param_wake);
  refreshWakeVolumeField();
}

void buildPlayerSelects() {
  buildEntitySelect(s_player_select, services::ha::selectedEntity());
  // The *stored* value, not the resolved one: an empty string means "follow
  // the player", and offering it back as the player's own entity_id would
  // quietly turn a default into a pin.
  buildEntitySelect(s_control_select, services::ha::storedControlEntity());
  if (s_control_select.param != nullptr) {
    buildInputSelect();
  }
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
  s_param_ma_entry.setValue(services::ha::maConfigEntry(), kMaEntryParamLen);
  s_param_ma_entry_forget.setValue("1", 1);
  {
    // The placeholder is what a blank field means: the address derived from
    // Home Assistant's, which is what effectiveUrl() gives while none is
    // stored.
    char derived[config::kMaUrlMaxLen + 1] = {};
    if (services::ma::storedUrl()[0] == '\0') {
      services::ma::effectiveUrl(derived, sizeof(derived));
    }
    snprintf(s_ma_url_attrs, sizeof(s_ma_url_attrs),
             " type=\"url\" placeholder=\"%s\"",
             derived[0] != '\0' ? derived : "http://homeassistant.local:8095");
    s_param_ma_url.setValue(services::ma::storedUrl(),
                            static_cast<int>(config::kMaUrlMaxLen));
  }
  refreshMaTokenField();
  buildRotationSelect();
  buildKeyboardSelect();
  buildTxPowerSelect();
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
 *  include, which reads exactly like a deliberate blank. The three dropdowns
 *  are registered only once the player list has loaded, so a page opened
 *  before that -- straight after a boot -- has none of them, and saving it
 *  would clear Volume & power and the player's input, where blank is a real
 *  choice. A field that was not on the page is left as it is instead. */
bool submitted(const char* id) {
  return s_wm.server != nullptr && s_wm.server->hasArg(id);
}

/**
 * The script WiFiManager puts in the head of every page it serves (it keeps
 * the pointer, so this never moves).
 *
 * On the page a save answers with -- /wifisave, or /paramsave for the
 * parameters page -- it goes back to the form it came from after
 * config::kPortalSavedReturnMs, long enough for a device that restarts to
 * apply the save to be back, replacing the saved page in the browser's
 * history rather than adding to it. Every other page ignores it. /0wifi
 * rather than /wifi for the same reason as redirectToForm().
 */
char s_portal_head[224] = {};

void buildPortalHeadScript() {
  const int written =
      snprintf(s_portal_head, sizeof(s_portal_head),
               "<script>(function(){var p=location.pathname;"
               "var to=p=='/wifisave'?'/0wifi':p=='/paramsave'?'/param':'';"
               "if(to)setTimeout(function(){location.replace(to);},%lu);})();"
               "</script>",
               config::kPortalSavedReturnMs);
  if (written < 0 || static_cast<size_t>(written) >= sizeof(s_portal_head)) {
    s_portal_head[0] = '\0';
    LOG_WARN("Portal: head script does not fit s_portal_head; saved page will "
             "not return on its own");
  }
}

/** True when this request carries the portal form's hidden marker. */
bool portalFormSubmitted() {
  return s_wm.server != nullptr && s_wm.server->hasArg(kPortalFormMarker);
}

/**
 * Send the caller to a settings form rather than acting on the request.
 *
 * /0wifi rather than /wifi: the two render the same form, but /wifi scans
 * for networks first, and a request that has just been turned away should
 * not cost a scan. /0wifi still carries a link to scan.
 */
void redirectToForm(const char* path) {
  s_wm.server->sendHeader("Location", path);
  s_wm.server->send(303, "text/plain", "");
}

/**
 * Claim the two save routes ahead of WiFiManager's own. Called from its web
 * server callback, which runs before it adds any route, and the server
 * dispatches to the first handler that matches, so these answer instead.
 *
 * WiFiManager runs a save for any request to /wifisave or /paramsave, form
 * or none, and reads a field the request did not send as blank -- so a
 * reload of the page a save answers with, or a Back to it, cleared the
 * config entry id and the Music Assistant URL, the fields where blank is a
 * real choice. A request without the form's marker is turned away here,
 * before a field is read, so nothing is saved and nothing needs putting
 * back. One with it goes to the stock handler untouched.
 */
void claimSaveRoutes() {
  s_wm.server->on("/wifisave", [] {
    if (!portalFormSubmitted()) {
      LOG_WARN("Portal: /wifisave with no form, ignored");
      redirectToForm("/0wifi");
      return;
    }
    s_wm.handleWifiSave();
  });
  s_wm.server->on("/paramsave", [] {
    if (!portalFormSubmitted()) {
      LOG_WARN("Portal: /paramsave with no form, ignored");
      redirectToForm("/param");
      return;
    }
    s_wm.handleParamSave();
  });
}

/** Store `control` as the Volume & power device, and `input` as the player's
 *  input on it if the new device has an input by that name -- a Triad's
 *  outputs all share one list, so moving to another zone should not lose
 *  it -- or none if it does not. */
void selectControlKeepingInput(const char* control, const char* input) {
  // Copied first: `input` may be the stored input, which the select below
  // can overwrite.
  char keep[config::kSourceNameMaxLen] = {};
  snprintf(keep, sizeof(keep), "%s", input != nullptr ? input : "");
  services::ha::selectControlEntity(control);
  if (keep[0] != '\0' && !(services::ha::controlIsSeparate() &&
                           listsInput(services::ha::controlEntity(), keep))) {
    LOG_WARN("Settings: %s has no input \"%s\", cleared",
             services::ha::controlEntity(), keep);
    keep[0] = '\0';
  }
  if (strcmp(keep, services::ha::storedControlInput()) != 0) {
    services::ha::selectControlInput(keep);
  }
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
  const bool control_changed =
      control != nullptr &&
      strcmp(control, services::ha::storedControlEntity()) != 0;
  if (control_changed) {
    // The dropdown listed the old device's inputs, so the choice sent with
    // this save was made from that list.
    selectControlKeepingInput(
        control, s_param_ctl_input != nullptr && submitted("ctl_input")
                     ? s_param_ctl_input->getValue()
                     : services::ha::storedControlInput());
  } else if (s_param_ctl_input != nullptr && submitted("ctl_input")) {
    const char* input = s_param_ctl_input->getValue();
    if (input != nullptr &&
        strcmp(input, services::ha::storedControlInput()) != 0) {
      services::ha::selectControlInput(input);
    }
  }

  // Player volume at switch-on: blank is off, a whole number 0..100 the
  // level.
  // Anything else keeps what is stored rather than guessing.
  if (s_param_wake != nullptr && submitted("wake_vol")) {
    const char* raw = s_param_wake->getValue();
    int pct = -2;
    if (raw == nullptr || raw[0] == '\0') {
      pct = -1;
    } else {
      char* end = nullptr;
      const long v = strtol(raw, &end, 10);
      if (end != raw && *end == '\0' && v >= 0 && v <= 100) {
        pct = static_cast<int>(v);
      }
    }
    if (pct == -2) {
      LOG_WARN("Portal: player volume at switch-on \"%s\" is not 0-100, kept",
               raw);
    } else if (pct != services::ha::playerWakeVolumePercent()) {
      services::ha::savePlayerWakeVolumePercent(pct);
    }
  }

  // WiFiManager blanked a missing field's value all the same, and the page
  // is rendered from those values: put the stored ones back, or the next
  // page would carry the blank. (The input dropdown is rebuilt below.)
  if (s_player_select.param != nullptr && !submitted(s_player_select.id)) {
    s_player_select.param->setValue(services::ha::selectedEntity(),
                                    kPlayerParamLen);
  }
  if (s_control_select.param != nullptr && !submitted(s_control_select.id)) {
    s_control_select.param->setValue(services::ha::storedControlEntity(),
                                     kPlayerParamLen);
  }

  // A new id replaces the stored one; a blank field keeps it. Only the box
  // clears it -- the field still shows the stored id when it is ticked --
  // and a different id typed with the box ticked is what is kept.
  const char* ma_entry =
      submitted("ma_entry") ? s_param_ma_entry.getValue() : nullptr;
  const char* entry_forget = s_param_ma_entry_forget.getValue();
  const bool typed = ma_entry != nullptr && ma_entry[0] != '\0';
  if (typed && strcmp(ma_entry, services::ha::maConfigEntry()) != 0) {
    services::ha::saveMaConfigEntry(ma_entry);
  } else if (entry_forget != nullptr && strcmp(entry_forget, "1") == 0) {
    services::ha::saveMaConfigEntry("");
  } else if (!typed && services::ha::maConfigEntry()[0] != '\0') {
    LOG_WARN("Portal: config entry id came back blank, kept %s",
             services::ha::maConfigEntry());
  }
  // WiFiManager left the field and the box as they were submitted; the page
  // is rendered from them, so put back what is stored.
  s_param_ma_entry.setValue(services::ha::maConfigEntry(), kMaEntryParamLen);
  s_param_ma_entry_forget.setValue("1", 1);

  const char* forget = s_param_ma_forget.getValue();
  if (forget != nullptr && strcmp(forget, "1") == 0) {
    services::ma::clearToken();
  }
  services::ma::saveSettings(s_param_ma_url.getValue(),
                             s_param_ma_token.getValue());
  refreshMaTokenField();

  const char* keyboard = s_param_keyboard.getValue();
  if (keyboard != nullptr && (keyboard[0] == '0' || keyboard[0] == '1') &&
      keyboard[1] == '\0') {
    const services::display::KeyboardLayout chosen =
        keyboard[0] == '1' ? services::display::KeyboardLayout::kQwerty
                           : services::display::KeyboardLayout::kAlphabetical;
    if (chosen != services::display::keyboardLayout()) {
      services::display::saveKeyboardLayout(chosen);
    }
  }
  buildKeyboardSelect();  // the choice just saved, marked

  const char* txpow = submitted("txpow") ? s_param_txpow.getValue() : nullptr;
  if (txpow != nullptr && txpow[0] != '\0') {
    char* end = nullptr;
    const long chosen = strtol(txpow, &end, 10);
    if (end != txpow && *end == '\0' && validTxPower(static_cast<int>(chosen)) &&
        chosen != txPower()) {
      saveTxPower(static_cast<int8_t>(chosen));
      applyTxPower();  // no restart: the radio takes it as it runs
    }
  }
  buildTxPowerSelect();  // the choice just saved, marked

  const char* rotation = s_param_rotation.getValue();
  if (rotation != nullptr && rotation[0] >= '0' && rotation[0] <= '3' &&
      rotation[1] == '\0') {
    const uint8_t chosen = static_cast<uint8_t>(rotation[0] - '0');
    if (chosen != services::display::rotation()) {
      services::display::saveRotation(chosen);
      s_restart_pending = true;
      s_restart_requested_ms = millis();
    }
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
      (server_changed || control_changed || s_player_select.param == nullptr)) {
    if (server_changed) {
      services::players::refresh();
    }
    buildPlayerSelects();
  } else if (s_param_ctl_input != nullptr) {
    buildInputSelect();  // the choice just saved, marked
  }
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamDefaults();
  // The two entity dropdowns register themselves from buildEntitySelect(),
  // whenever the list first becomes available. The page lists fields in the
  // order they are added: the form's hidden marker, which shows nothing,
  // then the transmit power, straight under the network and its password it
  // belongs with, then the device's own name, then what it talks to.
  wm.addParameter(&s_param_form_marker);
  wm.addParameter(&s_param_txpow);
  wm.addParameter(&s_param_device_name);
  wm.addParameter(&s_param_ha_url);
  wm.addParameter(&s_param_ha_token);
  wm.addParameter(&s_param_ma_entry);
  wm.addParameter(&s_param_ma_entry_forget);
  wm.addParameter(&s_param_ma_url);
  wm.addParameter(&s_param_ma_token);
  wm.addParameter(&s_param_ma_forget);
  wm.addParameter(&s_param_rotation);
  wm.addParameter(&s_param_keyboard);
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
  services::ma::clear();
  clearTxPower();  // a power too low to reconnect with must not survive
  LOG_INFO("WiFi credentials and Home Assistant settings cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  // Matched to the station path below, so the portal is reachable from
  // wherever the device will actually sit.
  applyTxPower();
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

/**
 * /link: how this device's Wi-Fi link is doing, readable where the remote
 * actually lives rather than on a serial console at a desk.
 *
 * The signal it hears and the power it answers with, side by side, because a
 * link can be strong one way and weak the other (see prepareSta()). With
 * ?scan=1 it also lists every access point in range, strongest first, the
 * one joined marked -- which says whether a nearer one was there to be had.
 * The scan is on request only: it takes a few seconds, and the screen waits
 * for it.
 */
void handleLinkPage() {
  auto& server = *s_wm.server;
  const bool scan = server.hasArg("scan");

  String page;
  page.reserve(2048);
  page += "<!DOCTYPE html><html><head><meta name='viewport' "
          "content='width=device-width,initial-scale=1'><title>";
  appendHtmlEscaped(page, services::device::name());
  page += " - Wi-Fi link</title></head><body style='font-family:verdana'>"
          "<h3>Wi-Fi link</h3><pre>";

  char line[160];
  if (wifiLinkUp()) {
    int8_t tx_quarter_dbm = 0;
    esp_wifi_get_max_tx_power(&tx_quarter_dbm);
    page += "Network       ";
    appendHtmlEscaped(page, WiFi.SSID().c_str());
    snprintf(line, sizeof(line),
             "\nAccess point  %s, channel %d\n"
             "Signal        %d dBm  (what this device hears)\n"
             "Transmit      %.1f dBm  (what it answers with)\n",
             WiFi.BSSIDstr().c_str(), static_cast<int>(WiFi.channel()),
             static_cast<int>(WiFi.RSSI()), tx_quarter_dbm / 4.0f);
    page += line;
  } else {
    page += "Not connected\n";
  }
  snprintf(line, sizeof(line), "Up            %lu s\n", millis() / 1000UL);
  page += line;
  page += "</pre>";

  if (scan) {
    const int n = WiFi.scanNetworks(false, false);
    if (n < 0) {
      page += "<p>Scan failed.</p>";
    } else {
      // Strongest first; the core does not promise an order.
      int order[64];
      const int count = n < 64 ? n : 64;
      for (int i = 0; i < count; ++i) {
        order[i] = i;
      }
      for (int i = 1; i < count; ++i) {
        for (int j = i; j > 0 && WiFi.RSSI(order[j]) > WiFi.RSSI(order[j - 1]);
             --j) {
          const int t = order[j];
          order[j] = order[j - 1];
          order[j - 1] = t;
        }
      }
      const String joined = wifiLinkUp() ? WiFi.BSSIDstr() : String();
      snprintf(line, sizeof(line),
               "<h3>In range: %d</h3><pre>   signal  ch  access point        "
               "network\n",
               n);
      page += line;
      for (int k = 0; k < count; ++k) {
        const int i = order[k];
        const String bssid = WiFi.BSSIDstr(i);
        snprintf(line, sizeof(line), "%s %4d dBm %3d  %s  ",
                 bssid == joined ? "*" : " ", static_cast<int>(WiFi.RSSI(i)),
                 static_cast<int>(WiFi.channel(i)), bssid.c_str());
        page += line;
        appendHtmlEscaped(page, WiFi.SSID(i).c_str());
        page += "\n";
      }
      page += "</pre><p>* the access point this device is on</p>";
      WiFi.scanDelete();
    }
  }
  page += "<p><a href='/link?scan=1'>Scan for access points</a> (a few "
          "seconds) &middot; <a href='/link'>Refresh</a> &middot; "
          "<a href='/'>Menu</a></p></body></html>";
  server.send(200, "text/html", page);
}

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  buildPortalHeadScript();
  s_wm.setCustomHeadElement(s_portal_head);
  // Registered as WiFiManager builds its web server, before its own routes:
  // the guards on its save routes, and pages of the firmware's own.
  s_wm.setWebServerCallback([] {
    claimSaveRoutes();
    s_wm.server->on("/link", handleLinkPage);
    for (const WebPage& page : s_web_pages) {
      if (page.path != nullptr) {
        auto* handler = page.handler;
        s_wm.server->on(page.path, [handler] { handler(*s_wm.server); });
      }
    }
  });
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
  // The dropdowns are built when the portal starts and after a save, not per
  // page, so on their own they show the lists as they were then: a player
  // renamed in Home Assistant since kept its old entity id on the page, and
  // the new one was not offered at all. As a page that shows them is
  // served, they are built again from a fresh list once the cached one is
  // older than config::kHaPlayerListTtlMs -- a fetch, so at most once per
  // that age -- and the input list too while it showed the stored input as
  // "(not listed now)": the volume device off, or Home Assistant not
  // answering yet, when it was asked. A fetch that fails leaves the list
  // stale, so attempts are held kListRebuildRetryMs apart: a portal page must
  // not wait out a timeout every time while Home Assistant is down.
  if (s_wm.server != nullptr) {
    s_wm.server->addMiddleware([](WebServer& server,
                                  Middleware::Callback next) {
      constexpr unsigned long kListRebuildRetryMs = 30000;
      static unsigned long s_rebuilt_ms = 0;
      static bool s_rebuilt = false;
      const String& uri = server.uri();
      if (uri == "/param" || uri == "/wifi" || uri == "/0wifi") {
        if (services::ha::configured() && services::players::stale() &&
            (!s_rebuilt || millis() - s_rebuilt_ms >= kListRebuildRetryMs)) {
          s_rebuilt = true;
          s_rebuilt_ms = millis();
          buildPlayerSelects();  // the input list with them
        } else if (s_input_unlisted) {
          buildInputSelect();
        }
      }
      return next();
    });
  }
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

/** Signal, and the power being answered with. Both, because either one on
 *  its own can look healthy while the link is not. */
void logLinkQuality() {
  int8_t tx_quarter_dbm = 0;
  esp_wifi_get_max_tx_power(&tx_quarter_dbm);
  LOG_INFO("WiFi: RSSI %d dBm, channel %d, tx %.1f dBm",
                static_cast<int>(WiFi.RSSI()),
                static_cast<int>(WiFi.channel()), tx_quarter_dbm / 4.0f);
}

/**
 * Transmit at the portal's "Wi-Fi transmit power", 11 dBm until one is chosen
 * (config::kWifiTxPowerQuarterDbm). Why the default is low:
 *
 * Above the 8.5 dBm this started at, and well below the 19.5 dBm an
 * ESP32-S3 is specified for. The low end was costing packets: on the Qualia
 * a DNS query to the router took 3503 ms, then 1038 ms, then 8 ms, which is
 * lwIP retransmitting a query nothing answered rather than a slow resolver,
 * and TCP connections to a host on the same subnet were timing out at six
 * seconds before occasionally getting through. Full power is not the answer
 * to that either -- it is more heat and more current for a device sitting a
 * room away from its access point, and a radio driven hard close to one can
 * be worse than a quieter one.
 *
 * Whether 11 dBm is enough is a question about this room rather than this
 * code, which is what logLinkQuality() above is for. RSSI alone will not
 * answer it: that is what this device hears from the access point, and the
 * access point was never the quiet end. A link lopsided that way reads as a
 * strong signal and drops packets anyway. The portal's /link page shows the
 * signal and the power in force, and the access points in range.
 *
 * Every channel is scanned before joining, and the strongest access point
 * with the network's name is the one joined. The core's default is a fast
 * scan, which joins the first match it comes to -- in a house with more
 * than one access point, whichever answers first on the lowest channel,
 * however far away. The full scan costs a second or two at connect.
 */
void prepareSta() {
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.mode(WIFI_STA);
  applyTxPower();  // after mode(): the core refuses it before the station runs
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

/**
 * Join a network chosen on the device, and keep it only if it connects.
 *
 * The attempt saves the new network as any join does -- WiFi.begin() with the
 * stack storing to flash -- and a failure undoes it: the previous network is
 * written back, or with none the saved one is cleared, so a mistyped password
 * cannot cost the network that worked, nor leave one that never did.
 *
 * Not the other way round, attempting from RAM and writing the config to
 * flash once it connects: that was tried, and the network was gone at the
 * next boot. Setting the config already in force wrote nothing to flash.
 * Writing back a different config, as a failure here does, is a change.
 */
bool joinNetwork(const char* ssid, const char* pass) {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }
  wifi_config_t previous = {};
  const bool had_previous =
      esp_wifi_get_config(WIFI_IF_STA, &previous) == ESP_OK &&
      previous.sta.ssid[0] != '\0';

  LOG_INFO("WiFi: joining \"%s\" (chosen on the device)", ssid);
  // Off the current network first, or tryConnectWithUi() sees a link that is
  // up and calls that a success.
  WiFi.disconnect(false);
  delay(100);
  // Storing to flash, both ways: the flag is what the core applies when it
  // next starts the stack, which a retry does, and the call is what applies
  // to the one running. A reset or the portal may have left either off.
  WiFi.persistent(true);
  esp_wifi_set_storage(WIFI_STORAGE_FLASH);
  const bool ok = tryConnectWithUi(String(ssid), String(pass), true);

  if (ok) {
    LOG_INFO("WiFi: joined \"%s\", saved", ssid);
    return true;
  }
  LOG_WARN("WiFi: could not join \"%s\"", ssid);
  wifi_mode_t now = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&now) != ESP_OK || now == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);  // a retry can leave the stack stopped
    delay(50);
  }
  esp_wifi_set_storage(WIFI_STORAGE_FLASH);
  if (had_previous) {
    esp_wifi_set_config(WIFI_IF_STA, &previous);
  } else {
    wifi_config_t none = {};
    esp_wifi_set_config(WIFI_IF_STA, &none);
  }
  return false;
}

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  notify(s_observer.portalStarted);
  s_wm.setConfigPortalBlocking(false);
  s_wm.startConfigPortal(config::kPortalApName);
  s_in_setup_portal = true;
  s_join_pending = false;
  bool joined = false;
  while (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    if (s_wm.process()) {
      joined = true;
      break;
    }
    // The display's turn: the network list and the password page live here,
    // since nothing else answers the finger until this loop returns.
    notify(s_observer.portalIdle);
    if (s_join_pending) {
      s_join_pending = false;
      s_wm.stopConfigPortal();
      if (joinNetwork(s_join_ssid, s_join_pass)) {
        joined = true;
        break;
      }
      // Back to the portal, the same way as the first time, and then say
      // what happened: the access point's callback puts the Setup card up.
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(50);
      s_wm.startConfigPortal(config::kPortalApName);
      if (s_observer.joinFailed != nullptr) {
        s_observer.joinFailed(s_join_ssid);
      }
      continue;
    }
    delay(10);
  }
  s_in_setup_portal = false;
  return joined || wifiLinkUp();
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

int wifiTxPowerSteps(const WifiTxPowerStep** steps) {
  *steps = kTxPowerSteps;
  return static_cast<int>(sizeof(kTxPowerSteps) / sizeof(kTxPowerSteps[0]));
}

int8_t wifiTxPower() { return txPower(); }

bool wifiSetTxPower(int8_t quarter_dbm) {
  if (!validTxPower(quarter_dbm)) {
    return false;
  }
  if (quarter_dbm != txPower()) {
    saveTxPower(quarter_dbm);
    applyTxPower();  // no restart: the radio takes it as it runs
  }
  return true;
}

void wifiSelectControl(const char* entity_id) {
  selectControlKeepingInput(entity_id, services::ha::storedControlInput());
}

void wifiSettingsChanged() {
  // Before the portal is first set up its fields are built from what is
  // stored when it is, so there is nothing to bring up to date.
  if (s_wm_configured) {
    refreshPortalParamDefaults();
  }
}

void wifiLinkStatus(WifiLinkStatus& out) {
  out = WifiLinkStatus{};
  out.connected = wifiLinkUp();
  if (!out.connected) {
    return;
  }
  snprintf(out.ssid, sizeof(out.ssid), "%s", WiFi.SSID().c_str());
  snprintf(out.ip, sizeof(out.ip), "%s", WiFi.localIP().toString().c_str());
  out.channel = WiFi.channel();
  out.rssi = WiFi.RSSI();
  int8_t tx_quarter_dbm = 0;
  esp_wifi_get_max_tx_power(&tx_quarter_dbm);
  out.tx_dbm = tx_quarter_dbm / 4.0f;
}

bool wifiScanStart() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }
  // In the background, so the screen keeps answering for the second or two
  // a scan of every channel takes. One the portal started is as good: its
  // results come back through scanComplete() the same way.
  const int16_t started = WiFi.scanNetworks(true, false);
  return started == WIFI_SCAN_RUNNING || started >= 0;
}

int wifiScanPoll(WifiNetwork* out, int capacity) {
  const int16_t found = WiFi.scanComplete();
  if (found == WIFI_SCAN_RUNNING) {
    return -1;
  }
  if (found < 0) {
    return -2;
  }
  int count = 0;
  for (int i = 0; i < found; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) {
      continue;  // hidden: nothing to show, nothing to tap
    }
    const int rssi = WiFi.RSSI(i);
    const bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    // One row per name, at its strongest access point's signal.
    int slot = -1;
    for (int j = 0; j < count; ++j) {
      if (strcmp(out[j].ssid, ssid.c_str()) == 0) {
        slot = j;
        break;
      }
    }
    if (slot >= 0) {
      if (rssi > out[slot].rssi) {
        out[slot].rssi = rssi;
        out[slot].secure = secure;
      }
      continue;
    }
    if (count < capacity) {
      slot = count++;
    } else {
      // Full: a stronger network takes the weakest one's place.
      int weakest = 0;
      for (int j = 1; j < count; ++j) {
        if (out[j].rssi < out[weakest].rssi) {
          weakest = j;
        }
      }
      if (capacity == 0 || rssi <= out[weakest].rssi) {
        continue;
      }
      slot = weakest;
    }
    snprintf(out[slot].ssid, sizeof(out[slot].ssid), "%s", ssid.c_str());
    out[slot].rssi = rssi;
    out[slot].secure = secure;
  }
  WiFi.scanDelete();

  // Strongest first.
  for (int i = 1; i < count; ++i) {
    const WifiNetwork n = out[i];
    int j = i;
    while (j > 0 && out[j - 1].rssi < n.rssi) {
      out[j] = out[j - 1];
      --j;
    }
    out[j] = n;
  }
  return count;
}

bool wifiPortalActive() { return s_in_setup_portal; }

void wifiRequestJoin(const char* ssid, const char* password) {
  if (ssid == nullptr || ssid[0] == '\0') {
    return;
  }
  snprintf(s_join_ssid, sizeof(s_join_ssid), "%s", ssid);
  snprintf(s_join_pass, sizeof(s_join_pass), "%s",
           password != nullptr ? password : "");
  s_join_pending = true;
}

bool wifiJoinNow(const char* ssid, const char* password) {
  if (ssid == nullptr || ssid[0] == '\0') {
    return false;
  }
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  const bool ok = joinNetwork(ssid, password != nullptr ? password : "");
  if (!ok) {
    // Back where it was: joinNetwork() put the saved config back in force.
    connectSavedNetwork(true);
  }
  WiFi.setAutoReconnect(true);
  if (wifiLinkUp()) {
    logLinkQuality();
  }
  return ok;
}

bool wifiAddWebPage(const char* path, void (*handler)(WebServer& server)) {
  for (WebPage& page : s_web_pages) {
    if (page.path == nullptr) {
      page.path = path;
      page.handler = handler;
      return true;
    }
  }
  return false;
}

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
      logLinkQuality();
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
    logLinkQuality();
    return true;
  }

  if (storedWifiCredentials() && connectSavedNetwork(true)) {
    WiFi.setAutoReconnect(true);
    LOG_INFO("Connected: %s  IP %s", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    logLinkQuality();
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
    logLinkQuality();
    return true;
  }

  LOG_WARN("WiFi connection failed");
  notify(s_observer.connectFailed);
  return false;
}
