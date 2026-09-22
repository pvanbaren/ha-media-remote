#pragma once

#include <cstdint>

#include <driver/gpio.h>

/**
 * Everything that is true of this particular piece of hardware, and nothing
 * that is true of the application running on it.
 *
 * Waveshare ESP32-S3-Touch-LCD-1.28: a 1.28" round GC9A01 at 240x240 on SPI,
 * a CST816S touch panel on I2C, a switchable backlight, 16 MB flash and 2 MB
 * PSRAM. The pins are not choices -- they come from the board variant
 * (framework-arduinoespressif32/variants/waveshare_esp32s3_touch_lcd_128/
 * pins_arduino.h), and the touch controller and backlight transistor are
 * soldered down.
 *
 * A different panel means a sibling of this header and a sibling of src/ui/,
 * selected by build_src_filter in platformio.ini. Nothing outside src/ui/ and
 * src/hardware/ includes this file: the services and the app layer are
 * written against no display at all.
 */
/** Counterpart to BOARD_PANEL_RGB: this panel has a command channel, so
 *  hardware/lgfx_config.hpp builds a real LovyanGFX device for it. */
#define BOARD_PANEL_SPI 1

namespace board {

constexpr char kName[] = "Waveshare ESP32-S3-Touch-LCD-1.28";

// --- Panel geometry --------------------------------------------------------
// The panel is round, so one number describes it. Every dimension in
// ui/theme.h is authored against kUiBaseSize and multiplied by kUiScale, so
// this is the only edit needed to move the interface to a larger round panel.
//
//   1.28" GC9A01  -> 240      1.43" / 1.85" GC9A01 -> 240 (same raster)
//   2.1"  CO5300  -> 480      2.8" / 3.4" AMOLED   -> 480 / 800
//
// A larger panel costs frame buffer -- diameter squared, doubled -- which on
// 2 MB of PSRAM leaves room to grow.
constexpr int kDisplayDiameter = 240;
constexpr int kDisplayWidth = kDisplayDiameter;
constexpr int kDisplayHeight = kDisplayDiameter;

/** Layout baseline. All ui/theme.h values are px at this size. */
constexpr int kUiBaseSize = 240;
constexpr float kUiScale = static_cast<float>(kDisplayDiameter) / kUiBaseSize;
/** The panel the text sizes were designed on, so no extra factor. */
constexpr float kTextScale = 1.0f;
/** Nor for the lists. */
constexpr float kListScale = 1.0f;

/** How far the transport row sits above the 240 px design's, in design
 *  pixels. None: this is the panel it was designed on. */
constexpr int kTransportRowLiftPx240 = 0;

/** True when the panel is a circle, so the UI clips text to the chord at its
 *  own y rather than to a bounding box. A square panel sets this false and
 *  ui::theme's chord helpers become the full width. */
constexpr bool kDisplayIsRound = true;

/** The controller has a command channel, so a small repaint can go straight
 *  over SPI without touching the composed frame. */
constexpr bool kPanelWritesDirect = true;

/** LovyanGFX's default RGB565 is byte-swapped, which is what an SPI panel
 *  wants on the wire, and pushImage converts on the way out regardless. */
constexpr bool kFrameNativeByteOrder = false;

// --- Display: GC9A01 on SPI ------------------------------------------------
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_14;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_9;
constexpr gpio_num_t kDisplayPinDc = GPIO_NUM_8;
constexpr gpio_num_t kDisplayPinMosi = GPIO_NUM_11;
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_10;
/** Genuinely switchable, so idle blanking is a real cut rather than a black
 *  frame and kDisplayBrightness is a real dimmer. */
constexpr gpio_num_t kDisplayPinBacklight = GPIO_NUM_2;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
/** GC9A01 modules generally need inversion on. */
constexpr bool kDisplayInvert = true;
/** LovyanGFX rgb_order: true = RGB, false = BGR. This panel is BGR -- with RGB
 *  its red and blue come out swapped. Barely visible on flat UI colour, and
 *  obvious the moment a photograph is put on screen, which is why cover art
 *  found it. Flip this if the panel shows blue where it should show red. */
constexpr bool kDisplayRgbOrder = false;

constexpr uint8_t kDisplayBrightness = 255;
/** Some modules drive BL active-low. Flip if the backlight is inverted. */
constexpr bool kDisplayBacklightInvert = false;
constexpr uint32_t kBacklightPwmHz = 12000;
constexpr uint8_t kBacklightPwmChannel = 7;

// --- Touch: CST816S capacitive controller (I2C) ----------------------------
// The controller is attached to the panel in lgfx_config.hpp and read through
// LovyanGFX's own driver, so hardware/touch.cpp only tracks presses and
// resolves gestures.
constexpr int kTouchI2cPort = 0;
constexpr gpio_num_t kTouchPinSda = GPIO_NUM_6;
constexpr gpio_num_t kTouchPinScl = GPIO_NUM_7;
constexpr gpio_num_t kTouchPinInt = GPIO_NUM_5;
constexpr gpio_num_t kTouchPinRst = GPIO_NUM_13;
constexpr uint8_t kTouchI2cAddress = 0x15;
constexpr uint32_t kTouchI2cHz = 400000;

/** Raster the CST816S reports against. LovyanGFX rescales it to the panel
 *  (Panel_Device::touchCalibrate), so kDisplayDiameter stays the only porting
 *  knob. The 1.28" panels report 0..239 whatever is bonded on top. */
constexpr int kTouchNativeSize = 240;

// Orientation fixes -- flip these if taps land mirrored or transposed.
constexpr bool kTouchSwapXy = false;
constexpr bool kTouchInvertX = false;
constexpr bool kTouchInvertY = false;

/** Press shorter than this is noise, not a tap. */
constexpr unsigned long kTouchTapMinMs = 20;
/** Press longer than this is a hold, not a tap. */
constexpr unsigned long kTouchTapMaxMs = 700;
/** Movement (px at kUiBaseSize) beyond which a press is a drag, not a tap. */
constexpr int kTouchTapSlopPx = 10;
/** Minimum drag (px at kUiBaseSize) before a swipe registers. */
constexpr int kTouchSwipeMinPx = 28;
/** I2C sample interval when no INT pin is wired. */
constexpr unsigned long kTouchPollIntervalMs = 16;

/** Thumbnail edge in px at kUiBaseSize, scaled with the panel like everything
 *  else. Each is a permanent sprite of edge^2 * 2 bytes: 38 px is 2.9 KB. */
constexpr int kThumbPx = 38;

/** Edge in px to ask the image proxy for. Rounded up to the server's ladder
 *  (0/80/160/256/512/1024), so 240 asks for 256: one step above the panel, and
 *  no upscaling. A board whose panel is large enough that matching it would be
 *  costly can ask for less and upscale instead. */
constexpr int kCoverArtRequestPx = kDisplayDiameter;

/** Shortest gap between full repaints of a scrolling list. 240x240 is 115 KB
 *  over SPI and this panel has kept up with the touch report rate since the
 *  beginning, so the ration is the report rate and nothing is rationed. A
 *  panel large enough for a repaint to cost more than a frame needs a real
 *  number here -- see the Qualia. */
constexpr unsigned long kListRedrawMinMs = 16;

/** Cover art is cached compressed, not decoded, so one buffer serves every
 *  repaint. Claimed once at boot and never resized: sizing it per image meant
 *  a free and a differently-sized malloc on every track change, which is how a
 *  long-running heap fragments.
 *
 *  128 KB. This panel is 240 px and so asks the image proxy for 256 px art,
 *  which measures tens of KB; this swallows that with room for a 512 px cover
 *  from a server that ignored the request. Zero disables caching entirely. */
constexpr size_t kCoverArtBufferBytes = 128u * 1024u;

}  // namespace board
