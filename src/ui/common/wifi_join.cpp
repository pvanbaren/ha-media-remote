#include "ui/wifi_join.h"

#include <Arduino.h>

#include <cstring>

#include "board/board.h"
#include "config.h"
#include "hardware/display_font.h"
#include "ui/canvas.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace ui::wifi_join {
namespace {

// --- State -------------------------------------------------------------------

constexpr int kMaxNetworks = 24;
/** WPA allows 63 characters of passphrase, or 64 hex digits of key. */
constexpr size_t kMaxPassword = 64;
/** WPA's shortest passphrase: JOIN stays grey below it. */
constexpr size_t kMinPassword = 8;

WifiNetwork s_networks[kMaxNetworks];
int s_count = 0;
bool s_scanning = false;
char s_note[64] = {};
char s_current[33] = {};

bool s_on_list = true;
int s_scroll_px = 0;
int s_chosen = -1;
char s_password[kMaxPassword + 1] = {};

// --- The list ----------------------------------------------------------------

constexpr int kTitleY = theme::kSearchQueryY;
/** Rows live below the title and slide under it as they scroll. */
constexpr int kListTop = theme::kSearchQueryY + theme::px(18);
constexpr int kPitch = theme::kListRowHeight + theme::kListRowGap;

/** The networks, then one more row to scan again. */
int rowCount() { return s_scanning ? 0 : s_count + 1; }

int rowTop(int row) { return kListTop + row * kPitch - s_scroll_px; }

int maxScrollPx() {
  const int rows = rowCount();
  if (rows <= 0) {
    return 0;
  }
  // Far enough for the last row's centre to reach the middle of the panel,
  // as on the browse list: on a circle it would otherwise rest in the pinched
  // bottom, the hardest place to hit.
  const int over = (rows - 1) * kPitch + theme::kListRowHeight / 2 + kListTop -
                   theme::kCenterY;
  return over > 0 ? over : 0;
}

void clampScroll() {
  const int limit = maxScrollPx();
  s_scroll_px = s_scroll_px > limit ? limit : s_scroll_px;
  s_scroll_px = s_scroll_px < 0 ? 0 : s_scroll_px;
}

/** As the search results' rows: the chord at whichever edge is further in. */
int rowHalfWidth(int top) {
  const int first = top > 0 ? top : 0;
  const int bottom = top + theme::kListRowHeight - 1;
  const int last = bottom < theme::kSize - 1 ? bottom : theme::kSize - 1;
  if (last < first) {
    return 0;
  }
  const int a = theme::chordHalfWidth(first);
  const int b = theme::chordHalfWidth(last);
  const int half = (a < b ? a : b) - theme::kListRowInset;
  return half > 0 ? half : 0;
}

/** 0 to 4 bars, from what this device hears. */
int bars(int rssi) {
  if (rssi >= -55) {
    return 4;
  }
  if (rssi >= -65) {
    return 3;
  }
  if (rssi >= -75) {
    return 2;
  }
  return rssi >= -85 ? 1 : 0;
}

/** Four bars rising left to right, `lit` of them lit, ending at `right`. */
void drawBars(lgfx::LovyanGFX& gfx, int right, int cy, int lit) {
  const int w = theme::px(2);
  const int gap = theme::px(1) > 0 ? theme::px(1) : 1;
  const int full = theme::px(10);
  const int bottom = cy + full / 2;
  int x = right - 4 * w - 3 * gap;
  for (int i = 0; i < 4; ++i) {
    const int h = full * (i + 1) / 4;
    gfx.fillRect(x, bottom - h, w, h,
                 i < lit ? theme::kTextSecondary : theme::kSurfaceRaised);
    x += w + gap;
  }
}

/** A padlock, centred on (cx, cy). */
void drawLock(lgfx::LovyanGFX& gfx, int cx, int cy, uint16_t colour) {
  const int w = theme::px(7);
  const int h = theme::px(5);
  const int r = theme::px(2) > 1 ? theme::px(2) : 2;
  const int body_top = cy - h / 2 + theme::px(1);
  gfx.fillRect(cx - w / 2, body_top, w, h, colour);
  // The shackle: an arch over the body.
  gfx.drawArc(cx, body_top, r + 1, r, 180, 360, colour);
}

void drawRow(lgfx::LovyanGFX& gfx, int row, int top) {
  const int half = rowHalfWidth(top);
  if (half <= 0) {
    return;
  }
  const int x = theme::kCenterX - half;
  const int w = half * 2;
  const int cy = top + theme::kListRowHeight / 2;

  if (row >= s_count) {
    gfx.fillRoundRect(x, top, w, theme::kListRowHeight, theme::kListRowRadius,
                      theme::kSurface);
    displayFontApplyHeight(gfx, theme::kListRowTextPx);
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.setTextColor(theme::kAccent);
    gfx.drawString("Scan again", theme::kCenterX, cy);
    return;
  }

  const WifiNetwork& n = s_networks[row];
  const bool current = s_current[0] != '\0' && strcmp(n.ssid, s_current) == 0;
  gfx.fillRoundRect(x, top, w, theme::kListRowHeight, theme::kListRowRadius,
                    current ? theme::kSurfaceRaised : theme::kSurface);

  // Signal at the right edge, the lock beside it.
  const int right = x + w - theme::kListTextInset;
  const int bars_w = 4 * theme::px(2) + 3 * theme::px(1);
  drawBars(gfx, right, cy, bars(n.rssi));
  const int lock_cx = right - bars_w - theme::px(8);
  if (n.secure) {
    drawLock(gfx, lock_cx, cy, theme::kTextMuted);
  }

  const int text_x = x + theme::kListTextInset;
  const int text_w = lock_cx - theme::px(6) - text_x;
  if (text_w <= 0) {
    return;
  }
  displayFontApplyHeight(gfx, theme::kListRowTextPx);
  gfx.setTextDatum(textdatum_t::middle_left);
  gfx.setTextColor(current ? theme::kAccent : theme::kTextPrimary);
  char label[text::kMaxLineLen];
  text::ellipsize(gfx, n.ssid, text_w, label, sizeof(label));
  gfx.drawString(label, text_x, cy);
}

void drawList(lgfx::LovyanGFX& gfx) {
  theme::fillListBackdrop(gfx, 0, theme::kSize);

  if (s_scanning) {
    displayFontApplyHeight(gfx, theme::kListTitleTextPx);
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.setTextColor(theme::kTextMuted);
    gfx.drawString("Scanning...", theme::kCenterX, theme::kCenterY);
  } else {
    clampScroll();
    gfx.setClipRect(0, kListTop, theme::kSize, theme::kSize - kListTop);
    for (int row = 0; row < rowCount(); ++row) {
      const int top = rowTop(row);
      if (top >= theme::kSize) {
        break;
      }
      if (top + theme::kListRowHeight > kListTop) {
        drawRow(gfx, row, top);
      }
    }
    gfx.clearClipRect();
    if (s_count == 0) {
      displayFontApplyHeight(gfx, theme::kListHeaderTextPx);
      gfx.setTextDatum(textdatum_t::middle_center);
      gfx.setTextColor(theme::kTextMuted);
      gfx.drawString("No networks found", theme::kCenterX,
                     rowTop(1) + theme::kListRowHeight / 2);
    }
  }

  // The title last, over anything that scrolled up behind it.
  const bool noted = s_note[0] != '\0';
  displayFontApplyHeight(gfx, theme::kListTitleTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(noted ? theme::kWarning : theme::kTextPrimary);
  char line[text::kMaxLineLen];
  text::ellipsize(gfx, noted ? s_note : "Choose a network",
                  theme::usableWidthAt(kTitleY, theme::kTextEdgeInset), line,
                  sizeof(line));
  gfx.drawString(line, theme::kCenterX, kTitleY);
}

Result tapList(int x, int y) {
  if (s_scanning || y < kListTop) {
    return Result::kNone;
  }
  for (int row = 0; row < rowCount(); ++row) {
    const int top = rowTop(row);
    if (y < top || y >= top + theme::kListRowHeight) {
      continue;
    }
    const int half = rowHalfWidth(top);
    if (x < theme::kCenterX - half || x > theme::kCenterX + half) {
      return Result::kNone;
    }
    if (row >= s_count) {
      return Result::kRescan;
    }
    s_chosen = row;
    s_password[0] = '\0';
    if (!s_networks[row].secure) {
      return Result::kJoin;
    }
    s_on_list = false;
    return Result::kChanged;
  }
  return Result::kNone;
}

// --- The keyboard -------------------------------------------------------------

constexpr char kShift = '\x01';
constexpr char kToDigits = '\x02';
constexpr char kToSymbols = '\x03';
constexpr char kToLetters = '\x04';
constexpr char kBackspace = '\b';

/** A page of four rows, as a phone lays them out: letters with shift and
 *  backspace either side of the bottom letter row, a page key and the space
 *  bar under them. Every printable ASCII character is on one of the pages. */
struct Page {
  const char* rows[theme::kKeyRows];
};
constexpr Page kLower = {{"qwertyuiop", "asdfghjkl", "\x01zxcvbnm\b", "\x02 "}};
constexpr Page kUpper = {{"QWERTYUIOP", "ASDFGHJKL", "\x01ZXCVBNM\b", "\x02 "}};
constexpr Page kDigits = {
    {"1234567890", "-/:;()$&@\"", "\x03.,?!'\b", "\x04 "}};
constexpr Page kSymbols = {
    {"[]{}#%^*+=", "_\\|~<>`", "\x02.,?!'\b", "\x04 "}};

enum class Mode : uint8_t { kLetters, kDigits, kSymbols };
Mode s_mode = Mode::kLetters;
bool s_shift = false;

const Page& page() {
  switch (s_mode) {
    case Mode::kDigits:
      return kDigits;
    case Mode::kSymbols:
      return kSymbols;
    default:
      return s_shift ? kUpper : kLower;
  }
}

int rowY(int row) { return theme::kKeyRow0Y + row * theme::kKeyRowPitch; }

/** In keys: the space bar is five, a page key two, the rest one. */
int keyUnits(char key) {
  if (key == ' ') {
    return 5;
  }
  if (key == kToDigits || key == kToSymbols || key == kToLetters) {
    return 2;
  }
  return 1;
}

int keyWidth(char key) {
  const int units = keyUnits(key);
  return units * theme::kQwertyKeyWidth + (units - 1) * theme::kKeyGap;
}

int rowLeft(const char* keys) {
  int width = 0;
  for (const char* k = keys; *k != '\0'; ++k) {
    width += keyWidth(*k) + (k == keys ? 0 : theme::kKeyGap);
  }
  return theme::kCenterX - width / 2;
}

const char* pageLabel(char key) {
  switch (key) {
    case kToDigits:
      return "123";
    case kToSymbols:
      return "#+=";
    case kToLetters:
      return "abc";
    default:
      return nullptr;
  }
}

void drawKey(lgfx::LovyanGFX& gfx, char key, int x, int y, int w) {
  const bool lit = key == kShift && s_shift;
  gfx.fillRoundRect(x, y, w, theme::kKeyHeight, theme::kKeyRadius,
                    lit ? theme::kAccent : theme::kSurface);
  const int cx = x + w / 2;
  const int cy = y + theme::kKeyHeight / 2;
  const uint16_t glyph = lit ? theme::kBackground : theme::kTextSecondary;

  if (key == kBackspace) {
    const int h = theme::px(5);
    const int gw = theme::px(6);
    gfx.fillTriangle(cx - gw, cy, cx, cy - h, cx, cy + h, glyph);
    gfx.fillRect(cx, cy - h / 2, gw, h, glyph);
    return;
  }
  if (key == kShift) {
    const int h = theme::px(5);
    const int gw = theme::px(5);
    gfx.fillTriangle(cx, cy - h, cx - gw, cy, cx + gw, cy, glyph);
    gfx.fillRect(cx - gw / 2, cy, gw, h, glyph);
    return;
  }
  if (key == ' ') {
    const int sw = theme::px(11);
    gfx.fillRect(cx - sw / 2, cy + theme::px(3), sw, theme::px(2), glyph);
    return;
  }

  displayFontApplyHeight(gfx, theme::kQwertyKeyTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  const char* label = pageLabel(key);
  gfx.setTextColor(label != nullptr ? theme::kTextSecondary
                                    : theme::kTextPrimary);
  if (label != nullptr) {
    gfx.drawString(label, cx, cy);
  } else {
    const char text[2] = {key, '\0'};
    gfx.drawString(text, cx, cy);
  }
}

/** What has been typed, its end showing when it outgrows the line: the end
 *  is where the next character goes. */
void drawPassword(lgfx::LovyanGFX& gfx) {
  displayFontApplyHeight(gfx, theme::kSearchQueryTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  const int room = theme::usableWidthAt(kTitleY, theme::kTextEdgeInset);

  if (s_password[0] == '\0') {
    char prompt[text::kMaxLineLen];
    snprintf(prompt, sizeof(prompt), "Password for %s",
             s_chosen >= 0 ? s_networks[s_chosen].ssid : "");
    char line[text::kMaxLineLen];
    text::ellipsize(gfx, prompt, room, line, sizeof(line));
    gfx.setTextColor(theme::kTextMuted);
    gfx.drawString(line, theme::kCenterX, kTitleY);
    return;
  }

  const char* shown = s_password;
  char tail[kMaxPassword + 4];
  if (gfx.textWidth(shown) > room) {
    // Drop characters from the front until the rest fits behind an ellipsis.
    const char* from = s_password;
    while (*from != '\0') {
      snprintf(tail, sizeof(tail), "...%s", from);
      if (gfx.textWidth(tail) <= room) {
        break;
      }
      ++from;
    }
    shown = tail;
  }
  gfx.setTextColor(theme::kAccent);
  gfx.drawString(shown, theme::kCenterX, kTitleY);
}

void drawKeyboard(lgfx::LovyanGFX& gfx) {
  displayFontEnsureLoaded(gfx);
  gfx.fillScreen(theme::kBackground);
  drawPassword(gfx);

  const Page& p = page();
  for (int row = 0; row < theme::kKeyRows; ++row) {
    int x = rowLeft(p.rows[row]);
    for (const char* k = p.rows[row]; *k != '\0'; ++k) {
      const int w = keyWidth(*k);
      drawKey(gfx, *k, x, rowY(row), w);
      x += w + theme::kKeyGap;
    }
  }

  const bool ready = strlen(s_password) >= kMinPassword;
  const int gx = theme::kCenterX - theme::kSearchGoWidth / 2;
  const int gy = theme::kSearchGoY - theme::kSearchGoHeight / 2;
  gfx.fillRoundRect(gx, gy, theme::kSearchGoWidth, theme::kSearchGoHeight,
                    theme::kKeyRadius, ready ? theme::kAccent : theme::kSurface);
  displayFontApplyHeight(gfx, theme::kKeyTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(ready ? theme::kBackground : theme::kTextMuted);
  gfx.drawString("JOIN", theme::kCenterX, theme::kSearchGoY);
}

Result pressKey(char key) {
  const size_t len = strlen(s_password);
  switch (key) {
    case kShift:
      s_shift = !s_shift;
      return Result::kChanged;
    case kToDigits:
      s_mode = Mode::kDigits;
      return Result::kChanged;
    case kToSymbols:
      s_mode = Mode::kSymbols;
      return Result::kChanged;
    case kToLetters:
      s_mode = Mode::kLetters;
      return Result::kChanged;
    case kBackspace:
      if (len == 0) {
        return Result::kNone;
      }
      s_password[len - 1] = '\0';
      return Result::kChanged;
    default:
      if (len >= kMaxPassword) {
        return Result::kNone;
      }
      s_password[len] = key;
      s_password[len + 1] = '\0';
      return Result::kChanged;
  }
}

Result tapKeyboard(int x, int y) {
  const int gy = theme::kSearchGoY - theme::kSearchGoHeight / 2;
  if (y >= gy && y < gy + theme::kSearchGoHeight &&
      x >= theme::kCenterX - theme::kSearchGoWidth / 2 &&
      x < theme::kCenterX + theme::kSearchGoWidth / 2) {
    return strlen(s_password) >= kMinPassword ? Result::kJoin : Result::kNone;
  }

  const Page& p = page();
  for (int row = 0; row < theme::kKeyRows; ++row) {
    const int top = rowY(row);
    if (y < top || y >= top + theme::kKeyHeight) {
      continue;
    }
    int left = rowLeft(p.rows[row]);
    for (const char* k = p.rows[row]; *k != '\0'; ++k) {
      const int w = keyWidth(*k);
      // The gap after a key belongs to it, as on the search keyboard.
      if (x >= left && x < left + w + theme::kKeyGap) {
        return pressKey(*k);
      }
      left += w + theme::kKeyGap;
    }
    return Result::kNone;
  }
  return Result::kNone;
}

}  // namespace

void open(const char* note, const char* current) {
  snprintf(s_note, sizeof(s_note), "%s", note != nullptr ? note : "");
  snprintf(s_current, sizeof(s_current), "%s",
           current != nullptr ? current : "");
  s_scanning = true;
  s_count = 0;
  s_on_list = true;
  s_scroll_px = 0;
  s_chosen = -1;
  s_password[0] = '\0';
  s_mode = Mode::kLetters;
  s_shift = false;
}

void setNetworks(const WifiNetwork* networks, int count) {
  s_count = count < 0 ? 0 : (count > kMaxNetworks ? kMaxNetworks : count);
  for (int i = 0; i < s_count; ++i) {
    s_networks[i] = networks[i];
  }
  s_scanning = false;
  s_scroll_px = 0;
}

bool onList() { return s_on_list; }

bool back() {
  if (s_on_list) {
    return false;
  }
  s_on_list = true;
  s_password[0] = '\0';
  s_mode = Mode::kLetters;
  s_shift = false;
  return true;
}

void draw() {
  lgfx::LovyanGFX& gfx = ui::canvas();
  displayFontEnsureLoaded(gfx);
  if (s_on_list) {
    drawList(gfx);
  } else {
    drawKeyboard(gfx);
  }
  ui::canvasPresent();
}

bool scrollByPx(int delta) {
  if (!s_on_list || s_scanning) {
    return false;
  }
  const int before = s_scroll_px;
  s_scroll_px += delta;
  clampScroll();
  return s_scroll_px != before;
}

bool atScrollLimit(int direction) {
  if (direction < 0) {
    return s_scroll_px <= 0;
  }
  return s_scroll_px >= maxScrollPx();
}

Result handleTap(int x, int y) {
  return s_on_list ? tapList(x, y) : tapKeyboard(x, y);
}

const char* chosenSsid() {
  return s_chosen >= 0 && s_chosen < s_count ? s_networks[s_chosen].ssid : "";
}

const char* password() { return s_password; }

}  // namespace ui::wifi_join
