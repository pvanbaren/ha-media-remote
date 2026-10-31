#pragma once

#include <cstdint>

#include <driver/gpio.h>

/**
 * Waveshare ESP32-P4-WIFI6-Touch-LCD-4B: a 4" square 720x720 ST7703 panel on
 * two-lane MIPI-DSI, GT911 touch, an ESP32-P4 with 32 MB of flash and 32 MB
 * of PSRAM, and an ESP32-C6 beside it for Wi-Fi. The Qualia's size of glass
 * on another chip altogether -- see include/hardware/rgb_panel.h for why it
 * still shares the Qualia's output stage.
 *
 * Not an S3, and three things follow from that.
 *
 *  - The P4 has no radio. Wi-Fi is the C6's, reached over SDIO through
 *    ESP-Hosted, which Arduino's WiFi library hides: the pins below are the
 *    core's defaults for this wiring (BOARD_SDIO_ESP_HOSTED_* in the esp32p4
 *    variant), and nothing in the firmware names them.
 *  - The panel is driven by the P4's DSI controller in video mode: like an
 *    RGB panel, a framebuffer in PSRAM scanned out without a break, but over
 *    two serial lanes rather than sixteen parallel ones, so none of the RGB
 *    boards' radio troubles. Its registers are set over the same link
 *    (DBI commands) before the video starts.
 *  - This board's P4 is revision 1.3, an engineering sample as far as the
 *    Arduino libraries go: platformio.ini builds it against esp32p4_es, the
 *    libraries for chips before revision 3. A revision 3 board needs the
 *    esp32-p4_r3 board definition instead.
 *
 * Values from Waveshare's BSP for this board, by way of HomeTiles
 * (src/devices/waveshare_4b), which drives it with Arduino_GFX.
 */
/** Driven as an RGB panel is, as far as hardware/lgfx_config.hpp and the
 *  allocation threshold are concerned: a framebuffer scanned out of PSRAM. */
#define BOARD_PANEL_RGB 1

namespace board {

constexpr char kName[] = "Waveshare ESP32-P4 4B 720x720 square";

// --- Panel geometry --------------------------------------------------------
// The Qualia's: 4" square, 720 px. Everything from here to the panel's own
// section is the Qualia's too, for the same glass at the same distance.
constexpr int kDisplayWidth = 720;
constexpr int kDisplayHeight = 720;

constexpr int kUiBaseSize = 240;
constexpr float kUiScale = static_cast<float>(kDisplayWidth) / kUiBaseSize;

/** theme::textPx() turns 15/17/20/24 into 34/38/45/54, the Qualia's faces --
 *  keep this, platformio.ini's board_build.embed_files and font_table.cpp in
 *  step. */
constexpr float kTextScale = 0.75f;

/** The lists at two thirds, about six rows on the glass, their text on the
 *  round 360 px board's 23/26/30. */
constexpr float kListScale = 2.0f / 3.0f;

constexpr int kListHeaderTextPx240 = 20;

constexpr int kTransportRowLiftPx240 = 0;

constexpr bool kDisplayIsRound = false;

/** The panel is a framebuffer: a "direct" repaint is a draw into the frame
 *  plus a present of that rectangle. */
constexpr bool kPanelWritesDirect = false;

/** The DSI controller reads its framebuffer as native little-endian RGB565,
 *  as the S3's RGB peripheral does. */
constexpr bool kFrameNativeByteOrder = true;

// --- MIPI-DSI --------------------------------------------------------------
/** Two data lanes at 480 Mbit/s each: 720x720 at 16 bits and 59 Hz is about
 *  490 Mbit/s of pixels (24 bits on the wire), which fits with room for the
 *  blanking. */
constexpr int kDsiLanes = 2;
constexpr uint32_t kDsiLaneMbps = 480;

/** The DPI pixel clock. With the porches below that is 840 x 764 a frame,
 *  59 Hz -- 61 MB/s read from PSRAM, against the P4's 200 MHz hex PSRAM. */
constexpr uint32_t kDsiPixelClockMhz = 38;
constexpr int kDsiHsyncPulse = 20;
constexpr int kDsiHsyncBackPorch = 50;
constexpr int kDsiHsyncFrontPorch = 50;
constexpr int kDsiVsyncPulse = 4;
constexpr int kDsiVsyncBackPorch = 20;
constexpr int kDsiVsyncFrontPorch = 20;

/** The DSI PHY's own supply: the P4's internal LDO channel 3 at 2.5 V. */
constexpr int kDsiPhyLdoChannel = 3;
constexpr int kDsiPhyLdoMillivolts = 2500;

/** The panel's supply, LDO channel 4 at 3.3 V. Cycled once at boot, as
 *  HomeTiles does: after a restart without a power cut the panel is still
 *  powered and part-way through whatever the last firmware left it doing. */
constexpr int kPanelLdoChannel = 4;
constexpr int kPanelLdoMillivolts = 3300;

constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_27;

// --- Backlight -------------------------------------------------------------
/** PWM, and active low: full duty is dark. Below about 48% duty the lamp is
 *  off altogether (HomeTiles' kVisibleBacklightRawMin, 122 of 255). */
constexpr gpio_num_t kDisplayPinBacklight = GPIO_NUM_26;
constexpr uint32_t kBacklightPwmHz = 5000;
constexpr uint8_t kBacklightPwmBits = 10;
constexpr bool kBacklightActiveLow = true;

// --- Touch: GT911 ----------------------------------------------------------
// On the board's I2C bus, which also carries the audio codecs. Reset is
// wired; the interrupt is not, so the address is whichever of the two the
// controller answers on, and it is polled.
constexpr gpio_num_t kTouchPinSda = GPIO_NUM_7;
constexpr gpio_num_t kTouchPinScl = GPIO_NUM_8;
constexpr gpio_num_t kTouchPinRst = GPIO_NUM_23;
constexpr uint32_t kTouchI2cHz = 400000;
constexpr uint8_t kTouchI2cAddress = 0x5D;
constexpr uint8_t kTouchI2cAddressAlt = 0x14;
constexpr int kTouchNativeSize = 720;

// Orientation fixes -- flip these if taps land mirrored or transposed.
constexpr bool kTouchSwapXy = false;
constexpr bool kTouchInvertX = false;
constexpr bool kTouchInvertY = false;

constexpr unsigned long kTouchTapMinMs = 20;
constexpr unsigned long kTouchTapMaxMs = 700;
constexpr int kTouchTapSlopPx = 10;
constexpr int kTouchSwipeMinPx = 28;
constexpr unsigned long kTouchPollIntervalMs = 16;
/** On a task of its own, as on the other square boards, so a long repaint
 *  cannot swallow a tap. */
constexpr bool kTouchSampleTask = true;

// --- BOOT button -----------------------------------------------------------
/** The P4's strapping pin, GPIO 35, where the S3's is GPIO 0. Active low. */
constexpr gpio_num_t kBootButtonPin = GPIO_NUM_35;

// --- Artwork ---------------------------------------------------------------
constexpr int kThumbPx = 38;
/** 1024 px, scaled down onto the 720 px panel, where 512 px was scaled up
 *  1.4x. Four times the decode of a 512 on every compose pass, for a P4.
 *  A 1024 px JPEG runs to a few hundred KB; the cache holds 1 MB, of 32 MB
 *  of PSRAM. */
constexpr int kCoverArtRequestPx = 1024;
constexpr size_t kCoverArtBufferBytes = 1024u * 1024u;

/** The Qualia's ration on list repaints is for an S3's octal PSRAM shared
 *  with a 24 MB/s scan-out. The P4's is several times faster; half the
 *  Qualia's gap to start with. */
constexpr unsigned long kListRedrawMinMs = 50;

/** No stand-off while composing: the PSRAM has the bandwidth to spare. */
constexpr int kComposeSlabRows = 0;

// --- Not used here ---------------------------------------------------------
// ES8311 codec and ES7210 microphones on the same I2C bus (0x18, 0x40), I2S
// on 9-13, the speaker amplifier's enable on 53; an SD slot; a CSI camera
// connector. None of it is wired up.

}  // namespace board
