#include "hardware/i2c_bus.h"

#include <Arduino.h>
#include <Wire.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "log.h"

namespace hw::i2c {
namespace {

/** Longer than any transaction this firmware makes at 400 kHz -- a touch
 *  report is 17 bytes, under half a millisecond -- and short enough that a
 *  device holding the bus costs the loop little. */
constexpr uint16_t kTimeoutMs = 20;

SemaphoreHandle_t s_mutex = nullptr;
bool s_ready = false;
int s_sda = -1;
int s_scl = -1;
uint32_t s_hz = 0;
uint32_t s_failures = 0;

/** A failure, logged while they are rare and then only every hundredth, so a
 *  device that has gone away cannot drown the console. */
void noteFailure(const char* what, uint8_t addr) {
  ++s_failures;
  if (s_failures <= 5 || s_failures % 100 == 0) {
    LOG_WARN("I2C: %s 0x%02X failed (%lu so far)", what, addr,
             static_cast<unsigned long>(s_failures));
  }
}

bool writeOnce(uint8_t addr, const uint8_t* data, size_t len) {
  Wire.beginTransmission(addr);
  if (len > 0 && Wire.write(data, len) != len) {
    Wire.endTransmission();
    return false;
  }
  return Wire.endTransmission() == 0;
}

bool writeReadOnce(uint8_t addr, const uint8_t* w, size_t wlen, uint8_t* out,
                   size_t rlen) {
  Wire.beginTransmission(addr);
  if (wlen > 0 && Wire.write(w, wlen) != wlen) {
    Wire.endTransmission();
    return false;
  }
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(static_cast<int>(addr), static_cast<int>(rlen)) !=
      static_cast<int>(rlen)) {
    return false;
  }
  for (size_t i = 0; i < rlen; ++i) {
    out[i] = static_cast<uint8_t>(Wire.read());
  }
  return true;
}

}  // namespace

bool begin(int sda, int scl, uint32_t hz) {
  if (s_ready) {
    if (sda == s_sda && scl == s_scl && hz == s_hz) {
      return true;
    }
    LOG_ERROR("I2C: already running on %d/%d at %lu Hz, not %d/%d at %lu Hz",
              s_sda, s_scl, static_cast<unsigned long>(s_hz), sda, scl,
              static_cast<unsigned long>(hz));
    return false;
  }
  if (s_mutex == nullptr) {
    s_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_mutex == nullptr) {
      LOG_ERROR("I2C: no memory for the bus lock");
      return false;
    }
  }
  if (!Wire.begin(sda, scl, hz)) {
    LOG_ERROR("I2C: Wire would not start on %d/%d", sda, scl);
    return false;
  }
  Wire.setTimeOut(kTimeoutMs);
  s_sda = sda;
  s_scl = scl;
  s_hz = hz;
  s_ready = true;
  LOG_INFO("I2C: bus on SDA %d, SCL %d at %lu kHz", sda, scl,
           static_cast<unsigned long>(hz / 1000));
  return true;
}

bool ready() { return s_ready; }

Lock::Lock() {
  if (s_mutex != nullptr) {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
  }
}

Lock::~Lock() {
  if (s_mutex != nullptr) {
    xSemaphoreGiveRecursive(s_mutex);
  }
}

bool write(uint8_t addr, const uint8_t* data, size_t len) {
  if (!s_ready) {
    return false;
  }
  Lock lock;
  // Once more on failure: the driver's state after a NACK elsewhere can fail
  // the next transaction once, and a busy device may miss one.
  if (writeOnce(addr, data, len) || writeOnce(addr, data, len)) {
    return true;
  }
  noteFailure("write to", addr);
  return false;
}

bool writeRead(uint8_t addr, const uint8_t* w, size_t wlen, uint8_t* out,
               size_t rlen) {
  if (!s_ready) {
    return false;
  }
  Lock lock;
  if (writeReadOnce(addr, w, wlen, out, rlen) ||
      writeReadOnce(addr, w, wlen, out, rlen)) {
    return true;
  }
  noteFailure("read from", addr);
  return false;
}

bool writeReg8(uint8_t addr, uint8_t reg, uint8_t value) {
  const uint8_t data[2] = {reg, value};
  return write(addr, data, sizeof(data));
}

bool readReg8(uint8_t addr, uint8_t reg, uint8_t& value) {
  return writeRead(addr, &reg, 1, &value, 1);
}

bool updateReg8(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t value) {
  Lock lock;
  uint8_t current = 0;
  if (!readReg8(addr, reg, current)) {
    return false;
  }
  return writeReg8(addr, reg,
                   static_cast<uint8_t>((current & ~mask) | (value & mask)));
}

uint32_t failures() { return s_failures; }

}  // namespace hw::i2c
