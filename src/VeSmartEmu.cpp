#include "VeSmartEmu.h"

#include <mbedtls/ccm.h>
#include <math.h>
#include <string.h>

namespace vesmart {
namespace {

constexpr uint8_t MSG_SEQUENCE   = 1;   // full 48-bit sequence sync (empty plaintext)
constexpr uint8_t MSG_COMPACT    = 2;   // compact encrypted VREG records
constexpr uint8_t COMPANY_LO     = 0xE1;  // Victron company id 0x02E1, little-endian
constexpr uint8_t COMPANY_HI     = 0x02;
constexpr uint8_t SENSOR_PRIORITY = 0x08;  // low nibble of compact plaintext[0]
constexpr uint8_t CURRENT_PRIORITY = 0x0C; // SmartSolar rejects Isense below priority 12

void appendU32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 0xFF);
}

void appendVreg(std::vector<uint8_t>& v, uint16_t vreg, const uint8_t* val, uint8_t len) {
  v.push_back(vreg & 0xFF);
  v.push_back((vreg >> 8) & 0xFF);
  v.push_back(len);
  v.insert(v.end(), val, val + len);
}

// 13-byte nonce = [type][u48 sequence LE][u32 source LE][u16 netId LE].
void buildNonce(uint8_t type, uint64_t seq, uint32_t source, uint16_t netId, uint8_t nonce[13]) {
  nonce[0] = type;
  for (int i = 0; i < 6; i++) nonce[1 + i] = (uint8_t)((seq >> (8 * i)) & 0xFF);
  for (int i = 0; i < 4; i++) nonce[7 + i] = (uint8_t)((source >> (8 * i)) & 0xFF);
  nonce[11] = netId & 0xFF;
  nonce[12] = (netId >> 8) & 0xFF;
}

// AES-128-CCM, tag length 4, no AAD. `ct` receives ptLen ciphertext bytes.
bool ccm(const uint8_t key[16], const uint8_t nonce[13], const uint8_t* pt, size_t ptLen,
         uint8_t* ct, uint8_t tag[4]) {
  uint8_t dummy = 0;  // mbedtls wants non-null buffers even at length 0
  mbedtls_ccm_context ctx;
  mbedtls_ccm_init(&ctx);
  bool ok = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
            mbedtls_ccm_encrypt_and_tag(&ctx, ptLen, nonce, 13, nullptr, 0,
                                        ptLen ? pt : &dummy, ptLen ? ct : &dummy,
                                        tag, 4) == 0;
  mbedtls_ccm_free(&ctx);
  return ok;
}

bool buildCompact(const uint8_t key[16], uint16_t netId, uint32_t source, uint64_t seq,
                  const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& manuf) {
  if (plaintext.size() > 13) return false;  // Victron's compact plaintext limit
  uint8_t nonce[13];
  buildNonce(MSG_COMPACT, seq, source, netId, nonce);
  uint8_t ct[13];
  uint8_t tag[4];
  if (!ccm(key, nonce, plaintext.data(), plaintext.size(), ct, tag)) return false;
  manuf.clear();
  manuf.push_back(COMPANY_LO);
  manuf.push_back(COMPANY_HI);
  manuf.push_back(MSG_COMPACT);
  manuf.push_back(netId & 0xFF);
  appendU32(manuf, source);
  for (int i = 0; i < 4; i++) manuf.push_back((uint8_t)((seq >> (8 * i)) & 0xFF));  // seq low 32 bits
  manuf.insert(manuf.end(), ct, ct + plaintext.size());
  manuf.insert(manuf.end(), tag, tag + 4);
  return true;
}

}  // namespace

bool encodeVoltage(float volts, int16_t& raw) {
  if (isnan(volts)) return false;
  float r = roundf(volts * 100.0f);
  if (r < -32768.0f || r >= 32767.0f) return false;  // 0x7FFF reserved for no-data
  raw = (int16_t)r;
  return true;
}

bool encodeTemperature(float celsius, uint16_t& raw) {
  if (isnan(celsius)) return false;
  float r = roundf(celsius * 100.0f + 27315.0f);
  if (r < 0.0f || r >= 65535.0f) return false;  // 0xFFFF reserved for no-data
  raw = (uint16_t)r;
  return true;
}

bool buildIdentity(const uint8_t key[16], uint16_t netId, uint32_t source, uint64_t seq,
                   int16_t voltageRaw, std::vector<uint8_t>& manuf) {
  std::vector<uint8_t> pt;
  pt.push_back(SENSOR_PRIORITY);
  uint8_t prod[4];
  for (int i = 0; i < 4; i++) prod[i] = (uint8_t)((PRODUCT_ID >> (8 * i)) & 0xFF);
  appendVreg(pt, VREG_PRODUCT, prod, 4);
  uint8_t v[2] = {(uint8_t)(voltageRaw & 0xFF), (uint8_t)((voltageRaw >> 8) & 0xFF)};
  appendVreg(pt, VREG_VOLTAGE, v, 2);
  return buildCompact(key, netId, source, seq, pt, manuf);
}

bool encodeCurrent(float amps, int32_t& raw) {
  if (isnan(amps)) return false;
  double r = round(static_cast<double>(amps) * 1000.0);
  if (r < -2147483648.0 || r >= 2147483647.0) return false;
  raw = static_cast<int32_t>(r);
  return true;
}

bool buildStatus(const uint8_t key[16], uint16_t netId, uint32_t source, uint64_t seq,
                 uint16_t tempRaw, std::vector<uint8_t>& manuf) {
  std::vector<uint8_t> pt;
  pt.push_back(SENSOR_PRIORITY);
  uint8_t fw[4];
  for (int i = 0; i < 4; i++) fw[i] = (uint8_t)((FIRMWARE_RECORD >> (8 * i)) & 0xFF);
  appendVreg(pt, VREG_FIRMWARE, fw, 4);
  uint8_t t[2] = {(uint8_t)(tempRaw & 0xFF), (uint8_t)((tempRaw >> 8) & 0xFF)};
  appendVreg(pt, VREG_TEMPERATURE, t, 2);
  return buildCompact(key, netId, source, seq, pt, manuf);
}

bool buildCurrent(const uint8_t key[16], uint16_t netId, uint32_t source, uint64_t seq,
                  int32_t currentRaw, std::vector<uint8_t>& manuf) {
  std::vector<uint8_t> pt;
  pt.push_back(CURRENT_PRIORITY);
  uint32_t bits = static_cast<uint32_t>(currentRaw);
  uint8_t c[4] = {
    static_cast<uint8_t>(bits), static_cast<uint8_t>(bits >> 8),
    static_cast<uint8_t>(bits >> 16), static_cast<uint8_t>(bits >> 24)
  };
  appendVreg(pt, VREG_CURRENT, c, 4);
  return buildCompact(key, netId, source, seq, pt, manuf);
}

bool buildSequence(const uint8_t key[16], uint16_t netId, uint32_t source, uint64_t seq,
                   std::vector<uint8_t>& manuf) {
  uint8_t nonce[13];
  buildNonce(MSG_SEQUENCE, seq, source, netId, nonce);
  uint8_t tag[4];
  if (!ccm(key, nonce, nullptr, 0, nullptr, tag)) return false;
  manuf.clear();
  manuf.push_back(COMPANY_LO);
  manuf.push_back(COMPANY_HI);
  manuf.push_back(MSG_SEQUENCE);
  manuf.push_back(netId & 0xFF);
  appendU32(manuf, source);
  for (int i = 0; i < 6; i++) manuf.push_back((uint8_t)((seq >> (8 * i)) & 0xFF));  // full 48-bit
  manuf.insert(manuf.end(), tag, tag + 4);
  return true;
}

}  // namespace vesmart
