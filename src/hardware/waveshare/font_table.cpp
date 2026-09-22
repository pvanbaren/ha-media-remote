#include "hardware/font_table.h"

extern "C" {
#define VLW_SYM(name) extern const uint8_t name[] asm(#name)
VLW_SYM(_binary_data_ui_font_15_vlw_start);
VLW_SYM(_binary_data_ui_font_17_vlw_start);
VLW_SYM(_binary_data_ui_font_20_vlw_start);
VLW_SYM(_binary_data_ui_font_24_vlw_start);
#undef VLW_SYM
}

namespace hw {
namespace {

/** The sizes ui/theme.h asks for at kUiScale 1.0. Ascending, which is what
 *  displayFontApplyHeight()'s nearest-match search assumes. */
const uint8_t* const kFonts[] = {
    _binary_data_ui_font_15_vlw_start,
    _binary_data_ui_font_17_vlw_start,
    _binary_data_ui_font_20_vlw_start,
    _binary_data_ui_font_24_vlw_start,
};

}  // namespace

const uint8_t* const* fontTable(size_t& count) {
  count = sizeof(kFonts) / sizeof(kFonts[0]);
  return kFonts;
}

}  // namespace hw
