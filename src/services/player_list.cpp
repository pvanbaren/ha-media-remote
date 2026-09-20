#include "services/player_list.h"

#include <Arduino.h>

#include "config.h"

namespace services::players {
namespace {

ha::PlayerEntry s_entries[config::kMaxPlayers];
int s_count = 0;
unsigned long s_fetched_ms = 0;

}  // namespace

bool refresh() {
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
