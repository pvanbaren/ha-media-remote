#include "services/player_list.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <new>

#include "config.h"

namespace services::players {
namespace {

/** PSRAM, claimed on the first refresh and kept: kMaxPlayers entries are
 *  about ten kilobytes, which internal RAM -- the TLS handshake's -- can do
 *  without. Internal RAM on a board with no PSRAM. */
ha::PlayerEntry* s_entries = nullptr;
int s_count = 0;
unsigned long s_fetched_ms = 0;

}  // namespace

bool refresh() {
  if (s_entries == nullptr) {
    void* memory = heap_caps_calloc_prefer(
        config::kMaxPlayers, sizeof(ha::PlayerEntry), 2,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
    if (memory == nullptr) {
      return false;
    }
    s_entries = static_cast<ha::PlayerEntry*>(memory);
    for (size_t i = 0; i < config::kMaxPlayers; ++i) {
      new (&s_entries[i]) ha::PlayerEntry();
    }
  }
  // fetchPlayers leaves the buffer untouched unless the whole request
  // succeeded, so the previous list survives a failed refresh and there is no
  // need to stage a copy on the caller's stack.
  const int count = ha::fetchPlayers(s_entries, config::kMaxPlayers);
  if (count < 0) {
    return false;
  }
  s_count = count;
  s_fetched_ms = millis();
  return true;
}

bool stale() {
  return s_count == 0 || millis() - s_fetched_ms >= config::kHaPlayerListTtlMs;
}

bool ensureEntries() {
  if (stale()) {
    refresh();
  }
  return s_count > 0;
}

int entryCount() { return s_count; }

const ha::PlayerEntry* entryAt(int index) {
  if (index < 0 || index >= s_count) {
    return nullptr;
  }
  return &s_entries[index];
}

}  // namespace services::players
