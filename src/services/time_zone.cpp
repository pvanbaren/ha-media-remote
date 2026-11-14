#include "services/time_zone.h"

#include <Preferences.h>
#include <freertos/FreeRTOS.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"

namespace services::time_zone {

namespace {

/** West to east. The rules are tzdata's, as POSIX TZ strings. */
constexpr Zone kZones[] = {
    {"Pacific/Honolulu", "Honolulu", "HST10", ""},
    {"America/Anchorage", "Anchorage", "AKST9AKDT,M3.2.0,M11.1.0",
     "America/Juneau America/Sitka America/Nome America/Yakutat"},
    {"America/Los_Angeles", "Los Angeles, Vancouver",
     "PST8PDT,M3.2.0,M11.1.0",
     "America/Vancouver America/Tijuana US/Pacific PST8PDT"},
    {"America/Phoenix", "Phoenix", "MST7", "US/Arizona America/Hermosillo"},
    {"America/Denver", "Denver, Edmonton", "MST7MDT,M3.2.0,M11.1.0",
     "America/Edmonton America/Boise US/Mountain MST7MDT"},
    {"America/Chicago", "Chicago, Winnipeg", "CST6CDT,M3.2.0,M11.1.0",
     "America/Winnipeg America/Indiana/Knox America/Menominee US/Central "
     "CST6CDT"},
    {"America/Mexico_City", "Mexico City", "CST6",
     "America/Monterrey America/Merida America/Regina America/Guatemala "
     "America/Costa_Rica"},
    {"America/New_York", "New York, Toronto", "EST5EDT,M3.2.0,M11.1.0",
     "America/Detroit America/Toronto America/Montreal America/Nassau "
     "America/Indiana/Indianapolis America/Kentucky/Louisville US/Eastern "
     "US/Michigan EST5EDT"},
    {"America/Bogota", "Bogota, Lima", "<-05>5",
     "America/Lima America/Panama America/Cancun America/Jamaica"},
    {"America/Halifax", "Halifax", "AST4ADT,M3.2.0,M11.1.0",
     "Atlantic/Bermuda America/Moncton"},
    {"America/Puerto_Rico", "Puerto Rico, Caracas", "AST4",
     "America/Caracas America/Santo_Domingo America/La_Paz"},
    {"America/St_Johns", "St. John's", "NST3:30NDT,M3.2.0,M11.1.0", ""},
    {"America/Sao_Paulo", "Sao Paulo, Buenos Aires", "<-03>3",
     "America/Argentina/Buenos_Aires America/Montevideo"},
    {"Etc/UTC", "UTC", "UTC0",
     "UTC Etc/GMT GMT Etc/Universal Africa/Abidjan Atlantic/Reykjavik"},
    {"Europe/London", "London, Dublin", "GMT0BST,M3.5.0/1,M10.5.0",
     "Europe/Dublin Europe/Belfast Europe/Guernsey Europe/Isle_of_Man "
     "Europe/Jersey"},
    {"Europe/Lisbon", "Lisbon", "WET0WEST,M3.5.0/1,M10.5.0",
     "Atlantic/Canary Atlantic/Madeira Atlantic/Faroe"},
    {"Africa/Lagos", "Lagos", "WAT-1",
     "Africa/Algiers Africa/Tunis Africa/Kinshasa"},
    {"Europe/Berlin", "Berlin, Paris, Rome, Madrid",
     "CET-1CEST,M3.5.0,M10.5.0/3",
     "Europe/Paris Europe/Rome Europe/Madrid Europe/Amsterdam "
     "Europe/Brussels Europe/Vienna Europe/Zurich Europe/Stockholm "
     "Europe/Oslo Europe/Copenhagen Europe/Prague Europe/Warsaw "
     "Europe/Budapest Europe/Belgrade Europe/Zagreb Europe/Ljubljana "
     "Europe/Bratislava Europe/Luxembourg Europe/Monaco Europe/Malta "
     "Europe/Andorra Europe/Tirane Europe/Sarajevo Europe/Skopje "
     "Europe/Podgorica Europe/Gibraltar CET"},
    {"Africa/Johannesburg", "Johannesburg", "SAST-2",
     "Africa/Maputo Africa/Harare Africa/Lusaka"},
    {"Africa/Cairo", "Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24", ""},
    {"Europe/Athens", "Athens, Helsinki, Kyiv",
     "EET-2EEST,M3.5.0/3,M10.5.0/4",
     "Europe/Helsinki Europe/Kyiv Europe/Kiev Europe/Bucharest "
     "Europe/Sofia Europe/Riga Europe/Tallinn Europe/Vilnius Asia/Nicosia "
     "EET"},
    {"Asia/Jerusalem", "Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0",
     "Asia/Tel_Aviv"},
    {"Europe/Istanbul", "Istanbul, Riyadh, Nairobi", "<+03>-3",
     "Asia/Riyadh Asia/Baghdad Asia/Qatar Asia/Kuwait Europe/Minsk "
     "Africa/Nairobi"},
    {"Europe/Moscow", "Moscow", "MSK-3", ""},
    {"Asia/Dubai", "Dubai", "<+04>-4",
     "Asia/Muscat Asia/Baku Asia/Tbilisi Asia/Yerevan Indian/Mauritius"},
    {"Asia/Karachi", "Karachi", "PKT-5", "Asia/Tashkent"},
    {"Asia/Kolkata", "India", "IST-5:30", "Asia/Calcutta Asia/Colombo"},
    {"Asia/Dhaka", "Dhaka", "<+06>-6", ""},
    {"Asia/Bangkok", "Bangkok, Jakarta", "<+07>-7",
     "Asia/Jakarta Asia/Ho_Chi_Minh Asia/Saigon"},
    {"Asia/Shanghai", "China, Singapore", "CST-8",
     "Asia/Hong_Kong Asia/Taipei Asia/Singapore Asia/Kuala_Lumpur "
     "Asia/Manila Asia/Macau PRC"},
    {"Australia/Perth", "Perth", "AWST-8", ""},
    {"Asia/Tokyo", "Tokyo, Seoul", "JST-9", "Asia/Seoul Japan"},
    {"Australia/Darwin", "Darwin", "ACST-9:30", ""},
    {"Australia/Adelaide", "Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3",
     "Australia/Broken_Hill"},
    {"Australia/Brisbane", "Brisbane", "AEST-10",
     "Pacific/Guam Pacific/Port_Moresby"},
    {"Australia/Sydney", "Sydney, Melbourne",
     "AEST-10AEDT,M10.1.0,M4.1.0/3",
     "Australia/Melbourne Australia/Canberra Australia/Hobart "
     "Australia/ACT Australia/NSW Australia/Victoria"},
    {"Pacific/Auckland", "Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3", "NZ"},
};
constexpr int kZoneCount = sizeof(kZones) / sizeof(kZones[0]);

/** Its own namespace, so a BOOT reset of the Home Assistant settings leaves
 *  it. */
constexpr char kPrefsNamespace[] = "time_zone";
constexpr char kPrefsChosenKey[] = "chosen";
constexpr char kPrefsHaIdKey[] = "ha_id";
constexpr char kPrefsHaPosixKey[] = "ha_posix";

/** Guards the three below: the poll task writes Home Assistant's, the loop
 *  reads them and writes the choice. */
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
char s_chosen[kIdMaxLen] = {};
char s_ha_id[kIdMaxLen] = {};
char s_ha_posix[kPosixMaxLen] = {};
std::atomic<bool> s_changed{false};

bool listed(const Zone& zone, const char* id) {
  if (strcmp(zone.id, id) == 0) {
    return true;
  }
  const size_t len = strlen(id);
  for (const char* p = zone.also; *p != '\0';) {
    const char* end = strchr(p, ' ');
    const size_t n = end != nullptr ? static_cast<size_t>(end - p) : strlen(p);
    if (n == len && strncmp(p, id, n) == 0) {
      return true;
    }
    p += n;
    while (*p == ' ') {
      ++p;
    }
  }
  return false;
}

/** "-0500" as a rule: "<-05>5". POSIX counts west as positive, the other
 *  way from %z. False for anything else. */
bool offsetRule(const char* offset, char* out, size_t out_len) {
  if (offset == nullptr || (offset[0] != '+' && offset[0] != '-') ||
      strlen(offset) != 5) {
    return false;
  }
  for (int i = 1; i < 5; ++i) {
    if (offset[i] < '0' || offset[i] > '9') {
      return false;
    }
  }
  const int hours = (offset[1] - '0') * 10 + (offset[2] - '0');
  const int minutes = (offset[3] - '0') * 10 + (offset[4] - '0');
  // East of Greenwich, as %z's '+', is negative in a rule.
  const char* sign = offset[0] == '+' ? "-" : "";
  if (minutes != 0) {
    snprintf(out, out_len, "<%c%02d%02d>%s%d:%02d", offset[0], hours,
             minutes, sign, hours, minutes);
  } else {
    snprintf(out, out_len, "<%c%02d>%s%d", offset[0], hours, sign, hours);
  }
  return true;
}

void copy(char* out, size_t out_len, const char* in) {
  snprintf(out, out_len, "%s", in != nullptr ? in : "");
}

}  // namespace

int count() { return kZoneCount; }

const Zone* at(int i) {
  return i >= 0 && i < kZoneCount ? &kZones[i] : nullptr;
}

const Zone* find(const char* id) {
  if (id == nullptr || id[0] == '\0') {
    return nullptr;
  }
  for (const Zone& zone : kZones) {
    if (listed(zone, id)) {
      return &zone;
    }
  }
  return nullptr;
}

void init() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return;  // nothing stored yet
  }
  char chosen[kIdMaxLen] = {};
  char ha_id[kIdMaxLen] = {};
  char ha_posix[kPosixMaxLen] = {};
  prefs.getString(kPrefsChosenKey, chosen, sizeof(chosen));
  prefs.getString(kPrefsHaIdKey, ha_id, sizeof(ha_id));
  prefs.getString(kPrefsHaPosixKey, ha_posix, sizeof(ha_posix));
  prefs.end();
  const Zone* zone = find(chosen);
  portENTER_CRITICAL(&s_mux);
  copy(s_chosen, sizeof(s_chosen), zone != nullptr ? zone->id : "");
  copy(s_ha_id, sizeof(s_ha_id), ha_id);
  copy(s_ha_posix, sizeof(s_ha_posix), ha_posix);
  portEXIT_CRITICAL(&s_mux);
}

const char* chosen() { return s_chosen; }

bool choose(const char* id) {
  const Zone* zone = find(id);
  if (id == nullptr || (id[0] != '\0' && zone == nullptr)) {
    return false;
  }
  const char* stored = zone != nullptr ? zone->id : "";
  if (strcmp(stored, s_chosen) == 0) {
    return true;
  }
  portENTER_CRITICAL(&s_mux);
  copy(s_chosen, sizeof(s_chosen), stored);
  portEXIT_CRITICAL(&s_mux);
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putString(kPrefsChosenKey, stored);
    prefs.end();
  }
  s_changed = true;
  LOG_INFO("Clock: time zone %s",
           zone != nullptr ? zone->id : "Home Assistant's");
  return true;
}

void noteHomeAssistant(const char* iana, const char* offset) {
  if (iana == nullptr || *iana == 0 || strlen(iana) >= kIdMaxLen) {
    return;
  }
  char rule[kPosixMaxLen] = {};
  const Zone* zone = find(iana);
  if (zone != nullptr) {
    copy(rule, sizeof(rule), zone->posix);
  } else if (!offsetRule(offset, rule, sizeof(rule))) {
    LOG_WARN("Clock: Home Assistant's zone \"%s\" (%s) not understood",
             iana, offset != nullptr ? offset : "");
    return;
  }
  bool same;
  portENTER_CRITICAL(&s_mux);
  same = strcmp(s_ha_id, iana) == 0 && strcmp(s_ha_posix, rule) == 0;
  if (!same) {
    copy(s_ha_id, sizeof(s_ha_id), iana);
    copy(s_ha_posix, sizeof(s_ha_posix), rule);
  }
  const bool in_force = s_chosen[0] == '\0';
  portEXIT_CRITICAL(&s_mux);
  if (same) {
    return;
  }
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putString(kPrefsHaIdKey, iana);
    prefs.putString(kPrefsHaPosixKey, rule);
    prefs.end();
  }
  LOG_INFO("Clock: Home Assistant's time zone is %s (%s)", iana, rule);
  if (in_force) {
    s_changed = true;
  }
}

void homeAssistantName(char* out, size_t out_len) {
  char id[kIdMaxLen];
  portENTER_CRITICAL(&s_mux);
  copy(id, sizeof(id), s_ha_id);
  portEXIT_CRITICAL(&s_mux);
  const Zone* zone = find(id);
  if (zone != nullptr) {
    copy(out, out_len, zone->name);
    return;
  }
  // The city: the last part of the name, its underscores spaces.
  const char* slash = strrchr(id, '/');
  copy(out, out_len, slash != nullptr ? slash + 1 : id);
  for (char* p = out; *p != '\0'; ++p) {
    if (*p == '_') {
      *p = ' ';
    }
  }
}

void posix(char* out, size_t out_len) {
  char id[kIdMaxLen];
  char ha[kPosixMaxLen];
  portENTER_CRITICAL(&s_mux);
  copy(id, sizeof(id), s_chosen);
  copy(ha, sizeof(ha), s_ha_posix);
  portEXIT_CRITICAL(&s_mux);
  const Zone* zone = find(id);
  copy(out, out_len,
       zone != nullptr ? zone->posix : ha[0] != '\0' ? ha : "UTC0");
}

bool takeChange() { return s_changed.exchange(false); }

}  // namespace services::time_zone
