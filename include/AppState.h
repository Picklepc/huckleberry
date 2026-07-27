#pragma once
// Shared state across Huckleberry modules (UI, net, BLE, web).
#include <Arduino.h>
#include <time.h>

static constexpr size_t HUCK_MAX_BATT_CELLS = 16;
static constexpr size_t HUCK_MAX_BATT_TEMPS = 6;
static constexpr size_t HUCK_VICTRON_HISTORY_DAYS = 31;

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

  // Derived cabin/ambient temperature. The EcoWorthy pack's temp sensor sits
  // in the trailer and tracks inside air closely, so we surface it system-wide
  // as the "inside" temperature (clock page, dashboard, thermostat reference).
  float    insideTempF = NAN;

  // Victron SmartSolar MPPT (Instant Readout)
  bool     solValid = false;
  uint8_t  solState = 0;       // 0 Off, 3 Bulk, 4 Absorption, 5 Float...
  uint8_t  solError = 0;
  float    solBattV = NAN;
  float    solBattA = NAN;
  float    solPvW   = NAN;
  float    solYieldKwh = NAN;
  float    solLoadA = NAN;
  float    solPvV = NAN;
  float    solLoadV = NAN;
  int8_t   solLoadState = -1;
  float    solTotalYieldKwh = NAN;
  float    solUserYieldKwh = NAN;
  float    solPeakTodayW = NAN;
  float    solBattMinTodayV = NAN;
  float    solBattMaxTodayV = NAN;
  float    solPvMaxTodayV = NAN;
  float    solMaxBattCurrentTodayA = NAN;
  uint16_t solBulkMinutesToday = 0;
  uint16_t solAbsorptionMinutesToday = 0;
  uint16_t solFloatMinutesToday = 0;
  uint16_t solDaySequence = 0;
  uint8_t  solHistoryDays = 0;
  uint32_t solProductId = 0;
  char     solModel[40] = {0};
  // Extended connected device info + energy (deepened data)
  char     solSerial[24] = {0};       // VREG 0x010A, ASCII
  uint32_t solFwVersion = 0;          // VREG 0x0102, raw (candidate)
  float    solYieldYesterdayKwh = NAN;      // VREG 0xEDD1
  float    solMaxPowerYesterdayW = NAN;     // VREG 0xEDD0
  float    solBattTempC = NAN;              // VREG 0xEDEC (external sensor)
  // Diagnostic: VREGs the charger reported as unknown (CBOR opcode 9) on the
  // last connected read — lets us confirm which candidate registers are valid
  // on real hardware without a reflash-and-guess loop.
  uint16_t solUnknownVregs[10] = {0};
  uint8_t  solUnknownVregCount = 0;
  uint32_t solConnectedLastMs = 0;
  int      solRssi  = 0;
  uint32_t solLastMs = 0;
};

struct VictronDay {
  bool valid = false;
  uint8_t ageDays = 0;
  uint16_t sequence = 0;
  float yieldKwh = NAN;
  float consumedKwh = NAN;
  float battMaxV = NAN;
  float battMinV = NAN;
  float peakPowerW = NAN;
  float maxBattCurrentA = NAN;
  float pvMaxV = NAN;
  uint16_t bulkMinutes = 0;
  uint16_t absorptionMinutes = 0;
  uint16_t floatMinutes = 0;
  uint8_t errors[4] = {0, 0, 0, 0};
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

void victronDayStore(size_t ageDays, const VictronDay& day);
bool victronDayCopy(size_t ageDays, VictronDay& out);
size_t victronValidDayCount();
float victronHistoryPeakPowerW();
