#include "ui/status_screens.h"

#include <Arduino.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/device_name.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace {

constexpr int kSpinnerDotCount = 10;
constexpr int kSpinnerRadius = ui::theme::px(113);
constexpr int kSpinnerDotRadius = ui::theme::px(2);
constexpr int kSpinnerEraseRadius = ui::theme::px(4);
constexpr float kSpinnerStepDeg = 6.0f;

struct SpinnerDot {
  int x = 0;
  int y = 0;
  bool drawn = false;
};

char s_connecting_ssid[40];
float s_spinner_angle_deg = -90.0f;
SpinnerDot s_spinner_dots[kSpinnerDotCount];
int s_spinner_head = 0;

/** Draw one centred line, ellipsised to the chord available at its own y. */
void drawCentredLine(const char* str, int y, int text_px, uint16_t color) {
  if (str == nullptr || str[0] == '\0') {
    return;
  }
  displayFontApplyHeight(tft, text_px);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(color);

  char line[ui::text::kMaxLineLen];
  ui::text::ellipsize(tft, str,
                      ui::theme::usableWidthAt(y, ui::theme::kTextEdgeInset),
                      line, sizeof(line));
  tft.drawString(line, ui::theme::kCenterX, y);
}

void resetSpinner() {
  s_spinner_angle_deg = -90.0f;
  s_spinner_head = 0;
  for (SpinnerDot& dot : s_spinner_dots) {
    dot.drawn = false;
  }
}

}  // namespace

void statusScreenMessage(const char* title, const char* line1,
                         const char* line2, uint16_t background,
                         uint16_t foreground) {
  displayFontEnsureLoaded(tft);
  tft.fillScreen(background);

  // Stack the lines around the centre so a one-, two- or three-line card all
  // sit in the same optical place.
  const int gap = ui::theme::kStatusLineGap;
  // Slots for the text, so they shrink with it on a board that sets its type
  // smaller than its layout.
  const int title_h = ui::theme::textPx(30);
  const int body_h = ui::theme::textPx(20);

  int total = title_h;
  if (line1 != nullptr && line1[0] != '\0') {
    total += gap + body_h;
  }
  if (line2 != nullptr && line2[0] != '\0') {
    total += gap + body_h;
  }

  int y = ui::theme::kCenterY - total / 2 + title_h / 2;
  drawCentredLine(title, y, ui::theme::kStatusTitleTextPx, foreground);
  y += title_h / 2;

  if (line1 != nullptr && line1[0] != '\0') {
    y += gap + body_h / 2;
    drawCentredLine(line1, y, ui::theme::kStatusBodyTextPx, foreground);
    y += body_h / 2;
  }
  if (line2 != nullptr && line2[0] != '\0') {
    y += gap + body_h / 2;
    drawCentredLine(line2, y, ui::theme::kStatusBodyTextPx, foreground);
  }
}

void statusScreenPortal() {
  char ap_line[64];
  char url_line[64];
  snprintf(ap_line, sizeof(ap_line), "Join %s", config::kPortalApName);
  snprintf(url_line, sizeof(url_line), "%s.local", services::device::name());
  statusScreenMessage("Setup", ap_line, url_line, config::kColorYellow,
                      config::kTextOnYellow);
  resetSpinner();
}

void statusScreenConnectFailed() {
  statusScreenMessage("No Wi-Fi", "Saved network", "did not answer",
                      config::kColorBlack, config::kTextOnBlack);
  resetSpinner();
}

void statusScreenWifiReset() {
  statusScreenMessage("Reset", "Settings cleared", "Rebooting",
                      config::kColorYellow, config::kTextOnYellow);
  resetSpinner();
}

void statusScreenNeedsHaSetup() {
  char url_line[64];
  snprintf(url_line, sizeof(url_line), "%s.local", services::device::name());
  statusScreenMessage("Link to HA", "Set URL and token at", url_line,
                      config::kColorYellow, config::kTextOnYellow);
  resetSpinner();
}

void statusScreenHaUnreachable(const char* detail) {
  statusScreenMessage("Home Assistant", "not reachable",
                      detail != nullptr ? detail : "", config::kColorBlack,
                      ui::theme::kWarning);
  resetSpinner();
}

void statusScreenNoPlayer() {
  // The player is chosen in the portal, not here -- the card is not a
  // control -- so it says where, as the Link to HA card does.
  char url_line[64];
  snprintf(url_line, sizeof(url_line), "%s.local", services::device::name());
  statusScreenMessage("No player", "Choose one at", url_line,
                      config::kColorBlack, config::kTextOnBlack);
  resetSpinner();
}

void statusScreenConnectingBegin(const char* ssid) {
  snprintf(s_connecting_ssid, sizeof(s_connecting_ssid), "%s",
           ssid != nullptr ? ssid : "network");
  statusScreenMessage("Connecting", s_connecting_ssid, "", config::kColorBlack,
                      config::kTextOnBlack);
  resetSpinner();
}

void statusScreenConnectingTick() {
  // A comet of dots chasing itself round the bezel: only two pixels change per
  // frame, so this stays cheap while a blocking connect attempt runs.
  const float rad = s_spinner_angle_deg * static_cast<float>(M_PI) / 180.0f;
  const int x = ui::theme::kCenterX +
                static_cast<int>(cosf(rad) * kSpinnerRadius);
  const int y = ui::theme::kCenterY +
                static_cast<int>(sinf(rad) * kSpinnerRadius);

  SpinnerDot& slot = s_spinner_dots[s_spinner_head];
  if (slot.drawn) {
    tft.fillCircle(slot.x, slot.y, kSpinnerEraseRadius, config::kColorBlack);
  }
  tft.fillCircle(x, y, kSpinnerDotRadius, config::kTextOnBlack);
  slot.x = x;
  slot.y = y;
  slot.drawn = true;

  s_spinner_head = (s_spinner_head + 1) % kSpinnerDotCount;
  s_spinner_angle_deg += kSpinnerStepDeg;
  if (s_spinner_angle_deg >= 270.0f) {
    s_spinner_angle_deg -= 360.0f;
  }
}
