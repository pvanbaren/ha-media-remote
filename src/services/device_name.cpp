#include "services/device_name.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cctype>
#include <cstring>

#include "config.h"
#include "log.h"

namespace services::device {
namespace {

/** Its own namespace, like the rotation's: holding BOOT to clear the Home
 *  Assistant settings should not also rename the device, and move the portal
 *  it is about to point at. */
constexpr char kPrefsNamespace[] = "device";
constexpr char kPrefsNameKey[] = "name";

bool s_loaded = false;
char s_name[kNameMaxLen + 1] = {};

}  // namespace

void cleanName(const char* requested, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  const size_t limit = out_len - 1 < kNameMaxLen ? out_len - 1 : kNameMaxLen;
  size_t n = 0;
  bool hyphen_owed = false;
  for (const char* p = requested != nullptr ? requested : ""; *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (isalnum(c)) {
      // A run of separators becomes one hyphen, and only between two
      // characters that stay: never leading, never trailing.
      const bool hyphen = hyphen_owed && n > 0;
      if (n + (hyphen ? 2 : 1) > limit) {
        break;
      }
      if (hyphen) {
        out[n++] = '-';
      }
      hyphen_owed = false;
      out[n++] = static_cast<char>(tolower(c));
    } else if (c == '-' || c == '_' || c == ' ' || c == '.') {
      hyphen_owed = true;
    }
    // Anything else is dropped: a hostname has no room for it.
  }
  out[n] = '\0';
}

const char* name() {
  if (!s_loaded) {
    s_loaded = true;
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, true)) {
      prefs.getString(kPrefsNameKey, s_name, sizeof(s_name));
      prefs.end();
    }
    char cleaned[kNameMaxLen + 1];
    cleanName(s_name, cleaned, sizeof(cleaned));
    if (cleaned[0] == '\0') {
      cleanName(config::kPortalHostname, cleaned, sizeof(cleaned));
    }
    memcpy(s_name, cleaned, sizeof(s_name));
  }
  return s_name;
}

bool saveName(const char* requested) {
  char cleaned[kNameMaxLen + 1];
  cleanName(requested, cleaned, sizeof(cleaned));
  if (cleaned[0] == '\0') {
    return false;
  }
  memcpy(s_name, cleaned, sizeof(s_name));
  s_loaded = true;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putString(kPrefsNameKey, s_name);
    prefs.end();
  }
  LOG_INFO("Device: name \"%s\" saved, applies at the next boot", s_name);
  return true;
}

}  // namespace services::device
