#include "services/history.h"

#include <Arduino.h>
#include <Preferences.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstring>
#include <strings.h>

#include "config.h"
#include "log.h"

namespace services::history {
namespace {

/** Its own namespace: a BOOT reset clears the Home Assistant settings, and
 *  what this room listens to is not one of them. */
constexpr char kPrefsNamespace[] = "history";
constexpr char kPrefsKey[] = "artists";
/** Bumped if Stored ever changes shape, so an old blob is dropped rather than
 *  read as the new one. */
constexpr uint8_t kVersion = 1;

/** What NVS keeps of an artist: its name, and the URI that plays it. Not its
 *  picture URL -- 160 bytes, more than the other two together, in a 20 KB
 *  partition the radio's calibration data and the Wi-Fi settings share, and
 *  the one part a search can always find again (refreshPictures()). */
struct Entry {
  char name[sizeof(ha::LibraryItem::name)];
  char uri[sizeof(ha::LibraryItem::uri)];
};

/** The whole list as stored: a header and the entries, written as one blob. */
struct Stored {
  uint8_t version;
  uint8_t count;
  uint8_t reserved[2];
  Entry entries[kMaxArtists];
};

/** What is kept of each entry in RAM only, in step with entries[]. */
struct Extra {
  /** A track by the artist that played here: what services::recommend seeds
   *  "similar tracks" from. After a restart a seed's top track stands in
   *  until one plays. */
  char track[sizeof(ha::LibraryItem::uri)];
  /** The artist's picture, from the search or pick that added them, or found
   *  again after a restart. */
  char picture[sizeof(ha::LibraryItem::image)];
  /** refreshPictures() has tried this entry since boot. */
  bool looked_up;
};

/** Both in PSRAM where there is some: the internal heap, which the TLS
 *  sessions live on, has better uses for them. */
Stored* s_store = nullptr;
Extra* s_extra = nullptr;
SemaphoreHandle_t s_lock = nullptr;
std::atomic<uint32_t> s_generation{0};

/** The list has changed since it was last written. persist() writes it on
 *  the network task, so a pick made from the touch loop does not stall it on
 *  a flash write -- at once when an artist new to the list was added
 *  (s_added), and otherwise at most once every kHistorySaveIntervalMs, so a
 *  run of artists moving to the front costs one write an hour, not one each. */
std::atomic<bool> s_dirty{false};
std::atomic<bool> s_added{false};
/** When the list was last written; boot counts. Network task only. */
unsigned long s_saved_ms = 0;

/** Network task only: the last artist handed in, so a run of states for the
 *  same track costs a string compare and nothing else. */
char s_last_noted[sizeof(ha::PlayerState::artist)] = {};

/** A search that went unanswered is tried again after this long -- not on
 *  every state for the same track, and not never. Network task only. */
constexpr unsigned long kRetryMs = 30000;
bool s_retry = false;
unsigned long s_retry_at = 0;

/** Names a search found no match for, so a station's "Artist - Title" text or
 *  an artist missing from every provider is not searched for again on every
 *  track that carries it. A small ring; network task only. */
constexpr int kFailedMemory = 6;
char s_failed[kFailedMemory][sizeof(ha::PlayerState::artist)] = {};
int s_failed_next = 0;

struct Guard {
  Guard() {
    if (s_lock != nullptr) {
      xSemaphoreTake(s_lock, portMAX_DELAY);
    }
  }
  ~Guard() {
    if (s_lock != nullptr) {
      xSemaphoreGive(s_lock);
    }
  }
};

void saveLocked() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.putBytes(kPrefsKey, s_store, sizeof(Stored));
    prefs.end();
  }
}

/** Move entry `from` to the front, sliding the ones above it down. */
void toFrontLocked(int from) {
  if (from <= 0) {
    return;
  }
  const Entry moved = s_store->entries[from];
  memmove(&s_store->entries[1], &s_store->entries[0],
          static_cast<size_t>(from) * sizeof(Entry));
  s_store->entries[0] = moved;
  const Extra extra = s_extra[from];
  memmove(&s_extra[1], &s_extra[0], static_cast<size_t>(from) * sizeof(Extra));
  s_extra[0] = extra;
}

/** A Music Assistant track URI, the only kind worth keeping as a seed. */
bool isTrackUri(const char* uri) {
  return uri != nullptr && strstr(uri, "://track/") != nullptr;
}

/** Put `item` at the front: moved there if its uri is on the list already --
 *  the lead artist of a collaboration, say -- or added, dropping the oldest
 *  when the list is full. */
void putFrontLocked(const ha::LibraryItem& item, const char* track) {
  int at = -1;
  for (int i = 0; i < s_store->count; ++i) {
    if (strcmp(s_store->entries[i].uri, item.uri) == 0) {
      at = i;
      break;
    }
  }
  if (at < 0) {
    at = s_store->count < kMaxArtists ? s_store->count++ : kMaxArtists - 1;
    Entry& entry = s_store->entries[at];
    snprintf(entry.name, sizeof(entry.name), "%s", item.name);
    snprintf(entry.uri, sizeof(entry.uri), "%s", item.uri);
    s_extra[at] = Extra{};
    s_added = true;
  }
  if (item.image[0] != '\0') {
    strlcpy(s_extra[at].picture, item.image, sizeof(s_extra[at].picture));
  }
  if (isTrackUri(track)) {
    strlcpy(s_extra[at].track, track, sizeof(s_extra[at].track));
  }
  toFrontLocked(at);
  s_dirty = true;
  ++s_generation;
}

bool failedBefore(const char* name) {
  for (const auto& failed : s_failed) {
    if (failed[0] != '\0' && strcasecmp(failed, name) == 0) {
      return true;
    }
  }
  return false;
}

void rememberFailure(const char* name) {
  snprintf(s_failed[s_failed_next], sizeof(s_failed[0]), "%s", name);
  s_failed_next = (s_failed_next + 1) % kFailedMemory;
}

/** Look `name` up in Music Assistant and keep only an exact match: the
 *  nearest-sounding artist is not the one that played, and a wrong face on
 *  the list is worse than a missing one. False when nothing matched. */
bool resolve(const char* name, ha::LibraryItem& out, bool& transport_failed) {
  ha::LibraryItem found[4];
  const int n = ha::searchArtists(name, found, 4);
  if (n < 0) {
    transport_failed = true;
    return false;
  }
  for (int i = 0; i < n; ++i) {
    if (strcasecmp(found[i].name, name) == 0) {
      out = found[i];
      return true;
    }
  }
  return false;
}

/** The first of several credited artists -- "A, B", "A & B", "A feat. B" --
 *  or an empty string when `name` names only one. */
void leadArtist(const char* name, char* out, size_t out_len) {
  // Not " and ": "Simon and Garfunkel" is one act, and its first word is
  // somebody else entirely.
  static const char* const kJoins[] = {", ", " & ", " feat. ", " feat ",
                                       " ft. ", " featuring ", " / "};
  const char* cut = nullptr;
  for (const char* join : kJoins) {
    const char* at = strstr(name, join);
    if (at != nullptr && at != name && (cut == nullptr || at < cut)) {
      cut = at;
    }
  }
  out[0] = '\0';
  if (cut == nullptr) {
    return;
  }
  const size_t n = static_cast<size_t>(cut - name);
  snprintf(out, out_len, "%.*s", static_cast<int>(n), name);
}

template <typename T>
T* psramCalloc(size_t n) {
  void* p = heap_caps_calloc(n, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == nullptr) {
    p = calloc(n, sizeof(T));
  }
  return static_cast<T*>(p);
}

}  // namespace

void init() {
  if (s_lock == nullptr) {
    s_lock = xSemaphoreCreateMutex();
  }
  if (s_store == nullptr) {
    s_store = psramCalloc<Stored>(1);
  }
  if (s_extra == nullptr) {
    s_extra = psramCalloc<Extra>(kMaxArtists);
  }
  if (s_store == nullptr || s_extra == nullptr) {
    s_store = nullptr;  // every call checks this one
    return;
  }
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    if (prefs.getBytesLength(kPrefsKey) == sizeof(Stored)) {
      prefs.getBytes(kPrefsKey, s_store, sizeof(Stored));
    }
    prefs.end();
  }
  if (s_store->version != kVersion || s_store->count > kMaxArtists) {
    memset(s_store, 0, sizeof(Stored));
    s_store->version = kVersion;
  }
  LOG_INFO("History: %u artist%s played here",
                static_cast<unsigned>(s_store->count),
                s_store->count == 1 ? "" : "s");
}

void notePlaying(const char* artist, const char* track) {
  if (s_store == nullptr || artist == nullptr || artist[0] == '\0') {
    return;
  }
  if (strcmp(artist, s_last_noted) == 0 &&
      !(s_retry && static_cast<long>(millis() - s_retry_at) >= 0)) {
    return;
  }
  snprintf(s_last_noted, sizeof(s_last_noted), "%s", artist);
  s_retry = false;

  {
    Guard guard;
    for (int i = 0; i < s_store->count; ++i) {
      if (strcasecmp(s_store->entries[i].name, artist) == 0) {
        if (isTrackUri(track)) {
          strlcpy(s_extra[i].track, track, sizeof(s_extra[i].track));
        }
        if (i > 0) {
          toFrontLocked(i);
          s_dirty = true;
          ++s_generation;
        }
        return;
      }
    }
  }

  if (failedBefore(artist) || ha::maConfigEntry()[0] == '\0') {
    return;
  }

  // The search is made without the lock: it is a network round trip, and the
  // browse load reading the list should not wait on it.
  ha::LibraryItem item;
  bool transport_failed = false;
  bool found = resolve(artist, item, transport_failed);
  if (!found && !transport_failed) {
    char lead[sizeof(s_last_noted)];
    leadArtist(artist, lead, sizeof(lead));
    if (lead[0] != '\0') {
      found = resolve(lead, item, transport_failed);
    }
  }
  if (!found) {
    // A dropped request is worth trying again shortly; a search that
    // answered and had no such artist is not.
    if (transport_failed) {
      s_retry = true;
      s_retry_at = millis() + kRetryMs;
      LOG_WARN("History: search for \"%s\" failed, retrying in %lu s",
                    artist, kRetryMs / 1000);
    } else {
      rememberFailure(artist);
      LOG_INFO("History: no artist found for \"%s\"", artist);
    }
    return;
  }

  Guard guard;
  putFrontLocked(item, track);
  LOG_INFO("History: %s played here", item.name);
}

void notePicked(const ha::LibraryItem& item) {
  if (s_store == nullptr || item.uri[0] == '\0' || item.name[0] == '\0' ||
      strcmp(item.media_type, "artist") != 0) {
    return;
  }
  Guard guard;
  putFrontLocked(item, nullptr);
  LOG_INFO("History: %s picked here", item.name);
}

void persist() {
  if (s_store == nullptr || !s_dirty) {
    return;
  }
  if (!s_added && millis() - s_saved_ms < config::kHistorySaveIntervalMs) {
    return;  // only reordered: it can wait for the hour
  }
  // Cleared before the write, so a change made during it is kept for the
  // next one rather than lost.
  s_dirty = false;
  s_added = false;
  Guard guard;
  saveLocked();
  s_saved_ms = millis();
}

void refreshPictures() {
  if (s_store == nullptr || ha::maConfigEntry()[0] == '\0') {
    return;
  }
  static unsigned long s_last_ms = 0;
  static bool s_started = false;
  if (s_started &&
      millis() - s_last_ms < config::kHistoryPictureLookupMs) {
    return;
  }

  char name[sizeof(Entry::name)];
  char uri[sizeof(Entry::uri)];
  {
    Guard guard;
    int i = 0;
    for (; i < s_store->count; ++i) {
      if (s_extra[i].picture[0] == '\0' && !s_extra[i].looked_up) {
        break;
      }
    }
    if (i == s_store->count) {
      return;  // every entry has its picture, or has been tried
    }
    s_extra[i].looked_up = true;
    strlcpy(name, s_store->entries[i].name, sizeof(name));
    strlcpy(uri, s_store->entries[i].uri, sizeof(uri));
  }
  s_started = true;
  s_last_ms = millis();

  // Without the lock, as in notePlaying(): a network round trip. The same
  // artist, by its URI where the search offers it, or by its exact name.
  ha::LibraryItem found[4];
  const int n = ha::searchArtists(name, found, 4);
  const ha::LibraryItem* match = nullptr;
  for (int i = 0; i < n && match == nullptr; ++i) {
    if (strcmp(found[i].uri, uri) == 0) {
      match = &found[i];
    }
  }
  for (int i = 0; i < n && match == nullptr; ++i) {
    if (strcasecmp(found[i].name, name) == 0) {
      match = &found[i];
    }
  }

  Guard guard;
  for (int i = 0; i < s_store->count; ++i) {
    if (strcmp(s_store->entries[i].uri, uri) != 0) {
      continue;  // found again by URI: it may have moved meanwhile
    }
    if (n < 0) {
      s_extra[i].looked_up = false;  // unanswered: worth another try
    } else if (match != nullptr && match->image[0] != '\0') {
      strlcpy(s_extra[i].picture, match->image, sizeof(s_extra[i].picture));
      ++s_generation;
      LOG_DEBUG("History: found %s's picture again", name);
    }
    break;
  }
}

int count() {
  Guard guard;
  return s_store != nullptr ? s_store->count : 0;
}

bool at(int index, ha::LibraryItem& out, char* track, size_t track_len) {
  Guard guard;
  if (s_store == nullptr || index < 0 || index >= s_store->count) {
    return false;
  }
  if (track != nullptr && track_len > 0) {
    strlcpy(track, s_extra[index].track, track_len);
  }
  const Entry& entry = s_store->entries[index];
  out = ha::LibraryItem{};
  snprintf(out.uri, sizeof(out.uri), "%s", entry.uri);
  snprintf(out.name, sizeof(out.name), "%s", entry.name);
  snprintf(out.image, sizeof(out.image), "%s", s_extra[index].picture);
  snprintf(out.media_type, sizeof(out.media_type), "%s", "artist");
  return true;
}

uint32_t generation() { return s_generation; }

}  // namespace services::history
