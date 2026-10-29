#include "hardware/rgb_panel.h"

#include <Arduino.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "esp_lcd_panel_ops.h"

#include "board/board.h"

/**
 * The framebuffer half of the output stage: what is done to the one buffer
 * the panel scans out of, whichever bus it goes out on. Opening the panel is
 * the other half -- rgb_panel.cpp for the S3's RGB peripheral, the board's
 * own for the P4's MIPI-DSI -- and hands over with rgbAdopt().
 */

namespace hw::rgb {
namespace {

constexpr int kW = board::kDisplayWidth;
constexpr int kH = board::kDisplayHeight;

esp_lcd_panel_handle_t s_panel = nullptr;

/**
 * The scan-out buffer, and there is one of it.
 *
 * One buffer holds exactly what is being shown, which is what rgbScroll()
 * needs: a list that has only moved is a move of the rows already there plus
 * a strip of new ones, rather than a recompose and a copy of 624 rows. A
 * second buffer would always be a flip behind the glass, so it could only
 * ever be recomposed and copied. The price is that every write lands in the
 * buffer being read, and the stand-offs below are what keep that from
 * breaking up the picture.
 */
uint16_t* s_fb = nullptr;

/** Half-open rectangle; empty when either extent collapses. */
struct Rect {
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;

  bool empty() const { return x1 <= x0 || y1 <= y0; }
};

/**
 * Rows to copy before standing off the bus for a moment.
 *
 * Both ends of this copy are in PSRAM, and so is the scan-out: GDMA has to
 * read 24 MB/s out of the same memory without ever missing a beat, because an
 * RGB panel has no frame memory to fall back on. A full-frame copy is a
 * megabyte in and a megabyte out issued as fast as the CPU can manage, which
 * takes longer than a frame -- so the scan-out spends an entire frame losing
 * arbitration, and the whole picture breaks up rather than a few lines of it.
 *
 * Pausing between slabs fixes that by not being on the bus: while the CPU
 * waits, GDMA has the memory to itself and refills the FIFO. It makes a large
 * present slower in wall-clock terms and costs nothing at all on the small
 * ones, which are the only kind that happen on a timer.
 */
constexpr int kCopySlabRows = 48;

/** A slab is counted in bytes, as kCopySlabRows full-width rows' worth. In
 *  rows, a copy 12 pixels wide and 624 tall -- the column a scroll bar runs
 *  down -- would stand off thirteen times for 15 KB. */
constexpr size_t kCopySlabBytes =
    static_cast<size_t>(kCopySlabRows) * kW * sizeof(uint16_t);

/** Counts bytes through the bus and stands off once per slab of them. */
class Pacer {
 public:
  void account(size_t bytes) {
    _since += bytes;
    if (_since >= kCopySlabBytes) {
      _since = 0;
      delay(1);  // let the scan-out have the bus back
    }
  }

 private:
  size_t _since = 0;
};

/** Edge of the square tiles a quarter-turned copy walks in: 32 px, a 64-byte
 *  cache line of either frame. See copyRegion(). */
constexpr int kTurnTile = 32;

/** Copy rectangle `r` of the upright frame `src` into `dst`, turned `turn`
 *  quarter turns clockwise, and return the rectangle of `dst` it wrote. The
 *  map is LovyanGFX's sprite rotation: (x, y) lands at (W-1-y, x) a quarter
 *  turn round, (W-1-x, H-1-y) upside down, and (y, W-1-x) three quarters.
 *
 *  A quarter turn makes the source's rows the destination's columns. Walked
 *  a source row at a time, every pixel written lands in a different cache
 *  line of the framebuffer, so a copy moved sixteen to thirty-two times the
 *  PSRAM it needed to. Behind a bounce buffer -- whose refill reads the same
 *  PSRAM against a deadline -- that starved the scan-out into vertical noise
 *  on the 4848S040 for as long as anything was being copied. So a quarter
 *  turn goes a square tile at a time instead, and down each destination run
 *  within it: each run written is a whole cache line, and the tile's source
 *  rows stay in cache across it. */
Rect copyRegion(uint16_t* dst, const uint16_t* src, const Rect& r,
                uint8_t turn) {
  if (r.empty()) {
    return r;
  }
  static_assert(kW == kH, "turning assumes a square panel");
  const size_t row_bytes = static_cast<size_t>(r.x1 - r.x0) * sizeof(uint16_t);
  Pacer pacer;
  switch (turn & 3) {
    case 2:
      for (int row = r.y0; row < r.y1; ++row) {
        const uint16_t* from = src + static_cast<size_t>(row) * kW;
        uint16_t* to = dst + static_cast<size_t>(kH - 1 - row) * kW + (kW - 1);
        for (int x = r.x0; x < r.x1; ++x) {
          *(to - x) = from[x];
        }
        pacer.account(row_bytes);
      }
      return Rect{kW - r.x1, kH - r.y1, kW - r.x0, kH - r.y0};
    case 1:
    case 3:
      for (int ty = r.y0; ty < r.y1; ty += kTurnTile) {
        const int ty1 = std::min(ty + kTurnTile, r.y1);
        for (int tx = r.x0; tx < r.x1; tx += kTurnTile) {
          const int tx1 = std::min(tx + kTurnTile, r.x1);
          for (int x = tx; x < tx1; ++x) {
            // Source column x is one destination row: row x a quarter turn
            // round, row W-1-x three quarters. Down the tile's rows, which
            // run along it -- backwards a quarter turn round.
            const uint16_t* from = src + x;
            if ((turn & 3) == 1) {
              uint16_t* to = dst + static_cast<size_t>(x) * kW + (kW - 1);
              for (int row = ty; row < ty1; ++row) {
                *(to - row) = from[static_cast<size_t>(row) * kW];
              }
            } else {
              uint16_t* to = dst + static_cast<size_t>(kW - 1 - x) * kW;
              for (int row = ty; row < ty1; ++row) {
                to[row] = from[static_cast<size_t>(row) * kW];
              }
            }
          }
        }
        pacer.account(row_bytes * static_cast<size_t>(ty1 - ty));
      }
      return (turn & 3) == 1 ? Rect{kW - r.y1, r.x0, kW - r.y0, r.x1}
                             : Rect{r.y0, kH - r.x1, r.y1, kH - r.x0};
    default:
      for (int row = r.y0; row < r.y1; ++row) {
        const size_t offset = static_cast<size_t>(row) * kW + r.x0;
        memcpy(dst + offset, src + offset, row_bytes);
        pacer.account(row_bytes);
      }
      return r;
  }
}

/**
 * Hand the written rows to the driver.
 *
 * Not a flip, despite the name of the call. With one framebuffer esp_lcd
 * recognises its own pointer, leaves the DMA where it already is, and flushes
 * the cache over exactly the rows named -- which is the whole reason this is
 * here. The pixels are in the buffer already; without this they would sit in
 * cache while the DMA read the old bytes underneath them.
 */
void flushRows(int y0, int y1) {
  esp_lcd_panel_draw_bitmap(s_panel, 0, y0, kW, y1, s_fb);
}

}  // namespace

void rgbAdopt(esp_lcd_panel_handle_t panel, uint16_t* framebuffer) {
  s_panel = panel;
  s_fb = framebuffer;
  // Black rather than whatever PSRAM held, so nothing garbage is on the glass
  // between the panel starting and the first frame being composed.
  memset(s_fb, 0, static_cast<size_t>(kW) * kH * sizeof(uint16_t));
  flushRows(0, kH);
}


void rgbFill(uint16_t colour) {
  if (s_fb == nullptr) {
    return;
  }
  const size_t pixels = static_cast<size_t>(kW) * kH;
  if (colour == 0) {
    memset(s_fb, 0, pixels * sizeof(uint16_t));
  } else {
    for (size_t i = 0; i < pixels; ++i) {
      s_fb[i] = colour;
    }
  }
  flushRows(0, kH);
}

void rgbPresent(const uint16_t* src, int x, int y, int w, int h,
                uint8_t turn) {
  if (s_fb == nullptr || src == nullptr) {
    return;
  }
  // Clamp rather than trust: a caller that got its arithmetic wrong should
  // lose a few pixels, not scribble past a megabyte of framebuffer.
  const Rect r{std::max(x, 0), std::max(y, 0), std::min(x + w, kW),
               std::min(y + h, kH)};
  if (r.empty()) {
    return;
  }
  const Rect written = copyRegion(s_fb, src, r, turn);
  flushRows(written.y0, written.y1);
}

bool rgbScroll(int y, int h, int dy, int keep_x, int keep_w) {
  if (s_fb == nullptr) {
    return false;
  }
  const int y0 = std::max(y, 0);
  const int y1 = std::min(y + h, kH);
  if (dy == 0 || y1 <= y0 || std::abs(dy) >= y1 - y0) {
    return false;  // nothing moves, or nothing survives the move
  }

  // Each row moves as up to two runs, either side of the columns being
  // kept. With nothing kept that is one run, the whole row.
  const int keep0 = std::min(std::max(keep_x, 0), kW);
  const int keep1 = keep_w > 0 ? std::min(std::max(keep_x + keep_w, 0), kW)
                               : keep0;
  struct Run {
    int x;
    int w;
  };
  Run runs[2];
  int run_count = 0;
  if (keep1 <= keep0) {
    runs[run_count++] = Run{0, kW};
  } else {
    if (keep0 > 0) {
      runs[run_count++] = Run{0, keep0};
    }
    if (keep1 < kW) {
      runs[run_count++] = Run{keep1, kW - keep1};
    }
  }

  // Each row's source and destination are different rows, so no single copy
  // overlaps itself; what has to be right is the order. Content moving up
  // reads from below the row it writes, so the walk goes down the screen and
  // never reads a row it has already overwritten. Content moving down is the
  // mirror image.
  const size_t row_px = static_cast<size_t>(kW);
  Pacer pacer;  // the same courtesy the copy pays the scan-out
  auto moveRow = [&](int row) {
    uint16_t* to = s_fb + static_cast<size_t>(row) * row_px;
    const uint16_t* from = s_fb + static_cast<size_t>(row - dy) * row_px;
    for (int i = 0; i < run_count; ++i) {
      const size_t bytes = static_cast<size_t>(runs[i].w) * sizeof(uint16_t);
      memcpy(to + runs[i].x, from + runs[i].x, bytes);
      pacer.account(bytes);
    }
  };
  if (dy < 0) {
    for (int row = y0; row < y1 + dy; ++row) {
      moveRow(row);
    }
  } else {
    for (int row = y1 - 1; row >= y0 + dy; --row) {
      moveRow(row);
    }
  }

  flushRows(y0, y1);
  return true;
}

}  // namespace hw::rgb
