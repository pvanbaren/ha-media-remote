#pragma once

#include <cstdint>

#include <driver/gpio.h>

/**
 * ESP32-S3 with a 1.75" round 466x466 CO5300 AMOLED, driven over quad SPI.
 *
 * The board is a Waveshare ESP32-S3-Touch-AMOLED-1.75 (Rev 1.1, with an SD
 * card slot): ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM, a CST9217 touch
 * controller, an ES8311 speaker codec and an ES7210 microphone ADC, and on
 * the same board an AXP2101 power manager and a QMI8658 IMU that this
 * firmware does not use.
 *
 * The 1.75C builds from this header too, as env amoled466c-s3, which sets
 * BOARD_AMOLED466_175C. Its pins are the 1.75's but for its resets: the
 * display's on GPIO 1 and the touch's on GPIO 2, where the 1.75 has its SD
 * card, against 39 and 40 on the 1.75. It has 32 MB of flash, of which the
 * build uses the first 16, and no SD card slot. It has not been tried on the
 * glass.
 *
 * Named for the panel, as the DFR1221's round360 is, rather than for a vendor
 * that makes several round boards.
 *
 * Provenance: the pins below are Waveshare's own pin_config.h
 * (examples/arduino/libraries/Mylibrary) in the 1.75C's repository,
 * waveshareteam/ESP32-S3-Touch-AMOLED-1.75C, with the resets the 1.75's
 * (waveshareteam/ESP32-S3-Touch-AMOLED-1.75); the panel's 6-column offset is
 * the one their Arduino_GFX examples construct the CO5300 with; the touch
 * protocol is SensorLib's TouchDrvCST92xx, which their examples use, with
 * both axes mirrored as they set it. It runs on the glass: the picture and
 * its colours are right, and a tapped player on the list is the one chosen.
 *
 * What is different about this panel, and why the rest of the board is laid
 * out as it is:
 *
 *  - An AMOLED has no backlight. Brightness is a command to the controller
 *    (0x51), which LovyanGFX's Panel_AMOLED sends from setBrightness().
 *  - The controller takes a window only at an even column and an even width,
 *    and at an even row and an even height. LovyanGFX drops a window wrong
 *    in columns without a word, and the controller one wrong in rows -- a
 *    frame pushed a row at a time left the panel dark. So nothing may draw
 *    on the glass directly (kPanelWritesDirect is false), and
 *    hardware/amoled466/display.cpp widens every rectangle it pushes to even
 *    columns and rows and pushes it two rows at a time.
 *  - LovyanGFX starts the CO5300 as LilyGO's watch wants it, with no wait
 *    after sleep-out; lgfx_config.hpp starts it as Waveshare's examples do.
 *  - LovyanGFX's AMOLED rotation applies the column offset the same way at
 *    every turn, which on a panel with an offset shifts the picture at 180
 *    and the quarter turns. The controller is therefore never turned: the
 *    frame is turned as it is copied out, as the Qualia's is, and touches are
 *    turned back by hw::mapPoint().
 */
/** A CO5300 on quad SPI. Not BOARD_PANEL_QSPI, which is the ST77916 and its
 *  backlight and CST816S; this one gets its own branch in lgfx_config.hpp. */
#define BOARD_PANEL_AMOLED 1

namespace board {

#ifdef BOARD_AMOLED466_175C
constexpr char kName[] = "Waveshare ESP32-S3-Touch-AMOLED-1.75C 466x466 round";
#else
constexpr char kName[] = "ESP32-S3 + 1.75\" round 466x466 CO5300 AMOLED";
#endif

// --- Panel geometry --------------------------------------------------------
constexpr int kDisplayDiameter = 466;
constexpr int kDisplayWidth = kDisplayDiameter;
constexpr int kDisplayHeight = kDisplayDiameter;

/** Layout baseline. All ui/theme.h values are px at this size. */
constexpr int kUiBaseSize = 240;
/** About 1.94 here, which is why this board embeds 29/33/39/47 px faces:
 *  theme::px() rounds 15 to 29, 17 to 33, 20 to 39 and 24 to 47. Keep those
 *  three lists in step -- the heights here, platformio.ini's
 *  board_build.embed_files, and font_table.cpp. */
constexpr float kUiScale = static_cast<float>(kDisplayDiameter) / kUiBaseSize;
constexpr float kTextScale = 1.0f;
constexpr float kListScale = 1.0f;

/** Section headers in the browse list, at the 240 px design size: a step
 *  below the rows, as on the 360 px panel. */
constexpr int kListHeaderTextPx240 = 15;

/** How far the transport row sits above the 240 px design's. The 360 px
 *  panel's figure, which is there to keep the play button clear of the
 *  elapsed chip once both are scaled up; to be checked on this glass. */
constexpr int kTransportRowLiftPx240 = 5;

constexpr bool kDisplayIsRound = true;

/** False, though the controller has a command channel: a direct draw at an
 *  odd column would be dropped (see above). Small repaints go through the
 *  composed frame and displayPresentFrame(), which aligns them. */
constexpr bool kPanelWritesDirect = false;

/** As on the other bus panels: LovyanGFX's byte-swapped RGB565. */
constexpr bool kFrameNativeByteOrder = false;

// --- Display: CO5300 on quad SPI -------------------------------------------
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_38;
constexpr gpio_num_t kDisplayPinIo0 = GPIO_NUM_4;
constexpr gpio_num_t kDisplayPinIo1 = GPIO_NUM_5;
constexpr gpio_num_t kDisplayPinIo2 = GPIO_NUM_6;
constexpr gpio_num_t kDisplayPinIo3 = GPIO_NUM_7;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_12;
#ifdef BOARD_AMOLED466_175C
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_1;
#else
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_39;
#endif

constexpr uint32_t kDisplaySpiWriteHz = 40000000;

/** The panel's 466 columns start 6 into the controller's memory, and its rows
 *  at 0. Waveshare's examples construct Arduino_GFX's CO5300 with exactly
 *  these (col_offset1 6, the rest 0). */
constexpr int kDisplayOffsetX = 6;
constexpr int kDisplayOffsetY = 0;
constexpr int kDisplayMemoryWidth = kDisplayWidth + kDisplayOffsetX;
constexpr int kDisplayMemoryHeight = kDisplayHeight + kDisplayOffsetY;

/** Waveshare's init sends no INVON. Flip this if black shows white. */
constexpr bool kDisplayInvert = false;

/** 0..255, sent as the controller's brightness. Lower than the LCDs' 255:
 *  an OLED ages with what it shows, and the now-playing screen holds the same
 *  buttons in the same place for as long as music plays. */
constexpr uint8_t kDisplayBrightness = 200;

// --- Touch: CST9217 capacitive controller (I2C) ----------------------------
// Read by hand in hardware/amoled466/touch_raw.cpp: LovyanGFX's nearest
// driver, Touch_CST226, speaks the same protocol but accepts only the
// CST226's chip id. The bus is shared with the audio codecs, and with the
// AXP2101 and the QMI8658, which this firmware does not talk to; hw::i2c
// owns it.
constexpr gpio_num_t kTouchPinSda = GPIO_NUM_15;
constexpr gpio_num_t kTouchPinScl = GPIO_NUM_14;
constexpr gpio_num_t kTouchPinInt = GPIO_NUM_11;
#ifdef BOARD_AMOLED466_175C
constexpr gpio_num_t kTouchPinRst = GPIO_NUM_2;
#else
constexpr gpio_num_t kTouchPinRst = GPIO_NUM_40;
#endif
constexpr uint8_t kTouchI2cAddress = 0x5A;
constexpr uint32_t kTouchI2cHz = 400000;

/** Raster the controller reports against. Waveshare sets it to the panel's
 *  466; touch_raw.cpp reads the controller's own figure at boot and scales
 *  from that when it differs. */
constexpr int kTouchNativeSize = kDisplayDiameter;

/** Both axes mirrored, as Waveshare's examples set them (setMirrorXY(true,
 *  true)). The panel itself is never turned, so these are the whole of it. */
constexpr bool kTouchSwapXy = false;
constexpr bool kTouchInvertX = true;
constexpr bool kTouchInvertY = true;

// Gesture timings: the Waveshare 1.28's, as on every round board.
constexpr unsigned long kTouchTapMinMs = 20;
constexpr unsigned long kTouchTapMaxMs = 700;
constexpr int kTouchTapSlopPx = 10;
constexpr int kTouchSwipeMinPx = 28;
constexpr unsigned long kTouchPollIntervalMs = 16;
/** Sampled from the loop, as on the other bus panels. */
constexpr bool kTouchSampleTask = false;

// --- Artwork ---------------------------------------------------------------
/** 38 at the design size, 74 px here. */
constexpr int kThumbPx = 38;

/** 466 asks the image proxy for 512: one step above the panel. */
constexpr int kCoverArtRequestPx = kDisplayWidth;

/** As the 360 px panel's, which also fetches 512 px art. */
constexpr size_t kCoverArtBufferBytes = 256u * 1024u;

/** A 466x466 frame is 434 KB, about 22 ms at 40 MHz across four lanes, and a
 *  turned one costs a copy on top: a repaint is about a frame and a half of
 *  touch reports. */
constexpr unsigned long kListRedrawMinMs = 24;

/** Written over a bus, not scanned out of PSRAM: nothing to stand off. */
constexpr int kComposeSlabRows = 0;

// --- Audio: ES8311 speaker codec, ES7210 microphone ADC --------------------
// One I2S port, both directions: the S3 sends the speaker on DOUT and reads
// the microphones on DIN, and is the clock for both codecs. They are set up
// over the touch bus, in hardware/es8311_es7210.cpp; pins and setup are
// Waveshare's BSP for the 1.75. Two microphones face the front, on the
// ES7210's MIC1 and MIC2; only MIC1 is used. Its MIC3 is the speaker looped
// back, for echo cancelling, which nothing here does.
#define BOARD_HAS_MIC 1
#define BOARD_AUDIO_ES8311_ES7210 1

/** The 1.75C's is GPIO 16, as Waveshare's pin_config.h for it has it. */
#ifdef BOARD_AMOLED466_175C
constexpr gpio_num_t kAudioPinMclk = GPIO_NUM_16;
#else
constexpr gpio_num_t kAudioPinMclk = GPIO_NUM_42;
#endif
constexpr gpio_num_t kAudioPinBclk = GPIO_NUM_9;
constexpr gpio_num_t kAudioPinLrck = GPIO_NUM_45;
constexpr gpio_num_t kAudioPinDout = GPIO_NUM_8;   // to the ES8311
constexpr gpio_num_t kAudioPinDin = GPIO_NUM_10;   // from the ES7210
/** The speaker amplifier's enable, high for on. */
constexpr gpio_num_t kAudioPinSpeakerEnable = GPIO_NUM_46;
constexpr uint8_t kAudioDacI2cAddress = 0x18;  // ES8311
constexpr uint8_t kAudioAdcI2cAddress = 0x40;  // ES7210

// --- Not used here ---------------------------------------------------------
// The AXP2101 and QMI8658 on the touch bus, and on the 1.75 the SD card, on
// GPIO 1 and 2 among others. Listed so that nobody reaches for one of those
// pins thinking it is spare.

}  // namespace board
