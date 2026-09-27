#pragma once

#include <LovyanGFX.hpp>

#include "ui/theme.h"

namespace ui::glow {

/**
 * A soft dark glow behind text drawn over cover art.
 *
 * The scrim darkens the whole frame evenly, which is enough over most covers
 * and not over a pale one. An outline would be the other answer; a glow reads
 * as the art falling away from the letters rather than as a stroke drawn round
 * them. It is the text's own shape: drawn white into a mask, blurred, and the
 * frame darkened by the result, before the text itself goes on top.
 *
 *   if (auto* mask = ui::glow::begin(top, height)) {
 *     drawTheText(*mask, top);   // the same text, `top` rows higher
 *     ui::glow::apply();
 *   }
 *   drawTheText(frame, 0);
 */

/** How far the glow spreads from a stroke. */
constexpr int kRadius = theme::px(3);
/** Room the blur needs past the text it surrounds, above and below. */
constexpr int kMargin = 2 * kRadius + theme::px(2);

/** An 8-bit mask the panel's width and `height` rows tall, cleared, for the
 *  text to be drawn into in white -- `top` rows higher than it sits on the
 *  frame. nullptr when no memory could be had; the text then goes on with no
 *  glow. PSRAM, made on first use and kept. */
lgfx::LGFXBase* begin(int top, int height);

/** Blur what was drawn into the mask since begin() and darken the frame by
 *  it. Between drawing and canvasPresent(), like ui::dim(). */
void apply();

}  // namespace ui::glow
