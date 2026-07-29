#pragma once
// VE.Smart Battery Sense broadcast packet builder.
//
// Direct C++ port of tools/victron_re/vesmart_packet.py, kept byte-for-byte
// compatible with its --self-test vectors. Builds the BLE manufacturer-specific
// data payload (starting at the Victron company-id bytes E1 02) for each
// advertisement. AES-128-CCM, 13-byte nonce, 4-byte tag, no AAD.
#include <Arduino.h>
#include <vector>

namespace vesmart {

// Smart Battery Sense v7 (v1.15) identity broadcast as the sensor product.
static constexpr uint32_t PRODUCT_ID      = 0x0000A3A5;
static constexpr uint32_t FIRMWARE_RECORD = 0x000115FF;
static constexpr uint16_t VREG_PRODUCT     = 0x0100;
static constexpr uint16_t VREG_FIRMWARE    = 0x0102;
static constexpr uint16_t VREG_VOLTAGE     = 0xED8D;  // signed 0.01 V
static constexpr uint16_t VREG_TEMPERATURE = 0xEDEC;  // unsigned 0.01 K
static constexpr uint16_t VREG_CURRENT     = 0xED8C;  // signed 1 mA (Isense)
static constexpr int16_t  VOLTAGE_NO_DATA  = 0x7FFF;  // never transmitted
static constexpr uint16_t TEMP_NO_DATA     = 0xFFFF;  // never transmitted
static constexpr int32_t  CURRENT_NO_DATA  = 0x7FFFFFFF;  // never transmitted

// Encode battery voltage (V) to signed 0.01 V raw. Returns false when out of the
// transmittable range; callers must then PAUSE, never send the no-data sentinel.
bool encodeVoltage(float volts, int16_t& raw);
// Encode battery temperature (deg C) to unsigned 0.01 K raw. Returns false when
// out of range.
bool encodeTemperature(float celsius, uint16_t& raw);
// Encode battery current (A, + = charging) to signed 1 mA raw. Returns false
// when out of range.
bool encodeCurrent(float amps, int32_t& raw);

// Each function fills `manuf` with the manufacturer-specific data for one BLE
// advertisement (E1 02 ...), ready for NimBLE setManufacturerData. `sequence`
// is a 48-bit monotonically increasing value; a fresh value is used per packet.
bool buildIdentity(const uint8_t key[16], uint16_t netId, uint32_t source,
                   uint64_t sequence, int16_t voltageRaw, std::vector<uint8_t>& manuf);
bool buildStatus(const uint8_t key[16], uint16_t netId, uint32_t source,
                 uint64_t sequence, uint16_t tempRaw, std::vector<uint8_t>& manuf);
// Optional third field (Isense), sent at the charger-required priority 12 and
// only while the BMS current is valid.
bool buildCurrent(const uint8_t key[16], uint16_t netId, uint32_t source,
                  uint64_t sequence, int32_t currentRaw, std::vector<uint8_t>& manuf);
bool buildSequence(const uint8_t key[16], uint16_t netId, uint32_t source,
                   uint64_t sequence, std::vector<uint8_t>& manuf);

}  // namespace vesmart
