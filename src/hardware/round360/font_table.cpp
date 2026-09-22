#include "hardware/font_table.h"

extern "C" {
#define VLW_SYM(name) extern const uint8_t name[] asm(#name)
VLW_SYM(_binary_data_ui_font_23_vlw_start);
VLW_SYM(_binary_data_ui_font_26_vlw_start);
VLW_SYM(_binary_data_ui_font_30_vlw_start);
VLW_SYM(_binary_data_ui_font_36_vlw_start);
#undef VLW_SYM
}

namespace hw {
namespace {

/** The sizes ui/theme.h asks for at kUiScale 1.5: px() turns the authored
 *  15/17/20/24 into 23/26/30/36. Ascending, which is what
 *  displayFontApplyHeight()'s nearest-match search assumes. */
const uint8_t* const kFonts[] = {
    _binary_data_ui_font_23_vlw_start,
    _binary_data_ui_font_26_vlw_start,
    _binary_data_ui_font_30_vlw_start,
    _binary_data_ui_font_36_vlw_start,
};

}  // namespace

const uint8_t* const* fontTable(size_t& count) {
  count = sizeof(kFonts) / sizeof(kFonts[0]);
  return kFonts;
}

}  // namespace hw
