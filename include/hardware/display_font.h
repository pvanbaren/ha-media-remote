#pragma once

#include <LovyanGFX.hpp>

/**
 * Anti-aliased UI text.
 *
 * One embedded VLW font per on-screen pixel height the UI uses, each rendered
 * natively at that height by scripts/build_ui_font.py. Nothing is scaled at
 * runtime: LovyanGFX's VLW scaler is nearest-neighbour, with no interpolation,
 * so a 15 px label taken from a 24 px master is visibly chunkier than one
 * rendered at 15 px.
 *
 * The sizes are 15, 17, 20 and 24 px, and ui/theme.h asks for exactly those.
 * Keep the set in step with board_build.embed_files in platformio.ini and with
 * the --heights argument to the generator.
 */

bool displayFontInit();
bool displayFontIsSmooth();

/** Load the default (body) font on gfx, for callers that draw text without a
 *  particular size in mind. */
bool displayFontEnsureLoaded(lgfx::LGFXBase& gfx);

/** Select the embedded font whose native height matches target_px and load it
 *  on gfx, skipping the reload when it is already active there.
 *
 *  Drawn at setTextSize(1.0) whenever the closest font is within a pixel of
 *  the request, which for every size in ui/theme.h means always. A request
 *  further out than that -- a port to a larger panel, where theme::px() scales
 *  everything up -- falls back to a fractional rescale of the nearest font
 *  rather than drawing the wrong size. Regenerate the set for that panel to
 *  get crisp text back. */
void displayFontApplyHeight(lgfx::LGFXBase& gfx, float target_px);

/** Bitmap GFXfont fallback; clears any runtime VLW font on this instance. */
void displayFontSetBitmap(lgfx::LGFXBase& gfx, const lgfx::GFXfont* font);
