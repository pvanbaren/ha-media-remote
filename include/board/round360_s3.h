#pragma once

#include <cstdint>

#include <driver/gpio.h>

/**
 * ESP32-S3 with a 1.85" round 360x360 ST77916, driven over quad SPI.
 *
 * The board is a DFRobot DFR1221, "ESP32-S3R8 1.85-inch Round Display Board":
 * ESP32-S3R8, 16 MB flash, 8 MB PSRAM, and on the same module an SD slot and
 * I2S audio in and out that this firmware does not use.
 *
 * Still named for the panel rather than the vendor, because the module was
 * first taken for a Waveshare ESP32-S3-Touch-LCD-1.85 and the two are easy to
 * confuse -- same size, same shape, same controller, different wiring. The
 * Waveshare puts the panel and touch resets behind a TCA9554 expander, uses
 * GPIO 40 and 41 as LCD clock and data, and the backlight on GPIO 5. This one
 * drives all of it directly and has no expander. A header named for a vendor
 * it does not belong to is worse than one named for the glass.
 *
 * Provenance: every pin below is DFRobot's own pincfg.h, from the ST77916
 * LVGL demo in their library bundle for this SKU, cross-checked against
 * LovyanGFX's config for the Guition JC3636W518C (src/lgfx_user/
 * LGFX_JC3636W518.hpp) -- the board its ST77916 driver was written against,
 * which agrees on every LCD pin. Nothing here is inferred.
 *
 * Two places this deliberately departs from the vendor demo. It clocks the
 * panel at 40 MHz rather than their 50, which is LovyanGFX's figure for this
 * controller and leaves margin on a bus shared with nothing; and it drives the
 * panel unrotated, which is why the touch transform below is the identity --
 * see the note there.
 */
/** Counterpart to BOARD_PANEL_SPI and BOARD_PANEL_RGB. Quad SPI carries the
 *  command/data distinction inside the transaction rather than on a DC wire,
 *  which is a different LovyanGFX bus setup rather than a variation on the
 *  three-wire one, so lgfx_config.hpp gives it its own branch. */
#define BOARD_PANEL_QSPI 1

namespace board {

constexpr char kName[] = "ESP32-S3 + 1.85\" round 360x360 ST77916";

// --- Panel geometry --------------------------------------------------------
// Round, so one number describes it, and ui/theme.h is authored against
// kUiBaseSize and scaled -- see kUiScale.
constexpr int kDisplayDiameter = 360;
constexpr int kDisplayWidth = kDisplayDiameter;
constexpr int kDisplayHeight = kDisplayDiameter;

/** Layout baseline. All ui/theme.h values are px at this size. */
constexpr int kUiBaseSize = 240;
/** 1.5 here, which is why this board embeds 23/26/30/36 px faces rather than
 *  the 15/17/20/24 the 240 px panel uses: theme::px() rounds 15 to 23, 17 to
 *  26, 20 to 30 and 24 to 36. Keep those three lists in step -- the heights
 *  here, platformio.ini's board_build.embed_files, and font_table.cpp. */
constexpr float kUiScale = static_cast<float>(kDisplayDiameter) / kUiBaseSize;
/** Text scales with the layout here: no extra factor. */
constexpr float kTextScale = 1.0f;
/** Lists scale with the layout too. */
constexpr float kListScale = 1.0f;

/** Section headers in the browse list, at the 240 px design size. A step
 *  below the row titles here, where three or four rows fill the glass and a
 *  header the same size as its rows would crowd them. */
constexpr int kListHeaderTextPx240 = 15;

/** How far the transport row sits above the 240 px design's, in design
 *  pixels. Scaled by 1.5 the play button's lower edge and the elapsed chip's
 *  upper one land on the same row of pixels -- the 23 px face is a pixel
 *  taller for its size than the 15 px one -- so here the row goes up by 8,
 *  which leaves the chip about 8 px clear and the title above about 11. */
constexpr int kTransportRowLiftPx240 = 5;

/** A circle, so the UI clips text to the chord at its own y. */
constexpr bool kDisplayIsRound = true;

/** The controller has a command channel, so a small repaint can go straight
 *  down the bus without touching the composed frame. */
constexpr bool kPanelWritesDirect = true;

/** LovyanGFX's default RGB565 is byte-swapped, which is what a panel on a bus
 *  wants on the wire, and pushImage converts on the way out regardless. */
constexpr bool kFrameNativeByteOrder = false;

// --- Display: ST77916 on quad SPI ------------------------------------------
// Four data lines and no MOSI, MISO or DC. LovyanGFX folds this into Bus_SPI
// behind LGFX_USE_QSPI, and defines that for itself on an ESP32-S3 with IDF
// 4.4 or newer (platforms/esp32/common.hpp), so the env sets no flag for it.
// Worth knowing rather than assuming: where that guard does not hold -- the
// P4 is excluded there today -- pin_io0..io3 are ignored, the panel comes up
// as plain SPI, and it stays blank without reporting anything wrong.
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_9;
constexpr gpio_num_t kDisplayPinIo0 = GPIO_NUM_11;
constexpr gpio_num_t kDisplayPinIo1 = GPIO_NUM_12;
constexpr gpio_num_t kDisplayPinIo2 = GPIO_NUM_13;
constexpr gpio_num_t kDisplayPinIo3 = GPIO_NUM_14;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_10;
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_47;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
/** The panel's init sequence sends INVON, so this flips it back. */
constexpr bool kDisplayInvert = true;
/** LovyanGFX rgb_order: true = RGB, false = BGR. The vendor config says RGB,
 *  but its own comment is equivocal about it -- flip this if the panel shows
 *  blue where it should show red. Flat UI colour barely shows the difference;
 *  a photograph makes it obvious, which is how cover art found it on the
 *  Waveshare. */
constexpr bool kDisplayRgbOrder = true;

// --- Backlight -------------------------------------------------------------
/** PWM-driven, so the panel stays dark until something drives it, and idle
 *  blanking is a real cut rather than a black frame. */
constexpr gpio_num_t kDisplayPinBacklight = GPIO_NUM_15;
constexpr uint8_t kDisplayBrightness = 255;
constexpr bool kDisplayBacklightInvert = false;
constexpr uint32_t kBacklightPwmHz = 5000;
/** Channel 0 rather than the Waveshare's 7; nothing else here claims one. */
constexpr uint8_t kBacklightPwmChannel = 0;

// --- Touch: CST816S capacitive controller (I2C) ----------------------------
// The controller is attached to the panel in lgfx_config.hpp and read through
// LovyanGFX's own driver, so hardware/round360/touch_raw.cpp only tracks
// presses and resolves gestures.
//
// SCL 8 and SDA 7 are the pins the vendor demo builds its I2C bus from, which
// is worth saying because they are not adjacent to anything else here and
// nothing about the board suggests them: GPIO 1..6 are the SD slot and 16..18
// the audio, so this pair is the only free one in that range.
constexpr int kTouchI2cPort = 0;
constexpr gpio_num_t kTouchPinSda = GPIO_NUM_7;
constexpr gpio_num_t kTouchPinScl = GPIO_NUM_8;
constexpr gpio_num_t kTouchPinInt = GPIO_NUM_41;
constexpr gpio_num_t kTouchPinRst = GPIO_NUM_40;
constexpr uint8_t kTouchI2cAddress = 0x15;
constexpr uint32_t kTouchI2cHz = 400000;

/** Raster the CST816S reports against, which on this panel is the panel's own
 *  -- the vendor constructs its touch driver with 360 x 360 rather than the
 *  240-ish raster the smaller CST816S panels report. LovyanGFX rescales it
 *  either way (Panel_Device::touchCalibrate). */
constexpr int kTouchNativeSize = 360;

// Orientation. The identity, and that is measured rather than hoped for: the
// vendor's setRotation() applies swapXY, mirrorX and mirrorY to the panel and
// the touch controller together, and its rotation-0 case sets all six to
// false. displayInit() drives this panel unrotated, so there is nothing to
// undo. Flip these if taps land mirrored or transposed.
constexpr bool kTouchSwapXy = false;
constexpr bool kTouchInvertX = false;
constexpr bool kTouchInvertY = false;

// Gesture timings rather than pins: shared by every board through
// hardware/touch_gestures.cpp. These are the Waveshare's values, which are a
// property of how people touch a small round panel rather than of the panel.
/** Press shorter than this is noise, not a tap. */
constexpr unsigned long kTouchTapMinMs = 20;
/** Press longer than this is a hold, not a tap. */
constexpr unsigned long kTouchTapMaxMs = 700;
/** Movement (px at kUiBaseSize) beyond which a press is a drag, not a tap. */
constexpr int kTouchTapSlopPx = 10;
/** Minimum drag (px at kUiBaseSize) before a swipe registers. */
constexpr int kTouchSwipeMinPx = 28;
/** I2C sample interval. The INT pin is wired, on GPIO 41, but the driver polls
 *  rather than waiting on an edge -- same as the other boards. */
constexpr unsigned long kTouchPollIntervalMs = 16;
/** Sample the touch controller on a task of its own rather than from the
 *  loop. Off here: touch is read through
 *  LovyanGFX's device object, which the loop is drawing through at the same
 *  time, and nothing has shown that to be safe from a second task. The loop
 *  samples instead, into the same queue.  */
constexpr bool kTouchSampleTask = false;

// --- Artwork ---------------------------------------------------------------
/** Thumbnail edge in px at kUiBaseSize: 38 becomes 57 here. Each is a
 *  permanent sprite of edge^2 * 2 bytes, so about 6.5 KB apiece. */
constexpr int kThumbPx = 38;

/** Edge in px to ask the image proxy for. Rounded up to the server's ladder
 *  (0/80/160/256/512/1024), so 360 asks for 512: one step above the panel, and
 *  no upscaling. */
constexpr int kCoverArtRequestPx = kDisplayWidth;

/** Cover art is cached compressed, not decoded, so one buffer serves every
 *  repaint, claimed once at boot and never resized. 512 px art runs to rather
 *  more than the 256 px the small panel fetches, so this is double the
 *  Waveshare's buffer. Art larger than this streams and decodes from the
 *  socket instead, at the cost of a re-fetch on every repaint. */
constexpr size_t kCoverArtBufferBytes = 256u * 1024u;

/** Shortest gap between full repaints of a scrolling list. A 360x360 frame is
 *  259,200 bytes, which is about 13 ms to push at 40 MHz across four lanes --
 *  quicker than the 240 px panel manages over its single lane, and well inside
 *  the touch report rate, so nothing needs rationing here. Contrast the
 *  Qualia, where a repaint costs more than a frame. */
constexpr unsigned long kListRedrawMinMs = 16;

/** No stand-off while composing: this panel is written over a bus rather
 *  than scanned out of PSRAM, so there is no continuous read to starve.
 *  See the Qualia's header for what this is for. */
constexpr int kComposeSlabRows = 0;

// --- Not used here ---------------------------------------------------------
// The module also carries an SD slot on GPIO 1..6, I2S audio out on 16/17/18
// with mute on 48, and an I2S microphone on 42/45/46. None of it is wired up
// -- listed so that nobody reaches for one of those pins thinking it is spare.

}  // namespace board
