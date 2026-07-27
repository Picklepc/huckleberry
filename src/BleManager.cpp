#include "BleManager.h"
#include "AppState.h"
#include "Settings.h"

#include <NimBLEDevice.h>
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
static uint32_t s_victronPairingPin = 0;
static volatile uint16_t s_victronTxCredits = 0;
static volatile bool s_victronCaptureActive = false;
// VREGs the charger rejected as unknown (CBOR opcode 9) during a read — used to
// confirm which candidate registers this model actually serves.
static std::vector<uint16_t> s_victronUnknownVregs;

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
      vTaskDelay(pdMS_TO_TICKS(1000));
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
      pollBattery();
      batteryRead = true;
    }

    now = millis();
    // Extended (connected) data refreshes every 15 minutes once established;
    // the first read waits until ~3 min after boot with a 15-min retry.
    bool initialHistoryDue = !historyRead && now >= 3 * 60 * 1000UL &&
                             (lastVictronAttempt == 0 || now - lastVictronAttempt >= 15 * 60 * 1000UL);
    bool historyRefreshDue = historyRead && now - lastVictronAttempt >= 15 * 60 * 1000UL;
    if (gNet.staConnected && (initialHistoryDue || historyRefreshDue)) {
      lastVictronAttempt = now;
      historyRead = pollVictronConnected() || historyRead;
    }

    vTaskDelay(pdMS_TO_TICKS(9000));
  }
}

void begin() {
  xTaskCreatePinnedToCore(bleTask, "ble", 8192, nullptr, 1, nullptr, 0);
}

} // namespace ble
