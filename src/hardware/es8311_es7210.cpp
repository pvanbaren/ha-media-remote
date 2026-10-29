#include "hardware/audio_codec.h"

// The headless build has no board; the other boards without these codecs
// leave BOARD_AUDIO_ES8311_ES7210 undefined, and this file is empty for them.
#if defined(BOARD_HEADER)
#include "board/board.h"
#endif

#if defined(BOARD_AUDIO_ES8311_ES7210)

#include <Arduino.h>

#include "hardware/i2c_bus.h"
#include "log.h"

/**
 * An ES8311 driving the speaker and an ES7210 reading the microphones, both
 * I2S slaves at 16 kHz with MCLK at 256 x fs, 4.096 MHz, set up over the
 * touch controller's I2C bus: the pair the AMOLED 1.75 and the P4 4B both
 * have, at the same addresses. Neither has a driver in the Arduino
 * framework (Espressif's esp_codec_dev is IDF-only), so the registers are
 * written by hand. The sequences are Waveshare's es8311.c and Espressif's
 * esp_codec_dev es7210.c, cut down to the one rate and format used here; run
 * on the AMOLED on 2026-10-14, where speech came back clear and Home
 * Assistant understood it.
 */
namespace hw::audio::codec {
namespace {

constexpr uint8_t kDac = board::kAudioDacI2cAddress;
constexpr uint8_t kAdc = board::kAudioAdcI2cAddress;

/** The ES7210's PGA: 3 dB a step from 0 dB at 0x00 to 30 dB at 0x0A, then
 *  1.5 dB steps to 37.5 dB at 0x0E, its most. hw::audio adds 12 dB in
 *  software on top; together, commands spoken in testing peaked at around
 *  a third of full scale. */
constexpr uint8_t kMicGain = 0x0E;

bool wr(uint8_t dev, uint8_t reg, uint8_t val) {
  return i2c::writeReg8(dev, reg, val);
}

bool upd(uint8_t dev, uint8_t reg, uint8_t mask, uint8_t val) {
  return i2c::updateReg8(dev, reg, mask, val);
}

uint8_t volumeRegister(int percent) {
  percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
  return static_cast<uint8_t>(percent == 0 ? 0 : percent * 256 / 100 - 1);
}

bool es8311Init(int volume_percent) {
  i2c::Lock lock;
  bool ok = true;
  ok &= wr(kDac, 0x00, 0x1F);  // reset
  delay(20);
  ok &= wr(kDac, 0x00, 0x00);
  ok &= wr(kDac, 0x00, 0x80);  // power on
  ok &= wr(kDac, 0x01, 0x3F);  // MCLK from its pin, every clock on
  ok &= upd(kDac, 0x06, 0x20, 0x00);  // SCLK not inverted
  // 4.096 MHz MCLK to 16 kHz: pre-divide 1, multiply 1, ADC and DAC divide
  // 1, single speed, LRCK divide 0x00FF, BCLK divide 4, oversampling 0x10.
  ok &= upd(kDac, 0x02, 0xF8, 0x00);
  ok &= wr(kDac, 0x03, 0x10);
  ok &= wr(kDac, 0x04, 0x10);
  ok &= wr(kDac, 0x05, 0x00);
  ok &= upd(kDac, 0x06, 0x1F, 0x03);
  ok &= upd(kDac, 0x07, 0x3F, 0x00);
  ok &= wr(kDac, 0x08, 0xFF);
  ok &= upd(kDac, 0x00, 0x40, 0x00);  // slave
  ok &= wr(kDac, 0x09, 0x0C);  // serial in: I2S, 16-bit
  ok &= wr(kDac, 0x0A, 0x0C);  // serial out: I2S, 16-bit
  ok &= wr(kDac, 0x0D, 0x01);  // analogue up
  ok &= wr(kDac, 0x0E, 0x02);  // PGA and ADC modulator
  ok &= wr(kDac, 0x12, 0x00);  // DAC up
  ok &= wr(kDac, 0x13, 0x10);  // output driver
  ok &= wr(kDac, 0x1C, 0x6A);
  ok &= wr(kDac, 0x37, 0x08);  // DAC equaliser bypassed
  ok &= wr(kDac, 0x32, volumeRegister(volume_percent));
  return ok;
}

/** MIC1 and MIC2 powered and routed, as the setup does twice over. */
bool es7210Mics() {
  bool ok = true;
  for (uint8_t r = 0x43; r <= 0x46; ++r) {
    ok &= upd(kAdc, r, 0x10, 0x00);
  }
  ok &= wr(kAdc, 0x4B, 0xFF);
  ok &= wr(kAdc, 0x4C, 0xFF);
  ok &= upd(kAdc, 0x01, 0x0B, 0x00);
  ok &= wr(kAdc, 0x4B, 0x00);
  ok &= upd(kAdc, 0x43, 0x10, 0x10);
  ok &= upd(kAdc, 0x44, 0x10, 0x10);
  ok &= wr(kAdc, 0x12, 0x00);
  return ok;
}

bool es7210Init() {
  i2c::Lock lock;
  bool ok = true;
  ok &= wr(kAdc, 0x00, 0xFF);  // reset
  ok &= wr(kAdc, 0x00, 0x41);
  ok &= wr(kAdc, 0x01, 0x3F);
  ok &= wr(kAdc, 0x09, 0x30);
  ok &= wr(kAdc, 0x0A, 0x30);
  ok &= wr(kAdc, 0x23, 0x2A);
  ok &= wr(kAdc, 0x22, 0x0A);
  ok &= wr(kAdc, 0x20, 0x0A);
  ok &= wr(kAdc, 0x21, 0x2A);
  ok &= upd(kAdc, 0x08, 0x01, 0x00);  // slave
  ok &= wr(kAdc, 0x40, 0x43);
  ok &= wr(kAdc, 0x41, 0x70);  // microphone bias 2.87 V
  ok &= wr(kAdc, 0x42, 0x70);
  ok &= wr(kAdc, 0x07, 0x20);
  ok &= wr(kAdc, 0x02, 0xC1);
  // Standard I2S, not TDM: MIC1 on the left, MIC2 on the right.
  ok &= es7210Mics();
  for (uint8_t r = 0x43; r <= 0x46; ++r) {
    ok &= upd(kAdc, r, 0x0F, kMicGain);
  }
  uint8_t clocks = 0x34;
  i2c::readReg8(kAdc, 0x01, clocks);
  ok &= upd(kAdc, 0x11, 0xE0, 0x60);  // 16-bit
  ok &= upd(kAdc, 0x11, 0x03, 0x00);  // I2S
  // Start.
  ok &= wr(kAdc, 0x01, clocks);
  ok &= wr(kAdc, 0x06, 0x00);
  ok &= wr(kAdc, 0x40, 0x43);
  for (uint8_t r = 0x47; r <= 0x4A; ++r) {
    ok &= wr(kAdc, r, 0x08);
  }
  ok &= es7210Mics();
  ok &= wr(kAdc, 0x40, 0x43);
  ok &= wr(kAdc, 0x00, 0x71);
  ok &= wr(kAdc, 0x00, 0x41);
  return ok;
}

}  // namespace

bool init(int volume_percent) {
  pinMode(board::kAudioPinSpeakerEnable, OUTPUT);
  digitalWrite(board::kAudioPinSpeakerEnable, LOW);
  // The touch controller usually started the bus already; idempotent.
  if (!i2c::begin(board::kTouchPinSda, board::kTouchPinScl,
                  board::kTouchI2cHz)) {
    return false;
  }
  // Their ids first, which only the real parts give: ES8311 0x83 0x11 at
  // 0xFD/0xFE, ES7210 0x72 0x10 at 0x3D/0x3E.
  uint8_t dac_id = 0;
  uint8_t adc_id = 0;
  i2c::readReg8(kDac, 0xFE, dac_id);
  i2c::readReg8(kAdc, 0x3E, adc_id);
  if (dac_id != 0x11 || adc_id != 0x10) {
    LOG_WARN("AUDIO: codec ids %02X %02X, not the ES8311's and ES7210's",
             dac_id, adc_id);
    return false;
  }
  const bool dac = es8311Init(volume_percent);
  const bool adc = es7210Init();
  if (!dac || !adc) {
    LOG_WARN("AUDIO: codec setup failed (ES8311 %s, ES7210 %s)",
             dac ? "ok" : "failed", adc ? "ok" : "failed");
    return false;
  }
  return true;
}

void setVolume(int percent) {
  wr(kDac, 0x32, volumeRegister(percent));
}

void speaker(bool on) {
  digitalWrite(board::kAudioPinSpeakerEnable, on ? HIGH : LOW);
}

}  // namespace hw::audio::codec

#endif  // BOARD_AUDIO_ES8311_ES7210
