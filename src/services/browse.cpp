#include "services/browse.h"

#include <Arduino.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstring>

#include "config.h"
#include "log.h"

namespace services::browse {
namespace {

ha::LibraryItem s_items[config::kMaxBrowseItems];
/** Staged here rather than on the caller's stack: ten items are ~3 KB, and
 *  refresh() runs on the Arduino loop task alongside an open TLS connection. */
ha::LibraryItem s_fetching[config::kMaxBrowseItems];
int s_count = 0;
Problem s_problem = Problem::kNotLoaded;

/** Guards everything above and the sprites below.
 *
 *  The poll task loads this in the background while the Arduino loop draws
 *  from it, which is the whole point of loading it in the background -- so the
 *  two have to be kept apart. Held for the duration of a load, which is
 *  seconds; the UI side only ever waits that long if it asks for a list at the
 *  exact moment one is being fetched, which is also the only moment it would
 *  have had to wait anyway. */
SemaphoreHandle_t s_lock = nullptr;
std::atomic<bool> s_busy{false};

/** Scoped lock. A no-op before init(), so nothing has to check.
 *
 *  Recursive, so that the UI can hold a ReadGuard across a whole frame and
 *  the per-call locks inside rowAt() and drawThumb() still nest. */
struct Guard {
  Guard() {
    if (s_lock != nullptr) {
      xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
  }
  ~Guard() {
    if (s_lock != nullptr) {
      xSemaphoreGiveRecursive(s_lock);
    }
  }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};

void copyTitle(Row& row, const char* title) {
  snprintf(row.title, sizeof(row.title), "%s", title != nullptr ? title : "");
}

/** Where each section's items start, and how many it got. A section that came
 *  back empty is dropped rather than shown as a heading over nothing. */
int s_section_start[config::kBrowseSectionCount] = {};
int s_section_count[config::kBrowseSectionCount] = {};
unsigned long s_fetched_ms = 0;

}  // namespace

void init() { s_lock = xSemaphoreCreateRecursiveMutex(); }

Problem problem() {
  Guard guard;
  return s_problem;
}

bool busy() { return s_busy; }

namespace {

/** The load itself. The caller holds the lock. */
bool loadLocked() {
  if (ha::maConfigEntry()[0] == '\0') {
    // Not an error, just not set up: get_library is addressed by config entry,
    // and there is no useful request to make without one.
    s_problem = Problem::kNoConfigEntry;
    LOG_INFO(
        "Browse: no Music Assistant config entry id - set it in the portal");
    return false;
  }

  int starts[config::kBrowseSectionCount] = {};
  int counts[config::kBrowseSectionCount] = {};
  int total = 0;
  bool any = false;

  for (int i = 0; i < config::kBrowseSectionCount; ++i) {
    const config::BrowseSection& section = config::kBrowseSections[i];
    starts[i] = total;
    counts[i] = 0;

    const int room = config::kMaxBrowseItems - total;
    if (room <= 0) {
      continue;
    }
    int want = section.limit;
    if (want <= 0 || want > room) {
      want = room;
    }

    const int got =
        ha::fetchLibrary(section.media_type, section.order_by, want,
                         &s_fetching[total], static_cast<size_t>(room));
    if (got < 0) {
      // One section failing should not empty the others; it simply shows
      // nothing under its heading, and the heading goes with it.
      continue;
    }
    any = true;
    counts[i] = got;
    total += got;
  }

  if (!any) {
    s_problem = Problem::kRequestFailed;
    return false;
  }
  s_problem = total > 0 ? Problem::kNone : Problem::kLibraryEmpty;

  memcpy(s_items, s_fetching, sizeof(s_items));
  memcpy(s_section_start, starts, sizeof(s_section_start));
  memcpy(s_section_count, counts, sizeof(s_section_count));
  s_count = total;
  s_fetched_ms = millis();
  return true;
}

}  // namespace

bool ensureLoaded() {
  Guard guard;
  if (s_count > 0 && millis() - s_fetched_ms < config::kBrowseTtlMs) {
    return true;
  }
  return loadLocked();
}

void preload() {
  if (!stale()) {
    return;
  }
  // Raised before the lock, and only when there is really work to do: a UI
  // caller checks this to decide whether to put a loading card up before
  // ensureLoaded() makes it wait. Set on every poll instead, it would flash
  // that card on swipes that had nothing to wait for.
  s_busy = true;
  ensureLoaded();
  s_busy = false;
}

bool stale() {
  Guard guard;
  return s_count == 0 || millis() - s_fetched_ms >= config::kBrowseTtlMs;
}

ReadGuard::ReadGuard() {
  if (s_lock != nullptr) {
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
  }
}

ReadGuard::~ReadGuard() {
  if (s_lock != nullptr) {
    xSemaphoreGiveRecursive(s_lock);
  }
}

int itemCount() {
  Guard guard;
  return s_count;
}

int rowCount() {
  Guard guard;
  int rows = 0;
  for (int i = 0; i < config::kBrowseSectionCount; ++i) {
    if (s_section_count[i] > 0) {
      rows += 1 + s_section_count[i];  // heading plus its items
    }
  }
  return rows;
}

bool rowAt(int index, Row& out) {
  Guard guard;
  if (index < 0) {
    return false;
  }
  int row = 0;
  for (int i = 0; i < config::kBrowseSectionCount; ++i) {
    if (s_section_count[i] == 0) {
      continue;
    }
    if (index == row) {
      out.header = true;
      copyTitle(out, config::kBrowseSections[i].title);
      out.item = -1;
      return true;
    }
    ++row;
    const int within = index - row;
    if (within < s_section_count[i]) {
      const int item = s_section_start[i] + within;
      out.header = false;
      copyTitle(out, s_items[item].name);
      out.item = item;
      return true;
    }
    row += s_section_count[i];
  }
  return false;
}

bool play(int index) {
  // The URI and type are copied out under the lock, so the service call --
  // which blocks for a round trip -- is made without holding it.
  char uri[sizeof(ha::LibraryItem::uri)] = {};
  char media_type[sizeof(ha::LibraryItem::media_type)] = {};
  {
    Guard guard;
    if (index < 0 || index >= s_count) {
      return false;
    }
    snprintf(uri, sizeof(uri), "%s", s_items[index].uri);
    snprintf(media_type, sizeof(media_type), "%s", s_items[index].media_type);
  }

  const char* entity = ha::selectedEntity();
  if (entity[0] == '\0') {
    return false;
  }
  return ha::playMedia(entity, uri, media_type);
}

bool imageUrlAt(int index, char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return false;
  }
  out[0] = '\0';
  Guard guard;
  if (index < 0 || index >= s_count) {
    return false;
  }
  snprintf(out, out_len, "%s", s_items[index].image);
  return true;
}

}  // namespace services::browse
