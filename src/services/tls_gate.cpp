#include "services/tls_gate.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"
#include "log.h"

namespace services::tls {
namespace {

/** Recursive, so that a client whose connect calls another of its own
 *  overloads, or a task that somehow nests two, cannot deadlock itself. */
SemaphoreHandle_t lock() {
  static StaticSemaphore_t storage;
  static SemaphoreHandle_t handle = xSemaphoreCreateRecursiveMutexStatic(&storage);
  return handle;
}

size_t internalFree() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

}  // namespace

void begin() {
  // Bounded, as a handshake is: one that hangs for its whole timeout must not
  // stop every other connection for good.
  const unsigned long asked_ms = millis();
  if (xSemaphoreTakeRecursive(lock(), pdMS_TO_TICKS(config::kTlsQueueWaitMs)) !=
      pdTRUE) {
    LOG_WARN("TLS: another handshake held on for %lu ms; going ahead",
             millis() - asked_ms);
    return;  // end() gives back only what was taken
  }
  const unsigned long room_from = millis();
  while (internalFree() < config::kTlsRoomBytes &&
         millis() - room_from < config::kTlsRoomWaitMs) {
    delay(20);
  }
  const unsigned long waited = millis() - asked_ms;
  if (waited >= 200) {
    LOG_INFO("TLS: waited %lu ms to start a handshake (internal %u free)",
             waited, static_cast<unsigned>(internalFree()));
  }
}

void end() {
  // Only the holder gives it back; one that went ahead without it does not.
  if (xSemaphoreGetMutexHolder(lock()) == xTaskGetCurrentTaskHandle()) {
    xSemaphoreGiveRecursive(lock());
  }
}

}  // namespace services::tls
