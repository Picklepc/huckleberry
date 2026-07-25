#pragma once
// Persistent settings (NVS via Preferences). Mirrors the spirit of R48's
// settings: full suite with sane defaults. Extended incrementally.
#include <Arduino.h>
#include <array>
#include <vector>

// Device-specific secrets (BLE keys/MACs) live in a gitignored secrets file so
// they never enter git history. See include/secrets.example.h.
#if __has_include("secrets.local.h")
#include "secrets.local.h"
#endif
#ifndef HUCK_VICTRON_MAC
#define HUCK_VICTRON_MAC ""
#endif
#ifndef HUCK_VICTRON_KEY
#define HUCK_VICTRON_KEY ""
#endif
#ifndef HUCK_BATTERY_MAC
#define HUCK_BATTERY_MAC ""
#endif

// One saved Wi-Fi network (home, campsites, ...). Ordered by priority.
struct WifiNet { String ssid; String pass; };

struct Settings {
  // Wi-Fi — multiple saved networks; the device auto-joins a known one in range.
  std::vector<WifiNet> networks;
  static constexpr size_t MAX_NETWORKS = 8;
  String hostname = "huckleberry";
  String apSsid   = "Huckleberry";   // no MAC suffix (one-off project)
  String apPass   = "";               // open by default for easy setup

  bool addNetwork(const String& ssid, const String& pass);  // add/update by ssid
  void removeNetwork(const String& ssid);
  bool hasNetworks() const { return !networks.empty(); }

  // Time
  String tz = "PST8PDT,M3.2.0,M11.1.0";  // America/Los_Angeles (Central Valley)
  bool   use24h = false;

  // Theme / display
  int  dayThemeIdx = 0;
  bool animations = true;
  int  dayBrightness = 85;    // day backlight 10-255
  int  nightBrightness = 30;  // night (home) backlight
  bool autoNight = true;      // black/white home page during the night window
  int  nightStartHour = 20;   // night begins (0-23)
  int  nightEndHour = 8;      // night ends (0-23)
  int  homeTimeoutSec = 60;   // inactivity -> return to clock
  bool dispOffEnable = false; // blank the screen during a window (wake on touch)
  int  dispOffStartHour = 23;
  int  dispOffEndHour = 6;

  // BLE device bindings (defaults from gitignored secrets.local.h; else set via web)
  String victronMac = HUCK_VICTRON_MAC;
  String victronKey = HUCK_VICTRON_KEY;   // 16-byte hex
  String batteryMac = HUCK_BATTERY_MAC;
  String gidroxMac  = "";     // TBD when unit arrives
  bool   bleEnabled = true;

  // Thermostat (Gidrox) — control layer TBD (BLE)
  int  setpointF = 70;
  int  mode = 0;              // 0 Auto, 1 Cool, 2 Heat, 3 Fan, 4 Off
  bool camping = true;        // camping vs storing preset
  int  storeMinF = 40;        // freeze protection when storing
  int  storeMaxF = 95;

  void load();
  void save();
};

extern Settings gSettings;

// Parse a "aabb.." or "aa:bb:.." hex string into bytes. Returns count.
size_t parseHexBytes(const String& hex, uint8_t* out, size_t maxLen);
