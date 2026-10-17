#include "hardware/guition4848.h"

#include <Arduino.h>

#include "board/board.h"
#include "log.h"

namespace hw::guition4848 {
namespace {

/** One ST7701 command: its byte, up to 16 parameters, and a pause after. */
struct InitCmd {
  uint8_t cmd;
  uint8_t len;
  uint8_t delay_ms;
  uint8_t data[16];
};

/**
 * The 4848S040's ST7701 init, byte for byte as the factory demo on this unit
 * sends it -- decoded from the Arduino_GFX operations table in its flash.
 * That is Arduino_GFX's st7701_type9_init_operations, the library's init for
 * this board, but for one 10 ms pause after the colour mode, which the
 * demo's older copy of the library has and the current one does not. The
 * 0xFF commands switch the controller's register banks (0x10, 0x11, 0x13,
 * then back to 0x00 for the standard commands); everything between them is
 * the panel vendor's tuning -- power, gamma, timing -- and is not to be
 * edited by reasoning about it.
 *
 * Ahead of it a software reset, as Arduino_GFX sends when there is no reset
 * line -- and there is none here, so a warm restart would otherwise find the
 * panel still set up -- but sent on its own by panelReset(), before the RGB
 * stream starts. At the end the ordinary ones: 18-bit colour driven 565 on
 * its top bits, sleep out and its wait, display on.
 */
const InitCmd kInit[] = {
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x10}},
    {0xC0, 2, 0, {0x3B, 0x00}},
    {0xC1, 2, 0, {0x0D, 0x02}},
    {0xC2, 2, 0, {0x31, 0x05}},
    {0xCD, 1, 0, {0x00}},
    {0xB0, 16, 0, {0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22,
                   0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18}},
    {0xB1, 16, 0, {0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22,
                   0x04, 0x11, 0x11, 0xA9, 0x32, 0x18}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x11}},
    {0xB0, 1, 0, {0x60}},
    {0xB1, 1, 0, {0x32}},
    {0xB2, 1, 0, {0x07}},
    {0xB3, 1, 0, {0x80}},
    {0xB5, 1, 0, {0x49}},
    {0xB7, 1, 0, {0x85}},
    {0xB8, 1, 0, {0x21}},
    {0xC1, 1, 0, {0x78}},
    {0xC2, 1, 0, {0x78}},
    {0xE0, 3, 0, {0x00, 0x1B, 0x02}},
    {0xE1, 11, 0, {0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44,
                   0x44}},
    {0xE2, 12, 0, {0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0,
                   0x00, 0x00}},
    {0xE3, 4, 0, {0x00, 0x00, 0x11, 0x11}},
    {0xE4, 2, 0, {0x44, 0x44}},
    {0xE5, 16, 0, {0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED,
                   0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}},
    {0xE6, 4, 0, {0x00, 0x00, 0x11, 0x11}},
    {0xE7, 2, 0, {0x44, 0x44}},
    {0xE8, 16, 0, {0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC,
                   0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}},
    {0xEB, 7, 0, {0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}},
    {0xEC, 2, 0, {0x3C, 0x00}},
    {0xED, 16, 0, {0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                   0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x13}},
    {0xE5, 1, 0, {0xE4}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x00}},
    {0x3A, 1, 10, {0x60}},
    {0x11, 0, 120, {}},
    {0x29, 0, 0, {}},
};

/** One 9-bit word, bit-banged as Arduino_SWSPI sends it: the data/command
 *  bit -- 0 for a command, 1 for a parameter -- then the byte, most
 *  significant bit first, each bit set up with the clock low and taken on
 *  its rising edge. By hand, as on the MaTouch, where the SPI peripheral
 *  could not be made to send what the panel takes. */
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

/** The 3-wire bus's pins as outputs, chip select high. */
void beginPins() {
  pinMode(static_cast<int>(board::kPanelSpiPinCs), OUTPUT);
  pinMode(static_cast<int>(board::kPanelSpiPinSda), OUTPUT);
  pinMode(static_cast<int>(board::kPanelSpiPinScl), OUTPUT);
  digitalWrite(static_cast<int>(board::kPanelSpiPinCs), HIGH);
  digitalWrite(static_cast<int>(board::kPanelSpiPinScl), LOW);
}

}  // namespace

void panelReset() {
  const int cs = static_cast<int>(board::kPanelSpiPinCs);
  beginPins();
  digitalWrite(cs, LOW);
  sendWord(false, 0x01);  // software reset
  digitalWrite(cs, HIGH);
  delay(120);
}

bool panelInit() {
  const int cs = static_cast<int>(board::kPanelSpiPinCs);
  beginPins();

  // Chip select low through a run of commands, high over each pause.
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
  // And high for good: the clock and data lines are the SD card's too.
  digitalWrite(cs, HIGH);
  LOG_INFO("4848S040: ST7701 initialised");
  return true;
}

}  // namespace hw::guition4848
