#include "hardware/qualia_rgb.h"

#include <Arduino.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_private/periph_ctrl.h"
#include "soc/periph_defs.h"

#include "board/board.h"
#include "log.h"

namespace hw::qualia {
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
 * Drive the panel bus as gently as it will tolerate.
 *
 * Nineteen pins switch at the pixel clock a few centimetres from a 2.4 GHz
 * front end, and on this board that was not a theoretical concern. Every HTTP
 * request took tens of seconds -- one succeeded in 72 -- while a PC on the
 * same network completed the TLS handshake to the same server in 4 ms.
 * Lowering the pixel clock to 12 MHz cut that to 4.3 s without speeding up a
 * single PSRAM copy, which is what ruled out bus contention and left
 * interference: less switching, fewer lost packets.
 *
 * The cost lands almost entirely on the TLS handshake, which is the part that
 * needs several round trips in a row to survive. Once keep-alive is holding
 * the connection open the same board answers in 77 to 179 ms, so what looks
 * like a slow device is a handshake being retried, not a slow one.
 *
 * Radiated emission goes with edge rate rather than with clock alone, and an
 * ESP32-S3 pin defaults to roughly 20 mA of drive it does not need for a few
 * centimetres of ribbon. Weakening it slows the edges and quietens the bus
 * without costing a hertz of refresh.
 *
 * Measured, at 16 MHz, on the first request after boot and then in steady
 * state once keep-alive holds the connection:
 *
 *   CAP_2 (~20 mA, the default)   72 s, and connects timing out at six
 *   CAP_1 (~10 mA)                11.4 s, then 26.7 / 9.4 / 2.7 s
 *   CAP_0 (~5 mA)                 0.5 s, then 28 / 16 / 16 ms
 *
 * For scale, 12 MHz at the default drive reached 77 ms in steady state. So the
 * clock and the drive are not two ways of buying the same thing: they add, and
 * either one pushed far enough is sufficient on its own.
 *
 * This runs at CAP_0, so the drive is carrying the radio on its own -- which
 * is what the table above says it can: 0.5 s for the first request and then
 * 28, 16, 16 ms, measured at 16 MHz. The clock is at 12 MHz as well, but for
 * PSRAM bandwidth rather than for the radio; board::kRgbPclkHz says why.
 *
 * 5 mA into a ribbon is not a lot, and that is the thing to doubt first if
 * the panel ever misbehaves. It is a drive strength rather than a timing, so
 * the failure would be speckle, dropped columns or a shifted image -- not a
 * glitch that comes and goes, which is a different fault with a different
 * cause (see platformio.ini).
 *
 * If the panel shows speckle, dropped columns or a shifted image, this is
 * still the first thing to put back: GPIO_DRIVE_CAP_2 is the default. It is
 * set after esp_lcd_new_rgb_panel(), which configures these pins itself and
 * would otherwise overwrite the capability.
 */
void quietenBus() {
  constexpr gpio_drive_cap_t kDrive = GPIO_DRIVE_CAP_0;  // ~5 mA
  for (int i = 0; i < 16; ++i) {
    gpio_set_drive_capability(static_cast<gpio_num_t>(board::kRgbDataPins[i]),
                              kDrive);
  }
  gpio_set_drive_capability(static_cast<gpio_num_t>(board::kRgbPinPclk), kDrive);
  gpio_set_drive_capability(static_cast<gpio_num_t>(board::kRgbPinDe), kDrive);
  // HSYNC and VSYNC switch at a thousandth of the data rate, so they
  // contribute nothing worth the risk of weakening a sync edge.
}

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

void copyRegion(uint16_t* dst, const uint16_t* src, const Rect& r) {
  if (r.empty()) {
    return;
  }
  const size_t row_bytes = static_cast<size_t>(r.x1 - r.x0) * sizeof(uint16_t);
  Pacer pacer;
  for (int row = r.y0; row < r.y1; ++row) {
    const size_t offset = static_cast<size_t>(row) * kW + r.x0;
    memcpy(dst + offset, src + offset, row_bytes);
    pacer.account(row_bytes);
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

bool rgbInit() {
  // A soft reboot -- esp_restart, a watchdog, a crash -- resets the CPU but
  // leaves the panel powered and LCD_CAM/GDMA possibly still clocking.
  // esp_lcd_new_rgb_panel then latches the scan-out DMA onto whatever VSYNC
  // phase is current, so framebuffer line 0 can land partway down the glass
  // and the whole image stays shifted until a full power cycle. Resetting the
  // peripheral first makes a warm boot start from the same clean state as a
  // cold one.
  periph_module_reset(PERIPH_LCD_CAM_MODULE);

  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;

  cfg.timings.pclk_hz = board::kRgbPclkHz;
  cfg.timings.h_res = kW;
  cfg.timings.v_res = kH;
  cfg.timings.hsync_pulse_width = board::kRgbHsyncPulse;
  cfg.timings.hsync_back_porch = board::kRgbHsyncBackPorch;
  cfg.timings.hsync_front_porch = board::kRgbHsyncFrontPorch;
  cfg.timings.vsync_pulse_width = board::kRgbVsyncPulse;
  cfg.timings.vsync_back_porch = board::kRgbVsyncBackPorch;
  cfg.timings.vsync_front_porch = board::kRgbVsyncFrontPorch;
  cfg.timings.flags.hsync_idle_low = board::kRgbHsyncIdleLow ? 1 : 0;
  cfg.timings.flags.vsync_idle_low = board::kRgbVsyncIdleLow ? 1 : 0;
  cfg.timings.flags.de_idle_high = board::kRgbDeIdleHigh ? 1 : 0;
  cfg.timings.flags.pclk_active_neg = board::kRgbPclkActiveNeg ? 1 : 0;
  cfg.timings.flags.pclk_idle_high = 0;

  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;

  // One framebuffer, so it always holds the frame on the glass and a scroll
  // can move it in place. See the note on s_fb.
  cfg.num_fbs = 1;

  // No bounce buffer: GDMA reads the framebuffer straight out of PSRAM.
  //
  // A bounce buffer sounds like insurance and is closer to the opposite. It
  // puts the CPU in the scan-out path -- an interrupt has to move a slab of
  // PSRAM into internal SRAM before the LCD FIFO drains, every time, forever
  // -- so any stall becomes a visible glitch. It earns its place only when
  // PSRAM cannot feed the panel at all. This one asks for 720*720*2 bytes at
  // 19.5 Hz, about 24 MB/s, which octal PSRAM can serve. What goes wrong on
  // this board is bursts -- a present or a compose holding the bus long
  // enough for the LCD FIFO to miss a deadline -- and a bounce buffer would
  // not escape those: it is filled from the same PSRAM, over the same bus,
  // and adds an interrupt deadline of its own.
  cfg.bounce_buffer_size_px = 0;
  cfg.dma_burst_size = 64;

  cfg.hsync_gpio_num = static_cast<int>(board::kRgbPinHsync);
  cfg.vsync_gpio_num = static_cast<int>(board::kRgbPinVsync);
  cfg.de_gpio_num = static_cast<int>(board::kRgbPinDe);
  cfg.pclk_gpio_num = static_cast<int>(board::kRgbPinPclk);
  cfg.disp_gpio_num = -1;
  for (int i = 0; i < 16; ++i) {
    cfg.data_gpio_nums[i] = board::kRgbDataPins[i];
  }
  cfg.flags.fb_in_psram = 1;

  const esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &s_panel);
  if (err != ESP_OK) {
    LOG_ERROR("Qualia: esp_lcd_new_rgb_panel failed: 0x%x", err);
    s_panel = nullptr;
    return false;
  }
  esp_lcd_panel_reset(s_panel);
  esp_lcd_panel_init(s_panel);
  quietenBus();

  void* fb = nullptr;
  if (esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb) != ESP_OK ||
      fb == nullptr) {
    LOG_ERROR("Qualia: get_frame_buffer failed");
    return false;
  }
  s_fb = static_cast<uint16_t*>(fb);
  // Black rather than whatever PSRAM held, so nothing garbage is on the glass
  // between the panel starting and the first frame being composed.
  memset(s_fb, 0, static_cast<size_t>(kW) * kH * sizeof(uint16_t));
  flushRows(0, kH);

  // Belt and braces on top of the peripheral reset above: ask the driver to
  // re-sync scan-out to VSYNC, which is Espressif's documented remedy for a
  // permanently shifted image.
  esp_lcd_rgb_panel_restart(s_panel);

  LOG_INFO("Qualia: RGB panel up, %dx%d @ %u MHz, 1 framebuffer", kW,
                kH, static_cast<unsigned>(board::kRgbPclkHz / 1000000));
  return true;
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

void rgbPresent(const uint16_t* src, int x, int y, int w, int h) {
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
  copyRegion(s_fb, src, r);
  flushRows(r.y0, r.y1);
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

}  // namespace hw::qualia
