#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "board/waveshare_s3.h"
#include "config.h"

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
