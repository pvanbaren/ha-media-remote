#include "ui/settings_list.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <cstdio>

#include "board/board.h"
#include "config.h"
#include "hardware/display_font.h"
#include "ui/back_button.h"
#include "ui/canvas.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace ui::settings_list {
namespace {

/** Enough for every media player and an option before them: the longest
 *  list of choices is the Volume & power device's. */
constexpr int kMaxRows = static_cast<int>(config::kMaxPlayers) + 2;
constexpr size_t kTextLen = 64;

struct Row {
  char text[kTextLen];
  char detail[kTextLen];
  SettingStyle style;
};

/** PSRAM, claimed on first use and kept. */
Row* s_rows = nullptr;
int s_count = 0;
char s_title[kTextLen] = {};
bool s_noted = false;
int s_scroll_px = 0;

constexpr int kTitleY = theme::kSearchQueryY;
/** The body size rather than the other lists' title size: a page heading
 *  over rows of the list's own size wants to be the larger of the two, and
 *  every board embeds this one. */
constexpr int kTitleTextPx = theme::kStatusBodyTextPx;
/** Rows live below the title and slide under it as they scroll, clear of
 *  its larger face. */
constexpr int kListTop = kTitleY + kTitleTextPx / 2 + theme::px(8);
constexpr int kPitch = theme::kListRowHeight + theme::kListRowGap;
/** A two-line row's lines, from its centre. */
constexpr int kLabelDy = -theme::listPx(11);
constexpr int kDetailDy = theme::listPx(7);

int rowTop(int row) { return kListTop + row * kPitch - s_scroll_px; }

int maxScrollPx() {
  // Far enough for the last row's centre to reach the middle of the panel,
  // as on the other lists: on a circle it would otherwise rest in the
  // pinched bottom, the hardest place to hit.
  const int over = (s_count - 1) * kPitch + theme::kListRowHeight / 2 +
                   kListTop - theme::kCenterY;
  return over > 0 ? over : 0;
}

void clampScroll() {
  const int limit = maxScrollPx();
  s_scroll_px = s_scroll_px > limit ? limit : s_scroll_px;
  s_scroll_px = s_scroll_px < 0 ? 0 : s_scroll_px;
}

/** As the other lists' rows: the chord at whichever edge is further in. */
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

void drawLine(lgfx::LovyanGFX& gfx, const char* str, int x, int y,
              int text_px, uint16_t color, int width) {
  displayFontApplyHeight(gfx, text_px);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(color);
  char line[text::kMaxLineLen];
  text::ellipsize(gfx, str, width, line, sizeof(line));
  gfx.drawString(line, x, y);
}

void drawRow(lgfx::LovyanGFX& gfx, const Row& row, int top) {
  const int half = rowHalfWidth(top);
  if (half <= 0) {
    return;
  }
  const int x = theme::kCenterX - half;
  const int w = half * 2;
  const int cy = top + theme::kListRowHeight / 2;
  const bool current = row.style == SettingStyle::kCurrent;
  gfx.fillRoundRect(x, top, w, theme::kListRowHeight, theme::kListRowRadius,
                    current ? theme::kSurfaceRaised : theme::kSurface);

  const uint16_t color = current ? theme::kAccent
                         : row.style == SettingStyle::kMuted
                             ? theme::kTextMuted
                             : theme::kTextPrimary;
  const int text_w = w - 2 * theme::kListTextInset;
  if (row.detail[0] == '\0') {
    drawLine(gfx, row.text, theme::kCenterX, cy, theme::kListRowTextPx, color,
             text_w);
    return;
  }
  drawLine(gfx, row.text, theme::kCenterX, cy + kLabelDy,
           theme::kListHeaderTextPx, theme::kTextMuted, text_w);
  drawLine(gfx, row.detail, theme::kCenterX, cy + kDetailDy,
           theme::kListRowTextPx, color, text_w);
}

}  // namespace

bool begin(const char* title, const char* note) {
  if (s_rows == nullptr) {
    s_rows = static_cast<Row*>(heap_caps_calloc(
        kMaxRows, sizeof(Row), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  s_count = 0;
  s_noted = note != nullptr && note[0] != '\0';
  snprintf(s_title, sizeof(s_title), "%s", s_noted ? note : title);
  return s_rows != nullptr;
}

void add(const char* text, const char* detail, SettingStyle style) {
  if (s_rows == nullptr || s_count >= kMaxRows) {
    return;
  }
  Row& row = s_rows[s_count++];
  snprintf(row.text, sizeof(row.text), "%s", text != nullptr ? text : "");
  snprintf(row.detail, sizeof(row.detail), "%s",
           detail != nullptr ? detail : "");
  row.style = style;
}

void open(int focus) {
  for (int i = 0; focus < 0 && i < s_count; ++i) {
    if (s_rows[i].style == SettingStyle::kCurrent) {
      focus = i;
    }
  }
  // In the middle of the panel, where it is easiest to read, as far as the
  // clamp allows. Measured from the top: rowTop() counts the old scroll.
  s_scroll_px = 0;
  if (focus >= 0 && focus < s_count) {
    s_scroll_px = rowTop(focus) + theme::kListRowHeight / 2 - theme::kCenterY;
  }
  clampScroll();
}

void draw() {
  lgfx::LovyanGFX& gfx = ui::canvas();
  displayFontEnsureLoaded(gfx);
  theme::fillListBackdrop(gfx, 0, theme::kSize);

  clampScroll();
  gfx.setClipRect(0, kListTop, theme::kSize, theme::kSize - kListTop);
  for (int i = 0; i < s_count; ++i) {
    const int top = rowTop(i);
    if (top >= theme::kSize) {
      break;
    }
    if (top + theme::kListRowHeight > kListTop) {
      drawRow(gfx, s_rows[i], top);
    }
  }
  gfx.clearClipRect();

  // The title and the back button last, over anything that scrolled up
  // behind them.
  int title_x = 0;
  int title_w = 0;
  back_button::lineSpan(kTitleY, kTitleTextPx, title_x, title_w);
  drawLine(gfx, s_title, title_x, kTitleY, kTitleTextPx,
           s_noted ? theme::kWarning : theme::kTextPrimary, title_w);
  back_button::draw(gfx);
  ui::canvasPresent();
}

bool scrollByPx(int delta) {
  const int before = s_scroll_px;
  s_scroll_px += delta;
  clampScroll();
  return s_scroll_px != before;
}

bool atScrollLimit(int direction) {
  return direction < 0 ? s_scroll_px <= 0 : s_scroll_px >= maxScrollPx();
}

int rowAt(int x, int y) {
  if (y < kListTop) {
    return -1;
  }
  for (int i = 0; i < s_count; ++i) {
    const int top = rowTop(i);
    if (y < top || y >= top + theme::kListRowHeight) {
      continue;
    }
    const int half = rowHalfWidth(top);
    if (x < theme::kCenterX - half || x > theme::kCenterX + half) {
      return -1;
    }
    return i;
  }
  return -1;
}

}  // namespace ui::settings_list
