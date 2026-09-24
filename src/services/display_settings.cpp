#include "services/display_settings.h"
#include "log.h"

#include <Arduino.h>
#include <Preferences.h>

namespace services::display {
namespace {

/** Its own namespace rather than "hamedia": holding BOOT to clear the Home
 *  Assistant settings must not also stand a wall-mounted panel back up the
 *  wrong way, with the setup screen it is about to show sideways. */
constexpr char kPrefsNamespace[] = "display";
constexpr char kPrefsRotationKey[] = "rot";

bool s_loaded = false;
uint8_t s_rotation = 0;

}  // namespace

uint8_t rotation() {
  if (!s_loaded) {
    s_loaded = true;
    Preferences prefs;
    if (prefs.begin(kPrefsNamespace, true)) {
      s_rotation = prefs.getUChar(kPrefsRotationKey, 0) & 3;
      prefs.end();
    }
  }
  return s_rotation;
}

void saveRotation(uint8_t quarter_turns) {
  s_rotation = quarter_turns & 3;
  s_loaded = true;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putUChar(kPrefsRotationKey, s_rotation);
    prefs.end();
  }
  LOG_INFO("Display: rotation %s saved, applies at the next boot",
                rotationLabel(s_rotation));
}

const char* rotationLabel(uint8_t quarter_turns) {
  switch (quarter_turns & 3) {
    case 1:
      return "+90";
    case 2:
      return "180";
    case 3:
      return "-90";
    default:
      return "0";
  }
}

}  // namespace services::display
