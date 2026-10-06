#pragma once

#include <cstdint>

#include <driver/gpio.h>

/**
 * Adafruit Qualia ESP32-S3 for RGB-666 displays (product 5800), carrying the
 * 4" square 720x720 capacitive panel (product 5794, TL040HDS20).
 *
 * Nothing here is a choice. The Qualia routes a 40-pin FPC straight to the
 * S3's LCD peripheral, so the data and sync pins are fixed by the board, and
 * the panel's own config lines are not GPIOs at all -- they hang off a TCA9554
 * I2C expander. 16 MB flash, 8 MB octal PSRAM.
 *
 * The panel is an RGB-parallel ("DotClock") device: the LCD peripheral streams
 * pixels continuously over 16 data lines plus PCLK/HSYNC/VSYNC/DE, and there
 * is **no command channel**. That shapes everything below it -- see
 * hardware/rgb_panel.h for the output stage and hardware/qualia_expander.h
 * for the reset that replaces a register init.
 *
 * Values transcribed from two known-good sources: the Qualia bring-up in
 * ESP32-Plane-Radar (docs/qualia_display.md), and Adafruit's CircuitPython
 * adafruit_qualia/displays/square40.py for the panel timings.
 */
/** How the panel is driven, for the one place that has to be a preprocessor
 *  decision: hardware/lgfx_config.hpp, which picks a LovyanGFX type. A
 *  constexpr cannot serve there -- #if cannot see one, and would read it as
 *  zero. Everything else in this header is an ordinary constant. */
#define BOARD_PANEL_RGB 1

namespace board {

constexpr char kName[] = "Adafruit Qualia ESP32-S3 + 4\" 720x720 square";

// --- Panel geometry --------------------------------------------------------
// Square, so width and height are the same number but for a different reason
// than on a circle: there is no diameter here, and no chord. ui::theme's
// chordHalfWidth() returns the full half-width on this board, which is what
// lets the list and search screens be shared with the round one.
constexpr int kDisplayWidth = 720;
constexpr int kDisplayHeight = 720;

/** Layout baseline. Every dimension in ui/theme.h is px at this size and gets
 *  multiplied by kUiScale -- 3.0 here, so a 46 px row becomes 138 px. */
constexpr int kUiBaseSize = 240;
constexpr float kUiScale = static_cast<float>(kDisplayWidth) / kUiBaseSize;

/** Text is scaled by this on top of kUiScale; layout is not. Three quarters
 *  here, because scaling the 1.28" design by pixel count alone overshoots on
 *  this panel: it packs more pixels per millimetre than the 240 px round one,
 *  so text at 3x came out more than twice the physical size.
 *
 *  theme::textPx() turns 15/17/20/24 into 34/38/45/54, and those are the faces
 *  embedded -- keep this, platformio.ini's board_build.embed_files and
 *  font_table.cpp in step. */
constexpr float kTextScale = 0.75f;

/** The browse and search lists -- rows, thumbnails, their text -- are scaled
 *  by this on top of everything above. Two thirds here, so a 4" panel shows
 *  about six rows where the 1.85" round one shows three or four: a list is
 *  for scanning, and this panel has the room to show more of it at once.
 *
 *  Rows come out 92 px, about 9 mm on this glass, still taller than the round
 *  panel's, so nothing gets harder to hit. And the list's text lands on
 *  23/26/30 px -- the round 360 px board's own faces, embedded here too. */
constexpr float kListScale = 2.0f / 3.0f;

/** Section headers in the browse list, at the 240 px design size, so they
 *  go through listTextPx() with everything else in the list.
 *
 *  20 here rather than 15: the same as the row titles below them. This panel
 *  shows about six rows at a time and the headers were reading as captions
 *  for the list rather than as part of it. They stay distinguishable by
 *  colour, which is what kAccent was already doing.
 *
 *  It also happens to leave the 23 px face with no user on this board. Still
 *  embedded, and harmless -- displayFontApplyHeight() picks by nearest size,
 *  so nothing reaches for it -- but it is 41 KB of flash doing nothing. */
constexpr int kListHeaderTextPx240 = 20;

/** How far the transport row sits above the 240 px design's, in design
 *  pixels. None on this board. */
constexpr int kTransportRowLiftPx240 = 0;

/** No corners to lose text into. */
constexpr bool kDisplayIsRound = false;

/** An RGB panel has no command channel -- the peripheral only streams what is
 *  in the framebuffer -- so there is nowhere to draw but the frame itself. A
 *  "direct" repaint here is a draw into the frame plus a present of that
 *  rectangle. */
constexpr bool kPanelWritesDirect = false;

/** The composed frame reaches the glass by memcpy, with nothing in between to
 *  convert it, and esp_lcd reads its framebuffer as native little-endian
 *  RGB565. So the canvas has to be laid out that way too -- LovyanGFX's
 *  default is byte-swapped for SPI, and using it here turns every colour into
 *  its opposite. It is the first thing to suspect if the screen comes up in
 *  wrong colours rather than no colours. */
constexpr bool kFrameNativeByteOrder = true;

// --- RGB-666 data bus ------------------------------------------------------
// LSB->MSB: blue[0..4], green[0..5], red[0..4]. The panel is 666 but the bus
// is driven 565: the LCD peripheral's 16 lines land on the panel's top bits,
// which costs one bit of red and blue and nothing visible.
constexpr int kRgbDataPins[16] = {40, 39, 38, 0,  45, 48, 47, 21,
                                  14, 13, 12, 11, 10, 9,  46, 3};
constexpr gpio_num_t kRgbPinHsync = GPIO_NUM_41;
constexpr gpio_num_t kRgbPinVsync = GPIO_NUM_42;
constexpr gpio_num_t kRgbPinDe = GPIO_NUM_2;
constexpr gpio_num_t kRgbPinPclk = GPIO_NUM_1;

// --- Panel timings (TL040HDS20) --------------------------------------------
// From Adafruit's square40.py, which is what CircuitPython ships and runs on
// this exact board and panel.
//
// The panel datasheet (TL040HDS20CT-B1502A rev 4.0, section 5.3) gives the
// horizontal porches the other way round -- back 46, front 44 -- and a 35 MHz
// typical dot clock. Total horizontal blanking is 92 either way, so the
// difference shows up as a horizontal offset rather than a loss of sync. If
// the image sits off-centre sideways, swap these two and nothing else.
// This clock is also two other things, and both of them cost something.
//
// It is a memory-bandwidth figure. An RGB panel has no frame memory, so GDMA
// reads the framebuffer out of PSRAM continuously: every MHz of pixel clock
// is 2 MB/s during active scan, and the panel cannot be starved of a byte of
// it without the LCD FIFO underrunning.
//
// And it is a radio setting. Sixteen data lines switching a few centimetres
// from a 2.4 GHz front end desense it, and this board proved it the hard way:
// every HTTP request took tens of seconds -- one success at 72 s, TCP
// connects timing out at six -- against a server that answers a TLS 1.3
// handshake in 4 ms from a PC on the same network. Dropping to 12 MHz took
// that request to 4.3 s while speeding up no PSRAM copy at all, which is what
// ruled out bandwidth and left interference.
//
// So why 12 MHz here. Not for the radio: two levers reach it and they add
// rather than substitute, this clock and the GPIO drive strength in
// src/hardware/qualia/rgb.cpp, and at GPIO_DRIVE_CAP_0 the drive alone was
// sufficient -- measured at 16 MHz, 0.5 s for the first request and then 28,
// 16, 16 ms, which is a healthy LAN.
//
// It is the bandwidth. At 16 MHz the scan-out wants 32 MB/s, and a scrolling
// list adds a 624-row present -- 1.8 MB in about 60 ms, another 31 MB/s --
// plus the compose, against an octal PSRAM good for perhaps 80 before access
// overhead. That left the frame shifting sideways as the list scrolled, and
// even with list scrolling moving rows in place (browse_list::scrollInPlace())
// the picture broke up at 16. The copy and the compose stand off the bus
// between slabs, and the every-VSYNC restart pulls a desynchronised frame
// back into line (platformio.ini), but those only spread the demand and
// recover from a miss. This is the one lever that lowers what the panel
// consumes: 24 MB/s, a quarter less, at the price of 19.5 Hz instead of 26.1.
//
// Raise it only with the list scrolling steadily at 12, and watch for the
// shift while flinging a long list, not on a static screen.
constexpr uint32_t kRgbPclkHz = 12 * 1000 * 1000;
constexpr int kRgbHsyncPulse = 2;
constexpr int kRgbHsyncBackPorch = 44;
constexpr int kRgbHsyncFrontPorch = 46;
constexpr int kRgbVsyncPulse = 2;
constexpr int kRgbVsyncBackPorch = 18;
constexpr int kRgbVsyncFrontPorch = 16;
/** square40.py sets pclk_active_high False, i.e. pixels latch on the falling
 *  edge. The round 720x720 panel wants the opposite, which is the single most
 *  common cause of a torn or shimmering image if it is ever wrong. */
constexpr bool kRgbPclkActiveNeg = true;
constexpr bool kRgbHsyncIdleLow = false;
constexpr bool kRgbVsyncIdleLow = false;
constexpr bool kRgbDeIdleHigh = false;

// --- TCA9554 I2C expander --------------------------------------------------
// The panel's reset line, the backlight and the two user buttons are all
// behind this, not on GPIOs.
constexpr int kExpanderI2cPort = 0;
constexpr gpio_num_t kExpanderPinSda = GPIO_NUM_8;
constexpr gpio_num_t kExpanderPinScl = GPIO_NUM_18;
constexpr uint8_t kExpanderI2cAddress = 0x3F;
constexpr uint32_t kExpanderI2cHz = 100000;

/** Direction register: bits 0/1/2/4/7 are outputs, the rest inputs. This is
 *  Adafruit's CircuitPython bus init for this board (0x78) with the backlight
 *  bit also made an output, so blanking can cut the lamp. */
constexpr uint8_t kExpanderConfig = 0x68;
/** Output-port shadow at reset, matching CircuitPython's gpio_data default. */
constexpr uint8_t kExpanderInitialOutput = 0xFD;

constexpr uint8_t kExpanderBitTftSck = 0;
constexpr uint8_t kExpanderBitTftCs = 1;
constexpr uint8_t kExpanderBitTftReset = 2;
constexpr uint8_t kExpanderBitTouchIrq = 3;
constexpr uint8_t kExpanderBitBacklight = 4;
constexpr uint8_t kExpanderBitButtonUp = 5;
constexpr uint8_t kExpanderBitButtonDown = 6;
constexpr uint8_t kExpanderBitTftMosi = 7;

/** The backlight bit is driven (active high), so displayBlank() turns the lamp
 *  off rather than just painting the framebuffer black. The initial output
 *  has bit 4 set, so the lamp is on from the moment the pin becomes an output.
 *  To go back to the panel's own default-on pull, set this false and put bit 4
 *  back in kExpanderConfig. */
constexpr bool kBacklightOnExpander = true;

// --- Touch: FT6336 on the shared I2C bus -----------------------------------
// FocalTech, and NOT at the usual 0x38 -- Adafruit's square40.py overrides the
// address to 0x48 for this panel, which is the first thing to check if touch
// appears dead.
constexpr gpio_num_t kTouchPinSda = kExpanderPinSda;
constexpr gpio_num_t kTouchPinScl = kExpanderPinScl;
constexpr uint8_t kTouchI2cAddress = 0x48;
constexpr uint32_t kTouchI2cHz = 400000;
/** The interrupt is an expander bit rather than a GPIO, so the driver polls
 *  I2C instead of waiting on an edge. */
constexpr int kTouchNativeSize = 720;

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
constexpr unsigned long kTouchPollIntervalMs = 16;
/** Sample the touch controller on a task of its own rather than from the
 *  loop. On here: the FT6336 is read over Wire,
 *  whose bus is shared with the expander and guarded for exactly this (see
 *  hw::qualia::BusGuard), and this panel's loop is the one with repaints
 *  long enough to swallow a whole tap. */
constexpr bool kTouchSampleTask = true;

// --- Artwork ---------------------------------------------------------------
/** Thumbnail edge in px at kUiBaseSize. Thumbnails sit in list rows, so they
 *  take kListScale as well as kUiScale: 38 becomes 76 here. Ten permanent
 *  sprites of 76^2 * 2 bytes is about 115 KB of PSRAM. */
constexpr int kThumbPx = 38;

/** Edge in px to ask the image proxy for, rounded up to the server's ladder
 *  (0/80/160/256/512/1024).
 *
 *  Deliberately not the panel size. Matching it would mean 1024 px art, and
 *  the cover is drawn as a full-screen backdrop, so the decode is the
 *  expensive part of every compose pass -- four times the pixels of a 512 and
 *  sixteen times a 256, on a panel whose frame already costs a megabyte.
 *  256 upscales 2.8x onto the glass, which is soft against a photograph but
 *  sits behind a scrim with text over it, and buys back both the decode time
 *  and the transfer.
 *
 *  Raise this to 512 if the softness shows; the buffer below has room. */
constexpr int kCoverArtRequestPx = 256;

/**
 * Shortest gap between full repaints of a scrolling list.
 *
 * A full repaint here composes 720x720 and then copies it into the buffer the
 * panel is scanning: a megabyte in and a megabyte out of the same PSRAM that
 * GDMA has to read 24 MB/s from without ever missing a beat. Measured at about
 * 75 ms, which is longer than a frame.
 *
 * The touch panel reports every 16 ms, so a list that repainted on every
 * sample would queue five repaints for every one it could finish, leave the
 * scan-out no bus at all, and break up the very picture it was drawing. This
 * is the ceiling that stops that: the finger is still tracked every sample and
 * only the painting is rationed.
 */
constexpr unsigned long kListRedrawMinMs = 100;

/**
 * Rows to draw before standing off the PSRAM bus for a moment.
 *
 * The scan-out reads this panel's framebuffer continuously and cannot be
 * starved of a byte: an RGB panel has no frame memory, so a FIFO underrun
 * breaks up the whole picture rather than a few lines of it. rgb.cpp pauses
 * between slabs of the copy for that reason, and composing needs the same
 * courtesy: a list repaint is a megabyte of backdrop and then a screenful of
 * rows, and issued as fast as the CPU can manage, nothing lets go of the bus
 * in between.
 *
 * 48 rows, matching the copy. Costs about a millisecond per slab and buys
 * back the frames the scan-out would otherwise drop.
 */
constexpr int kComposeSlabRows = 48;

/** Cover art is cached compressed. Sized well above what kCoverArtRequestPx
 *  asks for -- a 256 px JPEG is tens of KB -- because the headroom is free
 *  against 8 MB of PSRAM, and it means raising the request size is a one-line
 *  change rather than two. Art larger than this streams and decodes from the
 *  socket instead of caching, which costs a re-fetch on every repaint. */
constexpr size_t kCoverArtBufferBytes = 512u * 1024u;

}  // namespace board
