#include "hardware/korean_font.h"

#include <esp_heap_caps.h>

#include <cmath>
#include <cstring>

#include "hardware/inflate.h"

// stb_truetype, private to this file. Its working memory -- the edges of the
// glyph being rendered -- from PSRAM where there is some: internal RAM is
// what the TLS sessions run short of.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_malloc(size, user)                                     \
  ((void)(user), heap_caps_malloc_prefer((size), 2,                  \
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, \
                                         MALLOC_CAP_8BIT))
#define STBTT_free(ptr, user) ((void)(user), heap_caps_free(ptr))
#include "third_party/stb_truetype.h"

// The outline font, where the env embeds it (board_build.embed_files); weak,
// so a board without it links and goes without Korean.
extern const uint8_t kKoreanTtf[] asm("_binary_data_korean_ttf_start")
    __attribute__((weak));
// Or compressed (scripts/deflate_embeds.py), where an env embeds it so, and
// inflated into PSRAM the first time a Korean glyph is wanted.
extern const uint8_t kKoreanTtfPacked[] asm(
    "_binary_data_deflated_korean_ttf_z_start") __attribute__((weak));
extern const uint8_t kKoreanTtfPackedEnd[] asm(
    "_binary_data_deflated_korean_ttf_z_end") __attribute__((weak));

namespace {

/** Rendered glyphs kept per face, in sets of kWays: a glyph can sit in any
 *  way of its set, and a full set gives up its least recently used. At the
 *  largest face, 54 px, a glyph is under 3 KB. */
constexpr size_t kWays = 8;
constexpr size_t kSetBits = 4;
constexpr size_t kSlots = kWays << kSetBits;

/** The set for `c`. Not c modulo the set count: a Hangul syllable is
 *  0xAC00 + (initial x 21 + vowel) x 28 + final, so every syllable without a
 *  final consonant -- many of a name's -- is a multiple of 28 from the
 *  next, and modulo a power of two those share a quarter of the sets. A
 *  multiplicative hash spreads them over all of them. */
size_t setOf(uint16_t c) {
  return (static_cast<uint32_t>(c) * 0x9E3779B1u) >> (32 - kSetBits);
}

/** Where the glyph's VLW record keeps its 24-byte header and its bitmap:
 *  VLWfont::drawChar() reads the header at 28 + 28 x its index, here 0. */
constexpr uint32_t kHeaderAt = 28;
constexpr uint32_t kBitmapAt = kHeaderAt + 24;

stbtt_fontinfo s_info;
bool s_tried = false;
bool s_ready = false;

bool ready() {
  if (!s_tried) {
    s_tried = true;
    const uint8_t* ttf = kKoreanTtf;
    if (ttf == nullptr && kKoreanTtfPacked != nullptr) {
      ttf = hw::inflateEmbedded(kKoreanTtfPacked, kKoreanTtfPackedEnd);
    }
    if (ttf != nullptr) {
      const int offset = stbtt_GetFontOffsetForIndex(ttf, 0);
      s_ready = offset >= 0 && stbtt_InitFont(&s_info, ttf, offset) != 0;
    }
  }
  return s_ready;
}

void* psram(size_t bytes) {
  void* mem = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return mem != nullptr ? mem : heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

void put32(uint8_t* at, int32_t value) {
  const uint32_t v = static_cast<uint32_t>(value);
  at[0] = static_cast<uint8_t>(v >> 24);
  at[1] = static_cast<uint8_t>(v >> 16);
  at[2] = static_cast<uint8_t>(v >> 8);
  at[3] = static_cast<uint8_t>(v);
}

int32_t get32(const uint8_t* at) {
  return static_cast<int32_t>(static_cast<uint32_t>(at[0]) << 24 |
                              static_cast<uint32_t>(at[1]) << 16 |
                              static_cast<uint32_t>(at[2]) << 8 | at[3]);
}

/** A VLW face of one glyph, the one being drawn: its record is a rendered
 *  glyph's, and its line the real face's, so VLWfont::drawChar() puts it on
 *  the baseline with the letters around it. Its tables are its own arrays,
 *  and given back before ~VLWfont() could free them. */
struct OneGlyphFace : lgfx::VLWfont {
  uint16_t unicode[1] = {};
  uint32_t bitmap[1] = {kBitmapAt};
  lgfx::PointerWrapper record;

  OneGlyphFace() {
    gCount = 1;
    gUnicode = unicode;
    gBitmap = bitmap;
    _fontData = &record;
    _fontLoaded = true;
  }
  ~OneGlyphFace() override {
    gUnicode = nullptr;
    gBitmap = nullptr;
    _fontData = nullptr;
  }
};

}  // namespace

struct KoreanFont::Glyph {
  uint16_t code;
  int16_t width;
  int16_t advance;
  int16_t offset;
  /** The VLW record: kHeaderAt bytes unused, the header, the bitmap. */
  uint8_t* record;
};

void KoreanFont::attach(const lgfx::VLWfont* face, const uint8_t* face_data) {
  face_ = face;
  scale_ = 0.0f;
  if (face == nullptr || face_data == nullptr || !ready()) {
    return;
  }
  // The VLW header's third word is the em the face was rendered at, in
  // pixels (scripts/build_ui_font.py): the Korean is rendered at the same.
  const int32_t em_px = get32(face_data + 8);
  if (em_px > 0) {
    scale_ = stbtt_ScaleForMappingEmToPixels(&s_info,
                                             static_cast<float>(em_px));
  }
}

const KoreanFont::Glyph* KoreanFont::korean(uint16_t c) const {
  if (scale_ <= 0.0f || c < 0x80) {
    return nullptr;
  }
  uint16_t index = 0;
  if (face_->gCount > 0 && face_->getUnicodeIndex(c, &index)) {
    return nullptr;  // the face's own
  }
  if (kept_ == nullptr) {
    kept_ = static_cast<Glyph**>(psram(sizeof(Glyph*) * kSlots));
    if (kept_ == nullptr) {
      return nullptr;
    }
    memset(kept_, 0, sizeof(Glyph*) * kSlots);
  }
  // Most recently used first.
  Glyph** set = kept_ + setOf(c) * kWays;
  for (size_t way = 0; way < kWays; ++way) {
    Glyph* kept = set[way];
    if (kept != nullptr && kept->code == c) {
      memmove(set + 1, set, way * sizeof(Glyph*));
      set[0] = kept;
      return kept;
    }
  }

  const int glyph = stbtt_FindGlyphIndex(&s_info, c);
  if (glyph == 0) {
    return nullptr;  // not Korean either: the face's blank
  }
  int advance = 0;
  int bearing = 0;
  stbtt_GetGlyphHMetrics(&s_info, glyph, &advance, &bearing);
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  stbtt_GetGlyphBitmapBox(&s_info, glyph, scale_, scale_, &x0, &y0, &x1, &y1);
  const int w = x1 > x0 ? x1 - x0 : 0;
  const int h = y1 > y0 ? y1 - y0 : 0;

  auto* fresh = static_cast<Glyph*>(psram(sizeof(Glyph) + kBitmapAt + w * h));
  if (fresh == nullptr) {
    return nullptr;
  }
  fresh->code = c;
  fresh->width = static_cast<int16_t>(w);
  fresh->advance = static_cast<int16_t>(lroundf(advance * scale_));
  fresh->offset = static_cast<int16_t>(x0);
  fresh->record = reinterpret_cast<uint8_t*>(fresh + 1);
  uint8_t* header = fresh->record + kHeaderAt;
  put32(header, h);
  put32(header + 4, w);
  put32(header + 8, fresh->advance);
  put32(header + 12, -y0);  // top above the baseline
  put32(header + 16, x0);
  put32(header + 20, 0);
  if (w > 0 && h > 0) {
    stbtt_MakeGlyphBitmap(&s_info, fresh->record + kBitmapAt, w, h, w, scale_,
                          scale_, glyph);
  }

  heap_caps_free(set[kWays - 1]);
  memmove(set + 1, set, (kWays - 1) * sizeof(Glyph*));
  set[0] = fresh;
  return fresh;
}

void KoreanFont::getDefaultMetric(lgfx::FontMetrics* metrics) const {
  face_->getDefaultMetric(metrics);
}

bool KoreanFont::updateFontMetric(lgfx::FontMetrics* metrics,
                                  uint16_t uniCode) const {
  const Glyph* g = korean(uniCode);
  if (g == nullptr) {
    return face_->updateFontMetric(metrics, uniCode);
  }
  metrics->width = g->width;
  metrics->x_advance = g->advance;
  metrics->x_offset = g->offset;
  return true;
}

size_t KoreanFont::drawChar(lgfx::LGFXBase* gfx, int32_t x, int32_t y,
                            uint16_t c, const lgfx::TextStyle* style,
                            lgfx::FontMetrics* metrics,
                            int32_t& filled_x) const {
  const Glyph* g = korean(c);
  if (g == nullptr) {
    return face_->drawChar(gfx, x, y, c, style, metrics, filled_x);
  }
  static OneGlyphFace one;
  one.unicode[0] = c;
  one.record.set(g->record);
  one.yAdvance = face_->yAdvance;
  one.spaceWidth = face_->spaceWidth;
  one.ascent = face_->ascent;
  one.descent = face_->descent;
  one.maxAscent = face_->maxAscent;
  one.maxDescent = face_->maxDescent;
  return one.drawChar(gfx, x, y, c, style, metrics, filled_x);
}

bool koreanFontEmbedded() { return ready(); }
