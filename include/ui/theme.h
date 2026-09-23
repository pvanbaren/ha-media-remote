#pragma once

#include <cstdint>

#include <LovyanGFX.hpp>

#include "board/board.h"
#include "config.h"

namespace ui::theme {

// Every pixel dimension below is authored for a 240 px round panel and scaled
// to the active one via board::kUiScale. Changing board::kDisplayDiameter is
// the only edit needed to move the whole interface to a different round
// display -- nothing here is an absolute pixel count.
constexpr int px(int px240) {
  return static_cast<int>(px240 * board::kUiScale +
                          (px240 < 0 ? -0.5f : 0.5f));
}

/** As px(), for text: also scaled by board::kTextScale, which lets a board
 *  whose pixels are denser than the 240 px design's set its type smaller
 *  without moving any of the layout. Every *TextPx below goes through here,
 *  and each board embeds exactly the faces these come out at. */
constexpr int textPx(int px240) {
  return static_cast<int>(px240 * board::kUiScale * board::kTextScale + 0.5f);
}

/** As px() and textPx(), for the browse and search lists: also scaled by
 *  board::kListScale, so a panel with room to spare can show more rows at once
 *  without shrinking anything else on screen. */
constexpr int listPx(int px240) {
  return static_cast<int>(px240 * board::kUiScale * board::kListScale + 0.5f);
}
constexpr int listTextPx(int px240) {
  return static_cast<int>(px240 * board::kUiScale * board::kTextScale *
                              board::kListScale +
                          0.5f);
}

constexpr int kSize = board::kDisplayWidth;
constexpr int kCenterX = kSize / 2;
constexpr int kCenterY = kSize / 2;
constexpr int kRadius = kSize / 2;

// --- Colours (RGB565) ---
/** The plain status card: black ground, white text. Named here rather than
 *  taken from config, because a card's colours are a display decision. */
constexpr uint16_t kStatusPlain = 0x0000;
constexpr uint16_t kStatusPlainText = 0xFFFF;
constexpr uint16_t kBackground = 0x0000;       // black
constexpr uint16_t kSurface = 0x18E3;          // near-black card
constexpr uint16_t kSurfaceRaised = 0x3186;    // pressed / highlighted
constexpr uint16_t kAccent = 0x05FF;           // HA cyan-blue
constexpr uint16_t kTextPrimary = 0xFFFF;
constexpr uint16_t kTextSecondary = 0xC618;    // light grey
constexpr uint16_t kTextMuted = 0x8410;        // mid grey
constexpr uint16_t kArcTrack = 0x2945;         // unfilled arc, shared
constexpr uint16_t kVolumeFill = 0xFFFF;
constexpr uint16_t kVolumeMutedFill = 0x8410;  // level still shown, greyed
constexpr uint16_t kWarning = 0xFD20;          // amber

// --- Scrim over the cover art, so text stays legible on any image ---
/** Flat dim applied across the whole backdrop. */
constexpr uint8_t kScrimBaseAlpha = 90;
/** Extra gradient under the text and transport row. */
constexpr int kScrimGradientTop = px(96);
constexpr uint8_t kScrimGradientAlpha = 120;

// --- Volume slider, hugging the bezel ---
// LovyanGFX puts 0 degrees at 3 o'clock with angles increasing clockwise, so
// 12 o'clock is -90. Centring the span there sets its ends at the lower left
// and lower right, and leaves the remaining 108 degrees as a gap across the
// bottom -- clear of the transport row, and where the elapsed chip sits.
constexpr float kVolumeArcFraction = 0.70f;
constexpr float kVolumeArcSweepDeg = 360.0f * kVolumeArcFraction;
constexpr float kVolumeArcCenterDeg = -90.0f;
constexpr float kVolumeArcStartDeg =
    kVolumeArcCenterDeg - kVolumeArcSweepDeg / 2.0f;  // -216 == lower left
constexpr float kVolumeArcEndDeg =
    kVolumeArcCenterDeg + kVolumeArcSweepDeg / 2.0f;  // +36 == lower right

constexpr int kVolumeArcOuterRadius = px(118);
constexpr int kVolumeArcInnerRadius = px(111);
// There is no hit band any more. A press does not have to land on the arc at
// all -- a horizontal swipe anywhere in the top half drives it, and moves it
// by the distance the finger travelled. See ui::now_playing::volumeSwipeRegion.
/** Knob marking the current level. */
constexpr int kVolumeKnobRadius = px(6);

// --- Elapsed / total chip, in the gap at the foot of the volume arc ---
// Drawn on an opaque pill so the once-a-second update can be painted straight
// to the panel, without recomposing the cover art underneath it.
constexpr int kElapsedY = px(222);
constexpr int kElapsedTextPx = textPx(15);
constexpr int kElapsedPillPadX = px(7);
constexpr int kElapsedPillPadY = px(3);
constexpr int kElapsedPillRadius = px(8);

// --- Now-playing text block: artist above, track title below ---
// The artist is one line and the title may be two, so putting the artist first
// keeps the block's top edge at a fixed height whatever the title does -- and
// a title that wraps grows downwards, into the gap above the transport row,
// rather than shoving the artist up into the curve of the bezel.
constexpr int kSubtitleY = px(70);
constexpr int kSubtitleTextPx = textPx(20);
constexpr int kTitleBlockCenterY = px(112);
/** 1.2x the title's own height, so two lines sit close without touching. */
constexpr int kTitleLineHeight = textPx(29);
constexpr int kTitleMaxLines = 2;
constexpr int kTitleTextPx = textPx(24);
/** Text is clipped to the chord of the circle at its own y, less this inset. */
constexpr int kTextEdgeInset = px(16);

// --- Transport row: previous / play-pause / next ---
/** Raised on a board whose scaling brings the play button down onto the
 *  elapsed chip below it (board::kTransportRowLiftPx240). */
constexpr int kTransportRowY = px(181 - board::kTransportRowLiftPx240);
constexpr int kTransportSpacing = px(58);
constexpr int kTransportRadius = px(25);
constexpr int kTransportPlayRadius = px(30);
/** Hit radius is generous relative to the drawn disc -- fingers are blunt --
 *  but stays under half the spacing so neighbouring targets cannot overlap. */
constexpr int kTransportHitRadius = px(28);
constexpr int kGlyphHalfHeight = px(9);
constexpr int kGlyphHalfWidth = px(7);
constexpr int kGlyphPauseBarWidth = px(4);
constexpr int kGlyphPauseBarGap = px(4);
constexpr int kGlyphSkipBarWidth = px(3);
/** Gap between the skip glyph's triangle and its end bar. 2 rather than 1 so
 *  the glyph's total width -- 10 + 2 + 3 -- is odd at the baseline size and
 *  it centres on a pixel column rather than straddling two. */
constexpr int kGlyphSkipBarGap = px(2);

// --- Browse list (the swipe-up screen) ---
// Row metrics for a list on a round panel: rows are pinched by the circle at
// whichever of their edges sits further from the centre, so the geometry --
// not a fixed row count -- decides how many fit.
constexpr uint16_t kListBackdrop = 0x0841;  // just off black
constexpr int kListTitleTextPx = listTextPx(17);
/** Everything above this is the strip that dismisses the list on a tap. */
constexpr int kListTopY = px(32);
/** Section headings are a line of text, not a card, so they take far less
 *  room than a row -- which is what makes two sections fit on a 240 px
 *  circle at all. */
constexpr int kListHeaderHeight = listPx(20);
/** The board's call, because it depends on how many rows fit: a panel showing
 *  six wants its headers the same size as the rows they head, and one showing
 *  three wants them a step smaller. */
constexpr int kListHeaderTextPx = listTextPx(board::kListHeaderTextPx240);
/** Tall enough for an item thumbnail plus a little air either side; the
 *  thumbnail (board::kThumbPx) is sized to match. */
constexpr int kListRowHeight = listPx(46);
constexpr int kListRowGap = listPx(4);
/** Gap between the thumbnail and the name. */
constexpr int kListThumbGap = listPx(10);
constexpr int kListRowRadius = listPx(10);
constexpr int kListRowTextPx = listTextPx(20);
/** Rows are inset from the circle edge at their own y by this much. */
constexpr int kListRowInset = listPx(10);
constexpr int kListTextInset = listPx(12);
/** Scroll indicator arc on the right bezel. */
constexpr int kListScrollOuterRadius = px(118);
constexpr int kListScrollInnerRadius = px(114);

// --- Artist search: keyboard, then results ---
// Four rows of seven keys, sized against the chord at each row: the narrowest
// (y=72) allows 205 px and the row needs 201. The fifth row is one wide key,
// down where the circle has only 149 px to give at its bottom edge.
//
// That key is centred at 200 so it starts at 186, 4 px below the last key
// row's bottom at 182. It has to stay clear of that row: a tap is tested
// against it first, so any overlap would take the row's lowest taps as well
// as covering its keys.
constexpr int kSearchQueryY = px(34);
constexpr int kSearchQueryTextPx = textPx(20);
constexpr int kKeyRow0Y = px(72);
constexpr int kKeyRowPitch = px(28);
constexpr int kKeyWidth = px(27);
constexpr int kKeyHeight = px(26);
constexpr int kKeyGap = px(2);
constexpr int kKeyRadius = px(5);
constexpr int kKeyTextPx = textPx(17);
constexpr int kKeyRows = 4;
constexpr int kKeyCols = 7;
constexpr int kSearchGoY = px(200);
constexpr int kSearchGoWidth = px(130);
constexpr int kSearchGoHeight = px(28);

/** Results reuse the browse list's row metrics -- same shape, same artwork,
 *  same scrolling -- and start below this, which is the strip they scroll
 *  under and which backs out of the results on a tap. */
constexpr int kSearchResultTopY = px(38);

// --- Status / message screens ---
constexpr int kStatusLineGap = px(6);
constexpr int kStatusTitleTextPx = textPx(24);
constexpr int kStatusBodyTextPx = textPx(20);

/** Half-width of the circle at a given y -- how much horizontal room a row of
 *  text actually has on a round panel. Returns 0 outside the circle. */
int chordHalfWidth(int y);

/** Widest run of pixels usable at row `y`, inset from the bezel. */
int usableWidthAt(int y, int inset);

/**
 * Paint the list's background over `height` rows starting at `top`.
 *
 * Shape-specific, because rows are laid out to chordHalfWidth(), which on a
 * square panel is the full half-width. So a square list's rows run edge to
 * edge, and a circular backdrop would not cover the pixels they occupy -- a
 * partial repaint that painted a circle would leave the old rows behind in
 * the corners. The square build fills the band; the round build fills the
 * circle inside it, where the corners are off the glass anyway.
 *
 * Leaves no clip set.
 */
void fillListBackdrop(lgfx::LovyanGFX& gfx, int top, int height);

}  // namespace ui::theme
