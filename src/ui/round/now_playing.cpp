#include "ui/now_playing.h"

#include <Arduino.h>

#include <esp_heap_caps.h>

#include <cmath>
#include <cstring>

#include "board/board.h"
#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "ui/cover_art.h"
#include "ui/canvas.h"
#include "ui/peers_chip.h"
#include "ui/text.h"
#include "ui/text_glow.h"
#include "ui/theme.h"

namespace ui::now_playing {
namespace {

using services::ha::PlaybackState;
using services::ha::PlayerState;

/** Last state composed, so a pressed-button repaint keeps the same glyph
 *  instead of falling back to a default. */
PlayerState s_last_drawn;

/** What the ring the knob can cover looked like just before the volume arc
 *  went on, so a drag can put back what the knob was covering. The knob is
 *  wider than the arc -- a 6 px radius on a 7 px ring -- so drawing the new
 *  one over the old left a crescent either side of the ring, and its dark
 *  rim, at every step.
 *
 *  Laid out as a whole frame, so a pixel sits at the same offset in both, but
 *  only the ring is ever copied in -- see ringSpans(). PSRAM, claimed once. */
uint16_t* s_arc_backdrop = nullptr;
bool s_arc_backdrop_valid = false;
/** Where the knob is in the canvas right now. */
float s_arc_drawn_level = -1.0f;

struct Button {
  Action action;
  int cx;
  int cy;
  int radius;
};

Button buttonFor(Action action) {
  const int y = theme::kTransportRowY;
  switch (action) {
    case Action::kPrevious:
      return {Action::kPrevious, theme::kCenterX - theme::kTransportSpacing, y,
              theme::kTransportRadius};
    case Action::kNext:
      return {Action::kNext, theme::kCenterX + theme::kTransportSpacing, y,
              theme::kTransportRadius};
    case Action::kPlayPause:
    default:
      return {Action::kPlayPause, theme::kCenterX, y,
              theme::kTransportPlayRadius};
  }
}

bool isPlaying(const PlayerState& state) {
  return state.playback == PlaybackState::kPlaying;
}

/** Live state text for a player that has nothing to show. */
const char* idleLabel(const PlayerState& state) {
  switch (state.playback) {
    case PlaybackState::kPaused:
      return "Paused";
    case PlaybackState::kIdle:
      return "Idle";
    case PlaybackState::kOff:
      return "Off";
    case PlaybackState::kUnavailable:
      return "Unavailable";
    default:
      return "Nothing playing";
  }
}

bool supports(const PlayerState& state, uint32_t feature) {
  // A player that reports no features at all is usually an integration that
  // never set supported_features; assume the transport works rather than
  // greying out controls that would in fact respond.
  return state.supported_features == 0 ||
         (state.supported_features & feature) != 0;
}

bool enabledFor(Action action, const PlayerState& state) {
  if (state.playback == PlaybackState::kUnavailable ||
      state.playback == PlaybackState::kOff) {
    return false;
  }
  switch (action) {
    case Action::kPrevious:
      return supports(state, services::ha::kFeaturePreviousTrack);
    case Action::kNext:
      return supports(state, services::ha::kFeatureNextTrack);
    case Action::kPlayPause:
      return supports(state, services::ha::kFeaturePause |
                      services::ha::kFeaturePlay);
    default:
      return false;
  }
}

// --- Glyphs -----------------------------------------------------------------
// Drawn as vectors rather than bitmaps so they stay crisp at any diameter.

void drawPlayGlyph(lgfx::LGFXBase& gfx, int cx, int cy, uint16_t color) {
  const int gh = theme::kGlyphHalfHeight;
  const int gw = theme::kGlyphHalfWidth;
  // A triangle's visual centre sits a third of the way back from the apex;
  // nudge right so it looks centred in the disc rather than measured centred.
  const int x0 = cx - (gw * 2) / 3;
  gfx.fillTriangle(x0, cy - gh, x0, cy + gh, x0 + gw + gw / 2, cy, color);
}

void drawPauseGlyph(lgfx::LGFXBase& gfx, int cx, int cy, uint16_t color) {
  const int gh = theme::kGlyphHalfHeight;
  const int bar = theme::kGlyphPauseBarWidth;
  const int gap = theme::kGlyphPauseBarGap;
  const int x0 = cx - (bar * 2 + gap) / 2;
  gfx.fillRect(x0, cy - gh, bar, gh * 2, color);
  gfx.fillRect(x0 + bar + gap, cy - gh, bar, gh * 2, color);
}

/** One triangle and an end bar: |< for previous, >| for next.
 *
 *  The triangle is the play glyph's, the same height and the same one and a
 *  half half-widths long, so the three transport buttons read as one set.
 *
 *  Both directions are laid out inside the same bounding box and differ only
 *  in which end each piece occupies, which is what makes the two buttons exact
 *  mirrors of each other. Getting that wrong is easy and was: walking outward
 *  from the centre in the glyph's own direction put the backward bar past the
 *  right-hand edge instead of at the left one, so "previous" drew as <| and
 *  sat three pixels right of the button it was centred in. */
void drawSkipGlyph(lgfx::LGFXBase& gfx, int cx, int cy, bool forward,
                   uint16_t color) {
  const int gh = theme::kGlyphHalfHeight;
  const int gw = theme::kGlyphHalfWidth;
  const int triangle = gw + gw / 2;  // as drawPlayGlyph()
  const int bar = theme::kGlyphSkipBarWidth;
  const int total = triangle + theme::kGlyphSkipBarGap + bar;

  // One box for both directions. Sized from total - 1 because a span of N
  // pixels starting at `left` ends at left + N - 1.
  const int left = cx - (total - 1) / 2;
  const int right = left + total - 1;

  // The bar marks the end the triangle points towards.
  gfx.fillRect(forward ? right - bar + 1 : left, cy - gh, bar, gh * 2, color);

  // The triangle's base is at the far end from the bar, its apex towards it.
  const int base = forward ? left : right;
  const int apex = base + (forward ? triangle : -triangle);
  gfx.fillTriangle(base, cy - gh, base, cy + gh, apex, cy, color);
}

void drawTransportButton(lgfx::LovyanGFX& gfx, const Button& button,
                         const PlayerState& state, bool pressed, bool enabled) {
  const uint16_t fill = pressed ? theme::kSurfaceRaised : theme::kSurface;
  const uint16_t glyph = enabled ? theme::kTextPrimary : theme::kTextMuted;
  const int cy = button.cy;

  gfx.fillCircle(button.cx, cy, button.radius, fill);
  gfx.drawCircle(button.cx, cy, button.radius,
                 pressed ? theme::kAccent : theme::kTextMuted);

  switch (button.action) {
    case Action::kPrevious:
      drawSkipGlyph(gfx, button.cx, cy, false, glyph);
      break;
    case Action::kNext:
      drawSkipGlyph(gfx, button.cx, cy, true, glyph);
      break;
    case Action::kPlayPause:
      if (isPlaying(state)) {
        drawPauseGlyph(gfx, button.cx, cy, glyph);
      } else {
        drawPlayGlyph(gfx, button.cx, cy, glyph);
      }
      break;
    default:
      break;
  }
}

// --- Volume slider ----------------------------------------------------------

/** Point on the arc's centreline at `fraction` along it, in frame coords. */
void volumePoint(float fraction, int& x, int& y) {
  const float deg =
      theme::kVolumeArcStartDeg + theme::kVolumeArcSweepDeg * fraction;
  const float rad = deg * static_cast<float>(M_PI) / 180.0f;
  const float r =
      (theme::kVolumeArcInnerRadius + theme::kVolumeArcOuterRadius) / 2.0f;
  x = theme::kCenterX + static_cast<int>(cosf(rad) * r);
  y = theme::kCenterY + static_cast<int>(sinf(rad) * r);
}

/** The stretch of row `y` the knob can ever cover, as up to two spans of
 *  [first, last] columns; returns how many. An annulus a knob wide either side
 *  of the arc's centreline, cut off below the arc's two ends -- the gap
 *  between them, where the elapsed chip sits, never has a knob in it.
 *
 *  About a quarter of the frame on the 360 px panel, which is the whole point:
 *  this is what compose copies, every time, in case a drag follows. */
int ringSpans(int y, int spans[2][2]) {
  constexpr float kCentreline =
      (theme::kVolumeArcInnerRadius + theme::kVolumeArcOuterRadius) / 2.0f;
  constexpr int kPad = theme::kVolumeKnobRadius + 2;
  constexpr float kOuter = kCentreline + kPad;
  constexpr float kInner = kCentreline - kPad;

  int end_x = 0;
  int low_end = 0;
  int other_end = 0;
  volumePoint(0.0f, end_x, low_end);
  volumePoint(1.0f, end_x, other_end);
  if (y > (low_end > other_end ? low_end : other_end) + kPad) {
    return 0;
  }
  const float dy = static_cast<float>(y - theme::kCenterY);
  if (fabsf(dy) > kOuter) {
    return 0;
  }
  const int outer = static_cast<int>(ceilf(sqrtf(kOuter * kOuter - dy * dy)));
  const int left = theme::kCenterX - outer < 0 ? 0 : theme::kCenterX - outer;
  const int right = theme::kCenterX + outer >= theme::kSize
                        ? theme::kSize - 1
                        : theme::kCenterX + outer;
  if (fabsf(dy) >= kInner) {
    spans[0][0] = left;  // above or below the hole: one chord
    spans[0][1] = right;
    return 1;
  }
  const int inner = static_cast<int>(floorf(sqrtf(kInner * kInner - dy * dy)));
  spans[0][0] = left;
  spans[0][1] = theme::kCenterX - inner;
  spans[1][0] = theme::kCenterX + inner;
  spans[1][1] = right;
  return 2;
}

void saveArcBackdrop(lgfx::LovyanGFX& gfx) {
  s_arc_backdrop_valid = false;
  if (!ui::canvasReady()) {
    return;  // composing straight onto the panel: nothing to read back
  }
  if (s_arc_backdrop == nullptr) {
    // Laid out as a whole frame so a pixel is at the same offset in both,
    // though only the ring is ever filled.
    s_arc_backdrop = static_cast<uint16_t*>(heap_caps_malloc(
        static_cast<size_t>(theme::kSize) * theme::kSize * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (s_arc_backdrop == nullptr) {
    return;
  }
  // readRect() and pushImage() agree on the layout between themselves, so the
  // round trip is exact whatever byte order the canvas keeps.
  int spans[2][2];
  for (int y = 0; y < theme::kSize; ++y) {
    const int n = ringSpans(y, spans);
    for (int i = 0; i < n; ++i) {
      gfx.readRect(spans[i][0], y, spans[i][1] - spans[i][0] + 1, 1,
                   s_arc_backdrop + static_cast<size_t>(y) * theme::kSize +
                       spans[i][0]);
    }
  }
  s_arc_backdrop_valid = true;
}

void drawVolumeArc(lgfx::LovyanGFX& gfx, const PlayerState& state, float level) {
  if (level < 0.0f) {
    level = 0.0f;
  } else if (level > 1.0f) {
    level = 1.0f;
  }

  const int cy = theme::kCenterY;

  gfx.fillArc(theme::kCenterX, cy, theme::kVolumeArcInnerRadius,
              theme::kVolumeArcOuterRadius, theme::kVolumeArcStartDeg,
              theme::kVolumeArcEndDeg, theme::kArcTrack);

  // level is the HA volume_level, 0..1 -- 0.72 is 72% and fills 0.72 of the
  // 252 degree sweep, i.e. 181 degrees from the lower left, putting the knob
  // at roughly two o'clock.
  const float filled_end =
      theme::kVolumeArcStartDeg + theme::kVolumeArcSweepDeg * level;
  // Grey when nothing will be heard at that level: muted, or the volume
  // device switched off.
  const uint16_t fill = state.muted || state.control_off
                            ? theme::kVolumeMutedFill
                            : theme::kVolumeFill;
  if (filled_end > theme::kVolumeArcStartDeg + 0.5f) {
    gfx.fillArc(theme::kCenterX, cy, theme::kVolumeArcInnerRadius,
                theme::kVolumeArcOuterRadius, theme::kVolumeArcStartDeg,
                filled_end, fill);
  }

  // The knob is what makes it read as draggable rather than as a readout.
  int knob_x = 0;
  int knob_y = 0;
  volumePoint(level, knob_x, knob_y);
  gfx.fillCircle(knob_x, knob_y, theme::kVolumeKnobRadius, fill);
  gfx.drawCircle(knob_x, knob_y, theme::kVolumeKnobRadius,
                 theme::kBackground);
}

// --- Elapsed / total chip ---------------------------------------------------

/** Formats "1:23 / 4:56", or leaves `out` empty when there is no duration. */
void formatElapsed(const PlayerState& state, char* out, size_t out_len) {
  out[0] = '\0';
  const float position = services::ha::interpolatedPosition(state);
  if (position < 0.0f || state.duration_s <= 0.0f) {
    return;
  }
  char elapsed[12];
  char total[12];
  text::formatDuration(position, elapsed, sizeof(elapsed));
  text::formatDuration(state.duration_s, total, sizeof(total));
  snprintf(out, out_len, "%s / %s", elapsed, total);
}

void drawElapsedChip(lgfx::LovyanGFX& gfx, const PlayerState& state) {
  char label[32];
  formatElapsed(state, label, sizeof(label));
  if (label[0] == '\0') {
    return;
  }

  displayFontApplyHeight(gfx, theme::kElapsedTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);

  const int text_w = gfx.textWidth(label);
  const int text_h = gfx.fontHeight();
  const int w = text_w + 2 * theme::kElapsedPillPadX;
  const int h = text_h + 2 * theme::kElapsedPillPadY;

  gfx.fillRoundRect(theme::kCenterX - w / 2, theme::kElapsedY - h / 2, w,
                    h, theme::kElapsedPillRadius, theme::kSurface);
  gfx.setTextColor(theme::kTextSecondary);
  gfx.drawString(label, theme::kCenterX, theme::kElapsedY);
}

// --- Text -------------------------------------------------------------------

/** The rows the glow covers: from above the artist to below a two-line
 *  title, with room for the blur to spread both ways. See ui/text_glow.h. */
constexpr int kGlowTop =
    theme::kSubtitleY - theme::kSubtitleTextPx / 2 - ui::glow::kMargin;
constexpr int kGlowBottom =
    theme::kTitleBlockCenterY +
    ((theme::kTitleMaxLines - 1) * theme::kTitleLineHeight) / 2 +
    theme::kTitleTextPx / 2 + ui::glow::kMargin;

/** The track text as laid out once, so the glow and the text share it. */
struct TrackText {
  char lines[theme::kTitleMaxLines][text::kMaxLineLen] = {};
  int count = 0;
  char subtitle[text::kMaxLineLen] = {};
};

/** Draw `text` onto `gfx`, `dy` rows up from where it sits on the frame. With
 *  `mask`, everything in white, for the glow. */
void drawTrackLines(lgfx::LGFXBase& gfx, const TrackText& text, int dy,
                    bool mask) {
  gfx.setTextDatum(textdatum_t::middle_center);
  displayFontApplyHeight(gfx, theme::kTitleTextPx);
  gfx.setTextColor(theme::kTextPrimary);
  // Centre the block of lines on kTitleBlockCenterY rather than growing down
  // from it, so one- and two-line titles both sit in the same optical place.
  const int block_top = theme::kTitleBlockCenterY -
                        ((text.count - 1) * theme::kTitleLineHeight) / 2;
  for (int i = 0; i < text.count; ++i) {
    gfx.drawString(text.lines[i], theme::kCenterX,
                   block_top + i * theme::kTitleLineHeight - dy);
  }
  if (text.subtitle[0] != '\0') {
    displayFontApplyHeight(gfx, theme::kSubtitleTextPx);
    gfx.setTextColor(mask ? theme::kTextPrimary : theme::kTextSecondary);
    gfx.drawString(text.subtitle, theme::kCenterX, theme::kSubtitleY - dy);
  }
}

void drawTrackText(lgfx::LovyanGFX& gfx, const PlayerState& state,
                   bool over_art) {
  const bool has_title = state.title[0] != '\0';
  if (!has_title && state.hold_label) {
    return;  // most likely between tracks: blank, not "Nothing playing"
  }

  TrackText text;
  displayFontApplyHeight(gfx, theme::kTitleTextPx);
  text.count = text::wrap(
      gfx, has_title ? state.title : idleLabel(state),
      theme::usableWidthAt(theme::kTitleBlockCenterY, theme::kTextEdgeInset),
      theme::kTitleMaxLines, text.lines);
  if (state.subtitle[0] != '\0') {
    displayFontApplyHeight(gfx, theme::kSubtitleTextPx);
    text::ellipsize(
        gfx, state.subtitle,
        theme::usableWidthAt(theme::kSubtitleY, theme::kTextEdgeInset),
        text.subtitle, sizeof(text.subtitle));
  }

  // Only over art: on the plain backdrop there is nothing to stand out from.
  if (over_art) {
    if (auto* mask = ui::glow::begin(kGlowTop, kGlowBottom - kGlowTop)) {
      drawTrackLines(*mask, text, kGlowTop, true);
      ui::glow::apply();
    }
  }
  drawTrackLines(gfx, text, 0, false);
}

/** Backdrop when there is no art: a flat surface, so the text still reads. */
void drawPlainBackdrop(lgfx::LovyanGFX& gfx) {
  gfx.fillScreen(theme::kBackground);
  gfx.fillCircle(theme::kCenterX, theme::kCenterY, theme::kRadius,
                 theme::kSurface);
}

/** Draw the whole screen into the frame buffer. */
void compose(lgfx::LovyanGFX& gfx, const PlayerState& state) {
  displayFontEnsureLoaded(gfx);

  gfx.fillScreen(theme::kBackground);
  // (0, 0) is the top-left of the fit box, which is the whole panel; the
  // middle_center datum positions the art inside it.
  const bool over_art =
      ui::cover::hasArt() && ui::cover::draw(gfx, 0, 0, theme::kSize);
  if (over_art) {
    // Art is the backdrop, so everything above it needs a scrim: a flat dim
    // for the whole frame, plus a stronger ramp under the text and transport
    // row where contrast matters most.
    ui::dim(0, 0, board::kDisplayWidth, board::kDisplayHeight,
            theme::kScrimBaseAlpha);
    ui::dimGradient(theme::kScrimGradientTop,
                    board::kDisplayHeight - theme::kScrimGradientTop, 0,
                    theme::kScrimGradientAlpha);
  } else {
    drawPlainBackdrop(gfx);
  }

  drawTrackText(gfx, state, over_art);
  drawElapsedChip(gfx, state);
  if (volumeAvailable(state)) {
    saveArcBackdrop(gfx);
    drawVolumeArc(gfx, state, state.volume);
    s_arc_drawn_level = state.volume;
  } else {
    s_arc_backdrop_valid = false;
  }

  for (const Action action :
       {Action::kPrevious, Action::kPlayPause, Action::kNext}) {
    drawTransportButton(gfx, buttonFor(action), state, false,
                        enabledFor(action, state));
  }
  ui::peers_chip::draw(gfx, state);
}

}  // namespace

void draw(const PlayerState& state) {
  s_last_drawn = state;

  // One pass, decoding the cover art once. main.cpp still only recomposes
  // when something visible actually changed, because that decode is the
  // expensive part of a frame and position alone does not change the picture.
  compose(ui::canvas(), state);
  ui::canvasPresent();
}

void refreshElapsed(const PlayerState& state) {
  // Straight to the panel. The chip carries its own opaque pill, so repainting
  // it needs nothing from the cover art beneath -- which is the whole reason
  // it is a chip and not bare text on the scrim.
  drawElapsedChip(ui::panel(), state);
  if (!board::kPanelWritesDirect) {
    // ui::panel() is the composed frame on a framebuffer panel, so the chip
    // is not on the glass until its band is pushed. All of the band's width:
    // the pill is centred and changes width with its label, and a band a few
    // dozen rows tall costs nothing worth narrowing.
    ui::canvasPresentRegion(0, theme::kElapsedY - theme::px(14), theme::kSize,
                            theme::px(28));
    return;
  }
  // And into the canvas, where that is not the panel, so it keeps up there
  // too. A volume drag presents stretches of the canvas, and near either end
  // of the arc a stretch reaches the chip -- which the canvas would otherwise
  // still show at the time it was composed, torn against the live one.
  if (ui::canvasReady()) {
    drawElapsedChip(ui::canvas(), state);
  }
}

void refreshVolume(const PlayerState& state, float level) {
  if (!s_arc_backdrop_valid || s_arc_drawn_level < 0.0f) {
    // No copy of what is under the ring. Draw over it and let the next
    // compose clean up; the trail stays until then.
    drawVolumeArc(ui::panel(), state, level);
    if (!board::kPanelWritesDirect) {
      ui::canvasPresent();  // the frame, which is all ui::panel() reached
    }
    return;
  }

  // Rebuild the stretch of ring between where the knob was and where it is
  // going, knobs included, in the canvas -- and push only that. Bounded by
  // walking the arc between the two levels, since a stretch of it can bulge
  // well outside the box its two ends make.
  const float from = s_arc_drawn_level < level ? s_arc_drawn_level : level;
  const float to = s_arc_drawn_level < level ? level : s_arc_drawn_level;
  int x0 = theme::kSize;
  int y0 = theme::kSize;
  int x1 = -1;
  int y1 = -1;
  const int steps = 1 + static_cast<int>((to - from) * 64.0f);
  for (int i = 0; i <= steps; ++i) {
    int x = 0;
    int y = 0;
    volumePoint(from + (to - from) * i / steps, x, y);
    x0 = x < x0 ? x : x0;
    y0 = y < y0 ? y : y0;
    x1 = x > x1 ? x : x1;
    y1 = y > y1 ? y : y1;
  }
  const int pad = theme::kVolumeKnobRadius + 2;
  x0 = x0 - pad < 0 ? 0 : x0 - pad;
  y0 = y0 - pad < 0 ? 0 : y0 - pad;
  x1 = x1 + pad >= theme::kSize ? theme::kSize - 1 : x1 + pad;
  y1 = y1 + pad >= theme::kSize ? theme::kSize - 1 : y1 + pad;
  const int w = x1 - x0 + 1;
  const int h = y1 - y0 + 1;

  // Only the ring was kept, and only the ring needs putting back: the arc and
  // the knob never draw outside it.
  lgfx::LovyanGFX& gfx = ui::canvas();
  int spans[2][2];
  for (int row = y0; row <= y1; ++row) {
    const int n = ringSpans(row, spans);
    for (int i = 0; i < n; ++i) {
      const int a = spans[i][0] > x0 ? spans[i][0] : x0;
      const int b = spans[i][1] < x1 ? spans[i][1] : x1;
      if (a <= b) {
        gfx.pushImage(a, row, b - a + 1, 1,
                      s_arc_backdrop + static_cast<size_t>(row) * theme::kSize +
                          a);
      }
    }
  }
  gfx.setClipRect(x0, y0, w, h);
  drawVolumeArc(gfx, state, level);
  // The buttons went on after the arc, so they come back after it too, in
  // case the box reached one.
  for (const Action action :
       {Action::kPrevious, Action::kPlayPause, Action::kNext}) {
    drawTransportButton(gfx, buttonFor(action), s_last_drawn, false,
                        enabledFor(action, s_last_drawn));
  }
  gfx.clearClipRect();
  ui::canvasPresentRegion(x0, y0, w, h);
  s_arc_drawn_level = level;
}

bool volumeAvailable(const PlayerState& state) {
  if (state.volume < 0.0f) {
    return false;
  }
  if (state.playback == PlaybackState::kUnavailable ||
      state.playback == PlaybackState::kOff) {
    return false;
  }
  // Tested against the *control* entity: it is the one the slider drives, and
  // where the two differ it is entirely normal for the receiver to set
  // VOLUME_SET while the player streaming into it does not.
  return state.control_features == 0 ||
         (state.control_features & services::ha::kFeatureVolumeSet) != 0;
}

float volumeLevelPerPx() {
  // Length of the arc the knob runs along, in pixels, measured down its
  // centreline -- the same radius volumePoint() places the knob at. One pixel
  // of finger travel is one pixel of arc, so the knob keeps pace with the
  // hand rather than with the angle it happens to subtend.
  constexpr float kRadius =
      (theme::kVolumeArcInnerRadius + theme::kVolumeArcOuterRadius) / 2.0f;
  const float arc_px = 2.0f * static_cast<float>(M_PI) * kRadius *
                       (theme::kVolumeArcSweepDeg / 360.0f);
  return arc_px > 1.0f ? 1.0f / arc_px : 0.0f;
}

bool volumeSwipeRegion(int x, int y) {
  (void)x;
  return y < theme::kCenterY;
}

Action hitTest(int x, int y) {
  // Nearest target wins rather than first-match, so a tap that lands between
  // two buttons goes to the one it is actually closer to.
  constexpr int kHitRadiusSq =
      theme::kTransportHitRadius * theme::kTransportHitRadius;
  Action closest = Action::kNone;
  int closest_dist_sq = kHitRadiusSq + 1;

  for (const Action action :
       {Action::kPrevious, Action::kPlayPause, Action::kNext}) {
    const Button button = buttonFor(action);
    const int dx = x - button.cx;
    const int dy = y - button.cy;
    const int dist_sq = dx * dx + dy * dy;
    if (dist_sq <= kHitRadiusSq && dist_sq < closest_dist_sq) {
      closest = action;
      closest_dist_sq = dist_sq;
    }
  }
  return closest;
}

void showButtonPressed(Action action, bool pressed) {
  if (action == Action::kNone) {
    return;
  }
  // The discs are opaque, so redrawing one over the composed frame needs
  // nothing from the backdrop underneath it.
  const Button button = buttonFor(action);
  drawTransportButton(ui::panel(), button, s_last_drawn, pressed,
                      enabledFor(action, s_last_drawn));
  if (!board::kPanelWritesDirect) {
    // The frame, on a framebuffer panel: not on the glass until pushed.
    const int edge = button.radius * 2 + 1;
    ui::canvasPresentRegion(button.cx - button.radius,
                            button.cy - button.radius, edge, edge);
  }
}

}  // namespace ui::now_playing
