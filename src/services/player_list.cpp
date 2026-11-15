#include "services/player_list.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <cstring>
#include <new>

#include "config.h"

namespace services::players {
namespace {

/** PSRAM, claimed on the first refresh and kept: kMaxPlayers entries are
 *  over ten kilobytes, which internal RAM -- the TLS handshake's -- can do
 *  without. Internal RAM on a board with no PSRAM. */
ha::PlayerEntry* s_entries = nullptr;
int s_count = 0;
unsigned long s_fetched_ms = 0;
/** The Volume & power device the list was fetched for, which its Volume &
 *  power list offers whatever it supports. */
char s_fetched_control[config::kEntityIdMaxLen] = {};

bool offered(const ha::PlayerEntry& entry, Role role) {
  return entry.role == 'b' ||
         entry.role == (role == Role::kControl ? 'c' : 'p');
}

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
  char control[config::kEntityIdMaxLen];
  snprintf(control, sizeof(control), "%s", ha::storedControlEntity());
  const int count = ha::fetchPlayers(s_entries, config::kMaxPlayers, control);
  if (count < 0) {
    return false;
  }
  s_count = count;
  s_fetched_ms = millis();
  memcpy(s_fetched_control, control, sizeof(s_fetched_control));
  return true;
}

bool stale() {
  return s_count == 0 ||
         millis() - s_fetched_ms >= config::kHaPlayerListTtlMs ||
         strcmp(s_fetched_control, ha::storedControlEntity()) != 0;
}

bool ensureEntries() {
  if (stale()) {
    refresh();
  }
  return s_count > 0;
}

int entryCount(Role role) {
  int n = 0;
  for (int i = 0; i < s_count; ++i) {
    n += offered(s_entries[i], role) ? 1 : 0;
  }
  return n;
}

const ha::PlayerEntry* entryAt(Role role, int index) {
  if (index < 0) {
    return nullptr;
  }
  // Volume & power in two passes, the devices with inputs first, each by
  // name as fetched; the media player in one.
  const int passes = role == Role::kControl ? 2 : 1;
  for (int pass = 0; pass < passes; ++pass) {
    for (int i = 0; i < s_count; ++i) {
      const ha::PlayerEntry& entry = s_entries[i];
      if (!offered(entry, role) ||
          (passes == 2 && entry.inputs != (pass == 0))) {
        continue;
      }
      if (index-- == 0) {
        return &s_entries[i];
      }
    }
  }
  return nullptr;
}

}  // namespace services::players
