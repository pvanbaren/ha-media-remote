#include "ui/artwork.h"

#include <Arduino.h>

#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "board/board.h"
#include "config.h"
#include "log.h"
#include "services/ha_client.h"
#include "ui/cover_art.h"
#include "ui/theme.h"

namespace ui::artwork {
namespace {

/** One pool, sized for whichever screen wants more slots. */
constexpr int kSlots = config::kMaxBrowseItems > config::kMaxSearchResults
                           ? config::kMaxBrowseItems
                           : config::kMaxSearchResults;
static_assert(kSlots <= 32, "finished-slot masks are 32 bits");

constexpr size_t kUrlLen = sizeof(services::ha::LibraryItem::image);

/**
 * Where a slot is in its life.
 *
 * The loop writes kQueued and kEmpty; the worker writes kLoading and then
 * kReady or kFailed. Only kReady slots are ever drawn, which is what lets the
 * worker decode into a slot's sprite without a lock held: nothing reads the
 * pixels of a slot that is not ready.
 */
enum class State : uint8_t { kEmpty, kQueued, kLoading, kReady, kFailed };

/** One sprite per slot rather than one tall strip. A strip is a single
 *  allocation and the tidier claim on the heap, but drawing a slice of it
 *  means handing LovyanGFX a raw pointer and asserting a byte order, where
 *  pushSprite() converts between sprite and destination itself. A handful of
 *  small allocations taken once at boot and never freed cannot fragment
 *  anything either way, so correctness wins. */
LGFX_Sprite s_sprites[kSlots];
State s_state[kSlots] = {};
/** URL each slot holds, or is queued or loading for. Compared on a refresh so
 *  the same items do not refetch every image. */
char s_url[kSlots][kUrlLen] = {};
int s_px = 0;

/** Which screen the pool currently belongs to. Bumping the generation is how
 *  a switch tells the worker that whatever it is fetching is for nobody. */
Kind s_kind = Kind::kBrowse;
uint32_t s_generation = 0;
/** Slots that reached kReady or kFailed since the loop last asked. */
uint32_t s_finished = 0;

/** Guards everything above except the sprites' pixels (see State) and s_px,
 *  which is fixed after init(). Held only for bookkeeping and for the
 *  pushSprite() of a ready slot -- never across a fetch. */
SemaphoreHandle_t s_lock = nullptr;

struct Lock {
  Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_lock); }
};

/** How far the loop's walk has got since forget(). Loop only, so unguarded. */
int s_next = 0;

TaskHandle_t s_worker = nullptr;

/** Caller holds the lock. */
void dropAllLocked() {
  for (int i = 0; i < kSlots; ++i) {
    s_state[i] = State::kEmpty;
    s_url[i][0] = '\0';
  }
  ++s_generation;
  s_finished = 0;
  s_next = 0;
}

/** Caller holds the lock. */
void switchKindLocked(Kind kind) {
  if (kind != s_kind) {
    // The other screen's pixels are meaningless here.
    s_kind = kind;
    dropAllLocked();
  }
}

/** Host part of a URL, for grouping the queue by connection. Crude on
 *  purpose: it only has to agree with itself. */
void hostOf(const char* url, char* out, size_t out_len) {
  const char* start = strstr(url, "://");
  start = start != nullptr ? start + 3 : url;
  size_t len = strcspn(start, "/?#");
  if (len >= out_len) {
    len = out_len - 1;
  }
  memcpy(out, start, len);
  out[len] = '\0';
}

struct Job {
  int index = -1;
  uint32_t generation = 0;
  char url[kUrlLen] = {};
};

/**
 * Claim the next queued slot, preferring one on the host the last fetch
 * used.
 *
 * That preference is what makes the kept-alive connection pay: a list
 * interleaves its hosts, and taking the queue in row order reopened the
 * connection on nearly every image. By host, a batch costs one handshake per
 * host. Within a host, lowest index first -- the top of the list is what is
 * on screen.
 */
bool takeJob(Job& job, const char* last_host) {
  Lock lock;
  int pick = -1;
  char host[96];
  for (int i = 0; i < kSlots; ++i) {
    if (s_state[i] != State::kQueued) {
      continue;
    }
    if (pick < 0) {
      pick = i;
    }
    if (last_host[0] == '\0') {
      break;
    }
    hostOf(s_url[i], host, sizeof(host));
    if (strcmp(host, last_host) == 0) {
      pick = i;
      break;
    }
  }
  if (pick < 0) {
    return false;
  }
  s_state[pick] = State::kLoading;
  job.index = pick;
  job.generation = s_generation;
  memcpy(job.url, s_url[pick], kUrlLen);
  return true;
}

/** Publish a finished fetch -- unless the pool moved on while it ran, in
 *  which case the pixels are for nobody and the slot is left to whoever
 *  queued it since. */
void finishJob(const Job& job, bool drawn) {
  Lock lock;
  if (job.generation != s_generation ||
      s_state[job.index] != State::kLoading ||
      strcmp(s_url[job.index], job.url) != 0) {
    return;
  }
  s_state[job.index] = drawn ? State::kReady : State::kFailed;
  s_finished |= 1u << job.index;
}

/**
 * Fetch thumbnails off the Arduino loop.
 *
 * Each one used to run on the loop itself, and each is a network fetch --
 * 560 to 860 ms measured, most of it a TLS handshake -- during which touch
 * went unread. Sixteen of them at boot left the panel blind for more than
 * half of every ten seconds. Here they cost the loop nothing.
 *
 * Pinned to core 0, beside the Wi-Fi stack and away from the loop on core 1:
 * the handshake is CPU-bound (the S3 has no ECC accelerator), and sharing a
 * core with the loop would take time from it instead of stalling it, which
 * is better but not the point.
 */
void workerTask(void*) {
  char last_host[96] = {};
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

    bool fetched = false;
    Job job;
    while (takeJob(job, last_host)) {
      fetched = true;
      hostOf(job.url, last_host, sizeof(last_host));
      // Cleared first: the image is fitted inside the square, so anything not
      // square leaves bands that would otherwise show this slot's last image.
      s_sprites[job.index].fillSprite(theme::kBackground);
      const bool drawn =
          cover::fetchThumb(s_sprites[job.index], job.url, 0, 0, s_px);
      finishJob(job, drawn);
    }

    if (fetched) {
      // An idle TLS connection is tens of KB of internal RAM, held beside the
      // poll task's own for as long as nothing needs it. The next batch pays
      // one handshake per host to get it back.
      cover::releaseThumbConnection();
      last_host[0] = '\0';
    }
  }
}

int scaledPx() {
  // Thumbnails live in list rows, so they follow the list's scale.
  const int px = static_cast<int>(
      board::kThumbPx * board::kUiScale * board::kListScale + 0.5f);
  return px < 8 ? 8 : px;
}

}  // namespace

void init() {
  s_lock = xSemaphoreCreateMutex();
  s_px = scaledPx();
  for (int i = 0; i < kSlots; ++i) {
    s_sprites[i].setColorDepth(16);
    s_sprites[i].setPsram(true);
    if (s_sprites[i].createSprite(s_px, s_px) == nullptr) {
      for (int j = 0; j < i; ++j) {
        s_sprites[j].deleteSprite();
      }
      s_px = 0;
      LOG_WARN("Artwork: no room for %d %dpx thumbnails, names only",
                    kSlots, scaledPx());
      return;
    }
  }

  // 12 KB, as for the poll task: a TLS handshake plus the decoder's frame.
  //
  // Priority 0, below the loop. What keeps a slow host from spinning this
  // task and starving IDLE0 into the task watchdog is not the priority but
  // the client fetchThumb() uses, which sleeps instead of spinning -- see
  // services::Yielding, and setup() in app.cpp for why priority alone was
  // not enough.
  if (xTaskCreatePinnedToCore(workerTask, "thumbs", 12288, nullptr,
                              tskIDLE_PRIORITY, &s_worker, 0) != pdPASS) {
    s_worker = nullptr;
    LOG_ERROR("Artwork: no worker task, names only");
    return;
  }
  LOG_INFO("Artwork: %d %dpx thumbnails in PSRAM (%u bytes)", kSlots,
                s_px, static_cast<unsigned>(kSlots * s_px * s_px * 2));
}

int size() { return s_worker != nullptr ? s_px : 0; }

void forget(Kind kind) {
  if (s_lock == nullptr) {
    return;
  }
  Lock lock;
  switchKindLocked(kind);
  // Same screen: keep what is decoded and walk it again, so update() can
  // compare URLs and skip anything that did not change.
  s_next = 0;
}

uint32_t update(Kind kind, int count, UrlFn url_for) {
  if (s_worker == nullptr || url_for == nullptr) {
    return 0;
  }
  if (count > kSlots) {
    count = kSlots;
  }

  {
    Lock lock;
    switchKindLocked(kind);
  }

  // The walk calls url_for without the pool's lock held: it may take the
  // browse list's own lock, which a background load holds across the
  // network, and the worker must not be kept waiting behind that.
  bool queued = false;
  while (s_next < count) {
    const int index = s_next++;
    char url[kUrlLen] = {};
    const bool have = url_for(index, url, sizeof(url)) && url[0] != '\0';

    Lock lock;
    if (kind != s_kind) {
      break;  // cannot happen from the loop, but never write the wrong pool
    }
    if (!have) {
      s_state[index] = State::kEmpty;
      s_url[index][0] = '\0';
      continue;
    }
    const State state = s_state[index];
    if ((state == State::kReady || state == State::kQueued ||
         state == State::kLoading) &&
        strcmp(s_url[index], url) == 0) {
      continue;  // same entry, same image: already pixels, or on its way
    }
    memcpy(s_url[index], url, kUrlLen);
    s_state[index] = State::kQueued;
    queued = true;
  }
  if (queued) {
    xTaskNotifyGive(s_worker);
  }

  Lock lock;
  if (kind != s_kind) {
    return 0;
  }
  const uint32_t finished = s_finished;
  s_finished = 0;
  return finished;
}

bool draw(Kind kind, int index, lgfx::LovyanGFX& gfx, int x, int y) {
  if (index < 0 || index >= kSlots || s_worker == nullptr) {
    return false;
  }
  Lock lock;
  if (kind != s_kind || s_state[index] != State::kReady) {
    return false;
  }
  s_sprites[index].pushSprite(&gfx, x, y);
  return true;
}

}  // namespace ui::artwork
