#include "Settings.h"
#include <Preferences.h>

Settings gSettings;
static Preferences prefs;

static const char* NS = "huck";

void Settings::load() {
  prefs.begin(NS, true);
  wifiSsid = prefs.getString("wSsid", wifiSsid);
  wifiPass = prefs.getString("wPass", wifiPass);
  hostname = prefs.getString("host", hostname);
  apPass   = prefs.getString("apPass", apPass);
  tz       = prefs.getString("tz", tz);
  use24h   = prefs.getBool("use24h", use24h);
  dayThemeIdx = prefs.getInt("theme", dayThemeIdx);
  animations  = prefs.getBool("anim", animations);
  dayBrightness = prefs.getInt("bright", dayBrightness);
  autoNight = prefs.getBool("autoNight", autoNight);
  victronMac = prefs.getString("vMac", victronMac);
  victronKey = prefs.getString("vKey", victronKey);
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
  prefs.putString("wSsid", wifiSsid);
  prefs.putString("wPass", wifiPass);
  prefs.putString("host", hostname);
  prefs.putString("apPass", apPass);
  prefs.putString("tz", tz);
  prefs.putBool("use24h", use24h);
  prefs.putInt("theme", dayThemeIdx);
  prefs.putBool("anim", animations);
  prefs.putInt("bright", dayBrightness);
  prefs.putBool("autoNight", autoNight);
  prefs.putString("vMac", victronMac);
  prefs.putString("vKey", victronKey);
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
