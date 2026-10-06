#include "hardware/lcd28c.h"

#include <Arduino.h>
#include <Wire.h>

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "board/board.h"
#include "log.h"

namespace hw::lcd28c {
namespace {

/** TCA9554 registers. */
constexpr uint8_t kRegOutput = 0x01;
constexpr uint8_t kRegConfig = 0x03;

/** Shadow of the output port: the expander has no read-modify-write. */
uint8_t s_output = board::kExpanderInitialOutput;
bool s_present = false;
SemaphoreHandle_t s_bus = nullptr;

bool writeReg(uint8_t reg, uint8_t value) {
  BusGuard bus;
  Wire.beginTransmission(board::kExpanderI2cAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

void setBit(uint8_t bit, bool high) {
  if (!s_present) {
    return;
  }
  const uint8_t mask = static_cast<uint8_t>(1u << bit);
  s_output = high ? static_cast<uint8_t>(s_output | mask)
                  : static_cast<uint8_t>(s_output & ~mask);
  writeReg(kRegOutput, s_output);
}

/** One ST7701 command: its byte, up to 16 parameters, and a pause after. */
struct InitCmd {
  uint8_t cmd;
  uint8_t len;
  uint8_t delay_ms;
  uint8_t data[16];
};

/**
 * The 2.8C's ST7701 init, byte for byte as Waveshare's demo sends it
 * (ST7701S_screen_init, type 1) and Espressif's board config lists it. The
 * 0xFF commands switch between the controller's register banks -- 0x13,
 * 0x10, 0x11, then back to 0x00 for the standard commands -- and everything
 * between them is the panel vendor's tuning: power, gamma, timing. None of
 * it is to be edited by reasoning about it; if the panel ever needs a
 * change here, it comes from Waveshare.
 *
 * The last four are ordinary: sleep out, which wants 120 ms; 18-bit colour,
 * driven 565 on its top bits; no mirroring; tearing line on; display on.
 */
const InitCmd kInit[] = {
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x13}},
    {0xEF, 1, 0, {0x08}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x10}},
    {0xC0, 2, 0, {0x3B, 0x00}},
    {0xC1, 2, 0, {0x10, 0x0C}},
    {0xC2, 2, 0, {0x07, 0x0A}},
    {0xC7, 1, 0, {0x00}},
    {0xCC, 1, 0, {0x10}},
    {0xCD, 1, 0, {0x08}},
    {0xB0, 16, 0, {0x05, 0x12, 0x98, 0x0E, 0x0F, 0x07, 0x07, 0x09, 0x09, 0x23,
                   0x05, 0x52, 0x0F, 0x67, 0x2C, 0x11}},
    {0xB1, 16, 0, {0x0B, 0x11, 0x97, 0x0C, 0x12, 0x06, 0x06, 0x08, 0x08, 0x22,
                   0x03, 0x51, 0x11, 0x66, 0x2B, 0x0F}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x11}},
    {0xB0, 1, 0, {0x5D}},
    {0xB1, 1, 0, {0x3E}},
    {0xB2, 1, 0, {0x81}},
    {0xB3, 1, 0, {0x80}},
    {0xB5, 1, 0, {0x4E}},
    {0xB7, 1, 0, {0x85}},
    {0xB8, 1, 0, {0x20}},
    {0xC1, 1, 0, {0x78}},
    {0xC2, 1, 0, {0x78}},
    {0xD0, 1, 0, {0x88}},
    {0xE0, 3, 0, {0x00, 0x00, 0x02}},
    {0xE1, 11, 0, {0x06, 0x30, 0x08, 0x30, 0x05, 0x30, 0x07, 0x30, 0x00, 0x33,
                   0x33}},
    {0xE2, 12, 0, {0x11, 0x11, 0x33, 0x33, 0xF4, 0x00, 0x00, 0x00, 0xF4, 0x00,
                   0x00, 0x00}},
    {0xE3, 4, 0, {0x00, 0x00, 0x11, 0x11}},
    {0xE4, 2, 0, {0x44, 0x44}},
    {0xE5, 16, 0, {0x0D, 0xF5, 0x30, 0xF0, 0x0F, 0xF7, 0x30, 0xF0, 0x09, 0xF1,
                   0x30, 0xF0, 0x0B, 0xF3, 0x30, 0xF0}},
    {0xE6, 4, 0, {0x00, 0x00, 0x11, 0x11}},
    {0xE7, 2, 0, {0x44, 0x44}},
    {0xE8, 16, 0, {0x0C, 0xF4, 0x30, 0xF0, 0x0E, 0xF6, 0x30, 0xF0, 0x08, 0xF0,
                   0x30, 0xF0, 0x0A, 0xF2, 0x30, 0xF0}},
    {0xE9, 2, 0, {0x36, 0x01}},
    {0xEB, 7, 0, {0x00, 0x01, 0xE4, 0xE4, 0x44, 0x88, 0x40}},
    {0xED, 16, 0, {0xFF, 0x10, 0xAF, 0x76, 0x54, 0x2B, 0xCF, 0xFF, 0xFF, 0xFC,
                   0xB2, 0x45, 0x67, 0xFA, 0x01, 0xFF}},
    {0xEF, 6, 0, {0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}},
    {0xFF, 5, 0, {0x77, 0x01, 0x00, 0x00, 0x00}},
    {0x11, 0, 120, {}},
    {0x3A, 1, 0, {0x66}},
    {0x36, 1, 0, {0x00}},
    {0x35, 1, 0, {0x00}},
    {0x29, 0, 0, {}},
};

/** One 9-bit word: the data/command bit as a one-bit command phase -- 0 for
 *  a command, 1 for a parameter -- and the byte as an eight-bit address
 *  phase, which is how Waveshare's demo gets nine bits out of the SPI
 *  peripheral. Polling: each is a single word, and a queued transaction
 *  would cost more than it carries. */
void sendWord(spi_device_handle_t dev, bool data, uint8_t byte) {
  spi_transaction_t t = {};
  t.cmd = data ? 1 : 0;
  t.addr = byte;
  spi_device_polling_transmit(dev, &t);
}

}  // namespace

BusGuard::BusGuard() {
  if (s_bus != nullptr) {
    xSemaphoreTake(s_bus, portMAX_DELAY);
  }
}

BusGuard::~BusGuard() {
  if (s_bus != nullptr) {
    xSemaphoreGive(s_bus);
  }
}

bool expanderInit() {
  if (s_bus == nullptr) {
    s_bus = xSemaphoreCreateMutex();
  }
  Wire.begin(static_cast<int>(board::kI2cPinSda),
             static_cast<int>(board::kI2cPinScl), board::kI2cHz);

  Wire.beginTransmission(board::kExpanderI2cAddress);
  if (Wire.endTransmission() != 0) {
    LOG_ERROR("2.8C: TCA9554 not found at 0x%02X -- no panel, no touch",
              board::kExpanderI2cAddress);
    s_present = false;
    return false;
  }
  s_present = true;

  // The port first, then the direction, so no line is briefly driven to
  // whatever the port held -- the power-on state is every output high, and
  // one of them is the buzzer.
  s_output = board::kExpanderInitialOutput;
  writeReg(kRegOutput, s_output);
  writeReg(kRegConfig, board::kExpanderConfig);
  LOG_INFO("2.8C: TCA9554 up");
  return true;
}

bool panelInit() {
  // Reset, as Waveshare's demo does it, and then the 100 ms it waits before
  // the first command.
  setBit(board::kExpanderBitLcdReset, false);
  delay(10);
  setBit(board::kExpanderBitLcdReset, true);
  delay(100);

  spi_bus_config_t bus = {};
  bus.mosi_io_num = static_cast<int>(board::kPanelSpiPinSda);
  bus.miso_io_num = -1;
  bus.sclk_io_num = static_cast<int>(board::kPanelSpiPinScl);
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 16;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED) != ESP_OK) {
    LOG_ERROR("2.8C: SPI bus for the ST7701 init would not start");
    return false;
  }
  spi_device_interface_config_t dev_cfg = {};
  dev_cfg.command_bits = 1;
  dev_cfg.address_bits = 8;
  dev_cfg.mode = 0;
  dev_cfg.clock_speed_hz = static_cast<int>(board::kPanelSpiHz);
  dev_cfg.spics_io_num = -1;  // on the expander
  dev_cfg.queue_size = 1;
  spi_device_handle_t dev = nullptr;
  if (spi_bus_add_device(SPI2_HOST, &dev_cfg, &dev) != ESP_OK) {
    spi_bus_free(SPI2_HOST);
    LOG_ERROR("2.8C: SPI device for the ST7701 init would not attach");
    return false;
  }

  // Chip select held low across the whole sequence, as the demo holds it.
  setBit(board::kExpanderBitLcdCs, false);
  for (const InitCmd& c : kInit) {
    sendWord(dev, false, c.cmd);
    for (uint8_t i = 0; i < c.len; ++i) {
      sendWord(dev, true, c.data[i]);
    }
    if (c.delay_ms > 0) {
      delay(c.delay_ms);
    }
  }
  setBit(board::kExpanderBitLcdCs, true);

  // Done with the pins: they are the SD slot's too.
  spi_bus_remove_device(dev);
  spi_bus_free(SPI2_HOST);
  LOG_INFO("2.8C: ST7701 initialised");
  return true;
}

void touchReset() {
  // INT low through the reset and a moment after it -- the GT911 samples it
  // as reset is released to choose its address -- and then an input again,
  // since the controller drives it from then on.
  const gpio_num_t intr = board::kTouchPinInt;
  pinMode(static_cast<int>(intr), OUTPUT);
  digitalWrite(static_cast<int>(intr), LOW);
  delay(10);
  setBit(board::kExpanderBitTouchReset, false);
  delay(10);
  setBit(board::kExpanderBitTouchReset, true);
  delay(10);
  pinMode(static_cast<int>(intr), INPUT);
  // The GT911 answers nothing for its first 50 ms out of reset.
  delay(60);
}

}  // namespace hw::lcd28c
