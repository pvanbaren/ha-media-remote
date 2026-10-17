#include "hardware/font_table.h"

/**
 * The 4848S040's faces: kUiScale 2.0 and kTextScale 0.75 land theme::textPx()
 * on 23/26/30/36, the 2.8C's; and the lists, at kListScale 2/3 on top, on
 * 15/17/20, the 1.28" board's. Keep in step with platformio.ini's
 * board_build.embed_files and board/guition4848_s3.h.
 */
extern "C" {
#define VLW_SYM(name) extern const uint8_t name[] asm(#name)
VLW_SYM(_binary_data_ui_font_15_vlw_start);
VLW_SYM(_binary_data_ui_font_17_vlw_start);
VLW_SYM(_binary_data_ui_font_20_vlw_start);
VLW_SYM(_binary_data_ui_font_23_vlw_start);
VLW_SYM(_binary_data_ui_font_26_vlw_start);
VLW_SYM(_binary_data_ui_font_30_vlw_start);
VLW_SYM(_binary_data_ui_font_36_vlw_start);
#undef VLW_SYM
}

namespace hw {
namespace {

const uint8_t* const kFonts[] = {
    _binary_data_ui_font_15_vlw_start, _binary_data_ui_font_17_vlw_start,
    _binary_data_ui_font_20_vlw_start, _binary_data_ui_font_23_vlw_start,
    _binary_data_ui_font_26_vlw_start, _binary_data_ui_font_30_vlw_start,
    _binary_data_ui_font_36_vlw_start,
};

}  // namespace

const uint8_t* const* fontTable(size_t& count) {
  count = sizeof(kFonts) / sizeof(kFonts[0]);
  return kFonts;
}

}  // namespace hw
