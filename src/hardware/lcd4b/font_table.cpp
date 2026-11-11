#include "hardware/font_table.h"

#include <Arduino.h>

#include "hardware/inflate.h"
#include "log.h"

// Embedded compressed (scripts/deflate_embeds.py), and inflated into PSRAM
// on first use: the seven faces are 1.10 MB as they are and 0.23 MB
// compressed, and in a 4 MB app slot that is the difference between an image
// that fits comfortably and one that barely does.
extern "C" {
#define VLW_SYM(name)                                          \
  extern const uint8_t name##_start[] asm("_binary_" #name "_start"); \
  extern const uint8_t name##_end[] asm("_binary_" #name "_end")
VLW_SYM(data_deflated_ui_font_34_vlw_z);
VLW_SYM(data_deflated_ui_font_38_vlw_z);
VLW_SYM(data_deflated_ui_font_45_vlw_z);
VLW_SYM(data_deflated_ui_font_54_vlw_z);
VLW_SYM(data_deflated_ui_font_23_vlw_z);
VLW_SYM(data_deflated_ui_font_26_vlw_z);
VLW_SYM(data_deflated_ui_font_30_vlw_z);
#undef VLW_SYM
}

namespace hw {
namespace {

struct Packed {
  const uint8_t* start;
  const uint8_t* end;
};

#define VLW(name) {name##_start, name##_end}

/** The same four sizes the round build uses, at kUiScale 3.0 and kTextScale
 *  0.75: theme::textPx() turns 15/17/20/24 into 34/38/45/54. Rendered
 *  natively at those heights rather than scaled from another set, because
 *  LovyanGFX's VLW scaler is nearest-neighbour and enlarged text shows it.
 *
 *  The 38 px face measures 39, and the 23 px list face 24 -- the generator
 *  cannot land those exactly -- which is inside the pixel of slack
 *  display_font.cpp allows before it rescales, so both are still drawn at
 *  size 1.0.
 *
 *  Then three more for the browse and search lists, which kListScale sets
 *  smaller still: theme::listTextPx() turns 15/17/20 into 23/26/30, the same
 *  faces the round 360 px board embeds. They go last, not in height order,
 *  because display_font.cpp's kDefaultFont is an index -- the third entry, the
 *  20 px body size -- and the search is by measured height anyway. */
const Packed kPacked[] = {
    VLW(data_deflated_ui_font_34_vlw_z),
    VLW(data_deflated_ui_font_38_vlw_z),
    VLW(data_deflated_ui_font_45_vlw_z),
    VLW(data_deflated_ui_font_54_vlw_z),
    VLW(data_deflated_ui_font_23_vlw_z),
    VLW(data_deflated_ui_font_26_vlw_z),
    VLW(data_deflated_ui_font_30_vlw_z),
};
#undef VLW

constexpr size_t kCount = sizeof(kPacked) / sizeof(kPacked[0]);
const uint8_t* s_fonts[kCount] = {};
size_t s_count = 0;

}  // namespace

const uint8_t* const* fontTable(size_t& count) {
  if (s_count == 0) {
    const unsigned long started = millis();
    size_t total = 0;
    // In order, and no further than the first that will not inflate:
    // display_font.cpp takes the faces by index.
    while (s_count < kCount) {
      size_t len = 0;
      const uint8_t* face = inflateEmbedded(kPacked[s_count].start,
                                            kPacked[s_count].end, &len);
      if (face == nullptr) {
        LOG_WARN("Fonts: face %u would not inflate",
                 static_cast<unsigned>(s_count));
        break;
      }
      s_fonts[s_count++] = face;
      total += len;
    }
    LOG_INFO("Fonts: %u faces inflated, %u KB, in %lu ms",
             static_cast<unsigned>(s_count),
             static_cast<unsigned>(total / 1024), millis() - started);
  }
  count = s_count;
  return s_fonts;
}

}  // namespace hw
