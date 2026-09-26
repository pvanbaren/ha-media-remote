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
constexpr char kPrefsKeyboardKey[] = "kbd";

bool s_loaded = false;
uint8_t s_rotation = 0;
KeyboardLayout s_keyboard = KeyboardLayout::kAlphabetical;

void load() {
  if (s_loaded) {
    return;
  }
  s_loaded = true;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    s_rotation = prefs.getUChar(kPrefsRotationKey, 0) & 3;
    s_keyboard = prefs.getUChar(kPrefsKeyboardKey, 0) == 1
                     ? KeyboardLayout::kQwerty
                     : KeyboardLayout::kAlphabetical;
    prefs.end();
  }
}

}  // namespace

uint8_t rotation() {
  load();
  return s_rotation;
}

void saveRotation(uint8_t quarter_turns) {
  load();
  s_rotation = quarter_turns & 3;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putUChar(kPrefsRotationKey, s_rotation);
    prefs.end();
  }
  LOG_INFO("Display: rotation %s saved, applies at the next boot",
                rotationLabel(s_rotation));
}

KeyboardLayout keyboardLayout() {
  load();
  return s_keyboard;
}

void saveKeyboardLayout(KeyboardLayout layout) {
  load();
  s_keyboard = layout;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putUChar(kPrefsKeyboardKey, static_cast<uint8_t>(layout));
    prefs.end();
  }
  LOG_INFO("Display: %s search keyboard saved",
                layout == KeyboardLayout::kQwerty ? "QWERTY" : "alphabetical");
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
