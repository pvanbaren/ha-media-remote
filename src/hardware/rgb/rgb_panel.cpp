#include "hardware/rgb_panel.h"

#include <Arduino.h>

#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_private/periph_ctrl.h"
#include "soc/periph_defs.h"

#include "board/board.h"
#include "log.h"

namespace hw::rgb {
namespace {

constexpr int kW = board::kDisplayWidth;
constexpr int kH = board::kDisplayHeight;

/**
 * Drive the panel bus as gently as it will tolerate.
 *
 * Nineteen pins switch at the pixel clock a few centimetres from a 2.4 GHz
 * front end, and on the Qualia that was not a theoretical concern. Every HTTP
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
 * Each frame a fresh start, on a board that asks for it
 * (board::kRgbFrameByFrame): the panel in ESP-IDF's refresh-on-demand mode,
 * and the next frame asked for at the end of each one -- which stops the LCD
 * engine, resets it and its FIFO, points the DMA back at the top of the
 * framebuffer and starts again. That is how ESP-IDF 4.4 drove every RGB
 * panel. ESP-IDF 5 runs the engine without a break instead and, at each
 * VSYNC, resets only the FIFO and the DMA under it.
 */
IRAM_ATTR bool nextFrame(esp_lcd_panel_handle_t panel,
                         const esp_lcd_rgb_panel_event_data_t*, void*) {
  esp_lcd_rgb_panel_refresh(panel);
  return false;
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
  // can move it in place. See the note on s_fb in framebuffer.cpp.
  cfg.num_fbs = 1;

  // No bounce buffer, as a rule: GDMA reads the framebuffer straight out of
  // PSRAM. The 4848S040 is the exception, board::kRgbBounceBufferRows says
  // why; on every other board it is 0.
  //
  // A bounce buffer sounds like insurance and is closer to the opposite. It
  // puts the CPU in the scan-out path -- an interrupt has to move a slab of
  // PSRAM into internal SRAM before the LCD FIFO drains, every time, forever
  // -- so any stall becomes a visible glitch. It earns its place only when
  // PSRAM cannot feed the panel at all. The Qualia asks for 720*720*2 bytes at
  // 19.5 Hz, about 24 MB/s, which octal PSRAM can serve. What goes wrong on
  // that board is bursts -- a present or a compose holding the bus long
  // enough for the LCD FIFO to miss a deadline -- and a bounce buffer would
  // not escape those: it is filled from the same PSRAM, over the same bus,
  // and adds an interrupt deadline of its own.
  cfg.bounce_buffer_size_px =
      static_cast<size_t>(board::kRgbBounceBufferRows) * kW;
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
  cfg.flags.refresh_on_demand = board::kRgbFrameByFrame ? 1 : 0;

  esp_lcd_panel_handle_t panel = nullptr;
  const esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &panel);
  if (err != ESP_OK) {
    LOG_ERROR("RGB: esp_lcd_new_rgb_panel failed: 0x%x", err);
    return false;
  }
  esp_lcd_panel_reset(panel);
  esp_lcd_panel_init(panel);
  quietenBus();

  void* fb = nullptr;
  if (esp_lcd_rgb_panel_get_frame_buffer(panel, 1, &fb) != ESP_OK ||
      fb == nullptr) {
    LOG_ERROR("RGB: get_frame_buffer failed");
    return false;
  }
  rgbAdopt(panel, static_cast<uint16_t*>(fb));

  if constexpr (board::kRgbFrameByFrame) {
    esp_lcd_rgb_panel_event_callbacks_t callbacks = {};
    callbacks.on_vsync = nextFrame;
    esp_lcd_rgb_panel_register_event_callbacks(panel, &callbacks, nullptr);
    esp_lcd_rgb_panel_refresh(panel);  // the first; nextFrame() the rest
  } else {
    // Belt and braces on top of the peripheral reset above: ask the driver
    // to re-sync scan-out to VSYNC, which is Espressif's documented remedy
    // for a permanently shifted image.
    esp_lcd_rgb_panel_restart(panel);
  }

  LOG_INFO("RGB: panel up, %dx%d @ %u MHz, 1 framebuffer, %d bounce rows", kW,
           kH, static_cast<unsigned>(board::kRgbPclkHz / 1000000),
           board::kRgbBounceBufferRows);
  return true;
}

}  // namespace hw::rgb
