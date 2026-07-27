#include "Settings.h"
#include <Preferences.h>
#include <ArduinoJson.h>

Settings gSettings;
static Preferences prefs;

static const char* NS = "huck";
static constexpr int LAYOUT_REV = 3;
static constexpr int BG_REV = 1;

static int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static bool sameSlot(const LayoutSlot& slot, int16_t x, int16_t y, uint8_t scale) {
  return slot.x == x && slot.y == y && slot.scale == scale;
}

static LayoutSlot clampSlot(LayoutWidget w, LayoutSlot slot) {
  static const uint16_t dims[LAYOUT_WIDGET_COUNT][2] = {
    {252, 130}, {230, 24}, {158, 132}, {294, 138}, {146, 132}, {154, 152}
  };
  static const uint8_t minScale[LAYOUT_WIDGET_COUNT] = {70, 70, 75, 75, 75, 75};
  // Actual per-widget max at position (0,0): floor(min(320/w, 240/h) * 100).
  // Above this the LVGL transform_zoom pushes rendered pixels past the tile
  // boundary and the widget disappears.
  static const uint8_t maxScale[LAYOUT_WIDGET_COUNT] = {126, 139, 181, 108, 181, 157};
  int idx = (int)w;
  slot.scale = clampInt(slot.scale, minScale[idx], maxScale[idx]);
  int scaledW = ((int)dims[idx][0] * slot.scale + 99) / 100;
  int scaledH = ((int)dims[idx][1] * slot.scale + 99) / 100;
  int minX = min(0, 320 - scaledW);
  int maxX = max(0, 320 - scaledW);
  int minY = min(0, 240 - scaledH);
  int maxY = max(0, 240 - scaledH);
  slot.x = clampInt(slot.x, minX, maxX);
  slot.y = clampInt(slot.y, minY, maxY);
  return slot;
}

bool Settings::addNetwork(const String& ssid, const String& pass) {
  if (ssid.isEmpty()) return false;
  for (auto& n : networks) {         // update password if SSID already saved
    if (n.ssid == ssid) { n.pass = pass; return true; }
  }
  if (networks.size() >= MAX_NETWORKS) return false;
  networks.push_back({ssid, pass});
  return true;
}

void Settings::removeNetwork(const String& ssid) {
  for (size_t i = 0; i < networks.size(); i++)
    if (networks[i].ssid == ssid) { networks.erase(networks.begin() + i); return; }
}

void Settings::load() {
  prefs.begin(NS, true);
  // Networks: stored as a JSON array [{s,p},...]; migrate legacy single SSID.
  networks.clear();
  String nets = prefs.getString("nets", "");
  if (nets.length()) {
    JsonDocument d;
    if (!deserializeJson(d, nets)) {
      for (JsonObject o : d.as<JsonArray>()) {
        if (networks.size() >= MAX_NETWORKS) break;
        networks.push_back({ String(o["s"] | ""), String(o["p"] | "") });
      }
    }
  }
  if (networks.empty()) {  // migrate legacy single-network keys
    String ls = prefs.getString("wSsid", "");
    if (ls.length()) networks.push_back({ ls, prefs.getString("wPass", "") });
  }
  hostname = prefs.getString("host", hostname);
  apPass   = prefs.getString("apPass", apPass);
  tz       = prefs.getString("tz", tz);
  use24h   = prefs.getBool("use24h", use24h);
  dayThemeIdx = prefs.getInt("theme", dayThemeIdx);
  animations  = prefs.getBool("anim", animations);
  dayBrightness = prefs.getInt("bright", dayBrightness);
  nightBrightness = prefs.getInt("nbright", nightBrightness);
  autoNight = prefs.getBool("autoNight", autoNight);
  nightStartHour = prefs.getInt("nStart", nightStartHour);
  nightEndHour = prefs.getInt("nEnd", nightEndHour);
  homeTimeoutSec = prefs.getInt("hto", homeTimeoutSec);
  dispOffEnable = prefs.getBool("doEn", dispOffEnable);
  dispOffStartHour = prefs.getInt("doStart", dispOffStartHour);
  dispOffEndHour = prefs.getInt("doEnd", dispOffEndHour);
  pageBg[PAGE_CLOCK] = prefs.getString("dayBg", pageBg[PAGE_CLOCK]); // migrate old clock-only key
  for (size_t i = 0; i < pageBg.size(); i++) {
    char kb[8];
    snprintf(kb, sizeof(kb), "bg%u", (unsigned)i);
    pageBg[i] = prefs.getString(kb, pageBg[i]);
    if (pageBg[i] == "bg_jewel_01.jpg") pageBg[i] = "bg_charlie_01.jpg";
    char kt[8], kbox[8], kc[8];
    snprintf(kt, sizeof(kt), "pt%u", (unsigned)i);
    snprintf(kbox, sizeof(kbox), "pb%u", (unsigned)i);
    snprintf(kc, sizeof(kc), "pc%u", (unsigned)i);
    pageTheme[i] = (int8_t)clampInt(prefs.getInt(kt, pageTheme[i]), -1, 15);
    pageBox[i] = prefs.getBool(kbox, pageBox[i]);
    pageContrast[i] = (uint8_t)clampInt(prefs.getInt(kc, pageContrast[i]), 0, 2);
  }
  int bgRev = prefs.getInt("bgRev", 0);
  if (bgRev < 1 && pageBg[PAGE_CLOCK] == "bg_flower_01.jpg") {
    pageBg[PAGE_CLOCK] = "bg_indie_02.jpg";
  }
  for (size_t i = 0; i < layout.size(); i++) {
    char kx[8], ky[8], ks[8];
    snprintf(kx, sizeof(kx), "lX%u", (unsigned)i);
    snprintf(ky, sizeof(ky), "lY%u", (unsigned)i);
    snprintf(ks, sizeof(ks), "lS%u", (unsigned)i);
    layout[i].x = clampInt(prefs.getInt(kx, layout[i].x), -160, 320);
    layout[i].y = clampInt(prefs.getInt(ky, layout[i].y), -120, 240);
    layout[i].scale = clampInt(prefs.getInt(ks, layout[i].scale), 50, 180);
  }
  int layoutRev = prefs.getInt("layRev", 0);
  if (layoutRev < 1 && sameSlot(layout[LAYOUT_POWER_STATS], 194, 46, 100)) {
    layout[LAYOUT_POWER_STATS] = {168, 50, 100};
  }
  if (layoutRev < 2) {
    if (sameSlot(layout[LAYOUT_DAY_CLOCK], 16, 38, 100)) layout[LAYOUT_DAY_CLOCK] = {8, 30, 92};
    if (sameSlot(layout[LAYOUT_DAY_DATE], 2, 190, 100)) layout[LAYOUT_DAY_DATE] = {12, 12, 100};
    if (sameSlot(layout[LAYOUT_THERMO_CARD], 20, 42, 100)) layout[LAYOUT_THERMO_CARD] = {12, 72, 100};
    if (sameSlot(layout[LAYOUT_POWER_GAUGE], 18, 48, 100)) layout[LAYOUT_POWER_GAUGE] = {12, 70, 100};
    if (sameSlot(layout[LAYOUT_POWER_STATS], 168, 50, 100)) layout[LAYOUT_POWER_STATS] = {160, 70, 100};
    if (sameSlot(layout[LAYOUT_STATUS_CARD], 116, 36, 100)) layout[LAYOUT_STATUS_CARD] = {164, 46, 100};
  }
  if (layoutRev < 3 && sameSlot(layout[LAYOUT_DAY_CLOCK], 8, 30, 92)) {
    layout[LAYOUT_DAY_CLOCK] = {14, 30, 100};
  }
  for (size_t i = 0; i < layout.size(); i++) {
    layout[i] = clampSlot((LayoutWidget)i, layout[i]);
  }
  webAccent = prefs.getString("wAcc", webAccent);
  victronMac = prefs.getString("vMac", victronMac);
  victronKey = prefs.getString("vKey", victronKey);
  victronPin = prefs.getString("vPin", victronPin);
  batteryMac = prefs.getString("bMac", batteryMac);
  gidroxMac  = prefs.getString("gMac", gidroxMac);
  bleEnabled = prefs.getBool("ble", bleEnabled);
  setpointF = prefs.getInt("sp", setpointF);
  mode = prefs.getInt("mode", mode);
  camping = prefs.getBool("camp", camping);
  storeMinF = prefs.getInt("stMin", storeMinF);
  storeMaxF = prefs.getInt("stMax", storeMaxF);
  prefs.end();
}

void Settings::save() {
  prefs.begin(NS, false);
  {
    JsonDocument d;
    JsonArray arr = d.to<JsonArray>();
    for (auto& n : networks) { JsonObject o = arr.add<JsonObject>(); o["s"] = n.ssid; o["p"] = n.pass; }
    String out; serializeJson(d, out);
    prefs.putString("nets", out);
  }
  prefs.putString("host", hostname);
  prefs.putString("apPass", apPass);
  prefs.putString("tz", tz);
  prefs.putBool("use24h", use24h);
  prefs.putInt("theme", dayThemeIdx);
  prefs.putBool("anim", animations);
  prefs.putInt("bright", dayBrightness);
  prefs.putInt("nbright", nightBrightness);
  prefs.putBool("autoNight", autoNight);
  prefs.putInt("nStart", nightStartHour);
  prefs.putInt("nEnd", nightEndHour);
  prefs.putInt("hto", homeTimeoutSec);
  prefs.putBool("doEn", dispOffEnable);
  prefs.putInt("doStart", dispOffStartHour);
  prefs.putInt("doEnd", dispOffEndHour);
  prefs.putString("dayBg", pageBg[PAGE_CLOCK]); // retained for compatibility with existing NVS
  for (size_t i = 0; i < pageBg.size(); i++) {
    char kb[8];
    snprintf(kb, sizeof(kb), "bg%u", (unsigned)i);
    prefs.putString(kb, pageBg[i]);
    char kt[8], kbox[8], kc[8];
    snprintf(kt, sizeof(kt), "pt%u", (unsigned)i);
    snprintf(kbox, sizeof(kbox), "pb%u", (unsigned)i);
    snprintf(kc, sizeof(kc), "pc%u", (unsigned)i);
    pageTheme[i] = (int8_t)clampInt(pageTheme[i], -1, 15);
    prefs.putInt(kt, pageTheme[i]);
    prefs.putBool(kbox, pageBox[i]);
    pageContrast[i] = (uint8_t)clampInt(pageContrast[i], 0, 2);
    prefs.putInt(kc, pageContrast[i]);
  }
  for (size_t i = 0; i < layout.size(); i++) {
    char kx[8], ky[8], ks[8];
    snprintf(kx, sizeof(kx), "lX%u", (unsigned)i);
    snprintf(ky, sizeof(ky), "lY%u", (unsigned)i);
    snprintf(ks, sizeof(ks), "lS%u", (unsigned)i);
    layout[i] = clampSlot((LayoutWidget)i, layout[i]);
    prefs.putInt(kx, layout[i].x);
    prefs.putInt(ky, layout[i].y);
    prefs.putInt(ks, layout[i].scale);
  }
  prefs.putInt("layRev", LAYOUT_REV);
  prefs.putInt("bgRev", BG_REV);
  prefs.putString("wAcc", webAccent);
  prefs.putString("vMac", victronMac);
  prefs.putString("vKey", victronKey);
  prefs.putString("vPin", victronPin);
  prefs.putString("bMac", batteryMac);
  prefs.putString("gMac", gidroxMac);
  prefs.putBool("ble", bleEnabled);
  prefs.putInt("sp", setpointF);
  prefs.putInt("mode", mode);
  prefs.putBool("camp", camping);
  prefs.putInt("stMin", storeMinF);
  prefs.putInt("stMax", storeMaxF);
  prefs.end();
}

size_t parseHexBytes(const String& hex, uint8_t* out, size_t maxLen) {
  size_t n = 0;
  int hi = -1;
  for (size_t i = 0; i < hex.length() && n < maxLen; i++) {
    char c = hex[i];
    int v;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else continue;  // skip ':' , spaces
    if (hi < 0) hi = v;
    else { out[n++] = (hi << 4) | v; hi = -1; }
  }
  return n;
}
