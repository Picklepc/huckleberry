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
// recreated flower/weather art. Those screens exist here as themed placeholders.

#include <Arduino.h>
#include <lvgl.h>
#include <time.h>
#include <sys/time.h>
#include <vector>

#include "LGFX_Huckleberry.hpp"
#include "HuckTheme.h"
#include "SevenSegClock.h"
#include "AppState.h"
#include "Settings.h"
#include "Net.h"
#include "BleManager.h"
#include "WebApp.h"

// ---------------------------------------------------------------- display glue
static huck::LGFX lcd;

static constexpr int SCR_W = 320;
static constexpr int SCR_H = 240;
static constexpr int BUF_LINES = 40;
static constexpr uint32_t HOME_TIMEOUT_MS = 60000;  // 1-min return-to-clock

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
// Home-page decorations that are DAY-mode only (hidden in night mode so the
// clock stays a minimal black face + white digits + date).
static std::vector<lv_obj_t*> gNightHide;

static int  gDayThemeIdx = 0;
static bool gNightNow = false;
static lv_obj_t* gDateLabel = nullptr;
static lv_obj_t* gAmPmLabel = nullptr;
static lv_obj_t* gThermoSetLabel = nullptr;

// Live-data widgets updated from telemetry/net.
static lv_obj_t* gBSocArc=nullptr; static lv_obj_t* gBSocLbl=nullptr;
static lv_obj_t* gBVolts=nullptr;  static lv_obj_t* gBAmps=nullptr;
static lv_obj_t* gBSolarW=nullptr;  static lv_obj_t* gBSolState=nullptr;
static lv_obj_t* gStWifi=nullptr; static lv_obj_t* gStSsid=nullptr; static lv_obj_t* gStIp=nullptr;
static lv_obj_t* gStAp=nullptr;   static lv_obj_t* gStBatt=nullptr; static lv_obj_t* gStSolar=nullptr;
static lv_obj_t* gStTime=nullptr;

static const HuckTheme& dayTheme()  { return HUCK_THEMES[gDayThemeIdx]; }
static const HuckTheme& homeTheme() { return gNightNow ? HUCK_NIGHT_THEME : dayTheme(); }

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
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  gDayPanel.push_back(p);   // panels only exist on the day-themed pages
  return p;
}

static void trackAccent(lv_obj_t* o) { (gBuildingHome ? gHomeAccent : gDayAccent).push_back(o); }
static void trackHi(lv_obj_t* o)     { (gBuildingHome ? gHomeHi : gDayHi).push_back(o); }

// A 5-petal native-flower motif (poppy/lupine vibe), built from round objs.
// `petalR` is the size of each petal; the bloom spans ~3*petalR. Day-mode only.
static void mkFlower(lv_obj_t* parent, int cx, int cy, int petalR,
                     lv_color_t petal, lv_color_t center) {
  int reach = petalR;  // distance of petals from the center
  for (int i = 0; i < 5; i++) {
    float a = (float)i / 5.0f * 6.2831853f - 1.5708f;
    int px = cx + (int)(cosf(a) * reach) - petalR / 2;
    int py = cy + (int)(sinf(a) * reach) - petalR / 2;
    lv_obj_t* pt = lv_obj_create(parent);
    lv_obj_remove_style_all(pt);
    lv_obj_set_size(pt, petalR, petalR);
    lv_obj_set_pos(pt, px, py);
    lv_obj_set_style_radius(pt, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(pt, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(pt, petal, 0);
    lv_obj_clear_flag(pt, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    trackAccent(pt);
    gNightHide.push_back(pt);
  }
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, petalR * 3 / 4, petalR * 3 / 4);
  lv_obj_set_pos(c, cx - petalR * 3 / 8, cy - petalR * 3 / 8);
  lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(c, center, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  gNightHide.push_back(c);
}

// ------------------------------------------------------------------ theming
static void setBrightnessForPage() {
  bool homeNight = gNightNow && activeTileIndex() == 0;
  int dayB = gSettings.dayBrightness > 0 ? gSettings.dayBrightness : dayTheme().brightness;
  lcd.setBrightness(homeNight ? HUCK_NIGHT_THEME.brightness : dayB);
}

// Night mode keeps the home page minimal: clock + date only. Everything
// decorative (flowers, hint text, nav dots) hides until day mode returns.
static void updateHomeDecorVisibility() {
  for (auto* o : gNightHide) {
    if (gNightNow) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
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
  updateHomeDecorVisibility();
}

static void applyDayTheme() {
  const HuckTheme& t = dayTheme();
  for (int i = 1; i < 4; i++) {
    if (!gTiles[i]) continue;
    lv_obj_set_style_bg_color(gTiles[i], t.bg, 0);
    lv_obj_set_style_bg_opa(gTiles[i], LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(gTiles[i], t.text, 0);
  }
  for (auto* o : gDayPanel)  lv_obj_set_style_bg_color(o, t.panel, 0);
  for (auto* o : gDayAccent) lv_obj_set_style_bg_color(o, t.accent, 0);
  for (auto* o : gDayHi)     lv_obj_set_style_text_color(o, t.text_hi, 0);
}

static void applyAllThemes() {
  lv_obj_set_style_bg_color(lv_scr_act(), homeTheme().bg, 0);
  applyDayTheme();
  applyHomeTheme();
  setBrightnessForPage();
}

// ------------------------------------------------------------------ events
static void tileChanged(lv_event_t*) {
  applyHomeTheme();          // refresh dot indicator
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

  // Big native-flower blooms in the lower corners — day mode only.
  mkFlower(tile, 44, 190, 20, dayTheme().accent, dayTheme().accent2);
  mkFlower(tile, 276, 190, 20, dayTheme().accent2, dayTheme().accent);

  lv_obj_t* hint = mkLabel(tile, "tap for climate  •  swipe for power",
                           &lv_font_montserrat_12, dayTheme().text,
                           LV_ALIGN_BOTTOM_MID, 0, -12);
  gNightHide.push_back(hint);
  gBuildingHome = false;
}

static void buildThermoTile(lv_obj_t* tile) {
  mkLabel(tile, "CLIMATE", &lv_font_montserrat_16, dayTheme().accent, LV_ALIGN_TOP_MID, 0, 12);

  lv_obj_t* card = mkPanel(tile, 150, 150, LV_ALIGN_CENTER, 0, 6);
  char sp[8]; snprintf(sp, sizeof(sp), "%d°", gSettings.setpointF);
  gThermoSetLabel = mkLabel(card, sp, &lv_font_montserrat_48, dayTheme().text_hi,
                            LV_ALIGN_CENTER, 0, -8);
  trackHi(gThermoSetLabel);
  mkLabel(card, "SET POINT", &lv_font_montserrat_12, dayTheme().text, LV_ALIGN_CENTER, 0, 40);

  lv_obj_t* minus = lv_btn_create(tile);
  lv_obj_set_size(minus, 60, 90);
  lv_obj_align(minus, LV_ALIGN_LEFT_MID, 10, 6);
  lv_obj_set_style_bg_color(minus, dayTheme().panel, 0);
  gDayPanel.push_back(minus);
  lv_obj_add_event_cb(minus, setpointStep, LV_EVENT_CLICKED, (void*)(intptr_t)-1);
  trackHi(mkLabel(minus, "-", &lv_font_montserrat_40, dayTheme().text_hi, LV_ALIGN_CENTER, 0, 0));

  lv_obj_t* plus = lv_btn_create(tile);
  lv_obj_set_size(plus, 60, 90);
  lv_obj_align(plus, LV_ALIGN_RIGHT_MID, -10, 6);
  lv_obj_set_style_bg_color(plus, dayTheme().panel, 0);
  gDayPanel.push_back(plus);
  lv_obj_add_event_cb(plus, setpointStep, LV_EVENT_CLICKED, (void*)(intptr_t)1);
  trackHi(mkLabel(plus, "+", &lv_font_montserrat_40, dayTheme().text_hi, LV_ALIGN_CENTER, 0, 0));

  mkLabel(tile, "Gidrox 10k BTU  •  mode: AUTO  •  now 72°",
          &lv_font_montserrat_12, dayTheme().text, LV_ALIGN_BOTTOM_MID, 0, -14);
}

static void buildBatteryTile(lv_obj_t* tile) {
  mkLabel(tile, "POWER", &lv_font_montserrat_16, dayTheme().accent, LV_ALIGN_TOP_MID, 0, 12);

  lv_obj_t* arc = lv_arc_create(tile);
  lv_obj_set_size(arc, 150, 150);
  lv_obj_align(arc, LV_ALIGN_LEFT_MID, 14, 6);
  lv_arc_set_rotation(arc, 135);
  lv_arc_set_bg_angles(arc, 0, 270);
  lv_arc_set_value(arc, 0);
  lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(arc, dayTheme().panel, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, dayTheme().accent, LV_PART_INDICATOR);
  gBSocArc = arc;
  gBSocLbl = mkLabel(arc, "--%", &lv_font_montserrat_32, dayTheme().text_hi, LV_ALIGN_CENTER, 0, -6);
  trackHi(gBSocLbl);
  mkLabel(arc, "Eco-Worthy", &lv_font_montserrat_12, dayTheme().text, LV_ALIGN_CENTER, 0, 22);

  lv_obj_t* card = mkPanel(tile, 118, 150, LV_ALIGN_RIGHT_MID, -12, 6);
  gBVolts = mkLabel(card, "-- V", &lv_font_montserrat_20, dayTheme().text_hi, LV_ALIGN_TOP_MID, 0, 6);
  trackHi(gBVolts);
  gBAmps = mkLabel(card, "-- A", &lv_font_montserrat_14, dayTheme().text, LV_ALIGN_TOP_MID, 0, 34);
  mkLabel(card, "Solar", &lv_font_montserrat_14, dayTheme().accent2, LV_ALIGN_TOP_MID, 0, 64);
  gBSolarW = mkLabel(card, "-- W", &lv_font_montserrat_18, dayTheme().text_hi, LV_ALIGN_TOP_MID, 0, 86);
  trackHi(gBSolarW);
  gBSolState = mkLabel(card, "Victron MPPT", &lv_font_montserrat_12, dayTheme().text, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void buildStatusTile(lv_obj_t* tile) {
  mkLabel(tile, "STATUS", &lv_font_montserrat_16, dayTheme().accent, LV_ALIGN_TOP_MID, 0, 12);
  lv_obj_t* card = mkPanel(tile, 296, 182, LV_ALIGN_CENTER, 0, 8);
  auto row = [&](const char* k, int y) {
    mkLabel(card, k, &lv_font_montserrat_14, dayTheme().text, LV_ALIGN_TOP_LEFT, 14, y);
    return mkLabel(card, "--", &lv_font_montserrat_14, dayTheme().text_hi, LV_ALIGN_TOP_LEFT, 96, y);
  };
  gStWifi = row("Wi-Fi", 10);
  gStSsid = row("SSID", 32);
  gStIp   = row("IP", 54);
  gStAp   = row("AP", 76);
  gStBatt = row("Battery", 98);
  gStSolar= row("Solar", 120);
  gStTime = row("Time", 142);
  mkLabel(card, "Firmware", &lv_font_montserrat_14, dayTheme().text, LV_ALIGN_TOP_LEFT, 14, 164);
  mkLabel(card, "v0.2.0-b1", &lv_font_montserrat_14, dayTheme().text_hi, LV_ALIGN_TOP_LEFT, 96, 164);
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
  if (gBVolts) { if (t.battValid) { snprintf(b, sizeof(b), "%.2f V", t.battVolts); lv_label_set_text(gBVolts, b); } }
  if (gBAmps)  { if (t.battValid) { snprintf(b, sizeof(b), "%.1f A", t.battAmps); lv_label_set_text(gBAmps, b); } }
  if (gBSolarW){ if (t.solValid)  { snprintf(b, sizeof(b), "%.0f W", t.solPvW); lv_label_set_text(gBSolarW, b); } }
  if (gBSolState) {
    static const char* sn[] = {"Off","","","Bulk","Absorb","Float","","Equalize"};
    const char* s = (t.solState < 8 && sn[t.solState][0]) ? sn[t.solState] : "On";
    snprintf(b, sizeof(b), "Victron: %s", t.solValid ? s : "…");
    lv_label_set_text(gBSolState, b);
  }

  if (gStWifi) lv_label_set_text(gStWifi, gNet.staConnected ? "connected" : "offline");
  if (gStSsid) lv_label_set_text(gStSsid, gNet.ssid.isEmpty() ? "--" : gNet.ssid.c_str());
  if (gStIp)   lv_label_set_text(gStIp, gNet.ip.isEmpty() ? "--" : gNet.ip.c_str());
  if (gStAp)   lv_label_set_text(gStAp, gNet.apActive ? gNet.apSsid.c_str() : "off");
  if (gStBatt) lv_label_set_text(gStBatt, t.battValid ? "live" : "waiting…");
  if (gStSolar){ snprintf(b, sizeof(b), t.solValid ? "live (%ddBm)" : "waiting…", t.solRssi); lv_label_set_text(gStSolar, b); }
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

  bool night = huck_is_night(lt.tm_hour);
  if (night != gNightNow) { gNightNow = night; applyHomeTheme(); setBrightnessForPage(); }

  int h12 = lt.tm_hour % 12; if (h12 == 0) h12 = 12;
  gClock.setTime(h12, lt.tm_min, h12 < 10);
  gClock.setColon((lt.tm_sec & 1) == 0);

  if (gAmPmLabel) lv_label_set_text(gAmPmLabel, lt.tm_hour < 12 ? "AM" : "PM");
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

  if (now - lastSec >= 250) {   // clock + colon refresh
    lastSec = now;
    updateClock();
  }
  if (now - lastData >= 1000) { // live telemetry -> tiles
    lastData = now;
    updateDataTiles();
  }

  // 1-minute inactivity -> snap back to the home clock page.
  if (activeTileIndex() != 0 && lv_disp_get_inactive_time(nullptr) > HOME_TIMEOUT_MS) {
    lv_obj_set_tile_id(gTileview, 0, 0, LV_ANIM_ON);
  }

  lv_timer_handler();
  delay(5);
}
