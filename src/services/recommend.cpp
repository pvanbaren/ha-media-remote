#include "services/recommend.h"

#include <Arduino.h>
#include <WiFi.h>

#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>
#include <strings.h>

#include "config.h"
#include "log.h"
#include "ha_internal.h"
#include "services/history.h"
#include "services/ma_api.h"

namespace services::recommend {
namespace {

using ha::detail::eachElement;
using ha::detail::eachMember;
using ha::detail::readJson;

struct Candidate {
  ha::LibraryItem item;
  /** How many times the seeds pointed here; what draw() weighs by. */
  uint16_t weight;
};

/** The room's history as it stood when a pool was built or drawn from:
 *  names to leave out, and the seeds. PSRAM, like everything sizeable here. */
struct Snapshot {
  int count;
  char names[history::kMaxArtists][sizeof(ha::LibraryItem::name)];
  char uris[history::kMaxArtists][sizeof(ha::LibraryItem::uri)];
  char tracks[history::kMaxArtists][sizeof(ha::LibraryItem::uri)];
};

/** Published: what draw() reads, under s_lock. */
Candidate* s_pool = nullptr;
int s_count = 0;
SemaphoreHandle_t s_lock = nullptr;
std::atomic<uint32_t> s_generation{0};

/** The task's own: the pool being built, the response buffer and the
 *  history it was built against. Swapped with s_pool on success. */
Candidate* s_building = nullptr;
int s_building_count = 0;
char* s_response = nullptr;
Snapshot* s_build_history = nullptr;
Snapshot* s_draw_history = nullptr;

void takeSnapshot(Snapshot& out) {
  out.count = 0;
  ha::LibraryItem item;
  for (int i = 0; i < history::kMaxArtists &&
                  history::at(i, item, out.tracks[i], sizeof(out.tracks[i]));
       ++i) {
    strlcpy(out.names[i], item.name, sizeof(out.names[i]));
    strlcpy(out.uris[i], item.uri, sizeof(out.uris[i]));
    out.count = i + 1;
  }
}

bool inSnapshot(const Snapshot& snap, const char* name) {
  for (int i = 0; i < snap.count; ++i) {
    if (strcasecmp(snap.names[i], name) == 0) {
      return true;
    }
  }
  return false;
}

/** A fetchable thumbnail out of one MediaItemImage object. */
bool imageFrom(const char* image, char* out, size_t out_len) {
  char type[16] = {};
  char path[sizeof(ha::LibraryItem::image) + 1] = {};
  bool remote = false;
  bool too_long = false;
  eachMember(image, [&](const char* key, const char* value) {
    if (strcmp(key, "type") == 0 && *value == '"') {
      readJson(value, type, sizeof(type));
    } else if (strcmp(key, "path") == 0 && *value == '"') {
      readJson(value, path, sizeof(path));
      too_long = strlen(path) >= out_len;
    } else if (strcmp(key, "remotely_accessible") == 0) {
      remote = *value == 't';
    }
  });
  // A path that does not fit would be a broken URL, not a shorter one; and
  // one that is not remotely accessible is a file on the server's own disk.
  if (strcmp(type, "thumb") != 0 || !remote || too_long ||
      strncmp(path, "http", 4) != 0) {
    return false;
  }
  strlcpy(out, path, out_len);
  return true;
}

/** The first usable thumbnail of a media item: its metadata.images, or an
 *  ItemMapping's single image. */
void thumbOf(const char* object, char* out, size_t out_len) {
  eachMember(object, [&](const char* key, const char* value) {
    if (out[0] != '\0') {
      return;
    }
    if (strcmp(key, "image") == 0 && *value == '{') {
      imageFrom(value, out, out_len);
    } else if (strcmp(key, "metadata") == 0 && *value == '{') {
      eachMember(value, [&](const char* mkey, const char* mvalue) {
        if (strcmp(mkey, "images") == 0 && *mvalue == '[') {
          eachElement(mvalue, [&](const char* element) {
            return !imageFrom(element, out, out_len);
          });
        }
      });
    }
  });
}

/** An Artist or ItemMapping object as a playable item. */
bool artistFrom(const char* object, ha::LibraryItem& out) {
  out = ha::LibraryItem{};
  eachMember(object, [&](const char* key, const char* value) {
    if (strcmp(key, "name") == 0 && *value == '"') {
      readJson(value, out.name, sizeof(out.name));
    } else if (strcmp(key, "uri") == 0 && *value == '"') {
      readJson(value, out.uri, sizeof(out.uri));
    }
  });
  thumbOf(object, out.image, sizeof(out.image));
  strlcpy(out.media_type, "artist", sizeof(out.media_type));
  return out.name[0] != '\0' && strstr(out.uri, "://artist/") != nullptr;
}

void add(const ha::LibraryItem& item, uint16_t weight) {
  if (inSnapshot(*s_build_history, item.name)) {
    return;  // already on this room's list: not a discovery
  }
  for (int i = 0; i < s_building_count; ++i) {
    Candidate& c = s_building[i];
    if (strcmp(c.item.uri, item.uri) == 0 ||
        strcasecmp(c.item.name, item.name) == 0) {
      c.weight += weight;
      if (c.item.image[0] == '\0' && item.image[0] != '\0') {
        strlcpy(c.item.image, item.image, sizeof(c.item.image));
      }
      return;
    }
  }
  if (s_building_count < config::kRecommendPoolMax) {
    s_building[s_building_count++] = Candidate{item, weight};
  }
}

/** "provider://type/id" into its provider and id. */
bool splitUri(const char* uri, char* provider, size_t provider_len, char* id,
              size_t id_len) {
  const char* sep = strstr(uri, "://");
  const char* slash = strrchr(uri, '/');
  if (sep == nullptr || slash == nullptr || slash < sep + 3 ||
      slash[1] == '\0') {
    return false;
  }
  snprintf(provider, provider_len, "%.*s", static_cast<int>(sep - uri), uri);
  strlcpy(id, slash + 1, id_len);
  return true;
}

String itemArgs(const char* uri, int limit) {
  char provider[48];
  char id[64];
  if (!splitUri(uri, provider, sizeof(provider), id, sizeof(id))) {
    return String();
  }
  String args("{\"item_id\":");
  ha::detail::appendJson(args, id);
  args += ",\"provider_instance_id_or_domain\":";
  ha::detail::appendJson(args, provider);
  if (limit > 0) {
    args += ",\"limit\":";
    args += limit;
  }
  args += '}';
  return args;
}

/** The first of an artist's top tracks, into `out`: the seed for similar
 *  tracks when none of theirs has played here since boot. */
bool topTrack(const char* artist_uri, char* out, size_t out_len) {
  out[0] = '\0';
  const String args = itemArgs(artist_uri, 0);  // top_tracks takes no limit
  if (args.length() == 0 ||
      ma::call("music/artists/top_tracks", args, s_response,
               config::kMaResponseMaxBytes, config::kMaCallTimeoutMs) < 0) {
    return false;
  }
  eachElement(s_response, [&](const char* track) {
    eachMember(track, [&](const char* key, const char* value) {
      if (strcmp(key, "uri") == 0 && *value == '"') {
        readJson(value, out, out_len);
      }
    });
    return false;  // the first only
  });
  return strstr(out, "://track/") != nullptr;
}

/** Similar artists to one seed, into the pool. The count added, or -1 when
 *  the call failed. Earlier answers are the closer ones and count double. */
int fromSimilarArtists(const char* artist_uri) {
  const String args = itemArgs(artist_uri, config::kRecommendPerSeed);
  if (args.length() == 0) {
    return 0;
  }
  if (ma::call("music/artists/similar_artists", args, s_response,
               config::kMaResponseMaxBytes, config::kMaCallTimeoutMs) < 0) {
    return -1;
  }
  int n = 0;
  eachElement(s_response, [&](const char* element) {
    ha::LibraryItem item;
    if (artistFrom(element, item)) {
      add(item, n < 3 ? 2 : 1);
      ++n;
    }
    return n < config::kRecommendPerSeed;
  });
  return n;
}

/** The artists of tracks similar to one that played, into the pool: the
 *  radio-mode answer, for a provider with no similar artists of its own.
 *  An artist with no picture of its own borrows the track's cover. */
int fromSimilarTracks(const char* track_uri) {
  const String args = itemArgs(track_uri, config::kRecommendPerSeed * 2);
  if (args.length() == 0) {
    return 0;
  }
  if (ma::call("music/tracks/similar_tracks", args, s_response,
               config::kMaResponseMaxBytes, config::kMaCallTimeoutMs) < 0) {
    return -1;
  }
  int n = 0;
  int tracks = 0;
  eachElement(s_response, [&](const char* track) {
    ha::LibraryItem item;
    bool have = false;
    char cover[sizeof(item.image)] = {};
    eachMember(track, [&](const char* key, const char* value) {
      if (strcmp(key, "artists") == 0 && *value == '[' && !have) {
        eachElement(value, [&](const char* artist) {
          have = artistFrom(artist, item);
          return false;  // the first credit only
        });
      } else if (strcmp(key, "album") == 0 && *value == '{' &&
                 cover[0] == '\0') {
        thumbOf(value, cover, sizeof(cover));
      }
    });
    if (cover[0] == '\0') {
      thumbOf(track, cover, sizeof(cover));
    }
    if (have) {
      if (item.image[0] == '\0') {
        strlcpy(item.image, cover, sizeof(item.image));
      }
      add(item, 1);
      ++n;
    }
    // The answer ignores the limit; stop reading where the list stops being
    // close to the seed.
    return ++tracks < config::kRecommendPerSeed * 2;
  });
  return n;
}

/** Build a pool from the room's seeds and publish it. False when every call
 *  failed, which leaves the previous pool in place. */
bool build() {
  takeSnapshot(*s_build_history);
  s_building_count = 0;
  const int seeds = s_build_history->count < config::kRecommendSeedArtists
                        ? s_build_history->count
                        : config::kRecommendSeedArtists;
  bool answered = false;
  for (int i = 0; i < seeds; ++i) {
    const char* name = s_build_history->names[i];
    int n = fromSimilarArtists(s_build_history->uris[i]);
    answered = answered || n >= 0;
    const char* how = "similar artists";
    if (n <= 0) {
      // No similar artists from this seed's provider -- YouTube Music has
      // none -- so the tracks radio mode would pick stand in.
      char* track = s_build_history->tracks[i];
      if (track[0] != '\0' ||
          topTrack(s_build_history->uris[i], track,
                   sizeof(s_build_history->tracks[i]))) {
        n = fromSimilarTracks(track);
        answered = answered || n >= 0;
        how = "similar tracks";
      }
    }
    LOG_DEBUG("Recommend: %d from %s of %s", n < 0 ? 0 : n, how, name);
  }
  if (!answered) {
    LOG_WARN("Recommend: Music Assistant unreachable (%s)",
                  ma::lastError());
    return false;
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  Candidate* published = s_building;
  s_building = s_pool;
  s_pool = published;
  s_count = s_building_count;
  xSemaphoreGive(s_lock);
  ++s_generation;
  LOG_INFO("Recommend: %d artists like this room's last %d", s_count,
                seeds);
  return true;
}

/** FNV-1a over the seeds and the settings: a change in either means a new
 *  pool. */
uint32_t seedKey() {
  uint32_t hash = 2166136261u;
  auto mix = [&](const char* s) {
    for (; *s != '\0'; ++s) {
      hash = (hash ^ static_cast<uint8_t>(*s)) * 16777619u;
    }
    hash = (hash ^ 0xFFu) * 16777619u;
  };
  ha::LibraryItem item;
  for (int i = 0; i < config::kRecommendSeedArtists && history::at(i, item);
       ++i) {
    mix(item.uri);
  }
  const uint32_t settings = ma::generation();
  hash = (hash ^ settings) * 16777619u;
  return hash;
}

void task(void*) {
  uint32_t built_key = 0;
  bool built = false;
  unsigned long built_ms = 0;
  unsigned long failed_ms = 0;
  bool failed = false;
  uint32_t failed_key = 0;
  uint32_t pending_key = 0;
  unsigned long pending_since = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (!ma::configured() || WiFi.status() != WL_CONNECTED ||
        history::count() == 0) {
      continue;
    }
    const uint32_t key = seedKey();
    const unsigned long now = millis();
    // The seeds have to hold still for a moment first: a run of new artists
    // -- artist radio working through its list -- is one rebuild, not five.
    if (key != pending_key) {
      pending_key = key;
      pending_since = now;
      continue;
    }
    const bool due =
        !built || key != built_key ||
        now - built_ms >= config::kRecommendRefreshMs;
    if (!due || now - pending_since < config::kRecommendSettleMs ||
        (failed && key == failed_key &&
         now - failed_ms < config::kRecommendRetryMs)) {
      continue;
    }
    if (build()) {
      built = true;
      built_key = key;
      built_ms = now;
      failed = false;
    } else {
      failed = true;
      failed_key = key;
      failed_ms = now;
    }
  }
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
  if (s_lock != nullptr) {
    return;
  }
  s_lock = xSemaphoreCreateMutex();
  s_pool = psramCalloc<Candidate>(config::kRecommendPoolMax);
  s_building = psramCalloc<Candidate>(config::kRecommendPoolMax);
  s_build_history = psramCalloc<Snapshot>(1);
  s_draw_history = psramCalloc<Snapshot>(1);
  s_response = static_cast<char*>(heap_caps_malloc(
      config::kMaResponseMaxBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s_lock == nullptr || s_pool == nullptr || s_building == nullptr ||
      s_build_history == nullptr || s_draw_history == nullptr ||
      s_response == nullptr) {
    LOG_ERROR("Recommend: no memory, albums stand in");
    return;
  }
  // Its stack in PSRAM: it only talks HTTP and parses, never touches flash,
  // and internal RAM is what every TLS session is short of.
  if (xTaskCreatePinnedToCoreWithCaps(task, "recommend", 8192, nullptr,
                                      tskIDLE_PRIORITY, nullptr, tskNO_AFFINITY,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
          pdPASS &&
      xTaskCreate(task, "recommend", 8192, nullptr, tskIDLE_PRIORITY,
                  nullptr) != pdPASS) {
    LOG_ERROR("Recommend: no task, albums stand in");
  }
}

int draw(ha::LibraryItem* out, int want) {
  if (s_lock == nullptr || s_draw_history == nullptr || out == nullptr ||
      want <= 0) {
    return 0;
  }
  // Taken before the pool's lock, so the two locks are never held together.
  takeSnapshot(*s_draw_history);

  xSemaphoreTake(s_lock, portMAX_DELAY);
  uint32_t total = 0;
  bool taken[config::kRecommendPoolMax] = {};
  for (int i = 0; i < s_count; ++i) {
    // Played here since the pool was built: on Recent here now instead.
    taken[i] = inSnapshot(*s_draw_history, s_pool[i].item.name);
    if (!taken[i]) {
      total += s_pool[i].weight;
    }
  }
  int n = 0;
  while (n < want && total > 0) {
    uint32_t pick = esp_random() % total;
    for (int i = 0; i < s_count; ++i) {
      if (taken[i]) {
        continue;
      }
      if (pick < s_pool[i].weight) {
        out[n++] = s_pool[i].item;
        taken[i] = true;
        total -= s_pool[i].weight;
        break;
      }
      pick -= s_pool[i].weight;
    }
  }
  xSemaphoreGive(s_lock);
  return n;
}

uint32_t generation() { return s_generation; }

}  // namespace services::recommend
