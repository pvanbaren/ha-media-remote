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
 * The rendered glyph is drawn by the VLW code itself, through a face holding
 * just that glyph, so it blends, clips and fills its background exactly as
 * the letters around it do. With no outline font, those characters are
 * blanks, as they always were.
 *
 * Drawn from the loop only, like the faces themselves.
 */
class OutlineFallback : public lgfx::IFont {
 public:
  /** Put the outline fonts behind `face`, whose em -- read from its header
   *  -- they are rendered at. `face_data` is its VLW blob. */
  void attach(const lgfx::VLWfont* face, const uint8_t* face_data);

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

  const lgfx::VLWfont* face_ = nullptr;
  int32_t em_px_ = 0;
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
