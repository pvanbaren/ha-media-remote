#include "hardware/font_table.h"

#include <Arduino.h>

// For the clock face 101 px digits, listed below: LovyanGFX copies each
// glyph bitmap onto the drawing task stack (alloca(w * h) in
// VLWfont::drawChar), about 6 KB a digit, and the loop draws them. The
// default 8 KB overflowed on the first one; 16 KB costs 8 KB of internal RAM.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

extern "C" {
#define VLW_SYM(name) extern const uint8_t name[] asm(#name)
VLW_SYM(_binary_data_ui_font_29_vlw_start);
VLW_SYM(_binary_data_ui_font_33_vlw_start);
VLW_SYM(_binary_data_ui_font_39_vlw_start);
VLW_SYM(_binary_data_ui_font_47_vlw_start);
VLW_SYM(_binary_data_clock_font_101_vlw_start);
#undef VLW_SYM
}

namespace hw {
namespace {

/** The sizes ui/theme.h asks for at kUiScale 466/240 and kTextScale 1.0:
 *  textPx() turns the authored 15/17/20/24 into 29/33/39/47. Ascending, which
 *  is what displayFontApplyHeight()'s nearest-match search assumes. */
const uint8_t* const kFonts[] = {
    _binary_data_ui_font_29_vlw_start,
    _binary_data_ui_font_33_vlw_start,
    _binary_data_ui_font_39_vlw_start,
    _binary_data_ui_font_47_vlw_start,
    // The clock face's digits, at theme::kClockTextPx: '0'-'9' and ':' only,
    // which is all the clock asks of it and all any size near it is used for.
    _binary_data_clock_font_101_vlw_start,
};

}  // namespace

const uint8_t* const* fontTable(size_t& count) {
  count = sizeof(kFonts) / sizeof(kFonts[0]);
  return kFonts;
}

}  // namespace hw
