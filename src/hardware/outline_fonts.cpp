#include "hardware/outline_fonts.h"

#include <esp_heap_caps.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "hardware/font_pack.h"

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

/** How much higher a Thai tone mark goes over a vowel mark, in ems: from
 *  where Noto Sans Thai puts it, over the letter, to clear of the vowel. */
constexpr float kThaiToneLiftEm = 0.17f;

bool thaiUpperVowel(uint16_t c) {
  return c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E37) || c == 0x0E47;
}

bool thaiToneMark(uint16_t c) { return c >= 0x0E48 && c <= 0x0E4C; }

void* psram(size_t bytes) {
  void* mem = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return mem != nullptr ? mem : heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

// --- The outline fonts ----------------------------------------------------------

constexpr int kMaxSources = hw::font_pack::kMaxFonts + 1;

struct Source {
  stbtt_fontinfo info;
  const char* name;
};
/** The outline fonts, in the order to look in. */
Source* s_sources = nullptr;
int s_source_count = 0;
/** The pack's generation they were loaded from; 0, never loaded. */
uint32_t s_sources_of = 0;

bool addSource(const uint8_t* ttf, const char* name) {
  if (s_source_count >= kMaxSources) {
    return false;
  }
  Source& source = s_sources[s_source_count];
  const int offset = stbtt_GetFontOffsetForIndex(ttf, 0);
  if (offset < 0 || stbtt_InitFont(&source.info, ttf, offset) == 0) {
    return false;
  }
  source.name = name;
  ++s_source_count;
  return true;
}

/** The outline fonts, loaded again whenever the pack has changed. */
int sources() {
  const uint32_t generation = hw::font_pack::generation();
  if (generation == s_sources_of) {
    return s_source_count;
  }
  s_sources_of = generation;
  s_source_count = 0;
  if (s_sources == nullptr) {
    s_sources = static_cast<Source*>(psram(sizeof(Source) * kMaxSources));
    if (s_sources == nullptr) {
      return 0;
    }
  }
  if (const hw::font_pack::Info* pack = hw::font_pack::info()) {
    for (int i = 0; i < pack->count; ++i) {
      const hw::font_pack::Font& font = pack->fonts[i];
      addSource(font.data, font.name);
    }
  }
  return s_source_count;
}

// --- Drawing ----------------------------------------------------------------------

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

/** The character drawn last, of any face: what a Thai mark goes over. */
uint16_t s_drawn_before = 0;

}  // namespace

struct OutlineFallback::Glyph {
  uint16_t code;
  bool raised;
  int16_t width;
  int16_t advance;
  int16_t offset;
  /** The VLW record: kHeaderAt bytes unused, the header, the bitmap. */
  uint8_t* record;
};

void OutlineFallback::attach(const lgfx::VLWfont* face,
                             const uint8_t* face_data) {
  face_ = face;
  em_px_ = 0;
  generation_ = 0;
  if (face == nullptr || face_data == nullptr) {
    return;
  }
  // The VLW header's third word is the em the face was rendered at, in
  // pixels (scripts/build_ui_font.py): the outlines are rendered at the same.
  em_px_ = get32(face_data + 8);
}

void OutlineFallback::refresh() const {
  const int count = sources();
  if (generation_ == s_sources_of) {
    return;
  }
  generation_ = s_sources_of;
  if (kept_ != nullptr) {
    for (size_t i = 0; i < kSlots; ++i) {
      heap_caps_free(kept_[i]);
      kept_[i] = nullptr;
    }
  }
  if (scales_ == nullptr) {
    scales_ = static_cast<float*>(psram(sizeof(float) * kMaxSources));
    if (scales_ == nullptr) {
      return;
    }
  }
  for (int i = 0; i < count; ++i) {
    scales_[i] = stbtt_ScaleForMappingEmToPixels(&s_sources[i].info,
                                                 static_cast<float>(em_px_));
  }
}

const OutlineFallback::Glyph* OutlineFallback::outline(uint16_t c,
                                                       bool raised) const {
  if (em_px_ <= 0 || c < 0x80) {
    return nullptr;
  }
  uint16_t index = 0;
  if (face_->gCount > 0 && face_->getUnicodeIndex(c, &index)) {
    return nullptr;  // the face's own
  }
  refresh();
  if (s_source_count == 0 || scales_ == nullptr) {
    return nullptr;
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
    if (kept != nullptr && kept->code == c && kept->raised == raised) {
      memmove(set + 1, set, way * sizeof(Glyph*));
      set[0] = kept;
      return kept;
    }
  }

  int glyph = 0;
  int from = 0;
  for (; from < s_source_count; ++from) {
    glyph = stbtt_FindGlyphIndex(&s_sources[from].info, c);
    if (glyph != 0) {
      break;
    }
  }
  if (glyph == 0) {
    return nullptr;  // in no outline font either: the face's blank
  }
  const stbtt_fontinfo* info = &s_sources[from].info;
  const float scale = scales_[from];
  int advance = 0;
  int bearing = 0;
  stbtt_GetGlyphHMetrics(info, glyph, &advance, &bearing);
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  stbtt_GetGlyphBitmapBox(info, glyph, scale, scale, &x0, &y0, &x1, &y1);
  const int w = x1 > x0 ? x1 - x0 : 0;
  const int h = y1 > y0 ? y1 - y0 : 0;
  // A Thai mark sits over the letter before it, to the left of where it is
  // drawn; VLW keeps that offset in a signed byte.
  if (x0 < -128) {
    return nullptr;
  }
  const int lift =
      raised ? static_cast<int>(lroundf(kThaiToneLiftEm * em_px_)) : 0;

  auto* fresh = static_cast<Glyph*>(psram(sizeof(Glyph) + kBitmapAt + w * h));
  if (fresh == nullptr) {
    return nullptr;
  }
  fresh->code = c;
  fresh->raised = raised;
  fresh->width = static_cast<int16_t>(w);
  fresh->advance = static_cast<int16_t>(lroundf(advance * scale));
  fresh->offset = static_cast<int16_t>(x0);
  fresh->record = reinterpret_cast<uint8_t*>(fresh + 1);
  uint8_t* header = fresh->record + kHeaderAt;
  put32(header, h);
  put32(header + 4, w);
  put32(header + 8, fresh->advance);
  put32(header + 12, lift - y0);  // top above the baseline
  put32(header + 16, x0);
  put32(header + 20, 0);
  if (w > 0 && h > 0) {
    stbtt_MakeGlyphBitmap(info, fresh->record + kBitmapAt, w, h, w, scale,
                          scale, glyph);
  }

  heap_caps_free(set[kWays - 1]);
  memmove(set + 1, set, (kWays - 1) * sizeof(Glyph*));
  set[0] = fresh;
  return fresh;
}

void OutlineFallback::getDefaultMetric(lgfx::FontMetrics* metrics) const {
  face_->getDefaultMetric(metrics);
}

bool OutlineFallback::updateFontMetric(lgfx::FontMetrics* metrics,
                                       uint16_t uniCode) const {
  const Glyph* g = outline(uniCode, false);
  if (g == nullptr) {
    return face_->updateFontMetric(metrics, uniCode);
  }
  metrics->width = g->width;
  metrics->x_advance = g->advance;
  metrics->x_offset = g->offset;
  return true;
}

size_t OutlineFallback::drawChar(lgfx::LGFXBase* gfx, int32_t x, int32_t y,
                                 uint16_t c, const lgfx::TextStyle* style,
                                 lgfx::FontMetrics* metrics,
                                 int32_t& filled_x) const {
  const bool raised = thaiToneMark(c) && thaiUpperVowel(s_drawn_before);
  s_drawn_before = c;
  const Glyph* g = outline(c, raised);
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

void outlineFontsDescribe(char* out, size_t len) {
  if (len == 0) {
    return;
  }
  out[0] = '\0';
  const int count = sources();
  size_t used = 0;
  for (int i = 0; i < count && used < len; ++i) {
    const int n = snprintf(out + used, len - used, "%s%s", i ? ", " : "",
                           s_sources[i].name);
    used += n > 0 ? static_cast<size_t>(n) : 0;
  }
}
