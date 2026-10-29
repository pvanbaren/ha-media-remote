#include "hardware/rgb_panel.h"

#include <Arduino.h>

#include "driver/gpio.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#include "board/board.h"
#include "log.h"

/**
 * The 4B's panel opened for hardware/rgb_panel.h: an ST7703 on two-lane
 * MIPI-DSI, set up over the link in command mode (DBI) and then left
 * scanning one framebuffer out of PSRAM in video mode (DPI), which is all
 * framebuffer.cpp needs of it.
 *
 * The order is Espressif's own panel drivers': the DPI panel created, which
 * allocates the framebuffer; the panel reset; its registers; and only then
 * the video started, so the controller comes out of sleep into a picture
 * rather than into noise.
 */
namespace hw::rgb {
namespace {

constexpr int kW = board::kDisplayWidth;
constexpr int kH = board::kDisplayHeight;

/** One register write: command, parameters, and how long to wait after. */
struct InitCmd {
  uint8_t cmd;
  uint8_t len;
  uint16_t delay_ms;
  uint8_t data[64];
};

/** Waveshare's ST7703 sequence for this panel, as HomeTiles sends it
 *  (src/devices/waveshare_4b/vendor/displays_config.h). Ends with sleep-out
 *  and display-on. */
constexpr InitCmd kInit[] = {
    {0xB9, 3, 0, {0xF1, 0x12, 0x83}},
    {0xB1, 5, 0, {0x00, 0x00, 0x00, 0xDA, 0x80}},
    {0xB2, 3, 0, {0x3C, 0x12, 0x30}},
    {0xB3, 10, 0, {0x10, 0x10, 0x28, 0x28, 0x03, 0xFF, 0x00, 0x00, 0x00, 0x00}},
    {0xB4, 1, 0, {0x80}},
    {0xB5, 2, 0, {0x0A, 0x0A}},
    {0xB6, 2, 0, {0x97, 0x97}},
    {0xB8, 4, 0, {0x26, 0x22, 0xF0, 0x13}},
    {0xBA, 27, 0, {0x31, 0x81, 0x0F, 0xF9, 0x0E, 0x06, 0x20, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x00, 0x44, 0x25, 0x00, 0x90, 0x0A, 0x00,
                   0x00, 0x01, 0x4F, 0x01, 0x00, 0x00, 0x37}},
    {0xBC, 1, 0, {0x47}},
    {0xBF, 3, 0, {0x02, 0x11, 0x00}},
    {0xC0, 9, 0, {0x73, 0x73, 0x50, 0x50, 0x00, 0x00, 0x12, 0x70, 0x00}},
    {0xC1, 12, 0, {0x25, 0x00, 0x32, 0x32, 0x77, 0xE4, 0xFF, 0xFF, 0xCC, 0xCC,
                   0x77, 0x77}},
    {0xC6, 6, 0, {0x82, 0x00, 0xBF, 0xFF, 0x00, 0xFF}},
    {0xC7, 6, 0, {0xB8, 0x00, 0x0A, 0x10, 0x01, 0x09}},
    {0xC8, 4, 0, {0x10, 0x40, 0x1E, 0x02}},
    {0xCC, 1, 0, {0x0B}},
    {0xE0, 34, 0, {0x00, 0x0B, 0x10, 0x2C, 0x3D, 0x3F, 0x42, 0x3A, 0x07, 0x0D,
                   0x0F, 0x13, 0x15, 0x13, 0x14, 0x0F, 0x16, 0x00, 0x0B, 0x10,
                   0x2C, 0x3D, 0x3F, 0x42, 0x3A, 0x07, 0x0D, 0x0F, 0x13, 0x15,
                   0x13, 0x14, 0x0F, 0x16}},
    {0xE3, 14, 0, {0x07, 0x07, 0x0B, 0x0B, 0x0B, 0x0B, 0x00, 0x00, 0x00, 0x00,
                   0xFF, 0x00, 0xC0, 0x10}},
    {0xE9, 63, 0, {0xC8, 0x10, 0x0A, 0x00, 0x00, 0x80, 0x81, 0x12, 0x31, 0x23,
                   0x4F, 0x86, 0xA0, 0x00, 0x47, 0x08, 0x00, 0x00, 0x0C, 0x00,
                   0x00, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x98, 0x02,
                   0x8B, 0xAF, 0x46, 0x02, 0x88, 0x88, 0x88, 0x88, 0x88, 0x98,
                   0x13, 0x8B, 0xAF, 0x57, 0x13, 0x88, 0x88, 0x88, 0x88, 0x88,
                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00}},
    {0xEA, 61, 0, {0x97, 0x0C, 0x09, 0x09, 0x09, 0x78, 0x00, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x9F, 0x31, 0x8B, 0xA8, 0x31, 0x75, 0x88, 0x88,
                   0x88, 0x88, 0x88, 0x9F, 0x20, 0x8B, 0xA8, 0x20, 0x64, 0x88,
                   0x88, 0x88, 0x88, 0x88, 0x23, 0x00, 0x00, 0x02, 0x71, 0x00,
                   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                   0x00, 0x00, 0x00, 0x00, 0x40, 0x80, 0x81, 0x00, 0x00, 0x00,
                   0x00}},
    {0xEF, 3, 0, {0xFF, 0xFF, 0x01}},
    {0x11, 0, 250, {}},  // sleep out
    {0x29, 0, 50, {}},   // display on
};

/** An LDO channel at a voltage, held for good. */
esp_ldo_channel_handle_t acquireLdo(int channel, int millivolts) {
  esp_ldo_channel_config_t cfg = {};
  cfg.chan_id = channel;
  cfg.voltage_mv = millivolts;
  esp_ldo_channel_handle_t ldo = nullptr;
  const esp_err_t err = esp_ldo_acquire_channel(&cfg, &ldo);
  if (err != ESP_OK) {
    LOG_ERROR("DSI: LDO %d at %d mV failed: 0x%x", channel, millivolts, err);
    return nullptr;
  }
  return ldo;
}

/**
 * Power the PHY and the panel, each off and on again first.
 *
 * After a restart without a power cut -- an update, a crash -- both are
 * still powered and the panel still mid-way through whatever the last
 * firmware had it doing. HomeTiles cycles both for that reason, the panel
 * with its reset held, and so does this.
 */
bool powerUp() {
  pinMode(board::kDisplayPinRst, OUTPUT);
  digitalWrite(board::kDisplayPinRst, LOW);

  esp_ldo_channel_handle_t phy =
      acquireLdo(board::kDsiPhyLdoChannel, board::kDsiPhyLdoMillivolts);
  esp_ldo_channel_handle_t panel =
      acquireLdo(board::kPanelLdoChannel, board::kPanelLdoMillivolts);
  if (phy == nullptr || panel == nullptr) {
    return false;
  }
  esp_ldo_release_channel(phy);
  esp_ldo_release_channel(panel);
  delay(350);
  return acquireLdo(board::kDsiPhyLdoChannel, board::kDsiPhyLdoMillivolts) !=
             nullptr &&
         acquireLdo(board::kPanelLdoChannel, board::kPanelLdoMillivolts) !=
             nullptr;
}

}  // namespace

bool rgbInit() {
  if (!powerUp()) {
    return false;
  }
  delay(150);

  esp_lcd_dsi_bus_config_t bus_cfg = {};
  bus_cfg.bus_id = 0;
  bus_cfg.num_data_lanes = board::kDsiLanes;
  bus_cfg.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
  bus_cfg.lane_bit_rate_mbps = board::kDsiLaneMbps;
  esp_lcd_dsi_bus_handle_t bus = nullptr;
  esp_err_t err = esp_lcd_new_dsi_bus(&bus_cfg, &bus);
  if (err != ESP_OK) {
    LOG_ERROR("DSI: esp_lcd_new_dsi_bus failed: 0x%x", err);
    return false;
  }

  esp_lcd_dbi_io_config_t io_cfg = {};
  io_cfg.virtual_channel = 0;
  io_cfg.lcd_cmd_bits = 8;
  io_cfg.lcd_param_bits = 8;
  esp_lcd_panel_io_handle_t io = nullptr;
  err = esp_lcd_new_panel_io_dbi(bus, &io_cfg, &io);
  if (err != ESP_OK) {
    LOG_ERROR("DSI: esp_lcd_new_panel_io_dbi failed: 0x%x", err);
    return false;
  }

  esp_lcd_dpi_panel_config_t dpi_cfg = {};
  dpi_cfg.virtual_channel = 0;
  dpi_cfg.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dpi_cfg.dpi_clock_freq_mhz = board::kDsiPixelClockMhz;
  dpi_cfg.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
  // One framebuffer, as on the RGB boards: it always holds what is on the
  // glass, so a scrolled list can be moved in place.
  dpi_cfg.num_fbs = 1;
  dpi_cfg.video_timing.h_size = kW;
  dpi_cfg.video_timing.v_size = kH;
  dpi_cfg.video_timing.hsync_pulse_width = board::kDsiHsyncPulse;
  dpi_cfg.video_timing.hsync_back_porch = board::kDsiHsyncBackPorch;
  dpi_cfg.video_timing.hsync_front_porch = board::kDsiHsyncFrontPorch;
  dpi_cfg.video_timing.vsync_pulse_width = board::kDsiVsyncPulse;
  dpi_cfg.video_timing.vsync_back_porch = board::kDsiVsyncBackPorch;
  dpi_cfg.video_timing.vsync_front_porch = board::kDsiVsyncFrontPorch;
  esp_lcd_panel_handle_t panel = nullptr;
  err = esp_lcd_new_panel_dpi(bus, &dpi_cfg, &panel);
  if (err != ESP_OK) {
    LOG_ERROR("DSI: esp_lcd_new_panel_dpi failed: 0x%x", err);
    return false;
  }

  // Out of reset, then its registers, sleep-out and display-on last.
  digitalWrite(board::kDisplayPinRst, LOW);
  delay(120);
  digitalWrite(board::kDisplayPinRst, HIGH);
  delay(300);
  for (const InitCmd& c : kInit) {
    err = esp_lcd_panel_io_tx_param(io, c.cmd, c.len ? c.data : nullptr,
                                    c.len);
    if (err != ESP_OK) {
      LOG_ERROR("DSI: register 0x%02X not sent: 0x%x", c.cmd, err);
      return false;
    }
    if (c.delay_ms) {
      delay(c.delay_ms);
    }
  }

  void* fb = nullptr;
  if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb) != ESP_OK ||
      fb == nullptr) {
    LOG_ERROR("DSI: get_frame_buffer failed");
    return false;
  }
  rgbAdopt(panel, static_cast<uint16_t*>(fb));

  // The video, from the framebuffer just cleared.
  err = esp_lcd_panel_init(panel);
  if (err != ESP_OK) {
    LOG_ERROR("DSI: panel init failed: 0x%x", err);
    return false;
  }

  LOG_INFO("DSI: panel up, %dx%d @ %u MHz, %d lanes at %u Mbit/s", kW, kH,
           static_cast<unsigned>(board::kDsiPixelClockMhz), board::kDsiLanes,
           static_cast<unsigned>(board::kDsiLaneMbps));
  return true;
}

}  // namespace hw::rgb
