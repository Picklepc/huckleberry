#pragma once
// LovyanGFX device definition for the QMSD ZX2D80CE02S "SC05_X" board.
// ESP32-S3-WROOM-1 N8R2, ST7789 240x320 over an 8080 8-bit parallel bus,
// FT5x06 capacitive touch (I2C1), LEDC PWM backlight.
//
// Pin map + ST7789 init list transcribed from the vendor's open Arduino
// library smartpanle/PanelLan_esp32_arduino (src/board/sc05_x). Reimplemented
// here as a standalone LGFX_Device so we do not carry any stock-firmware code.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

namespace huck {

// ST7789 panel with the SC05_X-specific init sequence (gamma / porch tuned by
// the vendor for this exact glass — using it gives correct colors).
class Panel_SC05X : public lgfx::v1::Panel_ST7789 {
protected:
  const uint8_t* getInitCommands(uint8_t listno) const override {
    using namespace lgfx::v1;
    static constexpr uint8_t list0[] = {
      0x11, 0 + CMD_INIT_DELAY, 120,
      0x36, 1, 0x00,
      0x3A, 1, 0x05,
      0xB2, 5, 0x0C, 0x0C, 0x00, 0x33, 0x33,
      0xB7, 1, 0x46,
      0xBB, 1, 0x1B,
      0xC0, 1, 0x2C,
      0xC2, 1, 0x01,
      0xC3, 1, 0x0F,
      0xC4, 1, 0x20,
      0xC6, 1, 0x0F,
      0xD0, 2, 0xA4, 0xA1,
      0xD6, 1, 0xA1,
      0xE0, 14, 0xF0, 0x00, 0x06, 0x04, 0x05, 0x05, 0x31, 0x44, 0x48, 0x36, 0x12, 0x12, 0x2B, 0x34,
      0xE1, 14, 0xF0, 0x0B, 0x0F, 0x0F, 0x0D, 0x26, 0x31, 0x43, 0x47, 0x38, 0x14, 0x14, 0x2C, 0x32,
      0x21, 0,
      0x29, 0,
      0x2C, 0,
      0xFF, 0xFF,
    };
    switch (listno) {
      case 0: return list0;
      default: return nullptr;
    }
  }
};

class LGFX : public lgfx::LGFX_Device {
  Panel_SC05X          _panel;
  lgfx::Bus_Parallel8  _bus;
  lgfx::Light_PWM      _light;
  lgfx::Touch_FT5x06   _touch;

public:
  LGFX() {
    { // 8080 8-bit parallel bus
      auto cfg = _bus.config();
      cfg.port       = 0;
      cfg.freq_write = 20000000;
      cfg.pin_wr = 17;
      cfg.pin_rd = -1;
      cfg.pin_rs = 18;   // D/C
      cfg.pin_d0 = 16;
      cfg.pin_d1 = 40;
      cfg.pin_d2 = 15;
      cfg.pin_d3 = 7;
      cfg.pin_d4 = 41;
      cfg.pin_d5 = 42;
      cfg.pin_d6 = 2;
      cfg.pin_d7 = 1;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    { // panel
      auto cfg = _panel.config();
      cfg.pin_cs   = -1;
      cfg.pin_rst  = 3;
      cfg.pin_busy = -1;
      cfg.memory_width   = 240;
      cfg.memory_height  = 320;
      cfg.panel_width    = 240;
      cfg.panel_height   = 320;
      cfg.offset_x       = 0;
      cfg.offset_y       = 0;
      cfg.offset_rotation = 2;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable   = false;
      cfg.invert     = true;
      cfg.rgb_order  = true;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    { // LEDC backlight
      auto cfg = _light.config();
      cfg.pin_bl      = 47;
      cfg.invert      = false;
      cfg.freq        = 21111;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    { // FT5x06 capacitive touch on I2C1
      auto cfg = _touch.config();
      cfg.x_min = 0;
      cfg.x_max = 239;
      cfg.y_min = 0;
      cfg.y_max = 319;
      cfg.pin_int = 48;
      cfg.pin_rst = -1;
      cfg.bus_shared = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port = 1;
      cfg.pin_sda  = 8;
      cfg.pin_scl  = 9;
      cfg.freq     = 400000;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

} // namespace huck
