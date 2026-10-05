#include "ui/isolate_card.h"

#include <cstdio>

#include "hardware/display_font.h"
#include "ui/canvas.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace ui::isolate_card {
namespace {

using Buttons = IsolateCard::Buttons;

/** The title row the settings and the lists share, in the settings' heading
 *  size. */
constexpr int kTitleY = theme::kSearchQueryY;
constexpr int kTitleTextPx = theme::kStatusBodyTextPx;
/** What the rooms below are: the ones to go off, or that went off. */
constexpr int kNoteY = theme::px(66);
/** The rooms, one to a line. Three fit above the buttons on the round
 *  panel's narrowing lower half; more than that and the last line says how
 *  many are left over. */
constexpr int kRoomTopY = theme::px(94);
constexpr int kRoomPitch = theme::px(24);
constexpr int kMaxRoomLines = 3;
/** Low enough to clear the rooms, high enough that both buttons sit whole
 *  inside the round panel's chord. */
constexpr int kButtonY = theme::px(178);
constexpr int kButtonHeight = theme::px(34);
constexpr int kButtonWidth = theme::px(80);
constexpr int kButtonGap = theme::px(10);
constexpr int kOkWidth = theme::px(100);
/** A finger's worth around each button. */
constexpr int kHitPad = theme::px(6);

/** Isolate's centre, and the Cancel button's: Isolate on the left. */
constexpr int kIsolateX = theme::kCenterX - (kButtonWidth + kButtonGap) / 2;
constexpr int kCancelX = theme::kCenterX + (kButtonWidth + kButtonGap) / 2;

Buttons s_buttons = Buttons::kNone;

void drawLine(lgfx::LovyanGFX& gfx, const char* str, int x, int y,
              int text_px, uint16_t color, int width) {
  displayFontApplyHeight(gfx, text_px);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(color);
  char line[text::kMaxLineLen];
  text::ellipsize(gfx, str, width, line, sizeof(line));
  gfx.drawString(line, x, y);
}

void drawButton(lgfx::LovyanGFX& gfx, int cx, int w, const char* label,
                uint16_t fill, uint16_t color) {
  gfx.fillRoundRect(cx - w / 2, kButtonY - kButtonHeight / 2, w,
                    kButtonHeight, kButtonHeight / 2, fill);
  drawLine(gfx, label, cx, kButtonY, theme::kListRowTextPx, color,
           w - 2 * theme::px(6));
}

bool onButton(int x, int y, int cx, int w) {
  return x >= cx - w / 2 - kHitPad && x <= cx + w / 2 + kHitPad &&
         y >= kButtonY - kButtonHeight / 2 - kHitPad &&
         y <= kButtonY + kButtonHeight / 2 + kHitPad;
}

}  // namespace

void draw(const IsolateCard& card) {
  lgfx::LovyanGFX& gfx = ui::canvas();
  displayFontEnsureLoaded(gfx);
  theme::fillListBackdrop(gfx, 0, theme::kSize);
  s_buttons = card.buttons;

  drawLine(gfx, card.title, theme::kCenterX, kTitleY, kTitleTextPx,
           card.warning ? theme::kWarning : theme::kTextPrimary,
           theme::usableWidthAt(kTitleY, theme::kTextEdgeInset));

  if (card.note != nullptr && card.note[0] != '\0') {
    drawLine(gfx, card.note, theme::kCenterX, kNoteY,
             theme::kListHeaderTextPx, theme::kTextMuted,
             theme::usableWidthAt(kNoteY, theme::kTextEdgeInset));
  }

  // All of them if they fit, and otherwise one line fewer and a count of
  // the rest on the last.
  const int shown = card.room_count <= kMaxRoomLines ? card.room_count
                                                     : kMaxRoomLines - 1;
  for (int i = 0; i < shown; ++i) {
    const int y = kRoomTopY + i * kRoomPitch;
    drawLine(gfx, card.rooms[i], theme::kCenterX, y, theme::kListRowTextPx,
             theme::kTextPrimary,
             theme::usableWidthAt(y, theme::kTextEdgeInset));
  }
  if (shown < card.room_count) {
    char more[24];
    snprintf(more, sizeof(more), "and %d more", card.room_count - shown);
    const int y = kRoomTopY + shown * kRoomPitch;
    drawLine(gfx, more, theme::kCenterX, y, theme::kListRowTextPx,
             theme::kTextMuted,
             theme::usableWidthAt(y, theme::kTextEdgeInset));
  }

  switch (card.buttons) {
    case Buttons::kAsk:
      drawButton(gfx, kCancelX, kButtonWidth, "Cancel", theme::kSurfaceRaised,
                 theme::kTextPrimary);
      drawButton(gfx, kIsolateX, kButtonWidth, "Isolate", theme::kAccent,
                 theme::kBackground);
      break;
    case Buttons::kOk:
      drawButton(gfx, theme::kCenterX, kOkWidth, "OK", theme::kSurfaceRaised,
                 theme::kTextPrimary);
      break;
    case Buttons::kNone:
      break;
  }
  ui::canvasPresent();
}

Result hit(int x, int y) {
  switch (s_buttons) {
    case Buttons::kAsk:
      if (onButton(x, y, kIsolateX, kButtonWidth)) {
        return Result::kConfirm;
      }
      if (onButton(x, y, kCancelX, kButtonWidth)) {
        return Result::kCancel;
      }
      return Result::kNone;
    case Buttons::kOk:
      // Anywhere: a failure has been read once it is tapped.
      return Result::kCancel;
    case Buttons::kNone:
      break;
  }
  return Result::kNone;
}

}  // namespace ui::isolate_card
