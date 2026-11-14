#pragma once

#include <LovyanGFX.hpp>

#include <cstddef>

/**
 * What the UI faces lack -- Korean, Chinese, Japanese, Thai, Cyrillic,
 * Greek, the rest of Latin, symbols -- in the UI's text: one of the VLW
 * faces, with outline fonts behind it.
 *
 * The VLW faces are Latin -- 305 glyphs each -- and the Han characters alone
 * are thousands, which pre-rendered at every size would not fit anywhere.
 * So a character the face lacks is drawn from the first outline font that
 * has it, rendered by stb_truetype at the face's own em size the first time
 * it is wanted and kept. Everything else goes to the face as before.
 *
 * The outline fonts are the font pack's, in flash (hardware/font_pack.h),
 * in the order it lists them; a remote that has not downloaded it draws
 * these scripts as blanks. A pack written while the remote runs is drawn
 * from at once.
 *
 * Thai's vowel and tone marks, and the combining accents, are fonts'
 * zero-width glyphs that sit over the letter before them, and are drawn so.
 * A Thai tone mark over a vowel that is itself over the letter is drawn
 * higher, clear of it -- what a font's layout tables would do, which
 * stb_truetype does not read.
 *
 * The rendered glyph is drawn here, as the VLW code draws a face's: blended
 * into what is underneath where the canvas can be read, over the text's
 * background colour where it has one, clipped and scaled alike. Not
 * through the VLW code itself, which copies each glyph's bitmap onto the
 * stack first -- a few KB at the clock's size, more than the loop task
 * has. With no outline font, those characters are blanks, as they always
 * were.
 *
 * Or with no face at all (attachAtHeight()): every character from the
 * outline fonts, at a size no face is -- the clock's.
 *
 * Drawn from the loop only, like the faces themselves.
 */
class OutlineFallback : public lgfx::IFont {
 public:
  /** Put the outline fonts behind `face`, whose em -- read from its header
   *  -- they are rendered at. `face_data` is its VLW blob. */
  void attach(const lgfx::VLWfont* face, const uint8_t* face_data);

  /** Stand alone, with no face: every character from the outline fonts,
   *  the line `height_px` tall -- ascent and descent, as a face's
   *  fontHeight() is -- in the first font that has every character of
   *  `sample`. False, leaving it as it was, when none has, or there are
   *  no outline fonts. Cheap to call again with the same: what was
   *  rendered is kept until the size or the fonts change. */
  bool attachAtHeight(float height_px, const char* sample);

  font_type_t getType() const override { return ft_vlw; }
  void getDefaultMetric(lgfx::FontMetrics* metrics) const override;
  bool updateFontMetric(lgfx::FontMetrics* metrics,
                        uint16_t uniCode) const override;
  size_t drawChar(lgfx::LGFXBase* gfx, int32_t x, int32_t y, uint16_t c,
                  const lgfx::TextStyle* style, lgfx::FontMetrics* metrics,
                  int32_t& filled_x) const override;

  struct Glyph;

 private:
  /** The glyph for `c` at this face's size, rendered now if it is not kept;
   *  nullptr when the face has it itself, or no outline font does. `raised`
   *  is a Thai tone mark over a vowel. */
  const Glyph* outline(uint16_t c, bool raised) const;
  /** Let go of what was rendered from fonts that have changed. */
  void refresh() const;
  /** Draw `g` as VLWfont::drawChar() draws a glyph, and return its
   *  advance. */
  size_t drawGlyph(lgfx::LGFXBase* gfx, int32_t x, int32_t y,
                   const Glyph* g, const lgfx::TextStyle* style,
                   lgfx::FontMetrics* metrics, int32_t& filled_x) const;
  /** The line, the face's or attachAtHeight()'s. */
  int32_t maxAscent() const;

  const lgfx::VLWfont* face_ = nullptr;
  int32_t em_px_ = 0;
  /** With no face: the line, and what a character no font has advances. */
  int32_t max_ascent_ = 0;
  int32_t y_advance_ = 0;
  int32_t space_ = 0;
  /** Each outline font's scale at em_px_, for the fonts as they were at
   *  `generation_`. */
  mutable float* scales_ = nullptr;
  mutable uint32_t generation_ = 0;
  /** Rendered glyphs, in sets of a few, most recently used first. */
  mutable Glyph** kept_ = nullptr;
};

/** The scripts there are outline fonts for, for the log and the settings:
 *  "Korean, Chinese, Japanese, Thai"; empty when there are none. */
void outlineFontsDescribe(char* out, size_t len);
