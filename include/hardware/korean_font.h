#pragma once

#include <LovyanGFX.hpp>

/**
 * Korean in the UI's text: one of the VLW faces, with Hangul behind it.
 *
 * The VLW faces are Latin -- 369 glyphs each -- and Korean is 2,350 common
 * syllables, which pre-rendered at every size would not fit in an app slot.
 * So a character the face lacks is drawn from an outline font instead,
 * data/korean.ttf (Noto Sans KR, the KS X 1001 syllables and jamo; see
 * scripts/build_korean_font.py), rendered by stb_truetype at the face's own
 * em size the first time it is wanted and kept. Everything else goes to the
 * face as before.
 *
 * The rendered glyph is drawn by the VLW code itself, through a face holding
 * just that glyph, so it blends, clips and fills its background exactly as
 * the letters around it do. A board that does not embed the outline font
 * draws Korean as it always did, as blanks.
 *
 * Drawn from the loop only, like the faces themselves.
 */
class KoreanFont : public lgfx::IFont {
 public:
  /** Put Korean behind `face`, whose em -- read from its header -- the
   *  Korean is rendered at. `face_data` is its VLW blob. */
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
   *  nullptr when the face has it itself, or the outline font does not. */
  const Glyph* korean(uint16_t c) const;

  const lgfx::VLWfont* face_ = nullptr;
  float scale_ = 0.0f;
  /** Rendered glyphs, in sets of a few, most recently used first. */
  mutable Glyph** kept_ = nullptr;
};

/** Whether this build embeds the outline font, for the log. */
bool koreanFontEmbedded();
