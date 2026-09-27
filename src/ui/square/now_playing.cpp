#include "ui/now_playing.h"

/**
 * Now playing, on a square panel.
 *
 * Nearly all of this is the round screen unchanged -- the transport row, the
 * glyphs, the track text, the elapsed chip, the scrim over the cover art. One
 * idea does not survive the change of shape: a volume arc only makes sense
 * where there is a bezel to hug. Here it is a bar across the top.
 *
 * The *gesture* is the same either way, which is the point of the seam. A
 * sideways swipe across the upper half moves the volume; whether that is read
 * as an angle or a distance is this file's business, and src/app/ receives
 * Intent::kSetVolume from both.
 *
 * The other difference is invisible and matters more. This panel has no
 * command channel, so ui::panel() hands back the composed frame rather than
 * the glass. Every partial repaint therefore ends in canvasPresentRegion() to
 * push the rectangle it touched -- a megabyte a frame if that were skipped and
 * the whole screen presented instead.
 */

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "board/board.h"
#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "ui/cover_art.h"
#include "ui/canvas.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace ui::now_playing {
namespace {

using services::ha::PlaybackState;
using services::ha::PlayerState;

/** Last state composed, so a pressed-button repaint keeps the same glyph
 *  instead of falling back to a default. */
PlayerState s_last_drawn;

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
// A bar across the top, where the round build has an arc round the bezel.
// These live here rather than in ui/theme.h because they are the one piece of
// layout the two shapes genuinely do not share.

/** Inset from the panel edge, the same at the sides as above it, so the bar
 *  sits evenly in the corner. Close to the edge: it only has to be seen, not
 *  grabbed (see volumeSwipeRegion), and every row it gives up goes to the
 *  track text below it. */
constexpr int kBarInset = theme::px(14);
constexpr int kBarY = theme::px(14);
/** Slim, and a small knob: the bar is on screen the whole time a track
 *  plays, and it does not have to be grabbed -- a swipe anywhere in the top
 *  half moves it -- so it only has to read as a level. About the round
 *  build's arc and knob, which are px(7) thick and px(6) in radius. */
constexpr int kBarHeight = theme::px(6);
constexpr int kBarRadius = kBarHeight / 2;
constexpr int kBarLeft = kBarInset;
constexpr int kBarWidth = theme::kSize - 2 * kBarInset;
constexpr int kKnobRadius = theme::px(7);
/** Light grey rather than white, so the level sits behind the title instead
 *  of competing with it across the full width of the glass. */
constexpr uint16_t kBarFill = theme::kTextSecondary;

/** The rectangle a volume repaint touches, knob included, for the present. */
constexpr int kBarBandTop = kBarY - kKnobRadius - theme::px(2);
constexpr int kBarBandHeight = kBarHeight + 2 * kKnobRadius + theme::px(4);

/**
 * What the last compose put under the volume band, before the bar went on.
 *
 * A drag has to erase the old knob, and the knob overhangs the bar, so
 * something must be repainted behind it. Filling the band with a flat colour
 * does that but reads as a black stripe cut across the cover art -- the art is
 * the backdrop, and on this panel it runs edge to edge. Putting back the
 * pixels the compose laid down keeps the art intact under the slider.
 *
 * Nothing else is drawn into these rows (the subtitle starts below them), so
 * the copy stays valid until the next compose replaces it. PSRAM, because at
 * 720 px wide it is 155 KB; if it cannot be had, the drag falls back to the
 * flat fill rather than smearing.
 */
uint16_t* s_band_backdrop = nullptr;
bool s_band_valid = false;

void saveBandBackdrop(lgfx::LovyanGFX& gfx) {
  if (s_band_backdrop == nullptr) {
    s_band_backdrop = static_cast<uint16_t*>(heap_caps_malloc(
        static_cast<size_t>(theme::kSize) * kBarBandHeight * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (s_band_backdrop == nullptr) {
    s_band_valid = false;
    return;
  }
  // readRect() and pushImage() agree on the uint16_t layout between
  // themselves, so the round trip is exact whatever byte order the canvas
  // keeps.
  gfx.readRect(0, kBarBandTop, theme::kSize, kBarBandHeight, s_band_backdrop);
  s_band_valid = true;
}

void restoreBandBackdrop(lgfx::LovyanGFX& gfx) {
  if (s_band_valid) {
    gfx.pushImage(0, kBarBandTop, theme::kSize, kBarBandHeight,
                  s_band_backdrop);
  } else {
    gfx.fillRect(0, kBarBandTop, theme::kSize, kBarBandHeight,
                 theme::kBackground);
  }
}

/** Track, fill and knob. Paints over whatever is beneath without clearing
 *  it, so the caller decides what the bar sits on. */
void drawVolumeBar(lgfx::LovyanGFX& gfx, const PlayerState& state,
                   float level) {
  if (level < 0.0f) {
    level = 0.0f;
  } else if (level > 1.0f) {
    level = 1.0f;
  }

  gfx.fillRoundRect(kBarLeft, kBarY, kBarWidth, kBarHeight, kBarRadius,
                    theme::kArcTrack);

  const uint16_t fill = state.muted ? theme::kVolumeMutedFill : kBarFill;
  const int filled = static_cast<int>(kBarWidth * level + 0.5f);
  if (filled > 0) {
    gfx.fillRoundRect(kBarLeft, kBarY, filled, kBarHeight, kBarRadius, fill);
  }

  // The knob is what makes it read as draggable rather than as a readout.
  const int knob_x = kBarLeft + filled;
  const int knob_y = kBarY + kBarHeight / 2;
  gfx.fillCircle(knob_x, knob_y, kKnobRadius, fill);
  gfx.drawCircle(knob_x, knob_y, kKnobRadius, theme::kBackground);
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

/** Draws the chip and returns the pill's width, or 0 when there is none. The
 *  width is what refreshElapsed() needs to present only the columns it
 *  touched, and only the caller doing a partial repaint reads it. */
int drawElapsedChip(lgfx::LovyanGFX& gfx, const PlayerState& state) {
  char label[32];
  formatElapsed(state, label, sizeof(label));
  if (label[0] == '\0') {
    return 0;
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
  return w;
}

// --- Text -------------------------------------------------------------------

/** The round layout's text heights, raised into the room the bar gave up at
 *  the top. The round panel keeps its artist clear of the bezel's curve; a
 *  square has no curve, and at the round heights the block sat low, with a
 *  wide gap under the bar and a two-line title close over the transport row.
 *  Raised this far, the gap under the bar and the gap above the buttons come
 *  out about even whether the title takes one line or two. */
constexpr int kTextRaise = theme::px(10);
constexpr int kSubtitleY = theme::kSubtitleY - kTextRaise;
constexpr int kTitleBlockCenterY = theme::kTitleBlockCenterY - kTextRaise;
/** The scrim's stronger ramp starts under the text, so it moves up with it. */
constexpr int kScrimGradientTop = theme::kScrimGradientTop - kTextRaise;

void drawTrackText(lgfx::LovyanGFX& gfx, const PlayerState& state) {
  const bool has_title = state.title[0] != '\0';
  if (!has_title && state.hold_label) {
    return;  // most likely between tracks: blank, not "Nothing playing"
  }

  displayFontApplyHeight(gfx, theme::kTitleTextPx);
  gfx.setTextDatum(textdatum_t::middle_center);
  gfx.setTextColor(theme::kTextPrimary);

  const int title_width =
      theme::usableWidthAt(kTitleBlockCenterY, theme::kTextEdgeInset);

  char lines[theme::kTitleMaxLines][text::kMaxLineLen] = {};
  const int count =
      text::wrap(gfx, has_title ? state.title : idleLabel(state), title_width,
                 theme::kTitleMaxLines, lines);

  // Centre the block of lines on kTitleBlockCenterY rather than growing down
  // from it, so one- and two-line titles both sit in the same optical place.
  const int block_top = kTitleBlockCenterY -
                        ((count - 1) * theme::kTitleLineHeight) / 2;
  for (int i = 0; i < count; ++i) {
    gfx.drawString(lines[i], theme::kCenterX,
                   block_top + i * theme::kTitleLineHeight);
  }

  if (state.subtitle[0] == '\0') {
    return;
  }
  displayFontApplyHeight(gfx, theme::kSubtitleTextPx);
  gfx.setTextColor(theme::kTextSecondary);
  const int sub_width =
      theme::usableWidthAt(kSubtitleY, theme::kTextEdgeInset);
  char subtitle[text::kMaxLineLen];
  text::ellipsize(gfx, state.subtitle, sub_width, subtitle, sizeof(subtitle));
  gfx.drawString(subtitle, theme::kCenterX, kSubtitleY);
}

/** Backdrop when there is no art: a flat surface, so the text still reads.
 *  Edge to edge, because the glass is square -- this used to be the round
 *  build's black fill with a surface-coloured circle over it, which here
 *  drew a disc with black corners. */
void drawPlainBackdrop(lgfx::LovyanGFX& gfx) {
  gfx.fillScreen(theme::kSurface);
}

/** Draw the whole screen into the frame buffer. */
void compose(lgfx::LovyanGFX& gfx, const PlayerState& state) {
  displayFontEnsureLoaded(gfx);

  gfx.fillScreen(theme::kBackground);
  // (0, 0) is the top-left of the fit box, which is the whole panel; the
  // middle_center datum positions the art inside it.
  if (ui::cover::hasArt() &&
      ui::cover::draw(gfx, 0, 0, theme::kSize)) {
    // Art is the backdrop, so everything above it needs a scrim: a flat dim
    // for the whole frame, plus a stronger ramp under the text and transport
    // row where contrast matters most.
    ui::dim(0, 0, board::kDisplayWidth, board::kDisplayHeight,
            theme::kScrimBaseAlpha);
    ui::dimGradient(kScrimGradientTop,
                    board::kDisplayHeight - kScrimGradientTop, 0,
                    theme::kScrimGradientAlpha);
  } else {
    drawPlainBackdrop(gfx);
  }

  drawTrackText(gfx, state);
  drawElapsedChip(gfx, state);
  // Whether or not the bar is shown now: volume can become available without
  // anything else changing, and the first drag must still find the backdrop.
  saveBandBackdrop(gfx);
  if (volumeAvailable(state)) {
    drawVolumeBar(gfx, state, state.volume);
  }

  for (const Action action :
       {Action::kPrevious, Action::kPlayPause, Action::kNext}) {
    drawTransportButton(gfx, buttonFor(action), state, false,
                        enabledFor(action, state));
  }
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
  // The chip carries its own opaque pill, so repainting it needs nothing from
  // the cover art beneath -- which is the whole reason it is a chip and not
  // bare text on the scrim. ui::panel() is the composed frame here, so the
  // draw is followed by a present of just the chip's band.
  //
  // Only the chip's own columns, not the full width. Both ends of this copy
  // live in PSRAM, on the same bus already carrying 24 MB/s of scan-out that
  // the panel cannot be starved of, so the narrower the rectangle the less
  // time it spends competing for that bus.
  //
  // Presented as the union with the previous width, which is not a detail:
  // the pill is centred and changes width with the label -- "9:59" to "10:00"
  // and back again -- so presenting only the narrower of the two would leave
  // the wider one's ends on the glass.
  static int s_last_w = 0;
  const int w = drawElapsedChip(ui::panel(), state);
  const int previous = s_last_w;
  s_last_w = w;
  if (w <= 0 && previous <= 0) {
    return;  // nothing drawn now, and nothing left over to cover
  }
  const int widest = std::max(w, previous) + 2 * theme::px(2);
  ui::canvasPresentRegion(theme::kCenterX - widest / 2,
                          theme::kElapsedY - theme::px(14), widest,
                          theme::px(28));
}

void refreshVolume(const PlayerState& state, float level) {
  lgfx::LovyanGFX& gfx = ui::panel();
  restoreBandBackdrop(gfx);
  drawVolumeBar(gfx, state, level);
  // Only the band the bar and its knob occupy: about 100 of 720 rows, so a
  // drag costs a tenth of what presenting the frame would.
  ui::canvasPresentRegion(0, kBarBandTop, theme::kSize, kBarBandHeight);
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
  // One pixel of finger travel is one pixel of bar, so the knob keeps pace
  // with the hand. On a 720 px panel the bar is 672 px long, which makes a
  // full sweep of the screen almost exactly a full sweep of the volume.
  return kBarWidth > 1 ? 1.0f / static_cast<float>(kBarWidth) : 0.0f;
}

bool volumeSwipeRegion(int x, int y) {
  (void)x;
  // The upper half, as on the round panel -- not the bar itself. Landing a
  // fingertip on an 18 px bar is a precision task, and this is the control most
  // used without looking. The whole top of the screen is the target.
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

  // ui::panel() is the composed frame here, so the disc is not on the glass
  // until its square is pushed -- and a repaint the frame keeps to itself also
  // leaves the two scan-out buffers disagreeing about it.
  const int edge = button.radius * 2 + 1;
  ui::canvasPresentRegion(button.cx - button.radius,
                          button.cy - button.radius, edge, edge);
}

}  // namespace ui::now_playing
