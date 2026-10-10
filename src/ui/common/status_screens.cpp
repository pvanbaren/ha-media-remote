#include "ui/status_screens.h"

#include <Arduino.h>

#include <alloca.h>
#include <lgfx/utility/lgfx_qrcode.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/display_font.h"
#include "services/device_name.h"
#include "ui/back_button.h"
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

/** White modules around the sign-in code: the light margin a scanner needs
 *  to find its edges. The standard asks for four, which on a 21-module code
 *  is more border than code at the sizes a round panel can afford, and phone
 *  cameras find a code with two without trouble. */
constexpr int kQrQuietModules = 2;

/** Modules along one side of the code LovyanGFX will draw for `text` -- the
 *  smallest version that holds it, at the lowest error correction, as
 *  LGFXBase::qrcode() chooses -- or 0 when nothing does. */
int qrModules(const char* text) {
  for (uint8_t version = 1; version <= 40; ++version) {
    QRCode qr;
    auto* data =
        static_cast<uint8_t*>(alloca(lgfx_qrcode_getBufferSize(version)));
    if (lgfx_qrcode_initText(&qr, data, version, 0, text) == 0) {
      return qr.size;
    }
  }
  return 0;
}

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

namespace {

/** The gear that opens the settings: top right, at the right end of the
 *  title row, mirroring the back button at its left end, which leaves the
 *  foot of the panel to the lines. */
constexpr int kGearY = ui::theme::kSearchQueryY;
constexpr int kGearRadius = ui::theme::px(12);
/** Generous, as a tap target on glass should be, but short of the line
 *  below the title, as the back button's is. */
constexpr int kGearHitRadius = ui::theme::px(22);
/** Between the bezel, or the panel's edge, and the gear; and between the
 *  gear and a line beside it. As the back button's. */
constexpr int kGearEdgeGap = ui::theme::px(6);
constexpr int kGearTextGap = ui::theme::px(4);

int gearCenterX() {
  return ui::theme::kCenterX + ui::theme::chordHalfWidth(kGearY) -
         kGearEdgeGap - kGearRadius;
}

void drawGear(lgfx::LovyanGFX& gfx, int cx, int cy, int r, uint16_t color,
              uint16_t background) {
  constexpr int kTeeth = 8;
  const float body = r * 0.74f;
  const float tooth_half = r * 0.2f;
  for (int i = 0; i < kTeeth; ++i) {
    // Each tooth a rectangle from inside the body out to `r`, as two
    // triangles about the spoke at its angle.
    const float a = i * (2.0f * PI / kTeeth);
    const float ux = cosf(a);
    const float uy = sinf(a);
    const float nx = -uy * tooth_half;  // across the spoke
    const float ny = ux * tooth_half;
    const float in = body - 1.0f;
    const int x0 = cx + static_cast<int>(lroundf(ux * in + nx));
    const int y0 = cy + static_cast<int>(lroundf(uy * in + ny));
    const int x1 = cx + static_cast<int>(lroundf(ux * in - nx));
    const int y1 = cy + static_cast<int>(lroundf(uy * in - ny));
    const int x2 = cx + static_cast<int>(lroundf(ux * r - nx));
    const int y2 = cy + static_cast<int>(lroundf(uy * r - ny));
    const int x3 = cx + static_cast<int>(lroundf(ux * r + nx));
    const int y3 = cy + static_cast<int>(lroundf(uy * r + ny));
    gfx.fillTriangle(x0, y0, x1, y1, x2, y2, color);
    gfx.fillTriangle(x0, y0, x2, y2, x3, y3, color);
  }
  gfx.fillCircle(cx, cy, static_cast<int>(lroundf(body)), color);
  gfx.fillCircle(cx, cy, static_cast<int>(lroundf(r * 0.34f)), background);
}

}  // namespace

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
  // The channel rides on the network's line. What the device hears and what
  // it sends each have a line, now that the gear is up on the title row and
  // the foot of the panel is theirs.
  char wifi[48];
  char rx[16];
  char tx[16];
  snprintf(wifi, sizeof(wifi), "%s (ch %d)", status.ssid, status.channel);
  snprintf(rx, sizeof(rx), "%d dBm", status.rssi);
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
    pairs[n++] = {"Rx", rx};
    pairs[n++] = {"Tx", tx};
  } else {
    pairs[n++] = {"Up", up};
    pairs[n++] = {"", "No Wi-Fi"};
  }

  // The name and the lines under it, stacked around the centre, as the
  // cards are: the block sits in the same optical place however many lines
  // it has -- unless that would run off the foot of the panel, when it rises.
  const int title_h = ui::theme::textPx(30);
  const int line_h = ui::theme::textPx(22);
  const int total = title_h + ui::theme::kStatusLineGap + n * line_h;
  const int lowest = ui::theme::kSize - ui::theme::kStatusLineGap;
  int top = ui::theme::kCenterY - total / 2;
  if (top + total > lowest) {
    top = lowest - total;
  }
  int y = top + title_h / 2;
  // The name is the line that can come up between the back button and the
  // gear, and is then centred in what they leave of its row.
  {
    int name_x = 0;
    int name_w = 0;
    ui::back_button::lineSpan(y, title_h, name_x, name_w);
    if (abs(y - kGearY) < title_h / 2 + kGearRadius) {
      const int left = name_x - name_w / 2;
      int right = name_x + name_w / 2;
      const int clear = gearCenterX() - kGearRadius - kGearTextGap;
      right = clear < right ? clear : right;
      name_x = (left + right) / 2;
      name_w = right > left ? right - left : 0;
    }
    displayFontApplyHeight(gfx, ui::theme::kStatusTitleTextPx);
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.setTextColor(ui::theme::kTextPrimary);
    char name[ui::text::kMaxLineLen];
    ui::text::ellipsize(gfx, status.hostname, name_w, name, sizeof(name));
    gfx.drawString(name, name_x, y);
  }
  y += title_h / 2 + ui::theme::kStatusLineGap + line_h / 2;
  for (int i = 0; i < n; ++i) {
    drawPairLine(gfx, pairs[i].label, pairs[i].value, y,
                 ui::theme::kStatusBodyTextPx);
    y += line_h;
  }

  drawGear(gfx, gearCenterX(), kGearY, kGearRadius,
           ui::theme::kTextSecondary, ui::theme::kBackground);
  ui::back_button::draw(gfx);
  ui::canvasPresent();
}

bool statusScreenGearHit(int x, int y) {
  const int dx = x - gearCenterX();
  const int dy = y - kGearY;
  return dx * dx + dy * dy <= kGearHitRadius * kGearHitRadius;
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

void statusScreenHaSignIn(const char* qr_text, const char* url,
                          const char* name) {
  lgfx::LovyanGFX& gfx = ui::canvas();
  displayFontEnsureLoaded(gfx);
  gfx.fillScreen(config::kColorBlack);

  // The code a little above the middle, leaving the room below it to the
  // two lines of text, where the circle narrows. Drawn at a whole number of
  // pixels a module, with exactly kQrQuietModules of white round it:
  // LGFXBase::qrcode()'s own margin is four modules plus whatever is left
  // over from the division, which on a 480 px panel came to 64 px of white
  // each side of a 168 px code. The box stays inside the circle -- a square
  // in it is at most the diameter over the root of two.
  const int budget = ui::theme::kSize * 56 / 100;
  const int modules = qrModules(qr_text);
  const int cell = modules > 0 ? budget / (modules + 2 * kQrQuietModules) : 0;
  const int code = cell * modules;
  const int box = code + 2 * kQrQuietModules * cell;
  const int top = ui::theme::kCenterY - box / 2 - ui::theme::px(8);
  if (cell > 0) {
    gfx.fillRect(ui::theme::kCenterX - box / 2, top, box, box,
                 config::kColorWhite);
    gfx.qrcode(qr_text, ui::theme::kCenterX - code / 2,
               top + kQrQuietModules * cell, code, 1, false);
  }

  const int title_y = top / 2 + ui::theme::px(4);
  drawCentredLine(gfx, "Scan to link", title_y, ui::theme::kStatusBodyTextPx,
                  config::kTextOnBlack);
  // Typed rather than scanned, from a computer: the page behind the code,
  // straight under it where the circle is still wide enough to show it
  // whole. Then which Home Assistant, which can give way to the bezel.
  const char* bare = strstr(url, "://");
  const int url_y = top + box + ui::theme::textPx(20) * 3 / 4;
  drawCentredLine(gfx, bare != nullptr ? bare + 3 : url, url_y,
                  ui::theme::kStatusBodyTextPx, config::kTextOnBlack);
  drawCentredLine(gfx, name, url_y + ui::theme::textPx(20),
                  ui::theme::kStatusBodyTextPx, ui::theme::kTextMuted);
  ui::canvasPresent();
  resetSpinner();
}

void statusScreenHaUnreachable(const char* detail) {
  statusScreenMessage("Home Assistant", "not reachable",
                      detail != nullptr ? detail : "", config::kColorBlack,
                      ui::theme::kWarning);
  resetSpinner();
}

void statusScreenHaTokenRejected() {
  // Not "not reachable": Home Assistant answered, and the fix is not in the
  // network but a sign-in, which the card offers.
  statusScreenMessage("Home Assistant", "Token rejected", "Tap to link again",
                      config::kColorBlack, ui::theme::kWarning);
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
