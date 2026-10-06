#pragma once

#include <cstdint>

#include <driver/gpio.h>

/**
 * Waveshare ESP32-S3-Touch-LCD-2.8C: a 2.8" round 480x480 ST7701 on the S3's
 * RGB-parallel LCD peripheral, GT911 touch, ESP32-S3R8 (8 MB octal PSRAM)
 * and 16 MB flash.
 *
 * The same kind of panel as the Qualia's -- pixels streamed out of a
 * framebuffer over 16 data lines, no frame memory of its own -- but round,
 * so it takes the round screens and the shared RGB output stage
 * (hardware/rgb_panel.h). Unlike the Qualia's square panel this ST7701 needs
 * its registers programmed before it shows anything: a 3-wire SPI on GPIO 1
 * and 2, chip-selected through a TCA9554 expander, which also holds the
 * panel's and the touch controller's resets and the buzzer
 * (hardware/lcd28c.h).
 *
 * Provenance: every pin, timing and init byte below is Waveshare's, from the
 * board's wiki and its ESP-IDF demo (main/LCD_Driver/ST7701S.c), and agrees
 * with Espressif's own config for this board in ESP32_Display_Panel
 * (BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_2_8_C.h). The pixel clock is the one
 * departure; see kRgbPclkHz.
 */
/** How the panel is driven, for hardware/lgfx_config.hpp: a framebuffer, so
 *  LovyanGFX only rasterises. See qualia_720.h for why this one constant has
 *  to be a macro. */
#define BOARD_PANEL_RGB 1

namespace board {

constexpr char kName[] = "Waveshare ESP32-S3-Touch-LCD-2.8C";

// --- Panel geometry --------------------------------------------------------
constexpr int kDisplayDiameter = 480;
constexpr int kDisplayWidth = kDisplayDiameter;
constexpr int kDisplayHeight = kDisplayDiameter;

/** Layout baseline. All ui/theme.h values are px at this size, scaled by
 *  kUiScale -- 2.0 here. */
constexpr int kUiBaseSize = 240;
constexpr float kUiScale = static_cast<float>(kDisplayDiameter) / kUiBaseSize;

/** Text at three quarters of the layout's scale. At the full 2.0 it would be
 *  more than twice the physical size of the 1.28" panel's, on glass only a
 *  little bigger per pixel; at 1.5 it is a touch larger than the 1.85" round
 *  board's -- and theme::textPx() lands 15/17/20/24 exactly on that board's
 *  23/26/30/36 px faces, so the two share them. Keep this,
 *  platformio.ini's board_build.embed_files and font_table.cpp in step. */
constexpr float kTextScale = 0.75f;
/** The lists scale with the layout: rows come out 92 px. */
constexpr float kListScale = 1.0f;

/** Section headers in the browse list, at the 240 px design size: a step
 *  below the row titles, as on the other round panels. */
constexpr int kListHeaderTextPx240 = 15;

/** How far the transport row sits above the 240 px design's, in design
 *  pixels. None here: text at three quarters of the layout's scale leaves
 *  the chip below the buttons more room than the 240 px panel has. */
constexpr int kTransportRowLiftPx240 = 0;

constexpr bool kDisplayIsRound = true;

/** No command channel: a repaint is a draw into the frame and a present of
 *  the rectangle it touched. */
constexpr bool kPanelWritesDirect = false;

/** esp_lcd scans its framebuffer out as native little-endian RGB565, and the
 *  composed frame reaches it by memcpy -- see qualia_720.h. */
constexpr bool kFrameNativeByteOrder = true;

// --- RGB data bus ----------------------------------------------------------
// LSB->MSB: blue[0..4], green[0..5], red[0..4]. The panel is set up for 18-bit
// colour (COLMOD 0x66) and driven 565 on its top bits, as Waveshare's demo
// does.
constexpr int kRgbDataPins[16] = {5,  45, 48, 47, 21, 14, 13, 12,
                                  11, 10, 9,  46, 3,  8,  18, 17};
constexpr gpio_num_t kRgbPinHsync = GPIO_NUM_38;
constexpr gpio_num_t kRgbPinVsync = GPIO_NUM_39;
constexpr gpio_num_t kRgbPinDe = GPIO_NUM_40;
constexpr gpio_num_t kRgbPinPclk = GPIO_NUM_41;

// --- Panel timings ---------------------------------------------------------
/**
 * 12 MHz, where Waveshare and Espressif both run 18.
 *
 * The clock is what sets the scan-out's demand on PSRAM -- 2 bytes a pixel,
 * every pixel, whatever the panel's size -- and 18 MHz is 36 MB/s, half as
 * much again as the Qualia breaks up at while a list scrolls. 12 MHz is
 * 24 MB/s, the Qualia's own figure, for 43 Hz on this smaller frame; and it is
 * the clock BambuHelper settled on for this board, confirmed on the hardware.
 * Raise it only with a long list flung and scrolling steadily.
 */
constexpr uint32_t kRgbPclkHz = 12 * 1000 * 1000;
constexpr int kRgbHsyncPulse = 8;
constexpr int kRgbHsyncBackPorch = 10;
constexpr int kRgbHsyncFrontPorch = 50;
constexpr int kRgbVsyncPulse = 2;
constexpr int kRgbVsyncBackPorch = 18;
constexpr int kRgbVsyncFrontPorch = 8;
/** Pixels latch on the rising edge, unlike the Qualia's square panel. */
constexpr bool kRgbPclkActiveNeg = false;
constexpr bool kRgbHsyncIdleLow = false;
constexpr bool kRgbVsyncIdleLow = false;
constexpr bool kRgbDeIdleHigh = false;

// --- ST7701 register init: 3-wire SPI --------------------------------------
// Nine bits a word -- a data/command bit, then the byte -- on SDA and SCL,
// with chip select on the expander (kExpanderBitLcdCs). GPIO 1 and 2 are also
// the SD slot's CMD and CLK; the SPI bus is released once the panel is set
// up, and nothing here uses the slot.
constexpr gpio_num_t kPanelSpiPinSda = GPIO_NUM_1;
constexpr gpio_num_t kPanelSpiPinScl = GPIO_NUM_2;
constexpr uint32_t kPanelSpiHz = 4 * 1000 * 1000;

// --- Backlight -------------------------------------------------------------
constexpr gpio_num_t kDisplayPinBacklight = GPIO_NUM_6;
/** PWM, so blanking cuts the lamp rather than painting black behind it. */
constexpr uint32_t kBacklightPwmHz = 5000;
constexpr uint8_t kBacklightPwmBits = 10;
/** Full brightness, as a duty out of 2^kBacklightPwmBits. */
constexpr uint32_t kBacklightDuty = (1u << kBacklightPwmBits) - 1;

// --- I2C: the TCA9554 expander and the GT911 share it ----------------------
// So do the board's RTC (PCF85063) and IMU (QMI8658), which this firmware
// leaves alone. The wiki calls GPIO 7 and 15 unusable as anything else.
constexpr gpio_num_t kI2cPinSda = GPIO_NUM_15;
constexpr gpio_num_t kI2cPinScl = GPIO_NUM_7;
constexpr uint32_t kI2cHz = 400000;

constexpr uint8_t kExpanderI2cAddress = 0x20;
/** Bits of the expander's port: Waveshare's EXIO1..8, less one. */
constexpr uint8_t kExpanderBitLcdReset = 0;
constexpr uint8_t kExpanderBitTouchReset = 1;
constexpr uint8_t kExpanderBitLcdCs = 2;
constexpr uint8_t kExpanderBitSdCs = 3;
constexpr uint8_t kExpanderBitBuzzer = 7;
/** Every pin an output, as Waveshare's demo has it. */
constexpr uint8_t kExpanderConfig = 0x00;
/** The port at start-up: the resets released, both chip selects parked high
 *  and -- the one that matters -- the buzzer low. The TCA9554 powers up with
 *  its outputs latched high, so the first write has to say this, or the
 *  board beeps until something does. */
constexpr uint8_t kExpanderInitialOutput =
    (1u << kExpanderBitLcdReset) | (1u << kExpanderBitTouchReset) |
    (1u << kExpanderBitLcdCs) | (1u << kExpanderBitSdCs);

// --- Touch: GT911 ----------------------------------------------------------
/** 0x5D, which the GT911 takes when INT is held low as its reset is
 *  released; 0x14 with it high. hw::lcd28c::touchReset() holds it low. */
constexpr uint8_t kTouchI2cAddress = 0x5D;
constexpr gpio_num_t kTouchPinInt = GPIO_NUM_16;
/** The raster the GT911 reports against: its configured resolution, the
 *  panel's own here. */
constexpr int kTouchNativeSize = 480;

// Orientation, the identity as Waveshare's and Espressif's configs both have
// it. Flip these if taps land mirrored or transposed.
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
constexpr unsigned long kTouchPollIntervalMs = 16;
/** On a task of its own, as on the Qualia: the GT911 is read over Wire, on a
 *  bus shared with the expander and guarded for it (hw::lcd28c::BusGuard),
 *  and a framebuffer panel's loop has repaints long enough to swallow a tap. */
constexpr bool kTouchSampleTask = true;

// --- Artwork ---------------------------------------------------------------
/** Thumbnail edge in px at kUiBaseSize: 38 becomes 76 here, about 11.5 KB a
 *  sprite. */
constexpr int kThumbPx = 38;
/** 256 px art, scaled up to the panel, as the Qualia does. The cover is a
 *  backdrop behind a scrim and text, and decoding it is the slow part of a
 *  new track's repaint: 512 px -- the next step up from the panel on the
 *  image proxy's ladder -- measured 210 to 570 ms to decode here, and a
 *  quarter of the pixels is roughly a quarter of that. Raise it back to
 *  kDisplayWidth if the softness shows. */
constexpr int kCoverArtRequestPx = 256;
/** Cached compressed: a 256 px JPEG is tens of KB, and the room above that
 *  is what lets kCoverArtRequestPx go back up without touching this. */
constexpr size_t kCoverArtBufferBytes = 256u * 1024u;

/** Shortest gap between full repaints of a scrolling list. A full repaint is
 *  a 460 KB compose and the same again copied into the buffer being scanned
 *  out, both in PSRAM -- under half the Qualia's -- so half its ration. */
constexpr unsigned long kListRedrawMinMs = 50;

/** Rows to compose before standing off the PSRAM bus for the scan-out, as on
 *  the Qualia; see its header. */
constexpr int kComposeSlabRows = 48;

// --- Not used here ---------------------------------------------------------
// An SD slot (GPIO 42 D0, 1 CMD, 2 CLK, expander bit 3 CS), the PCF85063 RTC
// and QMI8658 IMU on the I2C bus, battery sense on GPIO 4 and the buzzer on
// expander bit 7. Listed so that nobody reaches for one of those pins thinking
// it is spare.

}  // namespace board
