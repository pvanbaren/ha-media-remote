#include "services/browse.h"

#include <Arduino.h>

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstring>

#include "config.h"
#include "log.h"
#include "services/history.h"
#include "services/ma_api.h"
#include "services/recommend.h"

namespace services::browse {
namespace {

ha::LibraryItem s_items[config::kMaxBrowseItems];
/** Staged here rather than on the caller's stack: ten items are ~3 KB, and
 *  refresh() runs on the Arduino loop task alongside an open TLS connection. */
ha::LibraryItem s_fetching[config::kMaxBrowseItems];
/** A library query's answer, before what the list already shows is taken out
 *  of it: PSRAM, claimed on the first one. */
ha::LibraryItem* s_library = nullptr;
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
/** Each section's heading as loaded: a room section standing in with the
 *  library says so. */
const char* s_section_title[config::kBrowseSectionCount] = {};
unsigned long s_fetched_ms = 0;
/** roomGeneration() when the list was loaded, so a new artist played here,
 *  or a new set of recommendations, reloads it rather than waiting out the
 *  TTL. Cheap: the room sections are drawn from memory. */
uint32_t s_room_generation = 0;
/** When the last load failed or could not be made (0 when it did not), and
 *  roomGeneration() then: preload() lets it be for config::kBrowseRetryMs
 *  unless that changes. */
unsigned long s_failed_ms = 0;
uint32_t s_failed_generation = 0;

/** Recent list, read once in init() and kept: every task asks, so no lock.
 *  Its own NVS namespace, so a BOOT reset of the Home Assistant settings
 *  leaves it, as it leaves the room's history. */
constexpr char kPrefsNamespace[] = "browse";
constexpr char kPrefsRecentHereKey[] = "recent_here";
std::atomic<bool> s_recent_here{config::kRecentHereByDefault};
/** Counts saves of it, for roomGeneration(). */
std::atomic<uint32_t> s_setting_generation{0};

}  // namespace

void init() {
  s_lock = xSemaphoreCreateRecursiveMutex();
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    s_recent_here =
        prefs.getBool(kPrefsRecentHereKey, config::kRecentHereByDefault);
    prefs.end();
  }
}

bool recentHere() { return s_recent_here; }

void saveRecentHere(bool on) {
  s_recent_here = on;
  ++s_setting_generation;
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putBool(kPrefsRecentHereKey, on);
    prefs.end();
  }
  LOG_INFO("Browse: Recent list %s", on ? "Room only" : "Music Assistant");
}

Problem problem() {
  Guard guard;
  return s_problem;
}

bool busy() { return s_busy; }

namespace {

/** The room's artists, up to `want`, into `out`. */
int roomArtists(int want, ha::LibraryItem* out) {
  int n = 0;
  for (int i = 0; n < want && history::at(i, out[n]); ++i) {
    ++n;
  }
  return n;
}

bool listed(const ha::LibraryItem* list, int count, const char* name) {
  for (int i = 0; i < count; ++i) {
    if (strcasecmp(list[i].name, name) == 0) {
      return true;
    }
  }
  return false;
}

/** Up to `want` items from a section's library query into list[have...],
 *  leaving out any of the `have` before them -- an artist on Recent here is
 *  not offered again under Recommended. Asks for a list's worth, so what is
 *  left out can be made up. The count added, or -1 when the request failed. */
int fetchUnique(const config::BrowseSection& section, bool favorites,
                int want, ha::LibraryItem* list, int have) {
  if (s_library == nullptr) {
    s_library = static_cast<ha::LibraryItem*>(heap_caps_calloc_prefer(
        config::kMaxBrowseItems, sizeof(ha::LibraryItem), 2,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT));
    if (s_library == nullptr) {
      return -1;
    }
  }
  const int got = ha::fetchLibrary(section.media_type, section.order_by,
                                   config::kMaxBrowseItems, s_library,
                                   config::kMaxBrowseItems, favorites);
  int n = 0;
  for (int i = 0; i < got && n < want; ++i) {
    if (!listed(list, have + n, s_library[i].name)) {
      list[have + n++] = s_library[i];
    }
  }
  return got < 0 ? -1 : n;
}

/** Where the room sections come from, as one number: a change in the
 *  history, the recommendations, the Music Assistant settings -- a token
 *  saved, which is what switches the similar artists on -- or Recent list
 *  means a reload. */
uint32_t roomGeneration() {
  return ((history::generation() * 2654435761u + recommend::generation()) *
              2654435761u +
          ma::generation()) *
             2654435761u +
         s_setting_generation;
}

/** The load itself. The caller holds the lock.
 *
 *  Recent here is this device's history while the setting is Room only, and
 *  the server's recent artists while it is Music Assistant or the history is
 *  empty.
 *  Recommended is artists like this room's, drawn from services::recommend,
 *  with a Music Assistant token -- without one there are no similar artists
 *  to draw -- and the library's favourite artists, topped up with others at
 *  random, without one or until there are any. */
bool loadLocked() {
  const bool recent_here = recentHere();
  const bool similar = ma::configured();
  const uint32_t generation = roomGeneration();
  if (ha::maConfigEntry()[0] == '\0' &&
      !(recent_here && history::count() > 0) && !similar) {
    // Not an error, just not set up: get_library is addressed by config entry,
    // and there is no useful request to make without one. The room's own
    // artists, and the ones like them, come from memory and need none. Said
    // once, not every time.
    if (s_problem != Problem::kNoConfigEntry) {
      LOG_INFO(
          "Browse: no Music Assistant config entry id - set it in the portal");
    }
    s_problem = Problem::kNoConfigEntry;
    s_failed_ms = millis() | 1;  // never 0, which means it did not fail
    s_failed_generation = generation;
    return false;
  }

  int starts[config::kBrowseSectionCount] = {};
  int counts[config::kBrowseSectionCount] = {};
  const char* titles[config::kBrowseSectionCount] = {};
  int total = 0;
  bool any = false;

  for (int i = 0; i < config::kBrowseSectionCount; ++i) {
    const config::BrowseSection& section = config::kBrowseSections[i];
    starts[i] = total;
    counts[i] = 0;
    titles[i] = section.title;

    const int room = config::kMaxBrowseItems - total;
    if (room <= 0) {
      continue;
    }
    int want = section.limit;
    if (want <= 0 || want > room) {
      want = room;
    }

    int got = 0;
    switch (section.source) {
      case config::BrowseSource::kRoomArtists:
        if (recent_here) {
          got = roomArtists(want, &s_fetching[total]);
        }
        break;
      case config::BrowseSource::kRoomSimilar:
        if (similar) {
          got = recommend::draw(&s_fetching[total], want);
        }
        break;
      case config::BrowseSource::kLibrary:
        break;
    }
    if (got <= 0) {
      // Not the room's, or the room has nothing yet: the library stands in,
      // and says so in the heading where one is given.
      if (section.source != config::BrowseSource::kLibrary &&
          section.fallback_title != nullptr) {
        titles[i] = section.fallback_title;
      }
      int favorites = section.favorites_first
                          ? fetchUnique(section, true, want, s_fetching, total)
                          : 0;
      if (favorites < 0) {
        favorites = 0;
      }
      got = favorites;
      if (favorites < want) {
        const int rest = fetchUnique(section, false, want - favorites,
                                     s_fetching, total + favorites);
        got = rest >= 0 ? favorites + rest : (favorites > 0 ? favorites : -1);
      }
    }
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
    s_failed_ms = millis() | 1;
    s_failed_generation = generation;
    return false;
  }
  s_failed_ms = 0;
  s_problem = total > 0 ? Problem::kNone : Problem::kLibraryEmpty;

  memcpy(s_items, s_fetching, sizeof(s_items));
  memcpy(s_section_start, starts, sizeof(s_section_start));
  memcpy(s_section_count, counts, sizeof(s_section_count));
  memcpy(s_section_title, titles, sizeof(s_section_title));
  s_count = total;
  s_fetched_ms = millis();
  s_room_generation = generation;
  return true;
}

}  // namespace

bool ensureLoaded() {
  Guard guard;
  if (s_count > 0 && millis() - s_fetched_ms < config::kBrowseTtlMs &&
      s_room_generation == roomGeneration()) {
    return true;
  }
  return loadLocked();
}

void preload() {
  if (!stale()) {
    return;
  }
  {
    // A load that just failed is not tried again on every poll.
    Guard guard;
    if (s_failed_ms != 0 && millis() - s_failed_ms < config::kBrowseRetryMs &&
        s_failed_generation == roomGeneration()) {
      return;
    }
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
  return s_count == 0 || millis() - s_fetched_ms >= config::kBrowseTtlMs ||
         s_room_generation != roomGeneration();
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
      copyTitle(out, s_section_title[i] != nullptr
                         ? s_section_title[i]
                         : config::kBrowseSections[i].title);
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

bool itemAt(int index, ha::LibraryItem& out) {
  Guard guard;
  if (index < 0 || index >= s_count) {
    return false;
  }
  out = s_items[index];
  return true;
}

bool playsAsRadio(int index) {
  Guard guard;
  return config::kArtistsPlayRadio && index >= 0 && index < s_count &&
         strcmp(s_items[index].media_type, "artist") == 0;
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
