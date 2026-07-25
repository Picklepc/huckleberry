// Huckleberry — horse-trailer command center (Phase A bring-up)
//
// Proves the QMSD ZX2D80CE02S board (ST7789 8080 8-bit + FT5x06 touch) and
// establishes the whole on-device UX skeleton:
//   Clock (home)  --tap-->  Thermostat  --swipe-->  Battery  --swipe-->  Status
//
// Theming rules:
//   * Day mode: every page uses the selected theme.
//   * Night mode (8pm-8am): applies ONLY to the home clock page -> black face,
//     white LEDs, dimmest. Other pages stay fully themed.
//   * After 1 minute of inactivity on any non-home page, snap back to the clock.
//
// Phase B layers in: Wi-Fi STA/AP, web app, NimBLE (Eco-Worthy battery + Victron
// MPPT), real Gidrox thermostat control, scheduling, browser/NTP time sync, and
// background-driven display layouts.

#include <Arduino.h>
#include <lvgl.h>
#include <time.h>
#include <sys/time.h>
#include <vector>
#include <SPIFFS.h>

#include "LGFX_Huckleberry.hpp"
#include "HuckTheme.h"
#include "SevenSegClock.h"
#include "AppState.h"
#include "Settings.h"
#include "Net.h"
#include "BleManager.h"
#include "WebApp.h"

LV_FONT_DECLARE(huck_font_heart_18);

#ifndef HUCK_DEBUG
#define HUCK_DEBUG 0
#endif
#if HUCK_DEBUG
#define HDBG(...) Serial.printf(__VA_ARGS__)
#else
#define HDBG(...)
#endif

// ---------------------------------------------------------------- display glue
static huck::LGFX lcd;
static lgfx::LGFX_Sprite gBgSprites[PAGE_COUNT] = {
  lgfx::LGFX_Sprite(&lcd), lgfx::LGFX_Sprite(&lcd),
  lgfx::LGFX_Sprite(&lcd), lgfx::LGFX_Sprite(&lcd)
};
static lv_img_dsc_t gBgDsc[PAGE_COUNT];
static String gLoadedBg[PAGE_COUNT];
static bool gBgReady[PAGE_COUNT] = {false, false, false, false};

static constexpr int SCR_W = 320;
static constexpr int SCR_H = 240;
static constexpr int BUF_LINES = 40;

static lv_color_t lvbuf1[SCR_W * BUF_LINES];
static lv_color_t lvbuf2[SCR_W * BUF_LINES];
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

static void disp_flush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px) {
  const int w = area->x2 - area->x1 + 1;
  const int h = area->y2 - area->y1 + 1;
  lcd.startWrite();
  lcd.setAddrWindow(area->x1, area->y1, w, h);
  lcd.writePixels(reinterpret_cast<lgfx::rgb565_t*>(px), w * h);
  lcd.endWrite();
  lv_disp_flush_ready(drv);
}

static void touch_read(lv_indev_drv_t*, lv_indev_data_t* data) {
  int32_t x, y;
  if (lcd.getTouch(&x, &y)) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ------------------------------------------------------------------- app state
static SevenSegClock gClock;
static lv_obj_t* gTileview = nullptr;
static lv_obj_t* gTiles[4] = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t* gDots[4]  = {nullptr, nullptr, nullptr, nullptr};

// Objects to recolor on theme change, split by which theme governs them.
// Home-tile objects flip with day/night; the rest always follow the day theme.
static bool gBuildingHome = false;
static std::vector<lv_obj_t*> gHomeAccent, gHomeHi;
static std::vector<lv_obj_t*> gDayAccent, gDayPanel, gDayHi;
// Day-home overlay objects hide in night mode so the locked LED clock remains minimal.
static std::vector<lv_obj_t*> gNightHide;

static int  gDayThemeIdx = 0;
static bool gNightNow = false;
static lv_obj_t* gDateLabel = nullptr;
static lv_obj_t* gAmPmLabel = nullptr;
static lv_obj_t* gThermoSetLabel = nullptr;
static lv_obj_t* gPageBg[PAGE_COUNT] = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t* gPageBgImg[PAGE_COUNT] = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t* gDayClockGroup = nullptr;
static lv_obj_t* gDayGreeting = nullptr;
static lv_obj_t* gDayHeartsTop = nullptr;
static lv_obj_t* gDayHeartsBottom = nullptr;
static lv_obj_t* gDayTimePanel = nullptr;
static lv_obj_t* gDayTimeLabel = nullptr;
static lv_obj_t* gDayAmPmLabel = nullptr;
static lv_obj_t* gDayDateLabel = nullptr;
static std::vector<lv_obj_t*> gDayOnly;

// Live-data widgets updated from telemetry/net.
static lv_obj_t* gBSocArc=nullptr; static lv_obj_t* gBSocLbl=nullptr;
static lv_obj_t* gBNetW=nullptr;   static lv_obj_t* gBSolarW=nullptr;
static lv_obj_t* gBSolarPct=nullptr; static lv_obj_t* gBChargeState=nullptr;
static lv_obj_t* gStWifi=nullptr; static lv_obj_t* gStSsid=nullptr; static lv_obj_t* gStIp=nullptr;
static lv_obj_t* gStAp=nullptr;   static lv_obj_t* gStBatt=nullptr; static lv_obj_t* gStSolar=nullptr;
static lv_obj_t* gStTime=nullptr;
static lv_obj_t* gThermoCard=nullptr; static lv_obj_t* gPowerGauge=nullptr;
static lv_obj_t* gPowerGroup=nullptr; static lv_obj_t* gPowerStatsCard=nullptr; static lv_obj_t* gStatusCard=nullptr;

static const HuckTheme& dayTheme()  { return HUCK_THEMES[gDayThemeIdx]; }
static const HuckTheme& homeTheme() { return gNightNow ? HUCK_NIGHT_THEME : dayTheme(); }
static const HuckTheme& displayTheme(DisplayPage page) {
  int idx = gSettings.pageTheme[page];
  if (idx < 0 || (size_t)idx >= HUCK_THEME_COUNT) idx = gDayThemeIdx;
  if (idx < 0 || (size_t)idx >= HUCK_THEME_COUNT) idx = 0;
  return HUCK_THEMES[idx];
}

static int activeTileIndex() {
  if (!gTileview) return 0;
  return (lv_obj_get_scroll_x(gTileview) + SCR_W / 2) / SCR_W;
}

// ------------------------------------------------------------------ ui helpers
static lv_obj_t* mkLabel(lv_obj_t* parent, const char* txt, const lv_font_t* font,
                         lv_color_t color, lv_align_t align, int xo, int yo) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_obj_align(l, align, xo, yo);
  return l;
}

static lv_obj_t* mkPanel(lv_obj_t* parent, int w, int h, lv_align_t align, int xo, int yo) {
  lv_obj_t* p = lv_obj_create(parent);
  lv_obj_remove_style_all(p);
  lv_obj_set_size(p, w, h);
  lv_obj_align(p, align, xo, yo);
  lv_obj_set_style_radius(p, 14, 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_80, 0);
  lv_obj_set_style_border_width(p, 1, 0);
  lv_obj_set_style_border_opa(p, LV_OPA_60, 0);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  gDayPanel.push_back(p);   // panels only exist on the day-themed pages
  return p;
}

static void trackHi(lv_obj_t* o)     { (gBuildingHome ? gHomeHi : gDayHi).push_back(o); }
static void trackDayOnly(lv_obj_t* o) { gDayOnly.push_back(o); }

static int layoutZoom(LayoutWidget w) {
  int scale = gSettings.layout[w].scale;
  if (scale < 50) scale = 50;
  if (scale > 180) scale = 180;
  return scale * 256 / 100;
}

static LayoutSlot clampRuntimeSlot(LayoutWidget w, LayoutSlot slot) {
  static const uint16_t dims[LAYOUT_WIDGET_COUNT][2] = {
    {252, 130}, {230, 24}, {158, 132}, {294, 138}, {146, 132}, {154, 152}
  };
  static const uint8_t minScale[LAYOUT_WIDGET_COUNT] = {70, 70, 75, 75, 75, 75};
  static const uint8_t maxScale[LAYOUT_WIDGET_COUNT] = {135, 140, 150, 125, 150, 150};
  int idx = (int)w;
  slot.scale = constrain((int)slot.scale, (int)minScale[idx], (int)maxScale[idx]);
  int scaledW = ((int)dims[idx][0] * slot.scale + 99) / 100;
  int scaledH = ((int)dims[idx][1] * slot.scale + 99) / 100;
  int minX = min(0, SCR_W - scaledW);
  int maxX = max(0, SCR_W - scaledW);
  int minY = min(0, SCR_H - scaledH);
  int maxY = max(0, SCR_H - scaledH);
  slot.x = constrain((int)slot.x, minX, maxX);
  slot.y = constrain((int)slot.y, minY, maxY);
  return slot;
}

static void applyLayout(lv_obj_t* o, LayoutWidget w) {
  if (!o) return;
  LayoutSlot raw = gSettings.layout[w];
  LayoutSlot slot = clampRuntimeSlot(w, raw);
  if (slot.x != raw.x || slot.y != raw.y || slot.scale != raw.scale) {
    HDBG("[LAYOUT] clamp w=%d (%d,%d,%u)->(%d,%d,%u)\n", (int)w,
         raw.x, raw.y, raw.scale, slot.x, slot.y, slot.scale);
    gSettings.layout[w] = slot;
  }
  lv_obj_set_pos(o, slot.x, slot.y);
  lv_obj_set_style_transform_pivot_x(o, 0, 0);
  lv_obj_set_style_transform_pivot_y(o, 0, 0);
  lv_obj_set_style_transform_zoom(o, layoutZoom(w), 0);
}

static DisplayPage pageForObject(lv_obj_t* o) {
  for (lv_obj_t* p = o; p; p = lv_obj_get_parent(p)) {
    for (int i = 0; i < PAGE_COUNT; i++) {
      if (p == gTiles[i]) return (DisplayPage)i;
    }
  }
  return PAGE_CLOCK;
}

static bool spiffsMounted() {
  static bool ok = false;
  if (!ok) ok = SPIFFS.begin(false);
  return ok;
}

static void byteSwapRgb565(void* buffer, size_t pixelCount) {
  auto* p = static_cast<uint16_t*>(buffer);
  for (size_t i = 0; i < pixelCount; i++) {
    p[i] = (uint16_t)((p[i] << 8) | (p[i] >> 8));
  }
}

static void loadPageBackground(DisplayPage page, bool force = false) {
  if (!gPageBgImg[page]) return;
  const String& bgName = gSettings.pageBg[page];
  if (!force && gBgReady[page] && gLoadedBg[page] == bgName) {
    lv_obj_clear_flag(gPageBgImg[page], LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_add_flag(gPageBgImg[page], LV_OBJ_FLAG_HIDDEN);
  gBgReady[page] = false;
  gLoadedBg[page] = "";
  if (bgName.isEmpty() || !spiffsMounted() || !psramFound()) return;

  String path = String("/bg/") + bgName;
  if (!SPIFFS.exists(path)) return;

  lgfx::LGFX_Sprite& sprite = gBgSprites[page];
  sprite.setPsram(true);
  sprite.setColorDepth(16);
  if (!sprite.getBuffer() && !sprite.createSprite(SCR_W, SCR_H)) return;
  sprite.fillScreen(0);
  if (!sprite.drawJpgFile(SPIFFS, path.c_str(), 0, 0, SCR_W, SCR_H, 0, 0, 1.0f)) return;
  byteSwapRgb565(sprite.getBuffer(), SCR_W * SCR_H);

  gBgDsc[page].header.always_zero = 0;
  gBgDsc[page].header.w = SCR_W;
  gBgDsc[page].header.h = SCR_H;
  gBgDsc[page].header.cf = LV_IMG_CF_TRUE_COLOR;
  gBgDsc[page].data_size = SCR_W * SCR_H * sizeof(lv_color_t);
  gBgDsc[page].data = static_cast<const uint8_t*>(sprite.getBuffer());
  lv_img_set_src(gPageBgImg[page], &gBgDsc[page]);
  lv_obj_clear_flag(gPageBgImg[page], LV_OBJ_FLAG_HIDDEN);
  gLoadedBg[page] = bgName;
  gBgReady[page] = true;
}

static void loadPageBackgrounds(bool force = false) {
  for (int i = 0; i < PAGE_COUNT; i++) loadPageBackground((DisplayPage)i, force);
}

static lv_obj_t* buildPageBackground(lv_obj_t* tile, DisplayPage page, bool dayOnly) {
  lv_obj_t* fill = lv_obj_create(tile);
  lv_obj_remove_style_all(fill);
  lv_obj_set_size(fill, SCR_W, SCR_H);
  lv_obj_set_pos(fill, 0, 0);
  lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
  lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  gPageBg[page] = fill;
  if (dayOnly) trackDayOnly(fill);

  lv_obj_t* img = lv_img_create(tile);
  lv_obj_set_pos(img, 0, 0);
  lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
  gPageBgImg[page] = img;
  if (dayOnly) trackDayOnly(img);
  return fill;
}

// ------------------------------------------------------------------ theming
// True if hour h falls in [start,end), wrapping past midnight when start>end.
static bool hourInWindow(int h, int start, int end) {
  if (start == end) return false;
  return (start < end) ? (h >= start && h < end) : (h >= start || h < end);
}
static bool computeNight(int hour) {
  return gSettings.autoNight && hourInWindow(hour, gSettings.nightStartHour, gSettings.nightEndHour);
}

static void setBrightnessForPage() {
  bool homeNight = gNightNow && activeTileIndex() == 0;
  int dayB = gSettings.dayBrightness > 0 ? gSettings.dayBrightness : dayTheme().brightness;
  lcd.setBrightness(homeNight ? gSettings.nightBrightness : dayB);
}

// Scheduled display-off: blank the backlight during the off-window after a short
// inactivity, and wake on touch (inactivity resets when the screen is touched).
static void updateDisplayPower() {
  static bool blanked = false;
  time_t now = time(nullptr); struct tm lt; localtime_r(&now, &lt);
  bool offWindow = gSettings.dispOffEnable &&
                   hourInWindow(lt.tm_hour, gSettings.dispOffStartHour, gSettings.dispOffEndHour);
  bool inactive = lv_disp_get_inactive_time(nullptr) > 15000;
  if (offWindow && inactive) {
    if (!blanked) { lcd.setBrightness(0); blanked = true; }
  } else if (blanked) {
    blanked = false;
    setBrightnessForPage();
  }
}

// Night mode keeps the home page minimal: clock + date only.
static void updateHomeDecorVisibility() {
  for (auto* o : gNightHide) {
    if (gNightNow) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
  for (auto* o : gDayOnly) {
    if (gNightNow) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
}

static bool clockUsesFramelessDarkStyle() {
  return gSettings.pageBg[PAGE_CLOCK] == "bg_indie_02.jpg";
}

static void applyDayHomeTheme() {
  const HuckTheme& t = displayTheme(PAGE_CLOCK);
  const bool frameless = clockUsesFramelessDarkStyle();
  const bool useBox = gSettings.pageBox[PAGE_CLOCK];
  const lv_color_t clockText = frameless ? lv_color_hex(0x243016) : t.text_hi;
  const lv_color_t clockAccent = frameless ? lv_color_hex(0x7A2118) : t.accent;
  const lv_color_t clockAccent2 = frameless ? lv_color_hex(0x5A3A16) : t.accent2;
  const lv_color_t dateText = frameless ? lv_color_hex(0x3A2A13) : t.text_hi;
  if (gPageBg[PAGE_CLOCK]) {
    lv_obj_set_style_bg_color(gPageBg[PAGE_CLOCK], t.bg, 0);
    lv_obj_set_style_bg_opa(gPageBg[PAGE_CLOCK], LV_OPA_COVER, 0);
  }
  if (gDayTimePanel) {
    lv_obj_set_style_bg_color(gDayTimePanel, t.panel, 0);
    lv_obj_set_style_border_color(gDayTimePanel, t.accent, 0);
    lv_obj_set_style_bg_opa(gDayTimePanel, useBox ? LV_OPA_90 : LV_OPA_0, 0);
    lv_obj_set_style_border_opa(gDayTimePanel, useBox ? LV_OPA_80 : LV_OPA_0, 0);
  }
  if (gDayGreeting) lv_obj_set_style_text_color(gDayGreeting, clockAccent, 0);
  if (gDayHeartsTop) lv_obj_set_style_text_color(gDayHeartsTop, clockAccent2, 0);
  if (gDayHeartsBottom) lv_obj_set_style_text_color(gDayHeartsBottom, clockAccent2, 0);
  if (gDayTimeLabel) lv_obj_set_style_text_color(gDayTimeLabel, clockText, 0);
  if (gDayAmPmLabel) lv_obj_set_style_text_color(gDayAmPmLabel, clockAccent, 0);
  if (gDayDateLabel) lv_obj_set_style_text_color(gDayDateLabel, dateText, 0);
}

static void applyDisplayLayouts() {
  applyLayout(gDayClockGroup, LAYOUT_DAY_CLOCK);
  applyLayout(gDayDateLabel, LAYOUT_DAY_DATE);
  applyLayout(gThermoCard, LAYOUT_THERMO_CARD);
  applyLayout(gPowerGroup, LAYOUT_POWER_GAUGE);
  applyLayout(gStatusCard, LAYOUT_STATUS_CARD);
}

static void applyHomeTheme() {
  const HuckTheme& t = homeTheme();
  if (gTiles[0]) {
    lv_obj_set_style_bg_color(gTiles[0], t.bg, 0);
    lv_obj_set_style_bg_opa(gTiles[0], LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(gTiles[0], t.text, 0);
  }
  gClock.applyTheme(t);
  for (auto* o : gHomeAccent) lv_obj_set_style_bg_color(o, t.accent, 0);
  for (auto* o : gHomeHi)     lv_obj_set_style_text_color(o, t.text_hi, 0);

  int act = activeTileIndex();
  for (int i = 0; i < 4; i++) {
    if (!gDots[i]) continue;
    bool on = (i == act);
    lv_obj_set_style_bg_color(gDots[i], on ? t.accent : t.text, 0);
    lv_obj_set_style_bg_opa(gDots[i], on ? LV_OPA_COVER : LV_OPA_40, 0);
  }
  applyDayHomeTheme();
  updateHomeDecorVisibility();
}

static void applyDayTheme() {
  for (int i = 1; i < 4; i++) {
    if (!gTiles[i]) continue;
    const HuckTheme& t = displayTheme((DisplayPage)i);
    lv_obj_set_style_bg_color(gTiles[i], t.bg, 0);
    lv_obj_set_style_bg_opa(gTiles[i], LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(gTiles[i], t.text, 0);
  }
  for (int i = PAGE_CLIMATE; i < PAGE_COUNT; i++) {
    if (!gPageBg[i]) continue;
    const HuckTheme& t = displayTheme((DisplayPage)i);
    lv_obj_set_style_bg_color(gPageBg[i], t.bg, 0);
    lv_obj_set_style_bg_opa(gPageBg[i], LV_OPA_COVER, 0);
  }
  for (auto* o : gDayPanel) {
    DisplayPage p = pageForObject(o);
    const HuckTheme& t = displayTheme(p);
    bool useBox = gSettings.pageBox[p];
    lv_obj_set_style_bg_color(o, t.panel, 0);
    lv_obj_set_style_bg_opa(o, useBox ? LV_OPA_80 : LV_OPA_0, 0);
    lv_obj_set_style_border_color(o, t.accent, 0);
    lv_obj_set_style_border_opa(o, useBox ? LV_OPA_60 : LV_OPA_0, 0);
  }
  for (auto* o : gDayAccent) lv_obj_set_style_bg_color(o, displayTheme(pageForObject(o)).accent, 0);
  for (auto* o : gDayHi)     lv_obj_set_style_text_color(o, displayTheme(pageForObject(o)).text_hi, 0);
}

static void applyAllThemes() {
  lv_obj_set_style_bg_color(lv_scr_act(), homeTheme().bg, 0);
  applyDayTheme();
  applyHomeTheme();
  applyDisplayLayouts();
  setBrightnessForPage();
}

// ------------------------------------------------------------------ events
static void tileChanged(lv_event_t*) {
  applyHomeTheme();          // refresh dot indicator
  setBrightnessForPage();
}

static void snapToHomeTile() {
  if (!gTileview) return;
  lv_obj_set_tile_id(gTileview, 0, 0, LV_ANIM_OFF);
  lv_obj_scroll_to_x(gTileview, 0, LV_ANIM_OFF);
  lv_obj_update_layout(gTileview);
  applyHomeTheme();
  setBrightnessForPage();
}

static void clockTapped(lv_event_t*) {
  lv_obj_set_tile_id(gTileview, 1, 0, LV_ANIM_ON);   // clock face -> thermostat
}

static void setpointStep(lv_event_t* e) {
  int dir = (int)(intptr_t)lv_event_get_user_data(e);
  gSettings.setpointF += dir;
  if (gSettings.setpointF < 45) gSettings.setpointF = 45;
  if (gSettings.setpointF > 90) gSettings.setpointF = 90;
  if (gThermoSetLabel) lv_label_set_text_fmt(gThermoSetLabel, "%d°", gSettings.setpointF);
  gSettings.save();
}

// ------------------------------------------------------------------ tiles
static void buildClockTile(lv_obj_t* tile) {
  gBuildingHome = true;
  gClock.create(tile, 44);

  lv_obj_t* hit = lv_obj_create(tile);   // tap target over the clock face
  lv_obj_remove_style_all(hit);
  lv_obj_set_size(hit, 300, 130);
  lv_obj_align(hit, LV_ALIGN_TOP_MID, 0, 40);
  lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(hit, LV_OPA_0, 0);
  lv_obj_add_event_cb(hit, clockTapped, LV_EVENT_CLICKED, nullptr);

  // AM/PM to the right of the minutes, vertically centered on the clock.
  gAmPmLabel = mkLabel(tile, "AM", &lv_font_montserrat_20, dayTheme().accent,
                       LV_ALIGN_TOP_LEFT, gClock.rightEdgeX() + 12,
                       44 + SevenSegClock::DH / 2 - 14);
  trackHi(gAmPmLabel);
  gDateLabel = mkLabel(tile, "-- --- --", &lv_font_montserrat_16, dayTheme().text,
                       LV_ALIGN_BOTTOM_MID, 0, -34);

  // Day-mode clock overlays the selected page background; night keeps the locked LED clock.
  buildPageBackground(tile, PAGE_CLOCK, true);

  gDayClockGroup = lv_obj_create(tile);
  lv_obj_remove_style_all(gDayClockGroup);
  lv_obj_set_size(gDayClockGroup, 252, 130);
  lv_obj_add_flag(gDayClockGroup, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(gDayClockGroup, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(gDayClockGroup, clockTapped, LV_EVENT_CLICKED, nullptr);
  trackDayOnly(gDayClockGroup);

  const char* hearts = "\xE2\x99\xA5\xE2\x99\xA5\xE2\x99\xA5";
  gDayHeartsTop = mkLabel(gDayClockGroup, hearts, &huck_font_heart_18,
                          dayTheme().accent2, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_text_letter_space(gDayHeartsTop, 10, 0);
  gDayGreeting = mkLabel(gDayClockGroup, "Good Morning!", &lv_font_montserrat_18,
                         dayTheme().accent, LV_ALIGN_TOP_MID, 0, 20);

  gDayTimePanel = lv_obj_create(gDayClockGroup);
  lv_obj_remove_style_all(gDayTimePanel);
  lv_obj_set_size(gDayTimePanel, 216, 62);
  lv_obj_align(gDayTimePanel, LV_ALIGN_TOP_MID, 0, 50);
  lv_obj_set_style_radius(gDayTimePanel, 22, 0);
  lv_obj_set_style_bg_opa(gDayTimePanel, LV_OPA_90, 0);
  lv_obj_set_style_border_width(gDayTimePanel, 2, 0);
  lv_obj_clear_flag(gDayTimePanel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  gDayTimeLabel = mkLabel(gDayTimePanel, "--:--", &lv_font_montserrat_40,
                          dayTheme().text_hi, LV_ALIGN_LEFT_MID, 16, -1);
  gDayAmPmLabel = mkLabel(gDayTimePanel, "AM", &lv_font_montserrat_18,
                          dayTheme().accent, LV_ALIGN_RIGHT_MID, -14, -10);
  gDayHeartsBottom = mkLabel(gDayClockGroup, hearts, &huck_font_heart_18,
                             dayTheme().accent2, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_obj_set_style_text_letter_space(gDayHeartsBottom, 10, 0);

  gDayDateLabel = mkLabel(tile, "--", &lv_font_montserrat_16, dayTheme().text_hi,
                          LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_width(gDayDateLabel, 230);
  lv_obj_set_style_text_align(gDayDateLabel, LV_TEXT_ALIGN_LEFT, 0);
  trackDayOnly(gDayDateLabel);
  gBuildingHome = false;
}

static void buildThermoTile(lv_obj_t* tile) {
  buildPageBackground(tile, PAGE_CLIMATE, false);

  lv_obj_t* card = mkPanel(tile, 158, 132, LV_ALIGN_TOP_LEFT, 12, 72);
  gThermoCard = card;
  char sp[8]; snprintf(sp, sizeof(sp), "%d\xC2\xB0", gSettings.setpointF);
  gThermoSetLabel = mkLabel(card, sp, &lv_font_montserrat_40, dayTheme().text_hi,
                            LV_ALIGN_TOP_MID, 0, 4);
  trackHi(gThermoSetLabel);
  mkLabel(card, "SET POINT", &lv_font_montserrat_12, dayTheme().text, LV_ALIGN_TOP_MID, 0, 48);

  lv_obj_t* minus = lv_btn_create(card);
  lv_obj_set_size(minus, 44, 42);
  lv_obj_align(minus, LV_ALIGN_BOTTOM_LEFT, 12, -20);
  lv_obj_set_style_bg_color(minus, dayTheme().panel, 0);
  lv_obj_set_style_bg_opa(minus, LV_OPA_70, 0);
  gDayPanel.push_back(minus);
  lv_obj_add_event_cb(minus, setpointStep, LV_EVENT_CLICKED, (void*)(intptr_t)-1);
  trackHi(mkLabel(minus, "-", &lv_font_montserrat_32, dayTheme().text_hi, LV_ALIGN_CENTER, 0, -1));

  lv_obj_t* plus = lv_btn_create(card);
  lv_obj_set_size(plus, 44, 42);
  lv_obj_align(plus, LV_ALIGN_BOTTOM_RIGHT, -12, -20);
  lv_obj_set_style_bg_color(plus, dayTheme().panel, 0);
  lv_obj_set_style_bg_opa(plus, LV_OPA_70, 0);
  gDayPanel.push_back(plus);
  lv_obj_add_event_cb(plus, setpointStep, LV_EVENT_CLICKED, (void*)(intptr_t)1);
  trackHi(mkLabel(plus, "+", &lv_font_montserrat_32, dayTheme().text_hi, LV_ALIGN_CENTER, 0, -1));

  mkLabel(card, gSettings.gidroxMac.isEmpty() ? "AC not paired" : "AC paired",
          &lv_font_montserrat_12, dayTheme().text, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void buildBatteryTile(lv_obj_t* tile) {
  buildPageBackground(tile, PAGE_POWER, false);

  gPowerGroup = lv_obj_create(tile);
  lv_obj_remove_style_all(gPowerGroup);
  lv_obj_set_size(gPowerGroup, 294, 138);
  lv_obj_set_pos(gPowerGroup, 12, 70);
  lv_obj_clear_flag(gPowerGroup, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* arc = lv_arc_create(gPowerGroup);
  gPowerGauge = arc;
  lv_obj_set_size(arc, 138, 138);
  lv_obj_align(arc, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_arc_set_rotation(arc, 135);
  lv_arc_set_bg_angles(arc, 0, 270);
  lv_arc_set_value(arc, 0);
  lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(arc, dayTheme().panel, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, dayTheme().accent, LV_PART_INDICATOR);
  gBSocArc = arc;
  gBSocLbl = mkLabel(arc, "--%", &lv_font_montserrat_32, dayTheme().text_hi, LV_ALIGN_CENTER, 0, -8);
  trackHi(gBSocLbl);
  mkLabel(arc, "Battery", &lv_font_montserrat_12, dayTheme().text, LV_ALIGN_CENTER, 0, 22);

  lv_obj_t* card = mkPanel(gPowerGroup, 146, 132, LV_ALIGN_TOP_LEFT, 148, 0);
  gPowerStatsCard = card;
  mkLabel(card, "Solar", &lv_font_montserrat_12, dayTheme().accent2, LV_ALIGN_TOP_LEFT, 12, 8);
  gBSolarW = mkLabel(card, "-- W", &lv_font_montserrat_20, dayTheme().text_hi, LV_ALIGN_TOP_LEFT, 12, 24);
  trackHi(gBSolarW);
  gBSolarPct = mkLabel(card, "learning max", &lv_font_montserrat_12, dayTheme().text, LV_ALIGN_TOP_LEFT, 12, 52);
  mkLabel(card, "Battery net", &lv_font_montserrat_12, dayTheme().accent2, LV_ALIGN_TOP_LEFT, 12, 76);
  gBNetW = mkLabel(card, "-- W", &lv_font_montserrat_18, dayTheme().text_hi, LV_ALIGN_TOP_LEFT, 12, 92);
  trackHi(gBNetW);
  gBChargeState = mkLabel(card, "--", &lv_font_montserrat_12, dayTheme().text, LV_ALIGN_BOTTOM_LEFT, 12, -6);
}

static void buildStatusTile(lv_obj_t* tile) {
  buildPageBackground(tile, PAGE_STATUS, false);
  lv_obj_t* card = mkPanel(tile, 154, 152, LV_ALIGN_TOP_LEFT, 164, 46);
  gStatusCard = card;
  auto row = [&](const char* k, int y) {
    mkLabel(card, k, &lv_font_montserrat_12, dayTheme().text, LV_ALIGN_TOP_LEFT, 10, y);
    return mkLabel(card, "--", &lv_font_montserrat_12, dayTheme().text_hi, LV_ALIGN_TOP_LEFT, 58, y);
  };
  gStWifi = row("Wi-Fi", 8);
  gStSsid = row("SSID", 28);
  gStIp   = row("IP", 48);
  gStAp   = row("AP", 68);
  gStBatt = row("Batt", 88);
  gStSolar= row("Solar", 108);
  gStTime = row("Time", 128);
}

// Push live telemetry + net status into the on-device tiles.
static void updateDataTiles() {
  teleLock();
  Telemetry t = gTele;
  teleUnlock();
  char b[40];

  if (gBSocLbl) {
    if (t.battValid && t.battSoc >= 0) { snprintf(b, sizeof(b), "%d%%", t.battSoc); lv_label_set_text(gBSocLbl, b); lv_arc_set_value(gBSocArc, t.battSoc); }
  }
  if (gBSolarW) {
    if (t.solValid && !isnan(t.solPvW)) snprintf(b, sizeof(b), "%.0f W", t.solPvW);
    else snprintf(b, sizeof(b), "-- W");
    lv_label_set_text(gBSolarW, b);
  }
  if (gBSolarPct) {
    if (t.solValid && !isnan(t.solPvW) && !isnan(t.solPvMaxW) && t.solPvMaxW > 0.5f) {
      int pct = constrain((int)lroundf(t.solPvW * 100.0f / t.solPvMaxW), 0, 999);
      snprintf(b, sizeof(b), "%d%% of %.0fW max", pct, t.solPvMaxW);
    } else {
      snprintf(b, sizeof(b), t.solValid ? "learning max" : "waiting");
    }
    lv_label_set_text(gBSolarPct, b);
  }
  if (gBNetW) {
    if (t.battValid && !isnan(t.battVolts) && !isnan(t.battAmps)) {
      float watts = t.battVolts * t.battAmps;
      snprintf(b, sizeof(b), "%c%.0f W", watts >= 0 ? '+' : '-', fabsf(watts));
    } else {
      snprintf(b, sizeof(b), "-- W");
    }
    lv_label_set_text(gBNetW, b);
  }
  if (gBChargeState) {
    static const char* sn[] = {"Off","","","Bulk","Absorb","Float","","Equalize"};
    const char* s = (t.solState < 8 && sn[t.solState][0]) ? sn[t.solState] : "On";
    if (t.solValid) snprintf(b, sizeof(b), "%s", s);
    if (!t.solValid) snprintf(b, sizeof(b), "waiting");
    if (t.battValid && !isnan(t.battAmps)) {
      snprintf(b, sizeof(b), "%s / %s",
               t.battAmps > 0.05f ? "charging" : (t.battAmps < -0.05f ? "using" : "steady"),
               t.solValid ? s : "solar wait");
    }
    lv_label_set_text(gBChargeState, b);
  }

  if (gStWifi) lv_label_set_text(gStWifi, gNet.staConnected ? "connected" : "offline");
  if (gStSsid) lv_label_set_text(gStSsid, gNet.ssid.isEmpty() ? "--" : gNet.ssid.c_str());
  if (gStIp)   lv_label_set_text(gStIp, gNet.ip.isEmpty() ? "--" : gNet.ip.c_str());
  if (gStAp)   lv_label_set_text(gStAp, gNet.apActive ? gNet.apSsid.c_str() : "off");
  if (gStBatt) lv_label_set_text(gStBatt, t.battValid ? "live" : "waiting");
  if (gStSolar){ snprintf(b, sizeof(b), t.solValid ? "live %ddBm" : "waiting", t.solRssi); lv_label_set_text(gStSolar, b); }
  if (gStBatt && !t.battValid) lv_label_set_text(gStBatt, "waiting");
  if (gStSolar && !t.solValid) lv_label_set_text(gStSolar, "waiting");
  if (gStTime) lv_label_set_text(gStTime, gNet.timeSource.c_str());
}

static void buildUi() {
  gTileview = lv_tileview_create(lv_scr_act());
  lv_obj_set_style_bg_opa(gTileview, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(gTileview, tileChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  gTiles[0] = lv_tileview_add_tile(gTileview, 0, 0, LV_DIR_RIGHT);
  gTiles[1] = lv_tileview_add_tile(gTileview, 1, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
  gTiles[2] = lv_tileview_add_tile(gTileview, 2, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
  gTiles[3] = lv_tileview_add_tile(gTileview, 3, 0, LV_DIR_LEFT);

  buildClockTile(gTiles[0]);
  buildThermoTile(gTiles[1]);
  buildBatteryTile(gTiles[2]);
  buildStatusTile(gTiles[3]);

  for (int i = 0; i < 4; i++) {   // page-indicator dots on the clock tile
    lv_obj_t* d = lv_obj_create(gTiles[0]);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 7, 7);
    lv_obj_align(d, LV_ALIGN_BOTTOM_MID, (i - 1) * 14 - 7, -2);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    gDots[i] = d;
    gNightHide.push_back(d);   // dots hide on the clock at night
  }
}

// ------------------------------------------------------------------ clock/time
static void seedClockFromBuild() {
  struct tm tmv = {};
  char mon[4] = {0};
  int day = 1, yr = 2026, hh = 12, mm = 0, ss = 0;
  sscanf(__DATE__, "%3s %d %d", mon, &day, &yr);
  sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
  static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* p = strstr(months, mon);
  int mo = p ? (int)((p - months) / 3) : 0;
  tmv.tm_year = yr - 1900; tmv.tm_mon = mo; tmv.tm_mday = day;
  tmv.tm_hour = hh; tmv.tm_min = mm; tmv.tm_sec = ss;
  time_t t = mktime(&tmv);
  struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
}

static void updateClock() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);

  bool night = computeNight(lt.tm_hour);
  if (night != gNightNow) { gNightNow = night; applyHomeTheme(); setBrightnessForPage(); }

  int h12 = lt.tm_hour % 12; if (h12 == 0) h12 = 12;
  gClock.setTime(h12, lt.tm_min, h12 < 10);
  gClock.setColon((lt.tm_sec & 1) == 0);

  if (gAmPmLabel) lv_label_set_text(gAmPmLabel, lt.tm_hour < 12 ? "AM" : "PM");
  if (gDayTimeLabel) {
    char tb[8];
    snprintf(tb, sizeof(tb), "%d:%02d", h12, lt.tm_min);
    lv_label_set_text(gDayTimeLabel, tb);
  }
  if (gDayAmPmLabel) lv_label_set_text(gDayAmPmLabel, lt.tm_hour < 12 ? "AM" : "PM");
  if (gDayGreeting) {
    const char* g = lt.tm_hour < 12 ? "Good Morning!" : (lt.tm_hour < 17 ? "Good Afternoon!" : "Good Evening!");
    lv_label_set_text(gDayGreeting, g);
  }
  if (gDayDateLabel) {
    static const char* wdFull[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* moFull[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
    lv_label_set_text_fmt(gDayDateLabel, "%s, %s %d, %d",
                          wdFull[lt.tm_wday], moFull[lt.tm_mon], lt.tm_mday, lt.tm_year + 1900);
  }
  if (gDateLabel) {
    static const char* wd[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* mo[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    lv_label_set_text_fmt(gDateLabel, "%s • %s %d", wd[lt.tm_wday], mo[lt.tm_mon], lt.tm_mday);
  }
}

// ------------------------------------------------------------------ setup/loop
void setup() {
  Serial.begin(115200);

  gSettings.load();
  gDayThemeIdx = gSettings.dayThemeIdx;
  if (gDayThemeIdx < 0 || (size_t)gDayThemeIdx >= HUCK_THEME_COUNT) gDayThemeIdx = 0;

  lcd.init();
  lcd.setRotation(1);          // landscape 320x240
  lcd.setBrightness(0);        // fade up after UI is drawn

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, lvbuf1, lvbuf2, SCR_W * BUF_LINES);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCR_W;
  disp_drv.ver_res = SCR_H;
  disp_drv.flush_cb = disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read;
  lv_indev_drv_register(&indev_drv);

  seedClockFromBuild();
  buildUi();
  loadPageBackgrounds();
  updateClock();
  applyAllThemes();

  lv_timer_handler();
  setBrightnessForPage();

  // Bring up connectivity, web app, and BLE after the UI is drawn.
  net::begin();
  web::begin();
  ble::begin();

  Serial.println("Huckleberry Phase B up.");
}

// Apply web-driven settings changes to the live UI.
static void applyUiChangesIfRequested() {
  if (!gUiApplyRequested) return;
  gUiApplyRequested = false;
  gDayThemeIdx = gSettings.dayThemeIdx;
  if (gDayThemeIdx < 0 || (size_t)gDayThemeIdx >= HUCK_THEME_COUNT) gDayThemeIdx = 0;
  if (gThermoSetLabel) lv_label_set_text_fmt(gThermoSetLabel, "%d°", gSettings.setpointF);
  time_t now = time(nullptr); struct tm lt; localtime_r(&now, &lt);
  gNightNow = computeNight(lt.tm_hour);   // reflect new night-window/autoNight now
  bool reloadBg = gBgReloadRequested;
  gBgReloadRequested = false;
  loadPageBackgrounds(reloadBg);
  applyAllThemes();
}

void loop() {
  static uint32_t lastTick = 0, lastSec = 0, lastData = 0;
  uint32_t now = millis();
  lv_tick_inc(now - lastTick);
  lastTick = now;

  net::loop();
  web::loop();
  applyUiChangesIfRequested();

  if (now - lastSec >= 250) {   // clock + colon refresh + display power
    lastSec = now;
    updateClock();
    updateDisplayPower();
  }
  if (now - lastData >= 1000) { // live telemetry -> tiles
    lastData = now;
    updateDataTiles();
  }

  // Inactivity -> snap back to the home clock page (configurable).
  uint32_t timeoutMs = (uint32_t)(gSettings.homeTimeoutSec > 0 ? gSettings.homeTimeoutSec : 60) * 1000;
  if (gTileview && lv_disp_get_inactive_time(nullptr) > timeoutMs && lv_obj_get_scroll_x(gTileview) != 0) {
    snapToHomeTile();
  }

  lv_timer_handler();
  delay(5);
}
