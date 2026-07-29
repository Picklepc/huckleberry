#pragma once
#include <Arduino.h>

namespace ble {
void begin();

// VE.Smart external-sense emulator runtime status, for the web UI. Settings
// (enabled, network id/name, keySet) live in gSettings; these are the live
// broadcast state owned by the BLE task.
struct VsStatus {
  bool     battFresh = false;    // battery voltage is fresh and encodable
  bool     solFresh = false;     // charger present recently (for card visibility)
  bool     broadcasting = false; // advertising sensor packets right now
  float    srcVolts = NAN;       // last broadcast voltage (V)
  float    srcTempC = NAN;       // last broadcast temperature (deg C)
  float    srcAmps = NAN;        // last broadcast current (A, Isense; NAN if unavailable)
  uint32_t adverts = 0;          // packets sent since boot
  // Network config and receiver diagnostics read back from the charger.
  bool     chargerRead = false;        // a read from the charger has completed
  bool     chargerIdOk = false;        // charger has a configured network
  bool     chargerKeyReadable = false; // charger returned a usable 16-byte key
  uint16_t chargerId = 0;
  char     chargerName[33] = {0};
  bool     chargerTxVregsReadable = false; // 0xEC15 was returned
  bool     chargerRxVregsReadable = false; // 0xEC16 was returned
  bool     chargerRssiReadable = false;    // 0xEC42 was returned
  uint8_t  chargerTxVregs = 0;              // VREGs transmitted by charger
  uint8_t  chargerRxVregs = 0;              // unique VREGs received since boot
  int8_t   chargerRssi = 0;                  // raw signed VE.Smart network RSSI
  bool     chargerInRangeReadable = false;   // 0xEC30/0xEC31 device list returned
  uint8_t  chargerInRangeCount = 0;          // includes the charger itself
  bool     chargerEmulatorSeen = false;      // configured Huckleberry source is listed
  uint8_t  chargerEmulatorAge = 0xff;        // seconds since last accepted broadcast
  uint16_t chargerEmulatorProduct = 0xffff;
  uint32_t chargerEmulatorVersion = 0xffffffff;
  bool     chargerRxStatusReadable = false;  // 0xEC20 per-VREG source table returned
  bool     chargerVoltageAccepted = false;   // ED8D has a live network source
  bool     chargerTempAccepted = false;      // EDEC has a live network source
  bool     chargerCurrentAccepted = false;   // ED8C has a live network source
  uint8_t  chargerVoltageAge = 0xff;         // seconds since accepted source update
  uint8_t  chargerTempAge = 0xff;
  uint8_t  chargerCurrentAge = 0xff;
  uint8_t  chargerVoltageClass = 0xff;       // compact source/priority nibble
  uint8_t  chargerTempClass = 0xff;
  uint8_t  chargerCurrentClass = 0xff;
  uint32_t chargerVoltageSource = 0xffffffff;
  uint32_t chargerTempSource = 0xffffffff;
  uint32_t chargerCurrentSource = 0xffffffff;
  bool     chargerSenseVoltageReadable = false;
  bool     chargerSenseTempReadable = false;
  bool     chargerSenseCurrentReadable = false;
  float    chargerSenseVoltage = NAN;
  float    chargerSenseTempC = NAN;
  float    chargerSenseCurrentA = NAN;
};
void vsStatus(VsStatus& out);

// Queue an on-demand connected read of the charger's VE.Smart network config;
// on success Huckleberry adopts that ID/name (+ key when readable).
void requestChargerNetworkRead();
}
