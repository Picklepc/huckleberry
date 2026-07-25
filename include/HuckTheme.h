#pragma once
// Huckleberry theming — a black-face digital-LED baseline plus swappable
// holiday/seasonal skins that recolor every screen. Central-Valley native
// flowers (poppy orange, lupine purple, tidy-tips yellow) and Indie the horse
// are the recurring motifs. Kept deliberately dim/warm so the clock never
// disturbs sleep in the trailer.

#include <lvgl.h>

struct HuckTheme {
  const char* name;
  lv_color_t  bg;        // screen / clock-face background
  lv_color_t  panel;     // cards / raised surfaces
  lv_color_t  seg_on;    // lit LED segment
  lv_color_t  seg_off;   // unlit segment (near-invisible on baseline)
  lv_opa_t    seg_off_opa;
  lv_color_t  accent;    // primary flower accent
  lv_color_t  accent2;   // secondary accent
  lv_color_t  text;      // muted body text
  lv_color_t  text_hi;   // brighter text
  uint8_t     brightness; // default backlight 0-255 (kept low for sleep)
};

// Palette entries are ordered; index 0 is the everyday baseline.
static const HuckTheme HUCK_THEMES[] = {
  // Midnight Meadow — the black-face amber-LED baseline.
  { "Midnight Meadow",
    lv_color_hex(0x000000), lv_color_hex(0x0d0a06),
    lv_color_hex(0xE0611A), lv_color_hex(0x140a03), LV_OPA_0,
    lv_color_hex(0xF4791F), lv_color_hex(0x8f7bd6),
    lv_color_hex(0x7c7264), lv_color_hex(0xc9b594), 60 },

  // Poppy Fields — spring/day skin, warmer and a touch brighter.
  { "Poppy Fields",
    lv_color_hex(0x0a0602), lv_color_hex(0x171008),
    lv_color_hex(0xFF7A1E), lv_color_hex(0x241206), 20,
    lv_color_hex(0xFF7A1E), lv_color_hex(0x7db56a),
    lv_color_hex(0x9a8a6a), lv_color_hex(0xffd9a0), 85 },

  // Valentine — Indie hearts, soft rose LEDs.
  { "Valentine",
    lv_color_hex(0x0b0305), lv_color_hex(0x1a070d),
    lv_color_hex(0xE64A6B), lv_color_hex(0x230510), 16,
    lv_color_hex(0xF76F8E), lv_color_hex(0xd98fb8),
    lv_color_hex(0x8a6472), lv_color_hex(0xffc2d4), 70 },

  // Evergreen — Christmas, warm green LEDs with gold accent.
  { "Evergreen",
    lv_color_hex(0x02060a), lv_color_hex(0x061109),
    lv_color_hex(0x2FA45A), lv_color_hex(0x04130a), 18,
    lv_color_hex(0xE8B54B), lv_color_hex(0xC8452F),
    lv_color_hex(0x6f8574), lv_color_hex(0xd8f0d0), 72 },

  // Harvest — Halloween/fall, ember orange on deep brown.
  { "Harvest",
    lv_color_hex(0x080401), lv_color_hex(0x140a03),
    lv_color_hex(0xF06A12), lv_color_hex(0x1e0d02), 20,
    lv_color_hex(0xF6A21E), lv_color_hex(0x8f5cc0),
    lv_color_hex(0x8a6f4a), lv_color_hex(0xf0c88a), 78 },
};

static const size_t HUCK_THEME_COUNT = sizeof(HUCK_THEMES) / sizeof(HUCK_THEMES[0]);

// Night mode (auto, 8pm-8am): pure black face, white LEDs, dimmest backlight.
// Overrides the selected day theme so nothing bright disturbs sleep.
static const HuckTheme HUCK_NIGHT_THEME = {
  "Night",
  lv_color_hex(0x000000), lv_color_hex(0x070707),
  lv_color_hex(0xEDEDED), lv_color_hex(0x0c0c0c), LV_OPA_0,
  lv_color_hex(0xBFC7D0), lv_color_hex(0x9aa4b0),
  lv_color_hex(0x565b61), lv_color_hex(0xcfd4da), 30
};

// Night window: hour >= 20 (8pm) or hour < 8 (before 8am).
static inline bool huck_is_night(int hour24) { return hour24 >= 20 || hour24 < 8; }
