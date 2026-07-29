#include "BleManager.h"
#include "AppState.h"
#include "Settings.h"
#include "VictronTrends.h"
#include "VeSmartEmu.h"

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <mbedtls/aes.h>
#include <cstring>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifndef HUCK_DEBUG
#define HUCK_DEBUG 0
#endif
#if HUCK_DEBUG
#define HDBG(...) Serial.printf(__VA_ARGS__)
#else
#define HDBG(...)
#endif

namespace ble {

static constexpr uint16_t VICTRON_CID = 0x02E1;

// JBD FF00 UUIDs
static NimBLEUUID JBD_SVC((uint16_t)0xFF00);
static NimBLEUUID JBD_NOTIFY((uint16_t)0xFF01);
static NimBLEUUID JBD_WRITE((uint16_t)0xFF02);

static NimBLEUUID VESMART_SVC("306b0001-b081-4037-83dc-e59fcc3cdfd0");
static NimBLEUUID VESMART_CONTROL("306b0002-b081-4037-83dc-e59fcc3cdfd0");
static NimBLEUUID VESMART_LAST_DATA("306b0003-b081-4037-83dc-e59fcc3cdfd0");
static NimBLEUUID VESMART_DATA("306b0004-b081-4037-83dc-e59fcc3cdfd0");

static uint8_t   s_vkey[16];
static bool      s_vkeyOk = false;
static NimBLEAddress s_victronAddress;
static bool      s_victronAddressOk = false;

// ---- bit helpers for the Victron packed payload (LSB-first) ----
static uint32_t bitsLE(const uint8_t* d, size_t len, size_t start, size_t n) {
  uint32_t v = 0;
  for (size_t i = 0; i < n; i++) {
    size_t bit = start + i;
    if (bit / 8 >= len) break;
    v |= (uint32_t)((d[bit / 8] >> (bit % 8)) & 1) << i;
  }
  return v;
}
static int32_t signExt(uint32_t v, int bits) {
  if (v & (1u << (bits - 1))) return (int32_t)(v - (1u << bits));
  return (int32_t)v;
}

// ---- Victron Instant Readout decode (mbedtls AES-CTR) ----
// `rec` points at manufacturer byte after the [company][0x10][model][0xA0] header,
// i.e. rec[0]=readout type (0x01 solar), rec[1..2]=nonce, rec[3]=key check.
static bool decryptVictron(const uint8_t* rec, size_t len, uint8_t* out,
                           size_t outCapacity, size_t& outLength) {
  if (len < 4 || !s_vkeyOk) return false;
  if (rec[0] != 0x01 || rec[3] != s_vkey[0]) return false;   // solar charger + key check
  outLength = len - 4;
  if (outLength > outCapacity) return false;
  uint16_t nonce = (uint16_t)rec[1] | ((uint16_t)rec[2] << 8);
  uint8_t nc[16] = {0}, sb[16] = {0};
  size_t off = 0;
  nc[0] = nonce & 0xFF; nc[1] = (nonce >> 8) & 0xFF;
  mbedtls_aes_context aes; mbedtls_aes_init(&aes);
  if (mbedtls_aes_setkey_enc(&aes, s_vkey, 128) != 0) { mbedtls_aes_free(&aes); return false; }
  int rc = mbedtls_aes_crypt_ctr(&aes, outLength, &off, nc, sb, rec + 4, out);
  mbedtls_aes_free(&aes);
  return rc == 0;
}

static void parseVictron(const uint8_t* payload, size_t length, int rssi) {
  if (length < 12) return;
  uint8_t state = bitsLE(payload, length, 0, 8);
  uint8_t err   = bitsLE(payload, length, 8, 8);
  int16_t bv = (int16_t)signExt(bitsLE(payload, length, 16, 16), 16);
  int16_t bc = (int16_t)signExt(bitsLE(payload, length, 32, 16), 16);
  uint16_t yd = bitsLE(payload, length, 48, 16);
  uint16_t pv = bitsLE(payload, length, 64, 16);
  uint16_t ld = bitsLE(payload, length, 80, 9);
  float pvW = (pv == 0xFFFF) ? NAN : (float)pv;
  teleLock();
  gTele.solState = state; gTele.solError = err;
  gTele.solBattV = (bv == 0x7FFF) ? NAN : bv * 0.01f;
  gTele.solBattA = (bc == 0x7FFF) ? NAN : bc * 0.1f;
  gTele.solYieldKwh = (yd == 0xFFFF) ? NAN : yd * 0.01f;
  gTele.solPvW = pvW;
  gTele.solLoadA = (ld == 0x1FF) ? NAN : ld * 0.1f;
  gTele.solRssi = rssi; gTele.solValid = true; gTele.solLastMs = millis();
  teleUnlock();
  HDBG("[BLE] victron pv=%.0fW state=%u rssi=%d\n",
       isnan(pvW) ? -1.0f : pvW, state, rssi);
}

static void handleVictronAdv(const NimBLEAdvertisedDevice* dev) {
  if (!dev->haveManufacturerData()) return;
  std::string m = dev->getManufacturerData();
  if (m.size() < 10) return;
  const uint8_t* raw = (const uint8_t*)m.data();
  uint16_t cid = (uint16_t)raw[0] | ((uint16_t)raw[1] << 8);
  if (cid != VICTRON_CID) return;
  if (raw[2] != 0x10) return;          // product advertisement
  if (raw[5] != 0xA0) return;          // instant-readout marker
  uint8_t payload[32];
  size_t payloadLength = 0;
  if (decryptVictron(raw + 6, m.size() - 6, payload, sizeof(payload), payloadLength))
    parseVictron(payload, payloadLength, dev->getRSSI());
}

// ---- scan callbacks ----
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (s_victronAddressOk &&
        memcmp(dev->getAddress().getVal(), s_victronAddress.getVal(), 6) == 0) {
      handleVictronAdv(dev);
    }
  }
};

// ---- JBD battery notification assembly ----
static uint8_t s_jbdBuf[128];
static size_t s_jbdLength = 0;
static volatile bool s_jbdFrameReady = false;

static void jbdNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (s_jbdLength + len > sizeof(s_jbdBuf)) {
    s_jbdLength = 0;
    s_jbdFrameReady = false;
    return;
  }
  memcpy(s_jbdBuf + s_jbdLength, data, len);
  s_jbdLength += len;
  if (s_jbdLength >= 4 && s_jbdBuf[0] == 0xDD && s_jbdBuf[s_jbdLength - 1] == 0x77)
    s_jbdFrameReady = true;
}

static bool parseJbdBasic(const uint8_t* data, size_t length) {
  if (length < 4 || data[0] != 0xDD || data[1] != 0x03 || data[2] != 0x00) return false;
  int ln = data[3];
  if ((int)length < 4 + ln || ln < 20) return false;
  const uint8_t* p = data + 4;
  float volt = ((p[0] << 8) | p[1]) / 100.0f;
  int16_t rawCur = (int16_t)((p[2] << 8) | p[3]);
  float cur = rawCur / 100.0f;
  float resid = ((p[4] << 8) | p[5]) / 100.0f;
  float nom = ((p[6] << 8) | p[7]) / 100.0f;
  int cycles = (p[8] << 8) | p[9];
  uint16_t protect = (ln > 17) ? ((p[16] << 8) | p[17]) : 0;
  uint8_t sw = (ln > 18) ? p[18] : 0;
  int soc = (ln > 19) ? p[19] : -1;
  uint8_t fet = (ln > 20) ? p[20] : 0;
  int cells = (ln > 21) ? p[21] : -1;
  int temps = (ln > 22) ? p[22] : -1;
  teleLock();
  gTele.battVolts = volt; gTele.battAmps = cur; gTele.battSoc = soc;
  gTele.battPowerW = volt * cur;
  gTele.battResidAh = resid; gTele.battNomAh = nom; gTele.battCycles = cycles;
  gTele.battProtect = protect; gTele.battSw = sw; gTele.battFet = fet;
  gTele.battCellCount = cells; gTele.battTempCount = temps;
  for (size_t i = 0; i < HUCK_MAX_BATT_TEMPS; i++) gTele.battTempsC[i] = NAN;
  if (temps > 0) {
    for (int i = 0; i < temps && i < (int)HUCK_MAX_BATT_TEMPS; i++) {
      int off = 23 + i * 2;
      if (off + 1 >= ln) break;
      uint16_t rawT = (uint16_t)((p[off] << 8) | p[off + 1]);
      gTele.battTempsC[i] = rawT / 10.0f - 273.15f;
    }
  }
  // Average the valid pack sensors → inside/ambient temperature (°F).
  float tempSum = 0.0f;
  int tempN = 0;
  for (size_t i = 0; i < HUCK_MAX_BATT_TEMPS; i++) {
    if (!isnan(gTele.battTempsC[i])) { tempSum += gTele.battTempsC[i]; tempN++; }
  }
  gTele.insideTempF = tempN ? (tempSum / tempN * 9.0f / 5.0f + 32.0f) : NAN;
  gTele.battValid = true; gTele.battLastMs = millis();
  teleUnlock();
  HDBG("[BLE] batt basic v=%.2f a=%.2f soc=%d resid=%.2fAh nom=%.2fAh cells=%d temps=%d fet=0x%02x prot=0x%04x\n",
       volt, cur, soc, resid, nom, cells, temps, fet, protect);
  return true;
}

static bool parseJbdCells(const uint8_t* data, size_t length) {
  if (length < 4 || data[0] != 0xDD || data[1] != 0x04 || data[2] != 0x00) return false;
  int ln = data[3];
  if ((int)length < 4 + ln) return false;
  const uint8_t* p = data + 4;
  int cells = ln / 2;
  if (cells > (int)HUCK_MAX_BATT_CELLS) cells = HUCK_MAX_BATT_CELLS;
  teleLock();
  for (size_t i = 0; i < HUCK_MAX_BATT_CELLS; i++) gTele.battCellMv[i] = 0;
  for (int i = 0; i < cells; i++) {
    gTele.battCellMv[i] = (uint16_t)((p[i * 2] << 8) | p[i * 2 + 1]);
  }
  if (gTele.battCellCount < 0 || gTele.battCellCount > cells) gTele.battCellCount = cells;
  gTele.battCellLastMs = millis();
  teleUnlock();
  HDBG("[BLE] batt cells=%d first=%umV\n", cells, cells > 0 ? (unsigned)((p[0] << 8) | p[1]) : 0);
  return true;
}

static bool requestJbdFrame(NimBLERemoteCharacteristic* write, uint8_t cmd, uint32_t timeoutMs = 3000) {
  uint16_t checksum = (uint16_t)(0x10000 - cmd);
  uint8_t req[] = {0xDD, 0xA5, cmd, 0x00, (uint8_t)(checksum >> 8), (uint8_t)(checksum & 0xFF), 0x77};
  s_jbdLength = 0;
  s_jbdFrameReady = false;
  write->writeValue(req, sizeof(req), false);
  uint32_t t0 = millis();
  while (!s_jbdFrameReady && millis() - t0 < timeoutMs) vTaskDelay(pdMS_TO_TICKS(20));
  if (!s_jbdFrameReady) HDBG("[BLE] jbd cmd=0x%02x timeout len=%u\n", cmd, (unsigned)s_jbdLength);
  return s_jbdFrameReady;
}

static void pollBattery() {
  if (gSettings.batteryMac.isEmpty()) return;
  s_jbdLength = 0;
  s_jbdFrameReady = false;
  NimBLEClient* c = NimBLEDevice::createClient();
  c->setConnectTimeout(6 * 1000);
  NimBLEAddress addr(std::string(gSettings.batteryMac.c_str()), BLE_ADDR_PUBLIC);
  bool ok = c->connect(addr);
  if (!ok) { Serial.println("[BLE] batt connect FAILED"); NimBLEDevice::deleteClient(c); return; }
  NimBLERemoteService* svc = c->getService(JBD_SVC);
  if (svc) {
    NimBLERemoteCharacteristic* notify = svc->getCharacteristic(JBD_NOTIFY);
    NimBLERemoteCharacteristic* write = svc->getCharacteristic(JBD_WRITE);
    if (notify && write && notify->canNotify()) {
      notify->subscribe(true, jbdNotify);
      vTaskDelay(pdMS_TO_TICKS(150));
      bool gotBasic = false;
      bool gotCells = false;
      if (requestJbdFrame(write, 0x03, 3000)) gotBasic = parseJbdBasic(s_jbdBuf, s_jbdLength);
      if (requestJbdFrame(write, 0x04, 3000)) gotCells = parseJbdCells(s_jbdBuf, s_jbdLength);
      if (!gotBasic) HDBG("[BLE] batt basic missing, cells=%d len=%u\n", gotCells, (unsigned)s_jbdLength);
      s_jbdFrameReady = gotBasic || gotCells;
    }
  }
  bool got = s_jbdFrameReady;
  c->disconnect();
  NimBLEDevice::deleteClient(c);
  Serial.printf("[BLE] batt connect=%d frame=%d soc=%d\n", ok, got, gTele.battSoc);
}

// ---- Minimal Victron connected read ----
static SemaphoreHandle_t s_victronCaptureMtx = nullptr;
static std::vector<uint8_t> s_victronCapture;
// Probe-only capture across ALL three characteristics, each notification tagged
// with its source (0xC0 control / 0x1D lastData / 0xDA data) + length byte — so
// we can see path/newPath responses that may not arrive on the data char.
static uint32_t s_victronPairingPin = 0;
static volatile uint16_t s_victronTxCredits = 0;
static volatile bool s_victronCaptureActive = false;
// VREGs the charger rejected as unknown (CBOR opcode 9) during a read — used to
// confirm which candidate registers this model actually serves.
static std::vector<uint16_t> s_victronUnknownVregs;

// VE.Smart network configuration and receiver diagnostics read back from the
// charger's own VREGs: 0xEC12 NetworkId, 0xEC13 NetworkKey, 0xEC14 NetworkName,
// 0xEC15 transmitted VREG count, 0xEC16 received VREG count, 0xEC42 network RSSI.
// This lets Huckleberry adopt the exact ID/key the charger already holds, so the
// user does not have to find the 16-byte key in VictronConnect. Guarded by
// teleLock(). Whether 0xEC13 is actually readable is charger/firmware dependent.
static bool     s_vsChargerRead = false;      // at least one read attempt completed
static bool     s_vsChargerIdOk = false;      // a non-FFFF network is configured
static uint16_t s_vsChargerId = 0;
static bool     s_vsChargerKeyOk = false;     // key readable and not all-FF
static uint8_t  s_vsChargerKey[16] = {0};
static char     s_vsChargerName[33] = {0};
static bool     s_vsChargerTxVregsOk = false;
static bool     s_vsChargerRxVregsOk = false;
static bool     s_vsChargerRssiOk = false;
static uint8_t  s_vsChargerTxVregs = 0;
static uint8_t  s_vsChargerRxVregs = 0;
static int8_t   s_vsChargerRssi = 0;
static bool     s_vsChargerInRangeOk = false;
static uint8_t  s_vsChargerInRangeCount = 0;
static bool     s_vsChargerEmulatorSeen = false;
static uint8_t  s_vsChargerEmulatorAge = 0xff;
static uint16_t s_vsChargerEmulatorProduct = 0xffff;
static uint32_t s_vsChargerEmulatorVersion = 0xffffffff;
static bool     s_vsChargerRxStatusOk = false;
static bool     s_vsChargerVoltageAccepted = false;
static bool     s_vsChargerTempAccepted = false;
static bool     s_vsChargerCurrentAccepted = false;
static uint8_t  s_vsChargerVoltageAge = 0xff;
static uint8_t  s_vsChargerTempAge = 0xff;
static uint8_t  s_vsChargerCurrentAge = 0xff;
static uint8_t  s_vsChargerVoltageClass = 0xff;
static uint8_t  s_vsChargerTempClass = 0xff;
static uint8_t  s_vsChargerCurrentClass = 0xff;
static uint32_t s_vsChargerVoltageSource = 0xffffffff;
static uint32_t s_vsChargerTempSource = 0xffffffff;
static uint32_t s_vsChargerCurrentSource = 0xffffffff;
static bool     s_vsChargerSenseVoltageOk = false;
static bool     s_vsChargerSenseTempOk = false;
static bool     s_vsChargerSenseCurrentOk = false;
static float    s_vsChargerSenseVoltage = NAN;
static float    s_vsChargerSenseTempC = NAN;
static float    s_vsChargerSenseCurrentA = NAN;
static volatile bool s_vsReadPending = false; // on-demand "read from charger" request
static volatile bool s_vsAdvPause = false;   // pause advertising for ordinary connected reads

class VictronClientCB : public NimBLEClientCallbacks {
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
    NimBLEDevice::injectPassKey(connInfo, s_victronPairingPin);
  }
};
static VictronClientCB s_victronClientCB;

static void victronNotifyCB(NimBLERemoteCharacteristic* characteristic, uint8_t* data,
                            size_t length, bool /*isNotify*/) {
  if (!s_victronCaptureMtx) return;
  const NimBLEUUID& uuid = characteristic->getUUID();
  bool isData = uuid == VESMART_DATA || uuid == VESMART_LAST_DATA;
  bool isControl = uuid == VESMART_CONTROL;
  xSemaphoreTake(s_victronCaptureMtx, portMAX_DELAY);
  if (isData && s_victronCaptureActive && s_victronCapture.size() + length <= 4096) {
    s_victronCapture.insert(s_victronCapture.end(), data, data + length);
  }
  if (isControl && length >= 2 && data[0] == 0xf9) {
    s_victronTxCredits += data[1];
  }
  xSemaphoreGive(s_victronCaptureMtx);
}

static bool takeVictronTxCredit(uint32_t timeoutMs) {
  uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    bool available = false;
    xSemaphoreTake(s_victronCaptureMtx, portMAX_DELAY);
    if (s_victronTxCredits > 0) {
      s_victronTxCredits--;
      available = true;
    }
    xSemaphoreGive(s_victronCaptureMtx);
    if (available) return true;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return false;
}

static bool readCborUnsigned(const std::vector<uint8_t>& message, size_t& offset, uint32_t& value) {
  if (offset >= message.size()) return false;
  uint8_t initial = message[offset++];
  if ((initial >> 5) != 0) return false;
  uint8_t additional = initial & 0x1f;
  if (additional < 24) {
    value = additional;
    return true;
  }
  size_t bytes = additional == 24 ? 1 : additional == 25 ? 2 : additional == 26 ? 4 : 0;
  if (bytes == 0 || offset + bytes > message.size()) return false;
  value = 0;
  for (size_t i = 0; i < bytes; i++) value = (value << 8) | message[offset++];
  return true;
}

struct VictronDeviceListItem {
  uint16_t instance;
  uint16_t parent;
};

static bool parseVictronDeviceList(const std::vector<uint8_t>& message,
                                   std::vector<VictronDeviceListItem>& devices) {
  size_t offset = 0;
  uint32_t opcode = 0;
  if (!readCborUnsigned(message, offset, opcode) || opcode != 2 || offset >= message.size()) return false;
  uint8_t arrayHeader = message[offset++];
  size_t remaining = SIZE_MAX;
  if (arrayHeader != 0x9f) {
    if ((arrayHeader >> 5) != 4 || (arrayHeader & 0x1f) >= 24) return false;
    remaining = arrayHeader & 0x1f;
    if ((remaining & 1) != 0) return false;
  }
  while (offset < message.size() && remaining != 0) {
    if (arrayHeader == 0x9f && message[offset] == 0xff) {
      offset++;
      break;
    }
    uint32_t instance = 0;
    uint32_t parent = 0;
    if (!readCborUnsigned(message, offset, instance) ||
        !readCborUnsigned(message, offset, parent) ||
        instance > UINT16_MAX || parent > UINT16_MAX) return false;
    devices.push_back({(uint16_t)instance, (uint16_t)parent});
    if (remaining != SIZE_MAX) remaining -= 2;
  }
  return !devices.empty() && remaining != 1;
}

static void appendCborUnsigned(std::vector<uint8_t>& message, uint32_t value) {
  if (value < 24) {
    message.push_back((uint8_t)value);
  } else if (value <= UINT8_MAX) {
    message.push_back(0x18);
    message.push_back((uint8_t)value);
  } else {
    message.push_back(0x19);
    message.push_back((uint8_t)(value >> 8));
    message.push_back((uint8_t)value);
  }
}

static void appendCborArrayHeader(std::vector<uint8_t>& message, size_t count) {
  if (count < 24) {
    message.push_back(0x80 | (uint8_t)count);
  } else {
    message.push_back(0x98);
    message.push_back((uint8_t)count);
  }
}

static bool readCborSigned(const std::vector<uint8_t>& message, size_t& offset, int32_t& value) {
  if (offset >= message.size()) return false;
  uint8_t major = message[offset] >> 5;
  if (major == 0) {
    uint32_t unsignedValue = 0;
    if (!readCborUnsigned(message, offset, unsignedValue)) return false;
    value = (int32_t)unsignedValue;
    return true;
  }
  if (major != 1) return false;
  uint8_t initial = message[offset++];
  uint8_t additional = initial & 0x1f;
  uint32_t encoded = 0;
  if (additional < 24) {
    encoded = additional;
  } else {
    size_t bytes = additional == 24 ? 1 : additional == 25 ? 2 : additional == 26 ? 4 : 0;
    if (bytes == 0 || offset + bytes > message.size()) return false;
    for (size_t i = 0; i < bytes; i++) encoded = (encoded << 8) | message[offset++];
  }
  value = -(int32_t)encoded - 1;
  return true;
}

static bool readCborByteString(const std::vector<uint8_t>& message, size_t& offset,
                               const uint8_t*& data, size_t& length) {
  if (offset >= message.size() || (message[offset] >> 5) != 2) return false;
  uint8_t additional = message[offset++] & 0x1f;
  uint32_t decodedLength = 0;
  if (additional < 24) {
    decodedLength = additional;
  } else {
    size_t bytes = additional == 24 ? 1 : additional == 25 ? 2 : additional == 26 ? 4 : 0;
    if (bytes == 0 || offset + bytes > message.size()) return false;
    for (size_t i = 0; i < bytes; i++) decodedLength = (decodedLength << 8) | message[offset++];
  }
  if (offset + decodedLength > message.size()) return false;
  data = message.data() + offset;
  length = decodedLength;
  offset += decodedLength;
  return true;
}

static uint16_t readLe16(const uint8_t* data) {
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t readLe32(const uint8_t* data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint32_t readLeValue(const uint8_t* data, size_t length) {
  if (length >= 4) return readLe32(data);
  if (length >= 2) return readLe16(data);
  return length == 1 ? data[0] : 0;
}

static void applyConnectedVictronValue(uint16_t vreg, const uint8_t* data, size_t length) {
  if (!data) return;
  if (vreg >= 0x1050 && vreg <= 0x106e && length >= 34) {
    uint32_t yieldRaw = readLe32(data + 1);
    uint32_t consumedRaw = readLe32(data + 5);
    uint16_t sequence = readLe16(data + 32);
    VictronDay day;
    day.valid = sequence != 0xffff && yieldRaw != 0xffffffff;
    day.ageDays = (uint8_t)(vreg - 0x1050);
    day.sequence = sequence;
    day.yieldKwh = yieldRaw == 0xffffffff ? NAN : yieldRaw * 0.01f;
    day.consumedKwh = consumedRaw == 0xffffffff ? NAN : consumedRaw * 0.01f;
    uint16_t battMax = readLe16(data + 9);
    uint16_t battMin = readLe16(data + 11);
    uint32_t peakPower = readLe32(data + 24);
    uint16_t maxCurrent = readLe16(data + 28);
    uint16_t pvMax = readLe16(data + 30);
    day.battMaxV = battMax == 0xffff ? NAN : battMax * 0.01f;
    day.battMinV = battMin == 0xffff ? NAN : battMin * 0.01f;
    day.peakPowerW = peakPower == 0xffffffff ? NAN : (float)peakPower;
    day.maxBattCurrentA = maxCurrent == 0xffff ? NAN : maxCurrent * 0.1f;
    day.pvMaxV = pvMax == 0xffff ? NAN : pvMax * 0.01f;
    day.bulkMinutes = readLe16(data + 18);
    day.absorptionMinutes = readLe16(data + 20);
    day.floatMinutes = readLe16(data + 22);
    for (size_t i = 0; i < 4; i++) day.errors[i] = data[14 + i];
    victronDayStore(day.ageDays, day);
    if (day.ageDays == 0 && day.valid) {
      teleLock();
      gTele.solPeakTodayW = day.peakPowerW;
      gTele.solBattMinTodayV = day.battMinV;
      gTele.solBattMaxTodayV = day.battMaxV;
      gTele.solPvMaxTodayV = day.pvMaxV;
      gTele.solMaxBattCurrentTodayA = day.maxBattCurrentA;
      gTele.solBulkMinutesToday = day.bulkMinutes;
      gTele.solAbsorptionMinutesToday = day.absorptionMinutes;
      gTele.solFloatMinutesToday = day.floatMinutes;
      gTele.solDaySequence = day.sequence;
      teleUnlock();
    }
    return;
  }

  teleLock();
  switch (vreg) {
    case 0x0100:
      if (length >= 3) gTele.solProductId = readLe16(data + 1);
      break;
    case 0x010b: {
      size_t copyLength = min(length, sizeof(gTele.solModel) - 1);
      memcpy(gTele.solModel, data, copyLength);
      gTele.solModel[copyLength] = '\0';
      break;
    }
    case 0x0201:
      if (length) gTele.solState = data[0];
      break;
    case 0xedd5:
      if (length >= 2) gTele.solBattV = readLe16(data) * 0.01f;
      break;
    case 0xedd7:
      if (length >= 2) gTele.solBattA = readLe16(data) * 0.1f;
      break;
    case 0xedda:
      if (length) gTele.solError = data[0];
      break;
    case 0xedd3:
      gTele.solYieldKwh = readLeValue(data, length) * 0.01f;
      break;
    case 0xedd2:
      gTele.solPeakTodayW = (float)readLeValue(data, length);
      break;
    case 0xedbc:
      gTele.solPvW = readLeValue(data, length) * 0.01f;
      break;
    case 0xedbb:
      if (length >= 2) gTele.solPvV = readLe16(data) * 0.01f;
      break;
    case 0xedad:
      if (length >= 2) gTele.solLoadA = readLe16(data) * 0.1f;
      break;
    case 0xeda9:
      if (length >= 2) gTele.solLoadV = readLe16(data) * 0.01f;
      break;
    case 0xeda8:
      if (length) gTele.solLoadState = data[0];
      break;
    case 0x104f:
      if (length >= 21) {
        uint32_t userYield = readLe32(data + 6);
        uint32_t totalYield = readLe32(data + 10);
        gTele.solUserYieldKwh = userYield == 0xffffffff ? NAN : userYield * 0.01f;
        gTele.solTotalYieldKwh = totalYield == 0xffffffff ? NAN : totalYield * 0.01f;
      }
      break;
    // ---- Deepened data (some VREGs are candidates; the unknown-VREG
    //      diagnostic confirms which the charger actually serves) ----
    case 0x0102:  // firmware version (candidate)
      gTele.solFwVersion = readLeValue(data, length);
      break;
    case 0x010a: {  // serial number, ASCII
      size_t copyLength = min(length, sizeof(gTele.solSerial) - 1);
      memcpy(gTele.solSerial, data, copyLength);
      gTele.solSerial[copyLength] = '\0';
      break;
    }
    case 0xedd1:  // yield yesterday (0.01 kWh)
      gTele.solYieldYesterdayKwh = readLeValue(data, length) * 0.01f;
      break;
    case 0xedd0:  // maximum power yesterday (W)
      gTele.solMaxPowerYesterdayW = (float)readLeValue(data, length);
      break;
    case 0xedec:  // battery temperature (0.01 K; external sensor)
      if (length >= 2) {
        uint16_t raw = readLe16(data);
        gTele.solBattTempC =
            (raw == 0xffff || raw == 0x7fff) ? NAN : (raw * 0.01f - 273.15f);
      }
      break;
  }
  teleUnlock();
}

static size_t parseConnectedVictronValues(const std::vector<uint8_t>& message) {
  size_t offset = 0;
  size_t values = 0;
  while (offset < message.size()) {
    uint32_t opcode = 0;
    if (!readCborUnsigned(message, offset, opcode)) break;
    if (opcode == 8) {
      uint32_t instance = 0;
      uint32_t vreg = 0;
      const uint8_t* data = nullptr;
      size_t length = 0;
      if (!readCborUnsigned(message, offset, instance) ||
          !readCborUnsigned(message, offset, vreg) ||
          !readCborByteString(message, offset, data, length)) break;
      applyConnectedVictronValue((uint16_t)vreg, data, length);
      values++;
    } else if (opcode == 9) {
      uint32_t instance = 0;
      uint32_t vreg = 0;
      int32_t reason = 0;
      if (!readCborUnsigned(message, offset, instance) ||
          !readCborUnsigned(message, offset, vreg) ||
          !readCborSigned(message, offset, reason)) break;
      if (s_victronUnknownVregs.size() < 32) {
        s_victronUnknownVregs.push_back((uint16_t)vreg);
      }
    } else if (opcode == 7) {
      uint32_t instance = 0;
      uint32_t requestOpcode = 0;
      int32_t result = 0;
      if (!readCborUnsigned(message, offset, instance) ||
          !readCborUnsigned(message, offset, requestOpcode) ||
          !readCborSigned(message, offset, result)) break;
    } else {
      break;
    }
  }
  return values;
}

static void startConnectedCapture() {
  xSemaphoreTake(s_victronCaptureMtx, portMAX_DELAY);
  s_victronCapture.clear();
  s_victronCaptureActive = true;
  xSemaphoreGive(s_victronCaptureMtx);
}

static void stopConnectedCapture(std::vector<uint8_t>& captured) {
  xSemaphoreTake(s_victronCaptureMtx, portMAX_DELAY);
  s_victronCaptureActive = false;
  captured.swap(s_victronCapture);
  xSemaphoreGive(s_victronCaptureMtx);
}

static bool sendConnectedGetValues(NimBLERemoteCharacteristic* lastData, uint16_t instance,
                                   const uint16_t* registers, size_t registerCount) {
  if (!takeVictronTxCredit(3000)) return false;
  std::vector<uint8_t> request = {0x05};
  appendCborUnsigned(request, instance);
  appendCborArrayHeader(request, registerCount);
  for (size_t i = 0; i < registerCount; i++) appendCborUnsigned(request, registers[i]);
  return lastData->writeValue(request.data(), request.size(), false);
}

static void appendCborByteString(std::vector<uint8_t>& message, const uint8_t* data,
                                 size_t length) {
  if (length < 24) {
    message.push_back(0x40 | static_cast<uint8_t>(length));
  } else {
    message.push_back(0x58);
    message.push_back(static_cast<uint8_t>(length));
  }
  message.insert(message.end(), data, data + length);
}

static bool sendConnectedSetBytes(NimBLERemoteCharacteristic* lastData, uint16_t instance,
                                  uint16_t vreg, const uint8_t* data, size_t length) {
  if (!takeVictronTxCredit(3000)) return false;
  std::vector<uint8_t> request = {0x06};
  appendCborUnsigned(request, instance);
  appendCborArrayHeader(request, 2);
  appendCborUnsigned(request, vreg);
  appendCborByteString(request, data, length);
  return lastData->writeValue(request.data(), request.size(), false);
}

static bool copyConnectedVreg(const std::vector<uint8_t>& message, uint16_t targetVreg,
                              std::vector<uint8_t>& output) {
  size_t offset = 0;
  bool found = false;
  while (offset < message.size()) {
    uint32_t opcode = 0;
    if (!readCborUnsigned(message, offset, opcode)) break;
    if (opcode == 8) {
      uint32_t instance = 0;
      uint32_t vreg = 0;
      const uint8_t* data = nullptr;
      size_t length = 0;
      if (!readCborUnsigned(message, offset, instance) ||
          !readCborUnsigned(message, offset, vreg) ||
          !readCborByteString(message, offset, data, length)) break;
      if (vreg == targetVreg) {
        output.assign(data, data + length);
        found = true;
      }
    } else if (opcode == 9) {
      uint32_t instance = 0;
      uint32_t vreg = 0;
      int32_t reason = 0;
      if (!readCborUnsigned(message, offset, instance) ||
          !readCborUnsigned(message, offset, vreg) ||
          !readCborSigned(message, offset, reason)) break;
    } else if (opcode == 7) {
      uint32_t instance = 0;
      uint32_t requestOpcode = 0;
      int32_t result = 0;
      if (!readCborUnsigned(message, offset, instance) ||
          !readCborUnsigned(message, offset, requestOpcode) ||
          !readCborSigned(message, offset, result)) break;
    } else {
      break;
    }
  }
  return found;
}

struct TrendConfig {
  uint8_t subTrendCount = 0;
  uint8_t maxPushSamples = 0;
  uint16_t sampleCount[4] = {};
  uint16_t intervalSeconds[4] = {};
};

struct TrendMetadata {
  TrendConfig config[HUCK_VICTRON_TREND_SERIES];
  uint32_t timeRef[HUCK_VICTRON_TREND_SERIES][4] = {};
  uint32_t activeTimeRef = 0;
  uint32_t activeEpoch = 0;
};

static bool readTrendMetadata(NimBLERemoteCharacteristic* lastData, uint16_t instance,
                              TrendMetadata& metadata) {
  uint16_t registers[2 + HUCK_VICTRON_TREND_SERIES * 2] = {0xec5d, 0xec5f};
  for (size_t i = 0; i < HUCK_VICTRON_TREND_SERIES; i++) {
    registers[2 + i] = 0xec4a + i;
    registers[2 + HUCK_VICTRON_TREND_SERIES + i] = 0xec52 + i;
  }
  startConnectedCapture();
  bool sent = sendConnectedGetValues(lastData, instance, registers,
                                     sizeof(registers) / sizeof(registers[0]));
  vTaskDelay(pdMS_TO_TICKS(900));
  std::vector<uint8_t> captured;
  stopConnectedCapture(captured);
  if (!sent) return false;

  const uint16_t expectedVregs[HUCK_VICTRON_TREND_SERIES] = {
    0xec89, 0xedbb, 0xec8a, 0xec88, 0xed8d, 0xed8f
  };
  std::vector<uint8_t> raw;
  if (!copyConnectedVreg(captured, 0xec5d, raw) ||
      raw.size() < HUCK_VICTRON_TREND_SERIES * 2) return false;
  for (size_t i = 0; i < HUCK_VICTRON_TREND_SERIES; i++) {
    if (readLe16(raw.data() + i * 2) != expectedVregs[i]) return false;
  }
  if (!copyConnectedVreg(captured, 0xec5f, raw) || raw.size() < 8) return false;
  metadata.activeTimeRef = readLe32(raw.data());
  metadata.activeEpoch = readLe32(raw.data() + 4);
  if (metadata.activeEpoch < 1600000000UL) return false;

  for (size_t series = 0; series < HUCK_VICTRON_TREND_SERIES; series++) {
    if (!copyConnectedVreg(captured, 0xec4a + series, raw) || raw.size() < 18) return false;
    TrendConfig& config = metadata.config[series];
    config.subTrendCount = min(static_cast<uint8_t>(4), raw[0]);
    config.maxPushSamples = raw[1];
    if (!config.subTrendCount || !config.maxPushSamples) return false;
    for (size_t tier = 0; tier < 4; tier++) {
      config.sampleCount[tier] = readLe16(raw.data() + 2 + tier * 4);
      config.intervalSeconds[tier] = readLe16(raw.data() + 4 + tier * 4);
    }
    if (!copyConnectedVreg(captured, 0xec52 + series, raw) || raw.size() < 16) return false;
    for (size_t tier = 0; tier < 4; tier++) {
      metadata.timeRef[series][tier] = readLe32(raw.data() + tier * 4);
    }
  }
  return true;
}

class TrendDayAccumulator {
 public:
  explicit TrendDayAccumulator(uint8_t series) : series_(series) {}

  bool add(uint32_t timestampUtc, float value) {
    time_t timestamp = static_cast<time_t>(timestampUtc);
    struct tm local = {};
    localtime_r(&timestamp, &local);
    uint32_t dateKey = static_cast<uint32_t>(local.tm_year + 1900) * 10000UL +
                       static_cast<uint32_t>(local.tm_mon + 1) * 100UL + local.tm_mday;
    uint8_t slot = static_cast<uint8_t>(local.tm_hour * 2 + local.tm_min / 30);
    uint32_t slotTimestamp = timestampUtc - (local.tm_min % 30) * 60UL - local.tm_sec;
    if (dateKey_ && dateKey != dateKey_ && !flush()) return false;
    if (!dateKey_) dateKey_ = dateKey;
    sums_[slot] += value;
    counts_[slot]++;
    timestamps_[slot] = slotTimestamp;
    return true;
  }

  bool flush() {
    if (!dateKey_) return true;
    VictronTrendBin bins[HUCK_VICTRON_INTRADAY_SLOTS];
    for (size_t slot = 0; slot < HUCK_VICTRON_INTRADAY_SLOTS; slot++) {
      if (!counts_[slot]) continue;
      bins[slot].valid = true;
      bins[slot].timestampUtc = timestamps_[slot];
      bins[slot].value = sums_[slot] / counts_[slot];
    }
    bool success = victronTrendMergeSeriesDay(dateKey_, series_, bins);
    dateKey_ = 0;
    memset(sums_, 0, sizeof(sums_));
    memset(counts_, 0, sizeof(counts_));
    memset(timestamps_, 0, sizeof(timestamps_));
    return success;
  }

 private:
  uint8_t series_;
  uint32_t dateKey_ = 0;
  float sums_[HUCK_VICTRON_INTRADAY_SLOTS] = {};
  uint16_t counts_[HUCK_VICTRON_INTRADAY_SLOTS] = {};
  uint32_t timestamps_[HUCK_VICTRON_INTRADAY_SLOTS] = {};
};

static bool decodeTrendValue(uint8_t series, const uint8_t* data, float& value) {
  if (series == VICTRON_TREND_OUTPUT_CURRENT) {
    if (data[0] == 0xff) return false;
    value = data[0] * 0.1f;
    return true;
  }
  if (series == VICTRON_TREND_BATTERY_TEMP) {
    if (data[0] == 0x7f) return false;
    value = static_cast<int8_t>(data[0]);
    return true;
  }
  uint16_t raw = readLe16(data);
  if (raw == 0xffff || raw == 0x7fff) return false;
  switch (series) {
    case VICTRON_TREND_PV_VOLTAGE: value = raw * 0.01f; break;
    case VICTRON_TREND_PV_POWER: value = raw; break;
    case VICTRON_TREND_BATTERY_VOLTAGE: value = static_cast<int16_t>(raw) * 0.01f; break;
    case VICTRON_TREND_CHARGE_CURRENT: value = static_cast<int16_t>(raw) * 0.1f; break;
    default: return false;
  }
  return true;
}

static bool parseTrendPush(const std::vector<uint8_t>& raw, uint8_t expectedSeries,
                           const TrendMetadata& metadata, TrendDayAccumulator& accumulator,
                           uint32_t& nextTimeRef, uint16_t& sampleCount) {
  if (raw.size() < 8 || raw[0] != expectedSeries) return false;
  uint32_t replyTimeRef = readLe32(raw.data() + 1);
  sampleCount = raw[5];
  uint16_t interval = readLe16(raw.data() + 6);
  size_t width = expectedSeries == VICTRON_TREND_OUTPUT_CURRENT ||
                 expectedSeries == VICTRON_TREND_BATTERY_TEMP ? 1 : 2;
  if (!sampleCount || !interval || raw.size() < 8 + sampleCount * width) return false;
  for (uint16_t index = 0; index < sampleCount; index++) {
    uint32_t sampleTimeRef = replyTimeRef - static_cast<uint32_t>(index) * interval;
    int64_t epoch = static_cast<int64_t>(metadata.activeEpoch) +
                    static_cast<int64_t>(sampleTimeRef) - metadata.activeTimeRef;
    if (epoch < 1600000000LL || epoch > 2200000000LL) continue;
    float value = NAN;
    if (decodeTrendValue(expectedSeries, raw.data() + 8 + index * width, value) &&
        !accumulator.add(static_cast<uint32_t>(epoch), value)) return false;
  }
  uint32_t span = static_cast<uint32_t>(sampleCount) * interval;
  nextTimeRef = replyTimeRef > span ? replyTimeRef - span : 0;
  return true;
}

static bool requestTrendPush(NimBLERemoteCharacteristic* lastData,
                             NimBLERemoteCharacteristic* control, uint16_t instance,
                             uint8_t series, uint32_t timeRef, uint8_t count,
                             uint16_t& requestNumber, std::vector<uint8_t>& raw) {
  if ((requestNumber++ % 24) == 0) {
    const uint8_t receiveCredit[] = {0xf9, 0x80};
    control->writeValue(receiveCredit, sizeof(receiveCredit), false);
  }
  uint8_t askData[6] = {
    series,
    static_cast<uint8_t>(timeRef), static_cast<uint8_t>(timeRef >> 8),
    static_cast<uint8_t>(timeRef >> 16), static_cast<uint8_t>(timeRef >> 24),
    count
  };
  startConnectedCapture();
  bool sent = sendConnectedSetBytes(lastData, instance, 0xec5b, askData, sizeof(askData));
  vTaskDelay(pdMS_TO_TICKS(55));
  const uint16_t pushRegister = 0xec5b;
  bool readSent = sent && sendConnectedGetValues(lastData, instance, &pushRegister, 1);
  vTaskDelay(pdMS_TO_TICKS(180));
  std::vector<uint8_t> captured;
  stopConnectedCapture(captured);
  if (readSent && copyConnectedVreg(captured, pushRegister, raw)) return true;

  startConnectedCapture();
  readSent = sendConnectedGetValues(lastData, instance, &pushRegister, 1);
  vTaskDelay(pdMS_TO_TICKS(260));
  stopConnectedCapture(captured);
  return readSent && copyConnectedVreg(captured, pushRegister, raw);
}

struct TrendFetchResult {
  uint32_t nextTimeRef = 0;
  uint32_t samples = 0;
  uint8_t blocks = 0;
};

static TrendFetchResult fetchTrendRange(NimBLERemoteCharacteristic* lastData,
                                        NimBLERemoteCharacteristic* control,
                                        uint16_t instance, uint8_t series,
                                        uint32_t startTimeRef, uint32_t sampleLimit,
                                        uint8_t blockLimit, uint8_t maxPush,
                                        const TrendMetadata& metadata,
                                        uint16_t& requestNumber,
                                        TrendDayAccumulator* sharedAccumulator = nullptr) {
  TrendFetchResult result;
  result.nextTimeRef = startTimeRef;
  TrendDayAccumulator localAccumulator(series);
  TrendDayAccumulator& accumulator = sharedAccumulator ? *sharedAccumulator : localAccumulator;
  while (result.nextTimeRef && result.samples < sampleLimit && result.blocks < blockLimit) {
    uint32_t remaining = sampleLimit - result.samples;
    uint8_t requested = static_cast<uint8_t>(min(static_cast<uint32_t>(maxPush), remaining));
    std::vector<uint8_t> raw;
    if (!requestTrendPush(lastData, control, instance, series, result.nextTimeRef,
                          requested, requestNumber, raw)) break;
    uint16_t received = 0;
    uint32_t next = 0;
    if (!parseTrendPush(raw, series, metadata, accumulator, next, received)) break;
    result.samples += received;
    result.blocks++;
    if (!next || next >= result.nextTimeRef) break;
    result.nextTimeRef = next;
  }
  if (!sharedAccumulator) accumulator.flush();
  return result;
}

static bool s_trendBootstrapComplete = false;

static uint8_t trendOldestHistoryAge() {
  if (!gTele.solHistoryDays) return 0;
  return min(static_cast<uint8_t>(HUCK_VICTRON_HISTORY_DAYS - 1),
             static_cast<uint8_t>(gTele.solHistoryDays - 1));
}

static bool trendGroupCoversHistory(uint8_t seriesStart, uint8_t oldestAge) {
  uint8_t seriesEnd = min(static_cast<uint8_t>(HUCK_VICTRON_TREND_SERIES),
                          static_cast<uint8_t>(seriesStart + 3));
  for (uint8_t series = seriesStart; series < seriesEnd; series++) {
    if (series == VICTRON_TREND_BATTERY_TEMP) continue;
    if (!victronTrendSeriesHasDayByAge(series, oldestAge)) return false;
  }
  return true;
}

static size_t fetchStoredTrends(NimBLERemoteCharacteristic* lastData,
                                 NimBLERemoteCharacteristic* control, uint16_t instance) {
  static bool seriesStartInitialized = false;
  static uint8_t seriesStart = 0;
  if (!victronTrendsBegin()) return 0;
  if (!seriesStartInitialized) {
    seriesStart = victronTrendSeriesHasData(VICTRON_TREND_PV_POWER) &&
                  !victronTrendSeriesHasData(VICTRON_TREND_BATTERY_VOLTAGE) ? 3 : 0;
    seriesStartInitialized = true;
  }
  uint8_t oldestAge = trendOldestHistoryAge();
  bool pvGroupComplete = trendGroupCoversHistory(0, oldestAge);
  bool batteryGroupComplete = trendGroupCoversHistory(3, oldestAge);
  if (pvGroupComplete != batteryGroupComplete) seriesStart = pvGroupComplete ? 3 : 0;
  TrendMetadata metadata;
  if (!readTrendMetadata(lastData, instance, metadata)) return 0;
  uint16_t requestNumber = 0;
  size_t blocks = 0;
  uint8_t seriesEnd = min(static_cast<uint8_t>(HUCK_VICTRON_TREND_SERIES),
                          static_cast<uint8_t>(seriesStart + 3));
  for (uint8_t series = seriesStart; series < seriesEnd; series++) {
    const TrendConfig& config = metadata.config[series];
    if (config.subTrendCount >= 4 && config.sampleCount[3] &&
        config.intervalSeconds[3] && metadata.timeRef[series][3]) {
      uint32_t latest = metadata.timeRef[series][3];
      TrendFetchResult newest = fetchTrendRange(
          lastData, control, instance, series, latest, config.maxPushSamples, 1,
          config.maxPushSamples, metadata, requestNumber);
      blocks += newest.blocks;
      uint64_t historySpan = static_cast<uint64_t>(config.sampleCount[3]) *
                             config.intervalSeconds[3];
      uint32_t oldest = historySpan < latest ? latest - static_cast<uint32_t>(historySpan) : 0;
      uint32_t cursor = victronTrendBackfillCursor(series);
      bool backfillComplete = cursor == UINT32_MAX;
      if (!cursor || cursor > latest || cursor < oldest) {
        cursor = newest.nextTimeRef;
      }
      if (!backfillComplete && cursor && cursor >= oldest) {
        uint32_t available = cursor - oldest + config.intervalSeconds[3];
        uint32_t target = min(available,
                              static_cast<uint32_t>(config.maxPushSamples) * 2UL);
        TrendFetchResult older = fetchTrendRange(
            lastData, control, instance, series, cursor, target, 2,
            config.maxPushSamples, metadata, requestNumber);
        blocks += older.blocks;
        bool complete = !older.nextTimeRef || older.nextTimeRef < oldest ||
                        older.samples >= available;
        victronTrendSetBackfillCursor(series, complete ? UINT32_MAX : older.nextTimeRef);
      }
    }
    TrendDayAccumulator recentAccumulator(series);
    for (int tier = min(static_cast<int>(config.subTrendCount), 3) - 1; tier >= 1; tier--) {
      if (!config.sampleCount[tier] || !config.intervalSeconds[tier] ||
          !metadata.timeRef[series][tier]) continue;
      uint8_t blockLimit = static_cast<uint8_t>(
          min(255UL, (static_cast<uint32_t>(config.sampleCount[tier]) +
                      config.maxPushSamples - 1) / config.maxPushSamples));
      TrendFetchResult recent = fetchTrendRange(
          lastData, control, instance, series, metadata.timeRef[series][tier],
          config.sampleCount[tier], blockLimit, config.maxPushSamples,
          metadata, requestNumber, &recentAccumulator);
      blocks += recent.blocks;
    }
    recentAccumulator.flush();
  }
  seriesStart = seriesEnd >= HUCK_VICTRON_TREND_SERIES ? 0 : seriesEnd;
  pvGroupComplete = trendGroupCoversHistory(0, oldestAge);
  batteryGroupComplete = trendGroupCoversHistory(3, oldestAge);
  s_trendBootstrapComplete = pvGroupComplete && batteryGroupComplete;
  Serial.printf("[BLE] Victron stored trends blocks=%u nextSeries=%u complete=%d heap=%u\n",
                static_cast<unsigned>(blocks), seriesStart, s_trendBootstrapComplete,
                static_cast<unsigned>(ESP.getFreeHeap()));
  return blocks;
}

static bool parseVictronPin(const String& value, uint32_t& pin) {
  if (value.length() != 6) return false;
  uint32_t parsed = 0;
  for (size_t i = 0; i < value.length(); i++) {
    if (value[i] < '0' || value[i] > '9') return false;
    parsed = parsed * 10 + (uint32_t)(value[i] - '0');
  }
  pin = parsed;
  return true;
}

// Read the charger's own VE.Smart network config (id/key/name) inside an
// established connected session. Stores whatever the charger returns; if 0xEC13
// is not readable on this firmware the key simply stays unavailable.
static void readVsNetworkFromCharger(NimBLERemoteCharacteristic* lastData, uint16_t instance) {
  const uint16_t regs[] = {
    0xec12, 0xec13, 0xec14, 0xec15, 0xec16, 0xec20, 0xec30, 0xec31,
    0xec42, 0xed8d, 0xedec, 0xed8c
  };
  startConnectedCapture();
  bool sent = sendConnectedGetValues(lastData, instance, regs,
                                     sizeof(regs) / sizeof(regs[0]));
  vTaskDelay(pdMS_TO_TICKS(1000));
  std::vector<uint8_t> captured;
  stopConnectedCapture(captured);
  if (!sent) return;

  std::vector<uint8_t> raw;
  bool idOk = false, keyOk = false;
  uint16_t id = 0;
  uint8_t key[16] = {0};
  char name[33] = {0};
  bool txVregsOk = false, rxVregsOk = false, rssiOk = false;
  uint8_t txVregs = 0, rxVregs = 0;
  int8_t rssi = 0;
  bool inRangeOk = false, emulatorSeen = false;
  uint8_t inRangeCount = 0, emulatorAge = 0xff;
  uint16_t emulatorProduct = 0xffff;
  uint32_t emulatorVersion = 0xffffffff;
  bool rxStatusOk = false, voltageAccepted = false, tempAccepted = false;
  bool currentAccepted = false;
  uint8_t voltageAge = 0xff, tempAge = 0xff, currentAge = 0xff;
  uint8_t voltageClass = 0xff, tempClass = 0xff, currentClass = 0xff;
  uint32_t voltageSource = 0xffffffff, tempSource = 0xffffffff;
  uint32_t currentSource = 0xffffffff;
  bool senseVoltageOk = false, senseTempOk = false, senseCurrentOk = false;
  float senseVoltage = NAN, senseTempC = NAN, senseCurrentA = NAN;
  const uint32_t expectedSource = gSettings.vsSourceAddr
      ? gSettings.vsSourceAddr : static_cast<uint32_t>(ESP.getEfuseMac());
  if (copyConnectedVreg(captured, 0xec12, raw) && raw.size() >= 2) {
    id = (uint16_t)(raw[0] | (raw[1] << 8));
    idOk = raw[0] != 0xFF || raw[1] != 0xFF;  // a configured network is not all-FF
  }
  if (copyConnectedVreg(captured, 0xec13, raw) && raw.size() >= 16) {
    memcpy(key, raw.data(), 16);
    for (int i = 0; i < 16; i++) if (key[i] != 0xFF) { keyOk = true; break; }
  }
  if (copyConnectedVreg(captured, 0xec14, raw) && !raw.empty()) {
    size_t n = raw.size() < 32 ? raw.size() : 32;
    memcpy(name, raw.data(), n);
    name[n] = 0;
  }
  if (copyConnectedVreg(captured, 0xec15, raw) && !raw.empty()) {
    txVregs = raw[0];
    txVregsOk = true;
  }
  if (copyConnectedVreg(captured, 0xec16, raw) && !raw.empty()) {
    rxVregs = raw[0];
    rxVregsOk = true;
  }
  if (copyConnectedVreg(captured, 0xec20, raw) && raw.size() >= 8) {
    rxStatusOk = true;
    for (size_t offset = 0; offset + 8 <= raw.size(); offset += 8) {
      uint16_t vreg = readLe16(raw.data() + offset);
      uint8_t age = raw[offset + 2];
      uint8_t priority = raw[offset + 3];
      uint32_t source = readLe32(raw.data() + offset + 4);
      bool accepted = age != 0xff && priority != 0xff && source != 0xffffffff;
      if (vreg == 0xed8d) {
        voltageAge = age;
        voltageClass = priority;
        voltageSource = source;
        voltageAccepted = accepted;
      } else if (vreg == 0xedec) {
        tempAge = age;
        tempClass = priority;
        tempSource = source;
        tempAccepted = accepted;
      } else if (vreg == 0xed8c) {
        currentAge = age;
        currentClass = priority;
        currentSource = source;
        currentAccepted = accepted;
      }
    }
  }
  if (copyConnectedVreg(captured, 0xec30, raw) && !raw.empty()) {
    inRangeCount = raw[0];
    inRangeOk = true;
  }
  if (copyConnectedVreg(captured, 0xec31, raw) && raw.size() >= 11) {
    inRangeOk = true;
    for (size_t offset = 0; offset + 11 <= raw.size(); offset += 11) {
      uint32_t source = readLe32(raw.data() + offset);
      if (source == 0xffffffff) break;
      if (source == expectedSource) {
        emulatorSeen = true;
        emulatorProduct = readLe16(raw.data() + offset + 4);
        emulatorAge = raw[offset + 6];
        emulatorVersion = readLe32(raw.data() + offset + 7);
        break;
      }
    }
  }
  // EC20 source attribution is stronger proof than the model-dependent EC31
  // device-list layout. Use it as a fallback when accepted sense values already
  // name this exact Huckleberry source.
  if (!emulatorSeen) {
    uint8_t acceptedAge = 0xff;
    if (voltageAccepted && voltageSource == expectedSource) acceptedAge = voltageAge;
    if (tempAccepted && tempSource == expectedSource && tempAge < acceptedAge) acceptedAge = tempAge;
    if (currentAccepted && currentSource == expectedSource && currentAge < acceptedAge) acceptedAge = currentAge;
    if (acceptedAge != 0xff) {
      emulatorSeen = true;
      emulatorAge = acceptedAge;
      emulatorProduct = static_cast<uint16_t>(vesmart::PRODUCT_ID);
      emulatorVersion = vesmart::FIRMWARE_RECORD;
    }
  }
  if (copyConnectedVreg(captured, 0xec42, raw) && !raw.empty()) {
    rssi = static_cast<int8_t>(raw[0]);
    rssiOk = true;
  }
  if (copyConnectedVreg(captured, 0xed8d, raw) && raw.size() >= 2) {
    int16_t value = static_cast<int16_t>(readLe16(raw.data()));
    if (value != 0x7fff && value != -1) {
      senseVoltage = value * 0.01f;
      senseVoltageOk = true;
    }
  }
  if (copyConnectedVreg(captured, 0xedec, raw) && raw.size() >= 2) {
    uint16_t value = readLe16(raw.data());
    if (value != 0xffff && value != 0x7fff) {
      senseTempC = value * 0.01f - 273.15f;
      senseTempOk = true;
    }
  }
  if (copyConnectedVreg(captured, 0xed8c, raw) && raw.size() >= 4) {
    int32_t value = static_cast<int32_t>(readLe32(raw.data()));
    if (value != vesmart::CURRENT_NO_DATA) {
      senseCurrentA = value * 0.001f;
      senseCurrentOk = true;
    }
  }
  teleLock();
  s_vsChargerRead = true;
  s_vsChargerIdOk = idOk;
  s_vsChargerId = id;
  s_vsChargerKeyOk = keyOk;
  memcpy(s_vsChargerKey, key, 16);
  strncpy(s_vsChargerName, name, sizeof(s_vsChargerName) - 1);
  s_vsChargerName[sizeof(s_vsChargerName) - 1] = 0;
  s_vsChargerTxVregsOk = txVregsOk;
  s_vsChargerRxVregsOk = rxVregsOk;
  s_vsChargerRssiOk = rssiOk;
  s_vsChargerTxVregs = txVregs;
  s_vsChargerRxVregs = rxVregs;
  s_vsChargerRssi = rssi;
  s_vsChargerInRangeOk = inRangeOk;
  s_vsChargerInRangeCount = inRangeCount;
  s_vsChargerEmulatorSeen = emulatorSeen;
  s_vsChargerEmulatorAge = emulatorAge;
  s_vsChargerEmulatorProduct = emulatorProduct;
  s_vsChargerEmulatorVersion = emulatorVersion;
  s_vsChargerRxStatusOk = rxStatusOk;
  s_vsChargerVoltageAccepted = voltageAccepted;
  s_vsChargerTempAccepted = tempAccepted;
  s_vsChargerCurrentAccepted = currentAccepted;
  s_vsChargerVoltageAge = voltageAge;
  s_vsChargerTempAge = tempAge;
  s_vsChargerCurrentAge = currentAge;
  s_vsChargerVoltageClass = voltageClass;
  s_vsChargerTempClass = tempClass;
  s_vsChargerCurrentClass = currentClass;
  s_vsChargerVoltageSource = voltageSource;
  s_vsChargerTempSource = tempSource;
  s_vsChargerCurrentSource = currentSource;
  s_vsChargerSenseVoltageOk = senseVoltageOk;
  s_vsChargerSenseTempOk = senseTempOk;
  s_vsChargerSenseCurrentOk = senseCurrentOk;
  s_vsChargerSenseVoltage = senseVoltage;
  s_vsChargerSenseTempC = senseTempC;
  s_vsChargerSenseCurrentA = senseCurrentA;
  teleUnlock();
  Serial.printf("[BLE] VE.Smart charger idOk=%d id=0x%04x key=%d name=%s tx=%s%u rx=%s%u range=%s%u emu=%d age=%u product=0x%04x rssi=%s%d acceptV=%d acceptT=%d acceptI=%d senseV=%.2f senseT=%.1f senseI=%.1f\n",
                idOk, id, keyOk, name, txVregsOk ? "" : "?", txVregs,
                rxVregsOk ? "" : "?", rxVregs, inRangeOk ? "" : "?", inRangeCount,
                emulatorSeen, emulatorAge, emulatorProduct, rssiOk ? "" : "?", rssi,
                voltageAccepted, tempAccepted, currentAccepted,
                senseVoltage, senseTempC, senseCurrentA);
}

static bool pollVictronConnected() {
  if (!s_victronCaptureMtx) s_victronCaptureMtx = xSemaphoreCreateMutex();
  if (!parseVictronPin(gSettings.victronPin, s_victronPairingPin)) return false;
  xSemaphoreTake(s_victronCaptureMtx, portMAX_DELAY);
  s_victronTxCredits = 0;
  s_victronCapture.clear();
  s_victronCaptureActive = false;
  xSemaphoreGive(s_victronCaptureMtx);

  NimBLEClient* client = NimBLEDevice::createClient();
  client->setClientCallbacks(&s_victronClientCB, false);
  client->setConnectTimeout(10 * 1000);
  bool connected = false;
  const uint8_t addressTypes[] = {BLE_ADDR_RANDOM, BLE_ADDR_PUBLIC};
  for (uint8_t addressType : addressTypes) {
    NimBLEAddress address(std::string(gSettings.victronMac.c_str()), addressType);
    if (client->connect(address)) {
      connected = true;
      break;
    }
  }
  if (!connected) {
    NimBLEDevice::deleteClient(client);
    return false;
  }

  NimBLERemoteCharacteristic* control = nullptr;
  NimBLERemoteCharacteristic* lastData = nullptr;
  NimBLERemoteCharacteristic* data = nullptr;
  bool subControl = false;
  bool subLast = false;
  bool subData = false;
  size_t liveValues = 0;
  do {
    if (!client->secureConnection(false) || !client->getConnInfo().isEncrypted()) break;
    NimBLERemoteService* service = client->getService(VESMART_SVC);
    if (!service) break;
    control = service->getCharacteristic(VESMART_CONTROL);
    lastData = service->getCharacteristic(VESMART_LAST_DATA);
    data = service->getCharacteristic(VESMART_DATA);
    if (!control || !lastData || !data) break;
    subControl = control->canNotify() && control->subscribe(true, victronNotifyCB);
    subLast = lastData->canNotify() && lastData->subscribe(true, victronNotifyCB);
    subData = data->canNotify() && data->subscribe(true, victronNotifyCB);
    if (!subControl || !subLast || !subData) break;

    const uint8_t chunkConfig[] = {0xfa, 0x80, 0xff};
    const uint8_t receiveCredit[] = {0xf9, 0x80};
    if (!control->writeValue(chunkConfig, sizeof(chunkConfig), false) ||
        !control->writeValue(receiveCredit, sizeof(receiveCredit), false) ||
        !takeVictronTxCredit(3000)) break;

    startConnectedCapture();
    const uint8_t getDevices[] = {0x01};
    bool requestedDevices = lastData->writeValue(getDevices, sizeof(getDevices), false);
    vTaskDelay(pdMS_TO_TICKS(700));
    std::vector<uint8_t> deviceMessage;
    stopConnectedCapture(deviceMessage);
    if (!requestedDevices) break;
    std::vector<VictronDeviceListItem> devices;
    if (!parseVictronDeviceList(deviceMessage, devices)) break;

    uint16_t targetInstance = 0;
    for (const auto& candidate : devices) {
      if (candidate.instance == 0) continue;
      bool hasChild = false;
      for (const auto& possibleChild : devices) {
        if (possibleChild.instance != candidate.instance &&
            possibleChild.parent == candidate.instance) {
          hasChild = true;
          break;
        }
      }
      if (!hasChild) targetInstance = candidate.instance;
    }
    if (targetInstance == 0 || !takeVictronTxCredit(3000)) break;
    std::vector<uint8_t> subscribeRequest = {0x03};
    appendCborUnsigned(subscribeRequest, targetInstance);
    if (!lastData->writeValue(subscribeRequest.data(), subscribeRequest.size(), false)) break;
    vTaskDelay(pdMS_TO_TICKS(200));

    const uint16_t liveRegisters[] = {
      0x0100, 0x010b, 0x010a, 0x0102, 0x0201,
      0xedd5, 0xedd7, 0xedda, 0xedd3, 0xedd2, 0xedd1, 0xedd0,
      0xedec,
      0xedbc, 0xedbb, 0xedb3,
      0xedad, 0xeda9, 0xeda8,
      0x104f
    };
    s_victronUnknownVregs.clear();
    startConnectedCapture();
    bool requestedLive = sendConnectedGetValues(
        lastData, targetInstance, liveRegisters, sizeof(liveRegisters) / sizeof(liveRegisters[0]));
    vTaskDelay(pdMS_TO_TICKS(1800));
    std::vector<uint8_t> liveMessage;
    stopConnectedCapture(liveMessage);
    if (!requestedLive) break;
    liveValues = parseConnectedVictronValues(liveMessage);
    if (liveValues == 0) break;

    // Read the charger's VE.Smart network config so Huckleberry can adopt the
    // exact ID/key it already holds (see readVsNetworkFromCharger).
    readVsNetworkFromCharger(lastData, targetInstance);
    // The charger firmware forces its VE.Smart receiver fully on while a GATT
    // connection is active. For an explicit emulator check, keep Huckleberry's
    // advertiser running concurrently, give the charger a complete sync/V/T
    // rotation, then read the per-source receive table again in that same link.
    if (!s_vsAdvPause && gSettings.vsEnabled) {
      vTaskDelay(pdMS_TO_TICKS(6500));
      readVsNetworkFromCharger(lastData, targetInstance);
    }

    teleLock();
    gTele.solUnknownVregCount =
        (uint8_t)min(s_victronUnknownVregs.size(),
                     sizeof(gTele.solUnknownVregs) / sizeof(gTele.solUnknownVregs[0]));
    for (uint8_t i = 0; i < gTele.solUnknownVregCount; i++) {
      gTele.solUnknownVregs[i] = s_victronUnknownVregs[i];
    }
    teleUnlock();
    if (gTele.solUnknownVregCount) {
      Serial.print("[BLE] Victron unknown VREGs:");
      for (uint8_t i = 0; i < gTele.solUnknownVregCount; i++) {
        Serial.printf(" 0x%04x", gTele.solUnknownVregs[i]);
      }
      Serial.println();
    }

    for (size_t firstDay = 0; firstDay < HUCK_VICTRON_HISTORY_DAYS; firstDay += 16) {
      uint16_t historyRegisters[16];
      size_t historyCount = min((size_t)16, HUCK_VICTRON_HISTORY_DAYS - firstDay);
      for (size_t i = 0; i < historyCount; i++) {
        historyRegisters[i] = (uint16_t)(0x1050 + firstDay + i);
      }
      startConnectedCapture();
      bool requestedHistory = sendConnectedGetValues(
          lastData, targetInstance, historyRegisters, historyCount);
      vTaskDelay(pdMS_TO_TICKS(2200));
      std::vector<uint8_t> historyMessage;
      stopConnectedCapture(historyMessage);
      if (!requestedHistory) break;
      parseConnectedVictronValues(historyMessage);
    }

    size_t validHistoryDays = victronValidDayCount();
    teleLock();
    gTele.solHistoryDays = (uint8_t)min(validHistoryDays, (size_t)UINT8_MAX);
    gTele.solConnectedLastMs = millis();
    gTele.solValid = true;
    teleUnlock();

    // Trend payloads are decoded one reply at a time and merged directly into
    // flash-backed 30-minute bins; native fine-grained arrays never persist in RAM.
    fetchStoredTrends(lastData, control, targetInstance);
  } while (false);

  xSemaphoreTake(s_victronCaptureMtx, portMAX_DELAY);
  s_victronCaptureActive = false;
  std::vector<uint8_t>().swap(s_victronCapture);
  xSemaphoreGive(s_victronCaptureMtx);
  if (subControl) control->unsubscribe();
  if (subLast) lastData->unsubscribe();
  if (subData) data->unsubscribe();
  client->disconnect();
  uint32_t disconnectStart = millis();
  while (client->isConnected() && millis() - disconnectStart < 1000) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  NimBLEDevice::deleteClient(client);
  Serial.printf("[BLE] Victron connected read values=%u heap=%u\n",
                (unsigned)liveValues, (unsigned)ESP.getFreeHeap());
  return liveValues > 0;
}

// ===================================================================
// VE.Smart external-sense emulator (broadcast-only sensor peer)
// ===================================================================
// Advertises EcoWorthy battery voltage, temperature, and current into a VictronConnect-
// created VE.Smart network. It only ever ADVERTISES; it never connects to the
// charger and never writes charging parameters. Broadcasting is gated on fresh,
// encodable battery data and pauses immediately when that data goes stale — the
// Victron no-data sentinels are never transmitted as measurements.
static constexpr uint32_t VS_BATT_FRESH_MS = 90000;   // battery measurement freshness
static constexpr uint32_t VS_SOL_FRESH_MS  = 60000;   // charger-present window (card visibility)
static constexpr uint32_t VS_DWELL_MS      = 1200;    // per-record advertising dwell
static constexpr uint64_t VS_SEQ_BLOCK     = 2048;    // sequence numbers reserved per NVS write
static constexpr int      VS_SYNC_BURST    = 8;       // leading type-1 syncs when a broadcast (re)starts

static SemaphoreHandle_t s_vsMtx = nullptr;
static bool     s_vsBattFresh = false;
static bool     s_vsSolFresh = false;
static bool     s_vsBroadcasting = false;
static float    s_vsSrcV = NAN;
static float    s_vsSrcT = NAN;
static float    s_vsSrcA = NAN;
static uint32_t s_vsAdverts = 0;
static uint8_t  s_vsRecord = 0;   // steady-state rotation: sync,V,sync,T,sync,I
static int      s_vsSyncBurst = 0;        // remaining leading type-1 syncs to emit
static bool     s_vsWasBroadcasting = false;    // to detect a fresh broadcast session

// 48-bit anti-replay sequence, reserved a block at a time so an unexpected
// reboot can never reuse a number that was already (or might have been) sent.
static Preferences s_vsPrefs;
static uint64_t s_vsSeq = 0;
static uint64_t s_vsSeqReservedTo = 0;
static bool     s_vsSeqInit = false;

static void vsReserveSeqBlock() {
  s_vsSeqReservedTo = s_vsSeq + VS_SEQ_BLOCK;
  s_vsPrefs.begin("huckvs", false);
  s_vsPrefs.putULong64("seq", s_vsSeqReservedTo);  // persist the high-water mark only
  s_vsPrefs.end();
}

static uint64_t vsNextSequence() {
  if (!s_vsSeqInit) {
    s_vsPrefs.begin("huckvs", true);
    uint64_t stored = s_vsPrefs.getULong64("seq", 0);
    s_vsPrefs.end();
    s_vsSeq = stored ? stored : 1;   // resume above anything previously reserved; never 0
    vsReserveSeqBlock();
    s_vsSeqInit = true;
  }
  if (s_vsSeq >= s_vsSeqReservedTo) vsReserveSeqBlock();
  return (s_vsSeq++) & 0xFFFFFFFFFFFFULL;
}

static void vsStopAdvertising() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (adv->isAdvertising()) adv->stop();
  if (s_vsMtx) {
    xSemaphoreTake(s_vsMtx, portMAX_DELAY);
    s_vsBroadcasting = false;
    xSemaphoreGive(s_vsMtx);
  }
}

// One advertising step: snapshot telemetry, update freshness for the UI, and
// either broadcast the next rotating record or stop. Returns true if broadcasting.
static bool vsTick() {
  float volts, tempC, amps;
  int tempCount;
  uint32_t battLast, solLast;
  teleLock();
  volts = gTele.battVolts;
  tempC = gTele.battTempsC[0];
  amps = gTele.battAmps;
  tempCount = gTele.battTempCount;
  battLast = gTele.battLastMs;
  solLast = gTele.solLastMs;
  teleUnlock();
  uint32_t now = millis();

  int16_t vRaw = 0;
  int32_t aRaw = 0;
  uint16_t tRaw = 0;
  bool dataFresh = battLast != 0 && (uint32_t)(now - battLast) < VS_BATT_FRESH_MS;
  bool voltageValid = dataFresh && vesmart::encodeVoltage(volts, vRaw);
  bool tempValid = dataFresh && tempCount > 0 && vesmart::encodeTemperature(tempC, tRaw);
  bool currentValid = dataFresh &&
                      vesmart::encodeCurrent(amps, aRaw);
  bool solFresh = solLast != 0 && (uint32_t)(now - solLast) < VS_SOL_FRESH_MS;

  uint8_t key[16];
  bool keyOk = parseHexBytes(gSettings.vsNetKey, key, 16) == 16;
  uint8_t idb[2];
  bool idOk = parseHexBytes(gSettings.vsNetId, idb, 2) == 2;
  uint16_t netId = idOk ? (uint16_t)(idb[0] | (idb[1] << 8)) : 0;
  uint32_t src = gSettings.vsSourceAddr ? gSettings.vsSourceAddr : (uint32_t)ESP.getEfuseMac();

  bool wantBroadcast = gSettings.vsEnabled && keyOk && idOk && voltageValid;

  if (s_vsMtx) {
    xSemaphoreTake(s_vsMtx, portMAX_DELAY);
    s_vsBattFresh = voltageValid;
    s_vsSolFresh = solFresh;
    xSemaphoreGive(s_vsMtx);
  }

  // A fresh broadcast session (enable, or resume after the data went stale) leads with a
  // burst of type-1 syncs: the charger drops every compact V/T record until it has
  // received + CCM-verified a sync from this source (RE of the SmartSolar RX handler
  // FUN_00029322), so we must (re)register the source before V/T can be accepted.
  if (wantBroadcast && !s_vsWasBroadcasting) {
    s_vsSyncBurst = VS_SYNC_BURST;
    s_vsRecord = 0;
  }
  s_vsWasBroadcasting = wantBroadcast;

  if (!wantBroadcast) {
    vsStopAdvertising();
    return false;
  }

  std::vector<uint8_t> manuf;
  uint64_t seq = vsNextSequence();
  bool built = false;
  float usedV = NAN, usedT = NAN, usedA = NAN;
  // After the leading burst, keep the sync frequent. The six-phase rotation
  // sync -> V -> sync -> T -> sync -> I puts a type-1 sync in half of all
  // packets. If BMS current is unavailable, the I phase becomes another sync.
  if (s_vsSyncBurst > 0) {
    s_vsSyncBurst--;
    built = vesmart::buildSequence(key, netId, src, seq, manuf);
  } else {
    uint8_t rec = s_vsRecord;
    s_vsRecord = (uint8_t)((s_vsRecord + 1) % 6);
    switch (rec) {
      case 1:  built = vesmart::buildIdentity(key, netId, src, seq, vRaw, manuf); usedV = volts; break;
      case 3:
        if (tempValid) {
          built = vesmart::buildStatus(key, netId, src, seq, tRaw, manuf);
          usedT = tempC;
        } else {
          built = vesmart::buildSequence(key, netId, src, seq, manuf);
        }
        break;
      case 5:
        if (currentValid) {
          built = vesmart::buildCurrent(key, netId, src, seq, aRaw, manuf);
          usedA = amps;
        } else {
          built = vesmart::buildSequence(key, netId, src, seq, manuf);
        }
        break;
      default: built = vesmart::buildSequence(key, netId, src, seq, manuf); break;
    }
  }
  if (!built) { vsStopAdvertising(); return false; }

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;   // manufacturer element only (fills the 31-byte packet)
  advData.setManufacturerData(manuf.data(), manuf.size());
  adv->setAdvertisementData(advData);
  // Legacy non-connectable/non-discoverable PDU mode is configured once in
  // vsAdvTask; here we only refresh the rotating sensor payload.
  if (!adv->isAdvertising()) adv->start();
  else adv->refreshAdvertisingData();

  if (s_vsMtx) {
    xSemaphoreTake(s_vsMtx, portMAX_DELAY);
    s_vsBroadcasting = true;
    if (!isnan(usedV)) s_vsSrcV = usedV;
    if (!isnan(usedT)) s_vsSrcT = usedT;
    else if (!tempValid) s_vsSrcT = NAN;
    if (!isnan(usedA)) s_vsSrcA = usedA;
    else if (!currentValid) s_vsSrcA = NAN;
    s_vsAdverts++;
    xSemaphoreGive(s_vsMtx);
  }
  return true;
}

// Fill an idle window while rotating sensor broadcasts (or just sleep). Keeps the
// UI freshness flags current every dwell even when the emulator is disabled.
// Continuous advertising task. Real VE.Smart sensors broadcast continuously so a
// receiver can catch the rotating records — especially the type-1 sequence sync
// it needs before it can decrypt the compact voltage/temp/current records. This
// dedicated task rotates one record every VS_DWELL_MS regardless of what the scan
// loop is doing, and is paused only for the connected read (s_vsAdvPause). NimBLE
// serializes the actual radio operations against the scan/connect task.
static void vsAdvTask(void*) {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setMinInterval(400);   // *0.625 ms = 250 ms
  adv->setMaxInterval(800);   // *0.625 ms = 500 ms
  // The SmartSolar radio callback accepts only legacy ADV_NONCONN_IND packets: its
  // first check is `(pduHeader & 0x0f) == 2` before the manufacturer data is queued
  // for FUN_00029322. Connectable ADV_IND (type 0) and scannable ADV_SCAN_IND
  // (type 6) are discarded before source registration or CCM verification.
  adv->setConnectableMode(BLE_GAP_CONN_MODE_NON);
  adv->setDiscoverableMode(BLE_GAP_DISC_MODE_NON);
  adv->enableScanResponse(false);
  for (;;) {
    if (s_vsAdvPause) {
      vsStopAdvertising();
      vTaskDelay(pdMS_TO_TICKS(300));
      continue;
    }
    vsTick();  // rotate + advertise one record, or stop if disabled/stale
    vTaskDelay(pdMS_TO_TICKS(VS_DWELL_MS));
  }
}

void vsStatus(VsStatus& out) {
  if (s_vsMtx) {
    xSemaphoreTake(s_vsMtx, portMAX_DELAY);
    out.battFresh = s_vsBattFresh;
    out.solFresh = s_vsSolFresh;
    out.broadcasting = s_vsBroadcasting;
    out.srcVolts = s_vsSrcV;
    out.srcTempC = s_vsSrcT;
    out.srcAmps = s_vsSrcA;
    out.adverts = s_vsAdverts;
    xSemaphoreGive(s_vsMtx);
  }
  teleLock();  // charger-read fields are guarded by teleLock, not s_vsMtx
  out.chargerRead = s_vsChargerRead;
  out.chargerIdOk = s_vsChargerIdOk;
  out.chargerKeyReadable = s_vsChargerKeyOk;
  out.chargerId = s_vsChargerId;
  strncpy(out.chargerName, s_vsChargerName, sizeof(out.chargerName) - 1);
  out.chargerName[sizeof(out.chargerName) - 1] = 0;
  out.chargerTxVregsReadable = s_vsChargerTxVregsOk;
  out.chargerRxVregsReadable = s_vsChargerRxVregsOk;
  out.chargerRssiReadable = s_vsChargerRssiOk;
  out.chargerTxVregs = s_vsChargerTxVregs;
  out.chargerRxVregs = s_vsChargerRxVregs;
  out.chargerRssi = s_vsChargerRssi;
  out.chargerInRangeReadable = s_vsChargerInRangeOk;
  out.chargerInRangeCount = s_vsChargerInRangeCount;
  out.chargerEmulatorSeen = s_vsChargerEmulatorSeen;
  out.chargerEmulatorAge = s_vsChargerEmulatorAge;
  out.chargerEmulatorProduct = s_vsChargerEmulatorProduct;
  out.chargerEmulatorVersion = s_vsChargerEmulatorVersion;
  out.chargerRxStatusReadable = s_vsChargerRxStatusOk;
  out.chargerVoltageAccepted = s_vsChargerVoltageAccepted;
  out.chargerTempAccepted = s_vsChargerTempAccepted;
  out.chargerCurrentAccepted = s_vsChargerCurrentAccepted;
  out.chargerVoltageAge = s_vsChargerVoltageAge;
  out.chargerTempAge = s_vsChargerTempAge;
  out.chargerCurrentAge = s_vsChargerCurrentAge;
  out.chargerVoltageClass = s_vsChargerVoltageClass;
  out.chargerTempClass = s_vsChargerTempClass;
  out.chargerCurrentClass = s_vsChargerCurrentClass;
  out.chargerVoltageSource = s_vsChargerVoltageSource;
  out.chargerTempSource = s_vsChargerTempSource;
  out.chargerCurrentSource = s_vsChargerCurrentSource;
  out.chargerSenseVoltageReadable = s_vsChargerSenseVoltageOk;
  out.chargerSenseTempReadable = s_vsChargerSenseTempOk;
  out.chargerSenseCurrentReadable = s_vsChargerSenseCurrentOk;
  out.chargerSenseVoltage = s_vsChargerSenseVoltage;
  out.chargerSenseTempC = s_vsChargerSenseTempC;
  out.chargerSenseCurrentA = s_vsChargerSenseCurrentA;
  teleUnlock();
}

void requestChargerNetworkRead() { s_vsReadPending = true; }

// Adopt the charger's VE.Smart network config into settings (id/name always; key
// only when the charger returned one). Idempotent: persists only when something
// changed, so it can run after every connected read without churning NVS. Runs
// on the BLE task after pollVictronConnected so it never races an in-flight read.
static void vsApplyChargerNetwork() {
  teleLock();
  bool read = s_vsChargerRead, idOk = s_vsChargerIdOk, keyOk = s_vsChargerKeyOk;
  uint16_t id = s_vsChargerId;
  uint8_t key[16];
  memcpy(key, s_vsChargerKey, 16);
  char name[33];
  strncpy(name, s_vsChargerName, sizeof(name));
  name[sizeof(name) - 1] = 0;
  teleUnlock();
  if (!read || !idOk) return;  // no network configured on the charger
  char idHex[5];
  snprintf(idHex, sizeof(idHex), "%02x%02x", id & 0xFF, (id >> 8) & 0xFF);
  bool changed = false;
  if (gSettings.vsNetId != idHex) { gSettings.vsNetId = idHex; changed = true; }
  if (name[0] && gSettings.vsNetName != name) { gSettings.vsNetName = name; changed = true; }
  if (keyOk) {
    char keyHex[33];
    for (int i = 0; i < 16; i++) snprintf(keyHex + i * 2, 3, "%02x", key[i]);
    if (gSettings.vsNetKey != keyHex) { gSettings.vsNetKey = keyHex; changed = true; }
  }
  if (gSettings.vsSourceAddr == 0) {
    uint32_t s = (uint32_t)ESP.getEfuseMac();
    gSettings.vsSourceAddr = s ? s : 0xA3A50001u;
    changed = true;
  }
  if (changed) gSettings.save();
}

static void bleTask(void*) {
  NimBLEDevice::init("Huckleberry");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(true, true, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);

  // load Victron key
  s_vkeyOk = parseHexBytes(gSettings.victronKey, s_vkey, 16) == 16;
  if (!gSettings.victronMac.isEmpty()) {
    s_victronAddress = NimBLEAddress(std::string(gSettings.victronMac.c_str()), BLE_ADDR_RANDOM);
    s_victronAddressOk = !s_victronAddress.isNull();
  }
  Serial.printf("[BLE] task start vkeyOk=%d battMac=%s victMac=%s ble=%d\n",
                s_vkeyOk, gSettings.batteryMac.c_str(), gSettings.victronMac.c_str(), gSettings.bleEnabled);

  if (!s_vsMtx) s_vsMtx = xSemaphoreCreateMutex();
  // Dedicated task keeps the sensor broadcast continuous, independent of the
  // scan/connect cadence below.
  xTaskCreatePinnedToCore(vsAdvTask, "vsadv", 4096, nullptr, 1, nullptr, 0);

  NimBLEScan* scan = NimBLEDevice::getScan();
  static ScanCB cb;
  scan->setScanCallbacks(&cb);
  scan->setActiveScan(false);
  scan->setInterval(500);
  scan->setWindow(50);
  scan->setMaxResults(0);

  uint32_t lastBatt = 0;
  uint32_t lastVictronAttempt = 0;
  bool batteryRead = false;
  bool historyRead = false;
  for (;;) {
    if (!gSettings.bleEnabled) {
      if (scan->isScanning()) scan->stop();
      vTaskDelay(pdMS_TO_TICKS(1000));  // vsAdvTask stops advertising when disabled
      continue;
    }

    scan->start(3000, false, true);
    vTaskDelay(pdMS_TO_TICKS(3200));
    if (scan->isScanning()) scan->stop();
    scan->clearResults();

    uint32_t now = millis();
    bool batteryDue = (!batteryRead && now >= 30000) ||
                      (batteryRead && now - lastBatt >= 60 * 1000UL);
    if (batteryDue) {
      lastBatt = now;
      pollBattery();  // brief BMS read; non-connectable advertising coexists with it
      batteryRead = true;
    }

    now = millis();
    // Wait for Wi-Fi to stabilize, retry failures after 30 minutes, and refresh
    // successful connected reads only every six hours.
    bool initialHistoryDue = !historyRead && now >= 3 * 60 * 1000UL &&
                             (lastVictronAttempt == 0 || now - lastVictronAttempt >= 30 * 60 * 1000UL);
    bool trendBootstrapDue = historyRead && !s_trendBootstrapComplete &&
                             now - lastVictronAttempt >= 15 * 60 * 1000UL;
    bool historyRefreshDue = historyRead && s_trendBootstrapComplete &&
                             now - lastVictronAttempt >= 6 * 60 * 60 * 1000UL;
    bool vsReadDue = s_vsReadPending && gNet.staConnected;
    if (gNet.staConnected && (initialHistoryDue || trendBootstrapDue || historyRefreshDue || vsReadDue)) {
      lastVictronAttempt = now;
      s_vsReadPending = false;
      // Normal history work pauses the advertiser to minimize controller load.
      // An explicit VE.Smart check deliberately keeps it running: the charger's
      // receiver is forced on while connected, giving us a deterministic accept test.
      bool concurrentVsCheck = vsReadDue && gSettings.vsEnabled;
      s_vsAdvPause = !concurrentVsCheck;
      if (s_vsAdvPause) vTaskDelay(pdMS_TO_TICKS(400));
      historyRead = pollVictronConnected() || historyRead;
      s_vsAdvPause = false;
      // Every connected read also refreshes the charger's VE.Smart network; adopt
      // it automatically in the background so no manual key entry is needed.
      vsApplyChargerNetwork();
    }

    vTaskDelay(pdMS_TO_TICKS(9000));  // sensor broadcast runs in vsAdvTask
  }
}

void begin() {
  xTaskCreatePinnedToCore(bleTask, "ble", 8192, nullptr, 1, nullptr, 0);
}

} // namespace ble
