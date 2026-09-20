#include "ui/artwork.h"

#include <Arduino.h>

#include <cstring>

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

/** One sprite per slot rather than one tall strip. A strip is a single
 *  allocation and the tidier claim on the heap, but drawing a slice of it
 *  means handing LovyanGFX a raw pointer and asserting a byte order, where
 *  pushSprite() converts between sprite and destination itself. A handful of
 *  small allocations taken once at boot and never freed cannot fragment
 *  anything either way, so correctness wins. */
LGFX_Sprite s_slots[kSlots];
bool s_ready[kSlots] = {};
/** URL each slot was decoded from, so a refresh returning the same items does
 *  not refetch every image. */
char s_url[kSlots][sizeof(services::ha::LibraryItem::image)] = {};
int s_px = 0;

/** Which screen the pool currently belongs to, and how far the walk has got. */
Kind s_kind = Kind::kBrowse;
int s_next = 0;

int scaledPx() {
  const int px = static_cast<int>(board::kThumbPx * board::kUiScale + 0.5f);
  return px < 8 ? 8 : px;
}

void dropAll() {
  for (int i = 0; i < kSlots; ++i) {
    s_ready[i] = false;
    s_url[i][0] = '\0';
  }
  s_next = 0;
}

}  // namespace

void init() {
  s_px = scaledPx();
  for (int i = 0; i < kSlots; ++i) {
    s_slots[i].setColorDepth(16);
    s_slots[i].setPsram(true);
    if (s_slots[i].createSprite(s_px, s_px) == nullptr) {
      for (int j = 0; j < i; ++j) {
        s_slots[j].deleteSprite();
      }
      s_px = 0;
      LOG_WARN("Artwork: no room for %d %dpx thumbnails, names only",
                    kSlots, scaledPx());
      return;
    }
  }
  LOG_INFO("Artwork: %d %dpx thumbnails in PSRAM (%u bytes)", kSlots,
                s_px, static_cast<unsigned>(kSlots * s_px * s_px * 2));
}

int size() { return s_px; }

void forget(Kind kind) {
  if (kind != s_kind) {
    // The other screen's pixels are meaningless here.
    s_kind = kind;
    dropAll();
    return;
  }
  // Same screen: keep what is decoded and walk it again, so loadNext() can
  // compare URLs and skip anything that did not change.
  s_next = 0;
}

bool loadNext(Kind kind, int count, UrlFn url_for) {
  if (s_px == 0 || url_for == nullptr) {
    return false;
  }
  if (kind != s_kind) {
    s_kind = kind;
    dropAll();
  }
  if (count > kSlots) {
    count = kSlots;
  }

  while (s_next < count) {
    const int index = s_next++;
    char url[sizeof(s_url[0])] = {};
    if (!url_for(index, url, sizeof(url)) || url[0] == '\0') {
      s_ready[index] = false;
      s_url[index][0] = '\0';
      continue;  // nothing to fetch; try the next on the next pass
    }
    if (s_ready[index] && strcmp(s_url[index], url) == 0) {
      continue;  // same entry, same image, already pixels
    }

    // Cleared first: drawUrl fits the image inside the square, so anything
    // not square leaves bands that would otherwise show the last slot's
    // pixels.
    s_slots[index].fillSprite(theme::kBackground);
    s_ready[index] =
        cover::drawUrl(s_slots[index], url, 0, 0, s_px);
    if (s_ready[index]) {
      snprintf(s_url[index], sizeof(s_url[index]), "%s", url);
    } else {
      s_url[index][0] = '\0';
    }
    return true;  // one image per call, so touch stays answerable
  }
  return false;
}

bool draw(Kind kind, int index, lgfx::LovyanGFX& gfx, int x, int y) {
  if (kind != s_kind || index < 0 || index >= kSlots || s_px == 0 ||
      !s_ready[index]) {
    return false;
  }
  s_slots[index].pushSprite(&gfx, x, y);
  return true;
}

}  // namespace ui::artwork
