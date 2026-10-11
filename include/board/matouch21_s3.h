#pragma once

#include <cstdint>

#include <driver/gpio.h>

/**
 * Makerfabs MaTouch ESP32-S3 Rotary IPS Display with Touch 2.1": a 2.1"
 * round 480x480 ST7701 on the S3's RGB-parallel LCD peripheral, CST826
 * touch, a rotary encoder with a push switch round the glass, ESP32-S3R8
 * (8 MB octal PSRAM) and 16 MB flash.
 *
 * The 2.8C's kind of panel -- a framebuffer streamed over 16 data lines,
 * registers set first over 3-wire SPI -- so it takes the round screens and
 * the shared RGB output stage (hardware/rgb_panel.h). Simpler around it: the
 * panel's chip select is a GPIO, there is no reset line and no expander, and
 * touch has a bus of its own (hardware/matouch21.h).
 *
 * Provenance: every pin, timing and init byte below is Makerfabs', from the
 * board's factory test as built in their repository's PlatformIO/fw_test
 * (Makerfabs/MaTouch-ESP32-S3-Rotary-IPS-Display-with-Touch-2.1-ST7701)
 * against Arduino_GFX 1.3.8 -- whose built image, flashed here, shows true
 * colours -- and that library's st7701_type5 init.
 */
/** A framebuffer panel, for hardware/lgfx_config.hpp; see qualia_720.h. */
#define BOARD_PANEL_RGB 1

namespace board {

constexpr char kName[] = "Makerfabs MaTouch ESP32-S3 Rotary 2.1\" ST7701";

// --- Panel geometry --------------------------------------------------------
constexpr int kDisplayDiameter = 480;
constexpr int kDisplayWidth = kDisplayDiameter;
constexpr int kDisplayHeight = kDisplayDiameter;

/** Layout baseline. All ui/theme.h values are px at this size, scaled by
 *  kUiScale -- 2.0 here. */
constexpr int kUiBaseSize = 240;
constexpr float kUiScale = static_cast<float>(kDisplayDiameter) / kUiBaseSize;

/** Text at three quarters of the layout's scale, as on the 2.8C, so the two
 *  share its 23/26/30/36 px faces. This glass packs the same pixels into
 *  2.1", so the text comes out about four fifths the physical size of the
 *  1.85" board's; raise this (and the faces) if it reads small. Keep this,
 *  platformio.ini's board_build.embed_files and font_table.cpp in step. */
constexpr float kTextScale = 0.75f;
constexpr float kListScale = 1.0f;

/** Section headers in the browse list, at the 240 px design size. */
constexpr int kListHeaderTextPx240 = 15;

/** As the 2.8C: text at three quarters of the layout leaves the chip room. */
constexpr int kTransportRowLiftPx240 = 0;

constexpr bool kDisplayIsRound = true;

/** No command channel: a repaint is a draw into the frame and a present. */
constexpr bool kPanelWritesDirect = false;

/** esp_lcd scans its framebuffer out as native little-endian RGB565. */
constexpr bool kFrameNativeByteOrder = true;

// --- RGB data bus ----------------------------------------------------------
// LSB->MSB: blue[0..4], green[0..5], red[0..4]. Red is 4/41/5/40/6 and blue
// 11/15/12/16/21, as in Makerfabs' factory test (fw_test, Arduino_GFX
// 1.3.8), which shows true colours here. Their older sketches label the two
// the other way round, for Arduino_GFX 1.3.1, and taken from those this
// glass showed red as green and lost blue whatever the init said. With these
// the panel is set to BGR order and 18-bit colour, as that build sets it.
constexpr int kRgbDataPins[16] = {11, 15, 12, 16, 21, 39, 7, 47,
                                  8,  48, 9,  4,  41, 5,  40, 6};
constexpr gpio_num_t kRgbPinHsync = GPIO_NUM_3;
constexpr gpio_num_t kRgbPinVsync = GPIO_NUM_42;
constexpr gpio_num_t kRgbPinDe = GPIO_NUM_2;
constexpr gpio_num_t kRgbPinPclk = GPIO_NUM_45;

// --- Panel timings ---------------------------------------------------------
/** 12 MHz, Makerfabs' own figure in Arduino_GFX's config and the 2.8C's: the
 *  PSRAM's share of a scan-out at this frame size. */
constexpr uint32_t kRgbPclkHz = 12 * 1000 * 1000;
constexpr int kRgbHsyncPulse = 8;
constexpr int kRgbHsyncBackPorch = 50;
constexpr int kRgbHsyncFrontPorch = 10;
constexpr int kRgbVsyncPulse = 8;
constexpr int kRgbVsyncBackPorch = 20;
constexpr int kRgbVsyncFrontPorch = 10;
constexpr bool kRgbPclkActiveNeg = false;
constexpr bool kRgbHsyncIdleLow = false;
constexpr bool kRgbVsyncIdleLow = false;
constexpr bool kRgbDeIdleHigh = false;

// --- ST7701 register init: 3-wire SPI --------------------------------------
// Nine bits a word on SDA and SCK, bit-banged as Makerfabs' build sends them
// (see panel.cpp), chip select a plain GPIO. No reset line: the init starts
// with a software reset. GPIO 0 is SDA here and also the BOOT button, which
// is read again once the init has let go of it.
constexpr gpio_num_t kPanelSpiPinCs = GPIO_NUM_1;
constexpr gpio_num_t kPanelSpiPinScl = GPIO_NUM_46;
constexpr gpio_num_t kPanelSpiPinSda = GPIO_NUM_0;

// --- Backlight -------------------------------------------------------------
constexpr gpio_num_t kDisplayPinBacklight = GPIO_NUM_38;
constexpr uint32_t kBacklightPwmHz = 5000;
constexpr uint8_t kBacklightPwmBits = 10;
constexpr uint32_t kBacklightDuty = (1u << kBacklightPwmBits) - 1;

// --- Touch: CST826 ---------------------------------------------------------
/** On a bus of its own. Neither its interrupt nor its reset is wired to the
 *  S3, so it is polled. */
constexpr gpio_num_t kTouchPinSda = GPIO_NUM_17;
constexpr gpio_num_t kTouchPinScl = GPIO_NUM_18;
constexpr uint32_t kTouchI2cHz = 400000;
constexpr uint8_t kTouchI2cAddress = 0x15;
constexpr int kTouchNativeSize = 480;

// Orientation: the identity until checked on the glass. Flip these if taps
// land mirrored or transposed.
constexpr bool kTouchSwapXy = false;
constexpr bool kTouchInvertX = false;
constexpr bool kTouchInvertY = false;

constexpr unsigned long kTouchTapMinMs = 20;
constexpr unsigned long kTouchTapMaxMs = 700;
constexpr int kTouchTapSlopPx = 10;
constexpr int kTouchSwipeMinPx = 28;
constexpr unsigned long kTouchPollIntervalMs = 16;
/** On a task of its own, as on the other framebuffer panels: a repaint is
 *  long enough to swallow a tap sampled from the loop. */
constexpr bool kTouchSampleTask = true;

// --- Rotary encoder --------------------------------------------------------
/** Quadrature on two pins with pull-ups, and the knob's push switch, active
 *  low. Not read yet: the remote has no use for it until the UI takes it. */
constexpr gpio_num_t kEncoderPinA = GPIO_NUM_13;
constexpr gpio_num_t kEncoderPinB = GPIO_NUM_10;
constexpr gpio_num_t kEncoderPinPush = GPIO_NUM_14;

// --- Artwork ---------------------------------------------------------------
constexpr int kThumbPx = 38;
/** Panel-size art, which the image proxy rounds up to 512 px, as the 2.8C. */
constexpr int kCoverArtRequestPx = kDisplayWidth;
constexpr size_t kCoverArtBufferBytes = 256u * 1024u;

/** As the 2.8C: the same frame, the same PSRAM. */
constexpr unsigned long kListRedrawMinMs = 50;
constexpr int kComposeSlabRows = 48;

}  // namespace board
