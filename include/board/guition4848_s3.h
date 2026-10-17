#pragma once

#include <cstdint>

#include <driver/gpio.h>

/**
 * Guition (Sunton) ESP32-4848S040: a 4" square 480x480 IPS ST7701 on the
 * S3's RGB-parallel LCD peripheral, GT911 touch, ESP32-S3 with 8 MB octal
 * PSRAM and 16 MB flash, and a CH340 to UART0 for USB. Sold under other
 * names too -- this one came as AITRIP's "ESP32-S3 4.0 inch Color LCD
 * Display Development Board, 480 x 480".
 *
 * The MaTouch 2.1"'s kind of panel -- a framebuffer streamed over 16 data
 * lines, registers set first over a bit-banged 3-wire SPI on plain GPIOs, no
 * reset line, no expander -- on square glass the size of the Qualia's. So it
 * takes the square screens, laid out as the Qualia's at two thirds of its
 * pixels: 2x the 240 px design where the Qualia is 3x, the same millimetres.
 *
 * Provenance: the pins are Arduino_GFX's for this board
 * (ESP32_4848S040_86BOX_GUITION in its examples' Arduino_GFX_dev_device.h),
 * and ESPHome's (GUITION-4848S040 in its mipi_rgb models) agree; the timings
 * are ESPHome's, for the reason given with them; the register init is the
 * one the factory demo on this unit sends, read out of its flash
 * (hardware/guition4848/panel.cpp).
 */
/** A framebuffer panel, for hardware/lgfx_config.hpp; see qualia_720.h. */
#define BOARD_PANEL_RGB 1

namespace board {

constexpr char kName[] = "Guition ESP32-4848S040 4\" 480x480 square";

// --- Panel geometry --------------------------------------------------------
constexpr int kDisplayWidth = 480;
constexpr int kDisplayHeight = 480;

/** Layout baseline. All ui/theme.h values are px at this size, scaled by
 *  kUiScale -- 2.0 here. */
constexpr int kUiBaseSize = 240;
constexpr float kUiScale = static_cast<float>(kDisplayWidth) / kUiBaseSize;

/** The Qualia's proportions, on glass the Qualia's size with two thirds of
 *  its pixels: text at three quarters of the layout's scale lands on
 *  23/26/30/36 px, and the lists at two thirds of that again on 15/17/20 --
 *  the Qualia's millimetres both. Keep these, platformio.ini's
 *  board_build.embed_files and font_table.cpp in step. */
constexpr float kTextScale = 0.75f;
constexpr float kListScale = 2.0f / 3.0f;

/** Section headers in the browse list, at the 240 px design size: as large
 *  as the row titles under them, as on the Qualia. */
constexpr int kListHeaderTextPx240 = 20;

constexpr int kTransportRowLiftPx240 = 0;

/** No corners to lose text into. */
constexpr bool kDisplayIsRound = false;

/** No command channel: a repaint is a draw into the frame and a present. */
constexpr bool kPanelWritesDirect = false;

/** esp_lcd scans its framebuffer out as native little-endian RGB565. */
constexpr bool kFrameNativeByteOrder = true;

// --- RGB data bus ----------------------------------------------------------
// LSB->MSB: blue[0..4], green[0..5], red[0..4], as Arduino_GFX lists them
// for this board. The panel is set to 18-bit colour and driven 565 on its
// top bits. GPIO 0, the BOOT button on other boards, is red's top bit here.
constexpr int kRgbDataPins[16] = {4, 5, 6,  7,  15, 8,  20, 3,
                                  46, 9, 10, 11, 12, 13, 14, 0};
constexpr gpio_num_t kRgbPinHsync = GPIO_NUM_16;
constexpr gpio_num_t kRgbPinVsync = GPIO_NUM_17;
constexpr gpio_num_t kRgbPinDe = GPIO_NUM_18;
constexpr gpio_num_t kRgbPinPclk = GPIO_NUM_21;

// --- Panel timings ---------------------------------------------------------
/**
 * ESPHome's, for this board: 12 MHz, rising edge, and the porches of its
 * generic ST7701S -- HSYNC 10 wide with 10 behind and 20 in front, VSYNC 10
 * with 10 and 10 -- which come to 45 Hz. Arduino_GFX's for it (8 / 50 / 10
 * and 8 / 20 / 10, 16 MHz in the vendor's demo) are as good on the glass;
 * these are the ones the board has run longest, with kRgbFrameByFrame.
 */
constexpr uint32_t kRgbPclkHz = 12 * 1000 * 1000;
constexpr int kRgbHsyncPulse = 10;
constexpr int kRgbHsyncBackPorch = 10;
constexpr int kRgbHsyncFrontPorch = 20;
constexpr int kRgbVsyncPulse = 10;
constexpr int kRgbVsyncBackPorch = 10;
constexpr int kRgbVsyncFrontPorch = 10;
constexpr bool kRgbPclkActiveNeg = false;
constexpr bool kRgbHsyncIdleLow = false;
constexpr bool kRgbVsyncIdleLow = false;
constexpr bool kRgbDeIdleHigh = false;
/**
 * A bounce buffer of this many rows, where the other framebuffer panels have
 * none (hardware/rgb_panel.h says why they do without).
 *
 * Without one this panel's whole picture jumped sideways for a moment on
 * every screen change and scroll -- the scan-out starved and caught up at the
 * next VSYNC -- where the 2.8C and the MaTouch, scanning the same 480x480
 * frame at the same clock, stay steady. A slower clock made it worse, not
 * better. The same board did the same under Arduino_GFX once on
 * arduino-esp32 3.x, and a bounce buffer of about ten rows is what fixed it
 * there (Arduino_GFX issue #728): larger tore animations, smaller left
 * artifacts -- and ESPHome's driver, which this board runs under too, takes
 * ten rows by default. Two buffers of it in internal RAM, 9.6 KB each; twenty
 * changed nothing, and took internal RAM to within 156 bytes of running out.
 *
 * Its refills are an interrupt that cannot run while the flash is written,
 * so an OTA update breaks the picture up for as long as it takes. Nothing
 * else here writes flash for long.
 */
constexpr int kRgbBounceBufferRows = 10;
/**
 * Each frame a fresh start (hardware/rgb_panel.h), where the other boards'
 * panels are scanned out without a break.
 *
 * Scanned out continuously, as ESP-IDF 5 does, this panel lost its place a
 * few minutes to ten after boot, every time: bands of rows out of place,
 * then the whole picture sat shifted up by about eight lines -- a VSYNC's
 * width -- while a count at each VSYNC showed the bounce buffer refilled on
 * time, every frame. ESPHome's porches, 16 MHz and the panel maker's own
 * init (HSD040BPN1) only moved when it started. The vendor's own firmware,
 * on ESP-IDF 4.4, never did it: that drives every RGB panel a frame at a
 * time, the LCD engine stopped and reset at the end of each and started
 * again. Driven that way here, through ESP-IDF 5's refresh-on-demand mode,
 * it stays steady. Why this glass needs it and the 2.8C's and the
 * MaTouch's do not is not known.
 */
constexpr bool kRgbFrameByFrame = true;

// --- ST7701 register init: 3-wire SPI --------------------------------------
// Nine bits a word, bit-banged as on the MaTouch (see panel.cpp); chip
// select a plain GPIO. SCK and SDA are the SD card's clock and MOSI as well,
// which the firmware does not use: with the panel's CS left high after the
// init, the panel ignores them.
constexpr gpio_num_t kPanelSpiPinCs = GPIO_NUM_39;
constexpr gpio_num_t kPanelSpiPinScl = GPIO_NUM_48;
constexpr gpio_num_t kPanelSpiPinSda = GPIO_NUM_47;

// --- Backlight -------------------------------------------------------------
constexpr gpio_num_t kDisplayPinBacklight = GPIO_NUM_38;
constexpr uint32_t kBacklightPwmHz = 5000;
constexpr uint8_t kBacklightPwmBits = 10;
constexpr uint32_t kBacklightDuty = (1u << kBacklightPwmBits) - 1;

// --- Touch: GT911 ----------------------------------------------------------
/** On a bus of its own. Neither its interrupt nor its reset reaches the S3,
 *  so it is polled -- and, with no reset to set it, its address is whichever
 *  of its two it took at power-up: this one first, then
 *  kTouchI2cAddressAlt. */
constexpr gpio_num_t kTouchPinSda = GPIO_NUM_19;
constexpr gpio_num_t kTouchPinScl = GPIO_NUM_45;
constexpr uint32_t kTouchI2cHz = 400000;
constexpr uint8_t kTouchI2cAddress = 0x5D;
constexpr uint8_t kTouchI2cAddressAlt = 0x14;
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

// --- Settings reset --------------------------------------------------------
/** GPIO 0 is a data line here, so there is no BOOT button to hold. Instead a
 *  finger held on the glass stands in for it -- but only a hold begun within
 *  this long of power-up, so that an ordinary long press later never comes
 *  near clearing anything. */
constexpr unsigned long kResetHoldWindowMs = 15000;
/** How long that hold must last before it counts as the button pressed.
 *  wifi_setup then wants config::kBootResetHoldMs (3 s) more of it, so the
 *  whole hold is 10 s. */
constexpr unsigned long kResetHoldLeadMs = 7000;

// --- Artwork ---------------------------------------------------------------
/** In list rows, so it takes kListScale as well: 38 becomes 51 px. */
constexpr int kThumbPx = 38;
/** Panel-size art, which the image proxy rounds up to 512 px. */
constexpr int kCoverArtRequestPx = kDisplayWidth;
constexpr size_t kCoverArtBufferBytes = 256u * 1024u;

/** As the other 480 px framebuffer panels: the same frame, the same PSRAM. */
constexpr unsigned long kListRedrawMinMs = 50;
constexpr int kComposeSlabRows = 48;

}  // namespace board
