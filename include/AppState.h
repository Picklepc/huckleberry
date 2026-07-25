#pragma once
// Shared state across Huckleberry modules (UI, net, BLE, web).
#include <Arduino.h>
#include <time.h>

static constexpr size_t HUCK_MAX_BATT_CELLS = 16;
static constexpr size_t HUCK_MAX_BATT_TEMPS = 6;

// ---- Live telemetry, written by the BLE task, read by UI + web ----
struct Telemetry {
  // Eco-Worthy battery (JBD FF00)
  bool     battValid = false;
  float    battVolts = NAN;
  float    battAmps  = NAN;    // + charging, - load
  float    battPowerW = NAN;
  int      battSoc   = -1;     // %
  float    battResidAh = NAN;
  float    battNomAh   = NAN;
  int      battCycles  = -1;
  int      battCellCount = -1;
  int      battTempCount = -1;
  float    battTempsC[HUCK_MAX_BATT_TEMPS] = {NAN, NAN, NAN, NAN, NAN, NAN};
  uint16_t battCellMv[HUCK_MAX_BATT_CELLS] = {0};
  uint16_t battProtect = 0;
  uint8_t  battFet = 0;
  uint8_t  battSw = 0;
  uint32_t battLastMs  = 0;
  uint32_t battCellLastMs = 0;

  // Victron SmartSolar MPPT (Instant Readout)
  bool     solValid = false;
  uint8_t  solState = 0;       // 0 Off, 3 Bulk, 4 Absorption, 5 Float...
  uint8_t  solError = 0;
  float    solBattV = NAN;
  float    solBattA = NAN;
  float    solPvW   = NAN;
  float    solPvMaxW = NAN;    // observed live peak since boot
  float    solYieldKwh = NAN;
  float    solLoadA = NAN;
  int      solRssi  = 0;
  uint32_t solLastMs = 0;
  uint32_t solPvMaxLastMs = 0;
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
extern volatile bool gBgReloadRequested;

// Thread-safe-ish access (single BLE writer, UI/web readers).
void teleLock();
void teleUnlock();
