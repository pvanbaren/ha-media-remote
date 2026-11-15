#include "hardware/font_table.h"

extern "C" {
#define VLW_SYM(name) extern const uint8_t name[] asm(#name)
VLW_SYM(_binary_data_ui_font_34_vlw_start);
VLW_SYM(_binary_data_ui_font_38_vlw_start);
VLW_SYM(_binary_data_ui_font_45_vlw_start);
VLW_SYM(_binary_data_ui_font_54_vlw_start);
VLW_SYM(_binary_data_ui_font_26_vlw_start);
VLW_SYM(_binary_data_ui_font_30_vlw_start);
#undef VLW_SYM
}

namespace hw {
namespace {

/** The same four sizes the round build uses, at kUiScale 3.0 and kTextScale
 *  0.75: theme::textPx() turns 15/17/20/24 into 34/38/45/54. Rendered
 *  natively at those heights rather than scaled from another set, because
 *  LovyanGFX's VLW scaler is nearest-neighbour and enlarged text shows it.
 *
 *  The 38 px face measures 39 -- the generator cannot land it exactly --
 *  which is inside the pixel of slack display_font.cpp allows before it
 *  rescales, so it is still drawn at size 1.0.
 *
 *  Then two more for the browse and search lists, which kListScale sets
 *  smaller still: theme::listTextPx() turns 17/20 into 26/30 -- the title
 *  strip and the rows, and the section headers, which kListHeaderTextPx240
 *  sets at 20 too -- the same faces the round 360 px board embeds. No 23:
 *  nothing asks for a size near it since the headers went to 20. They go
 *  last, not in height order,
 *  because display_font.cpp's kDefaultFont is an index -- the third entry, the
 *  20 px body size -- and the search is by measured height anyway. */
const uint8_t* const kFonts[] = {
    _binary_data_ui_font_34_vlw_start,
    _binary_data_ui_font_38_vlw_start,
    _binary_data_ui_font_45_vlw_start,
    _binary_data_ui_font_54_vlw_start,
    _binary_data_ui_font_26_vlw_start,
    _binary_data_ui_font_30_vlw_start,
};

}  // namespace

const uint8_t* const* fontTable(size_t& count) {
  count = sizeof(kFonts) / sizeof(kFonts[0]);
  return kFonts;
}

}  // namespace hw
