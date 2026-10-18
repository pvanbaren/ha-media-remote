#pragma once

#include <cstdint>

namespace ui::cover_colour {

/**
 * The colour a cover gives now playing's highlights: the most prominent
 * saturated hue in it, not its average -- an average turns most covers a
 * muddy brown or grey. Grey, near-black and near-white pixels do not count,
 * and a cover with too few coloured pixels -- black and white artwork --
 * gives none, so the theme's own accent stays.
 *
 * The idea, and its thresholds, are HomeTiles' (src/types/media/
 * cover_color.h, MIT): a 32 x 32 grid of samples, each coloured one weighed
 * by its chroma into one of 24 hues, and the hue with the most weight around
 * it averaged. What is added here is making the result a highlight: lifted
 * to full brightness and kept saturated enough to stand apart from the
 * volume's white.
 *
 * `pixels` is a frame of RGB565 `stride` pixels wide; the box (x, y, w, h)
 * inside it is what is sampled. `swapped` when each pixel's two bytes are
 * stored the other way round, as LovyanGFX's own sprites keep them. False,
 * leaving `colour` alone, when the cover has no clear colour.
 */
bool pick(const uint16_t* pixels, int stride, int x, int y, int w, int h,
          bool swapped, uint16_t& colour);

}  // namespace ui::cover_colour
