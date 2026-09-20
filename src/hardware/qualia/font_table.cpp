#include "hardware/font_table.h"

extern "C" {
#define VLW_SYM(name) extern const uint8_t name[] asm(#name)
VLW_SYM(_binary_data_ui_font_34_vlw_start);
VLW_SYM(_binary_data_ui_font_38_vlw_start);
VLW_SYM(_binary_data_ui_font_45_vlw_start);
VLW_SYM(_binary_data_ui_font_54_vlw_start);
VLW_SYM(_binary_data_ui_font_23_vlw_start);
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
 *  The 34 px face measures 35 -- the generator cannot land that one exactly --
 *  which is inside the pixel of slack display_font.cpp allows before it
 *  rescales, so it is still drawn at size 1.0.
 *
 *  Then three more for the browse and search lists, which kListScale sets
 *  smaller still: theme::listTextPx() turns 15/17/20 into 23/26/30, the same
 *  faces the round 360 px board embeds. They go last, not in height order,
 *  because display_font.cpp's kDefaultFont is an index -- the third entry, the
 *  20 px body size -- and the search is by measured height anyway. */
const uint8_t* const kFonts[] = {
    _binary_data_ui_font_34_vlw_start,
    _binary_data_ui_font_38_vlw_start,
    _binary_data_ui_font_45_vlw_start,
    _binary_data_ui_font_54_vlw_start,
    _binary_data_ui_font_23_vlw_start,
    _binary_data_ui_font_26_vlw_start,
    _binary_data_ui_font_30_vlw_start,
};

}  // namespace

const uint8_t* const* fontTable(size_t& count) {
  count = sizeof(kFonts) / sizeof(kFonts[0]);
  return kFonts;
}

}  // namespace hw
