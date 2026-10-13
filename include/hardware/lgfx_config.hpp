#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "board/board.h"
#include "config.h"

#if defined(BOARD_PANEL_RGB)

/**
 * An RGB panel is not a LovyanGFX device at all.
 *
 * The S3's LCD peripheral streams a framebuffer continuously and offers no
 * command channel, so there is nothing for LovyanGFX to talk to. esp_lcd owns
 * the output (hardware/rgb_panel.h) and LovyanGFX is used purely as a
 * rasteriser into ui::canvas's sprite.
 *
 * `tft` still exists because the shared UI code names it, but it is never
 * created and never drawn to: board::kPanelWritesDirect is false, so
 * ui::panel() hands back the canvas instead. A sprite is the cheapest type
 * that satisfies the symbol without pretending to be a panel.
 */
class LGFX : public lgfx::LGFX_Sprite {
 public:
  LGFX() { setColorDepth(16); }
};

#elif defined(BOARD_PANEL_AMOLED)

/**
 * LovyanGFX device: CO5300 AMOLED on quad SPI.
 *
 * The bus is the ST77916's, but the panel has no backlight -- setBrightness()
 * is a command to the controller -- and no touch controller hung off it: the
 * CST9217 is read by hand in hardware/amoled466/touch_raw.cpp. Panel_CO5300's
 * own geometry is another watch's 410x502, so all of it is set here.
 */

/**
 * Panel_CO5300, started as Waveshare's examples start this board's panel
 * (Arduino_GFX's CO5300 sequence) rather than as LovyanGFX starts LilyGO's
 * watch. LovyanGFX's list sends display-on straight after sleep-out, with no
 * wait between them; Arduino_GFX gives the controller 120 ms to wake. Here
 * sleep-out comes first and has its 120 ms, and the reset has the 200 ms
 * Arduino_GFX gives it (Panel_Device::init waits 64). Whether LovyanGFX's own
 * list would also do was not tried: the dark panel at bring-up turned out to
 * be the row-at-a-time present (see hardware/amoled466/display.cpp).
 */
struct Panel_CO5300_Waveshare : public lgfx::Panel_CO5300 {
  const uint8_t* getInitCommands(uint8_t listno) const override {
    static constexpr uint8_t list0[] = {
        0x00, CMD_INIT_DELAY, 140,  // NOP, finishing the reset's 200 ms
        0x11, CMD_INIT_DELAY, 120,  // sleep out
        0xFE, 1, 0x00,              // command page 0
        0xC4, 1, 0x80,              // SPI mode control
        0x3A, 1, 0x55,              // 16 bits a pixel
        0x53, 1, 0x20,              // brightness control on
        0x63, 1, 0xFF,              // brightness in high-brightness mode
        0x29, CMD_INIT_DELAY, 10,   // display on
        0x51, 1, 0xD0,              // brightness; displayInit() sets its own
        0x58, 1, 0x00,              // contrast enhancement off
        0xFF, 0xFF,                 // end
    };
    return listno == 0 ? list0 : nullptr;
  }
};

class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_SPI _bus;
  Panel_CO5300_Waveshare _panel;

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = board::kDisplaySpiWriteHz;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = static_cast<int>(board::kDisplayPinSclk);
      cfg.pin_io0 = static_cast<int>(board::kDisplayPinIo0);
      cfg.pin_io1 = static_cast<int>(board::kDisplayPinIo1);
      cfg.pin_io2 = static_cast<int>(board::kDisplayPinIo2);
      cfg.pin_io3 = static_cast<int>(board::kDisplayPinIo3);
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = static_cast<int>(board::kDisplayPinCs);
      cfg.pin_rst = static_cast<int>(board::kDisplayPinRst);
      cfg.pin_busy = -1;
      cfg.panel_width = board::kDisplayWidth;
      cfg.panel_height = board::kDisplayHeight;
      cfg.memory_width = board::kDisplayMemoryWidth;
      cfg.memory_height = board::kDisplayMemoryHeight;
      cfg.offset_x = board::kDisplayOffsetX;
      cfg.offset_y = board::kDisplayOffsetY;
      cfg.offset_rotation = 0;
      // Nothing reads the panel back: the frame is kept in PSRAM.
      cfg.readable = false;
      cfg.invert = board::kDisplayInvert;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};

#elif defined(BOARD_PANEL_QSPI)

/**
 * LovyanGFX device: ST77916 on quad SPI.
 *
 * Four data lines instead of MOSI and MISO, and no DC pin -- the command/data
 * distinction rides inside the QSPI transaction rather than on a wire.
 * LovyanGFX folds all of that into Bus_SPI behind LGFX_USE_QSPI, which it
 * defines for itself on an ESP32-S3 with IDF 4.4 or newer; where that does not
 * hold, pin_io0..io3 are ignored, the panel is set up as plain SPI, and it
 * stays blank without complaining.
 *
 * The CST816S hangs off the panel the same way it does on the SPI board, so
 * taps arrive through getTouch() already scaled to panel pixels and
 * hardware/round360/touch_raw.cpp has nothing to do but read them.
 */
class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_SPI _bus;
  lgfx::Panel_ST77916 _panel;
  lgfx::Light_PWM _light;
  lgfx::Touch_CST816S _touch;

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = board::kDisplaySpiWriteHz;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = static_cast<int>(board::kDisplayPinSclk);
      // Bus_SPI only takes the quad path when all four of these are set.
      cfg.pin_io0 = static_cast<int>(board::kDisplayPinIo0);
      cfg.pin_io1 = static_cast<int>(board::kDisplayPinIo1);
      cfg.pin_io2 = static_cast<int>(board::kDisplayPinIo2);
      cfg.pin_io3 = static_cast<int>(board::kDisplayPinIo3);
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = static_cast<int>(board::kDisplayPinCs);
      cfg.pin_rst = static_cast<int>(board::kDisplayPinRst);
      cfg.pin_busy = -1;
      cfg.panel_width = board::kDisplayWidth;
      cfg.panel_height = board::kDisplayHeight;
      cfg.memory_width = board::kDisplayWidth;
      cfg.memory_height = board::kDisplayHeight;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      // The QSPI read path is not implemented, so nothing may read back.
      cfg.readable = false;
      cfg.invert = board::kDisplayInvert;
      cfg.rgb_order = board::kDisplayRgbOrder;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = static_cast<int>(board::kDisplayPinBacklight);
      cfg.invert = board::kDisplayBacklightInvert;
      cfg.freq = board::kBacklightPwmHz;
      cfg.pwm_channel = board::kBacklightPwmChannel;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.i2c_port = board::kTouchI2cPort;
      cfg.i2c_addr = board::kTouchI2cAddress;
      cfg.pin_sda = static_cast<int>(board::kTouchPinSda);
      cfg.pin_scl = static_cast<int>(board::kTouchPinScl);
      cfg.pin_int = static_cast<int>(board::kTouchPinInt);
      cfg.pin_rst = static_cast<int>(board::kTouchPinRst);
      cfg.freq = board::kTouchI2cHz;
      // Touch is on I2C, the panel on quad SPI; nothing to hand back and
      // forth, and pin_io0..io3 are not the touch bus however similar the
      // numbers look.
      cfg.bus_shared = false;
      // The raster the controller reports, not the panel's -- though on this
      // board they are the same 360, which is unusual for a CST816S.
      cfg.x_min = 0;
      cfg.x_max = board::kTouchNativeSize - 1;
      cfg.y_min = 0;
      cfg.y_max = board::kTouchNativeSize - 1;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

#else

/** LovyanGFX device: GC9A01 on SPI, with the backlight and the CST816S hung
 *  off it where the board has them. Pin values come from config.h.
 *
 *  Attaching the touch controller here rather than driving it by hand means
 *  taps arrive through getTouch(), already scaled from the controller's raster
 *  to panel pixels -- see Panel_Device::touchCalibrate(). hardware/touch.cpp
 *  is then only press tracking and gesture classification. */
class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_SPI _bus;
  lgfx::Panel_GC9A01 _panel;
  lgfx::Light_PWM _light;
  lgfx::Touch_CST816S _touch;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.freq_write = board::kDisplaySpiWriteHz;
      cfg.pin_sclk = static_cast<int>(board::kDisplayPinSclk);
      cfg.pin_mosi = static_cast<int>(board::kDisplayPinMosi);
      cfg.pin_miso = -1;
      cfg.pin_dc = static_cast<int>(board::kDisplayPinDc);
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = static_cast<int>(board::kDisplayPinCs);
      cfg.pin_rst = static_cast<int>(board::kDisplayPinRst);
      cfg.panel_width = board::kDisplayWidth;
      cfg.panel_height = board::kDisplayHeight;
      cfg.memory_width = board::kDisplayWidth;
      cfg.memory_height = board::kDisplayHeight;
      cfg.invert = board::kDisplayInvert;
      cfg.rgb_order = board::kDisplayRgbOrder;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = static_cast<int>(board::kDisplayPinBacklight);
      cfg.invert = board::kDisplayBacklightInvert;
      cfg.freq = board::kBacklightPwmHz;
      cfg.pwm_channel = board::kBacklightPwmChannel;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      auto cfg = _touch.config();
      cfg.i2c_port = board::kTouchI2cPort;
      cfg.i2c_addr = board::kTouchI2cAddress;
      cfg.pin_sda = static_cast<int>(board::kTouchPinSda);
      cfg.pin_scl = static_cast<int>(board::kTouchPinScl);
      cfg.pin_int = static_cast<int>(board::kTouchPinInt);
      cfg.pin_rst = static_cast<int>(board::kTouchPinRst);
      cfg.freq = board::kTouchI2cHz;
      // Touch is on I2C, the panel on SPI; nothing to hand back and forth.
      cfg.bus_shared = false;
      // The raster the controller reports, not the panel's: setTouch() builds
      // the mapping between the two from exactly these numbers.
      cfg.x_min = 0;
      cfg.x_max = board::kTouchNativeSize - 1;
      cfg.y_min = 0;
      cfg.y_max = board::kTouchNativeSize - 1;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

#endif  // BOARD_PANEL_RGB
