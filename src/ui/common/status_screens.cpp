#include "ui/status_screens.h"

#include <Arduino.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/display_font.h"
#include "services/device_name.h"
#include "ui/canvas.h"
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
void drawCentredLine(lgfx::LovyanGFX& gfx, const char* str, int y, int text_px,
                     uint16_t color) {
  if (str == nullptr || str[0] == '\0') {
    return;
  }
  displayFontApplyHeight(gfx, text_px);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(color);

  char line[ui::text::kMaxLineLen];
  ui::text::ellipsize(gfx, str,
                      ui::theme::usableWidthAt(y, ui::theme::kTextEdgeInset),
                      line, sizeof(line));
  gfx.drawString(line, ui::theme::kCenterX, y);
}

/** One "label value" line, centred as a pair: the label grey, the value
 *  white, the value ellipsised to what the chord at `y` leaves it. An empty
 *  label leaves the value alone on its line. */
void drawPairLine(lgfx::LovyanGFX& gfx, const char* label, const char* value,
                  int y, int text_px) {
  if (label[0] == '\0') {
    drawCentredLine(gfx, value, y, text_px, ui::theme::kTextPrimary);
    return;
  }
  displayFontApplyHeight(gfx, text_px);
  gfx.setTextDatum(textdatum_t::middle_left);
  const int gap = gfx.textWidth(" ");
  const int label_w = gfx.textWidth(label);
  const int room = ui::theme::usableWidthAt(y, ui::theme::kTextEdgeInset);
  char shown[ui::text::kMaxLineLen];
  ui::text::ellipsize(gfx, value, room - label_w - gap, shown, sizeof(shown));
  const int total = label_w + gap + gfx.textWidth(shown);
  const int x = ui::theme::kCenterX - total / 2;
  gfx.setTextColor(ui::theme::kTextMuted);
  gfx.drawString(label, x, y);
  gfx.setTextColor(ui::theme::kTextPrimary);
  gfx.drawString(shown, x + label_w + gap, y);
}

void resetSpinner() {
  s_spinner_angle_deg = -90.0f;
  s_spinner_head = 0;
  for (SpinnerDot& dot : s_spinner_dots) {
    dot.drawn = false;
  }
}

/** Push the square a spinner dot occupies, and nothing else. */
void presentDot(int x, int y, int radius) {
  const int side = radius * 2 + 1;
  ui::canvasPresentRegion(x - radius, y - radius, side, side);
}

}  // namespace

void statusScreenMessage(const char* title, const char* line1,
                         const char* line2, uint16_t background,
                         uint16_t foreground) {
  lgfx::LovyanGFX& gfx = ui::canvas();
  displayFontEnsureLoaded(gfx);
  gfx.fillScreen(background);

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
  drawCentredLine(gfx, title, y, ui::theme::kStatusTitleTextPx, foreground);
  y += title_h / 2;

  if (line1 != nullptr && line1[0] != '\0') {
    y += gap + body_h / 2;
    drawCentredLine(gfx, line1, y, ui::theme::kStatusBodyTextPx, foreground);
    y += body_h / 2;
  }
  if (line2 != nullptr && line2[0] != '\0') {
    y += gap + body_h / 2;
    drawCentredLine(gfx, line2, y, ui::theme::kStatusBodyTextPx, foreground);
  }

  ui::canvasPresent();
}

void statusScreenDevice(const ui::DeviceStatus& status) {
  lgfx::LovyanGFX& gfx = ui::canvas();
  displayFontEnsureLoaded(gfx);
  gfx.fillScreen(ui::theme::kBackground);

  char up[24];
  const unsigned long s = status.uptime_s;
  if (s >= 86400UL) {
    snprintf(up, sizeof(up), "%lud %luh %lum", s / 86400UL, s / 3600UL % 24UL,
             s / 60UL % 60UL);
  } else if (s >= 3600UL) {
    snprintf(up, sizeof(up), "%luh %lum", s / 3600UL, s / 60UL % 60UL);
  } else {
    snprintf(up, sizeof(up), "%lum %lus", s / 60UL, s % 60UL);
  }
  // The channel rides on the network's line rather than taking one of its
  // own: with a separate volume device there are eight lines under the name,
  // and the 240 px circle has room for about that many.
  char wifi[48];
  char signal[16];
  char tx[16];
  snprintf(wifi, sizeof(wifi), "%s (ch %d)", status.ssid, status.channel);
  snprintf(signal, sizeof(signal), "%d dBm", status.rssi);
  snprintf(tx, sizeof(tx), "%.1f dBm", status.tx_dbm);

  struct Pair {
    const char* label;
    const char* value;
  };
  Pair pairs[8];
  int n = 0;
  // What it controls first, then the device and its link. With a separate
  // volume device and an input chosen for the player, the input is what the
  // room is switched to, so it takes the player's line; the input named
  // after the player needs no line of its own. The names, the input, the
  // address and the network say what they are, so they go unlabelled.
  if (status.control[0] != '\0' && status.input[0] != '\0') {
    pairs[n++] = {"", status.input};
  } else {
    pairs[n++] = {"", status.player[0] != '\0' ? status.player : "No player"};
  }
  if (status.control[0] != '\0') {
    pairs[n++] = {"", status.control};
  }
  if (status.connected) {
    pairs[n++] = {"", status.ip};
    pairs[n++] = {"Up", up};
    pairs[n++] = {"", wifi};
    pairs[n++] = {"Rx", signal};
    pairs[n++] = {"Tx", tx};
  } else {
    pairs[n++] = {"Up", up};
    pairs[n++] = {"", "No Wi-Fi"};
  }

  // The name and the lines under it, stacked around the centre, as the
  // cards are: the block sits in the same optical place however many lines
  // it has.
  const int title_h = ui::theme::textPx(30);
  const int line_h = ui::theme::textPx(22);
  const int total = title_h + ui::theme::kStatusLineGap + n * line_h;
  int y = ui::theme::kCenterY - total / 2 + title_h / 2;
  drawCentredLine(gfx, status.hostname, y, ui::theme::kStatusTitleTextPx,
                  ui::theme::kTextPrimary);
  y += title_h / 2 + ui::theme::kStatusLineGap + line_h / 2;
  for (int i = 0; i < n; ++i) {
    drawPairLine(gfx, pairs[i].label, pairs[i].value, y,
                 ui::theme::kStatusBodyTextPx);
    y += line_h;
  }

  ui::canvasPresent();
}

void statusScreenPortal() {
  // Two ways in: the access point and a browser, or a tap and the network
  // list on the device itself.
  char ap_line[64];
  snprintf(ap_line, sizeof(ap_line), "Join %s", config::kPortalApName);
  statusScreenMessage("Setup", ap_line, "or tap to choose Wi-Fi",
                      config::kColorYellow, config::kTextOnYellow);
  resetSpinner();
}

void statusScreenConnectFailed() {
  statusScreenMessage("No Wi-Fi", "Saved network failed", "Tap to choose",
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
  // frame, so this stays cheap while a blocking connect attempt runs. Only the
  // two squares those dots occupy are pushed, which is what keeps it cheap on
  // a panel where a full present is a megabyte.
  const float rad = s_spinner_angle_deg * static_cast<float>(M_PI) / 180.0f;
  const int x = ui::theme::kCenterX +
                static_cast<int>(cosf(rad) * kSpinnerRadius);
  const int y = ui::theme::kCenterY +
                static_cast<int>(sinf(rad) * kSpinnerRadius);

  lgfx::LovyanGFX& gfx = ui::canvas();
  SpinnerDot& slot = s_spinner_dots[s_spinner_head];
  if (slot.drawn) {
    gfx.fillCircle(slot.x, slot.y, kSpinnerEraseRadius, config::kColorBlack);
    presentDot(slot.x, slot.y, kSpinnerEraseRadius);
  }
  gfx.fillCircle(x, y, kSpinnerDotRadius, config::kTextOnBlack);
  presentDot(x, y, kSpinnerDotRadius);
  slot.x = x;
  slot.y = y;
  slot.drawn = true;

  s_spinner_head = (s_spinner_head + 1) % kSpinnerDotCount;
  s_spinner_angle_deg += kSpinnerStepDeg;
  if (s_spinner_angle_deg >= 270.0f) {
    s_spinner_angle_deg -= 360.0f;
  }
}
