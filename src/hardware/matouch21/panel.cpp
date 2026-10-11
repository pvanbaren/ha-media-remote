#include "hardware/matouch21.h"

#include <Arduino.h>


#include "board/board.h"
#include "log.h"

namespace hw::matouch21 {
namespace {

/** One ST7701 command: its byte, up to 16 parameters, and a pause after. */
struct InitCmd {
  uint8_t cmd;
  uint8_t len;
  uint8_t delay_ms;
  uint8_t data[16];
};

/**
 * The MaTouch 2.1"'s ST7701 init, byte for byte as Arduino_GFX's
 * st7701_type5_init_operations sends it -- the init Makerfabs' own factory
 * test uses for this panel. The 0xFF commands switch the controller's
 * register banks (0x10, 0x11, 0x13, then back to 0x00 for the standard
 * commands); everything between them is the panel vendor's tuning -- power,
 * gamma, timing -- and is not to be edited by reasoning about it.
 *
 * Ahead of it a software reset, since no reset line reaches the panel and a
 * warm restart would otherwise find it still set up; and at the end the
 * ordinary ones: BGR order, 18-bit colour driven 565 on its top bits, sleep
 * out and its wait, display on.
 */
const InitCmd kInit[] = {
    {0x01, 0, 120, {}},  // software reset
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x10}},
    {0xC0, 2, 0, {0x3B, 0x00}},
    {0xC1, 2, 0, {0x0B, 0x02}},
    {0xC2, 2, 0, {0x00, 0x02}},
    {0xCC, 1, 0, {0x10}},
    {0xCD, 1, 0, {0x08}},
    {0xB0, 16, 0, {0x02, 0x13, 0x1B, 0x0D, 0x10, 0x05, 0x08, 0x07, 0x07, 0x24,
                   0x04, 0x11, 0x0E, 0x2C, 0x33, 0x1D}},
    {0xB1, 16, 0, {0x05, 0x13, 0x1B, 0x0D, 0x11, 0x05, 0x08, 0x07, 0x07, 0x24,
                   0x04, 0x11, 0x0E, 0x2C, 0x33, 0x1D}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x11}},
    {0xB0, 1, 0, {0x5D}},
    {0xB1, 1, 0, {0x43}},
    {0xB2, 1, 0, {0x81}},
    {0xB3, 1, 0, {0x80}},
    {0xB5, 1, 0, {0x43}},
    {0xB7, 1, 0, {0x85}},
    {0xB8, 1, 0, {0x20}},
    {0xC1, 1, 0, {0x78}},
    {0xC2, 1, 0, {0x78}},
    {0xD0, 1, 0, {0x88}},
    {0xE0, 3, 0, {0x00, 0x00, 0x02}},
    {0xE1, 11, 0, {0x03, 0xA0, 0x00, 0x00, 0x04, 0xA0, 0x00, 0x00, 0x00, 0x20,
                   0x20}},
    {0xE2, 13, 0, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00}},
    {0xE3, 4, 0, {0x00, 0x00, 0x11, 0x00}},
    {0xE4, 2, 0, {0x22, 0x00}},
    {0xE5, 16, 0, {0x05, 0xEC, 0xA0, 0xA0, 0x07, 0xEE, 0xA0, 0xA0, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xE6, 4, 0, {0x00, 0x00, 0x11, 0x00}},
    {0xE7, 2, 0, {0x22, 0x00}},
    {0xE8, 16, 0, {0x06, 0xED, 0xA0, 0xA0, 0x08, 0xEF, 0xA0, 0xA0, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0xEB, 7, 0, {0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00}},
    {0xED, 16, 0, {0xFF, 0xFF, 0xFF, 0xBA, 0x0A, 0xBF, 0x45, 0xFF, 0xFF, 0x54,
                   0xFB, 0xA0, 0xAB, 0xFF, 0xFF, 0xFF}},
    {0xEF, 6, 0, {0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x13}},
    {0xEF, 1, 0, {0x08}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x00}},
    // BGR order and 18-bit colour, as Arduino_GFX 1.3.8 sends them -- the
    // library Makerfabs' factory test is built with, which shows true colours
    // on this glass. They hold only with the pins in that build's order:
    // board::kRgbDataPins.
    {0x36, 1, 0, {0x08}},
    {0x3A, 1, 0, {0x60}},
    {0x11, 0, 100, {}},
    {0x29, 0, 50, {}},
};

/** One 9-bit word, bit-banged as Arduino_SWSPI sends it in Makerfabs' build:
 *  the data/command bit -- 0 for a command, 1 for a parameter -- then the
 *  byte, most significant bit first, each bit set up with the clock low and
 *  taken on its rising edge. By hand rather than through the SPI peripheral:
 *  through it, at 4 MHz the colours came out wrong and at 500 kHz the panel
 *  stayed dark, and this is the one way the panel is known to take it. */
void sendWord(bool data, uint8_t byte) {
  const int sda = static_cast<int>(board::kPanelSpiPinSda);
  const int scl = static_cast<int>(board::kPanelSpiPinScl);
  digitalWrite(sda, data ? HIGH : LOW);
  digitalWrite(scl, HIGH);
  digitalWrite(scl, LOW);
  for (uint8_t bit = 0x80; bit != 0; bit >>= 1) {
    digitalWrite(sda, (byte & bit) != 0 ? HIGH : LOW);
    digitalWrite(scl, HIGH);
    digitalWrite(scl, LOW);
  }
}

}  // namespace

bool panelInit() {
  const int cs = static_cast<int>(board::kPanelSpiPinCs);
  const int sda = static_cast<int>(board::kPanelSpiPinSda);
  const int scl = static_cast<int>(board::kPanelSpiPinScl);
  pinMode(cs, OUTPUT);
  pinMode(sda, OUTPUT);
  pinMode(scl, OUTPUT);
  digitalWrite(cs, HIGH);
  digitalWrite(scl, LOW);

  // Chip select framed as Makerfabs' build frames it: low through a run of
  // commands, high over each pause -- the reset's, sleep-out's -- and low
  // again after.
  digitalWrite(cs, LOW);
  for (const InitCmd& c : kInit) {
    sendWord(false, c.cmd);
    for (uint8_t i = 0; i < c.len; ++i) {
      sendWord(true, c.data[i]);
    }
    if (c.delay_ms > 0) {
      digitalWrite(cs, HIGH);
      delay(c.delay_ms);
      digitalWrite(cs, LOW);
    }
  }
  digitalWrite(cs, HIGH);

  // GPIO 0 was the data line; it is the BOOT button again from here.
  pinMode(sda, INPUT_PULLUP);
  LOG_INFO("MaTouch: ST7701 initialised");
  return true;
}

}  // namespace hw::matouch21
