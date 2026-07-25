#ifndef LV_CONF_H
#define LV_CONF_H

// Partial override file — anything not set here uses LVGL 8.4 defaults.
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (64U * 1024U)

#define LV_TICK_CUSTOM 0
#define LV_USE_LOG 0

#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 0

// Fonts we use across the clock + web-parity UI.
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

#define LV_USE_ANIMATION 1
#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_BTN 1
#define LV_USE_CANVAS 1
#define LV_USE_LABEL 1
#define LV_USE_LINE 1
#define LV_USE_METER 1
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_OBJ 1

#endif // LV_CONF_H
