#pragma once
// Shared state across Huckleberry modules (UI, net, BLE, web).
#include <Arduino.h>
#include <time.h>

// ---- Live telemetry, written by the BLE task, read by UI + web ----
struct Telemetry {
  // Eco-Worthy battery (JBD FF00)
  bool     battValid = false;
  float    battVolts = NAN;
  float    battAmps  = NAN;    // + charging, - load
  int      battSoc   = -1;     // %
  float    battResidAh = NAN;
  float    battNomAh   = NAN;
  int      battCycles  = -1;
  uint32_t battLastMs  = 0;

  // Victron SmartSolar MPPT (Instant Readout)
  bool     solValid = false;
  uint8_t  solState = 0;       // 0 Off, 3 Bulk, 4 Absorption, 5 Float...
  uint8_t  solError = 0;
  float    solBattV = NAN;
  float    solBattA = NAN;
  float    solPvW   = NAN;
  float    solYieldKwh = NAN;
  float    solLoadA = NAN;
  int      solRssi  = 0;
  uint32_t solLastMs = 0;
};

// ---- Network status ----
struct NetStatus {
  bool   staConnected = false;
  bool   apActive = false;
  String ssid;
  String ip;
  String apSsid;
  bool   timeSynced = false;   // NTP or browser push has set the clock
  String timeSource = "build"; // "ntp" | "browser" | "build"
};

extern Telemetry gTele;
extern NetStatus gNet;

// Set by the web layer when settings that affect the on-device UI change
// (theme, brightness, animations, thermostat). The UI loop applies + clears it.
extern volatile bool gUiApplyRequested;

// Thread-safe-ish access (single BLE writer, UI/web readers).
void teleLock();
void teleUnlock();
