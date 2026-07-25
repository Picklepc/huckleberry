#include "BleManager.h"
#include "AppState.h"
#include "Settings.h"

#include <NimBLEDevice.h>
#include <mbedtls/aes.h>
#include <vector>

namespace ble {

static constexpr uint16_t VICTRON_CID = 0x02E1;

// JBD FF00 UUIDs
static NimBLEUUID JBD_SVC((uint16_t)0xFF00);
static NimBLEUUID JBD_NOTIFY((uint16_t)0xFF01);
static NimBLEUUID JBD_WRITE((uint16_t)0xFF02);

static uint8_t   s_vkey[16];
static bool      s_vkeyOk = false;

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
static bool decryptVictron(const uint8_t* rec, size_t len, std::vector<uint8_t>& out) {
  if (len < 4 || !s_vkeyOk) return false;
  if (rec[0] != 0x01 || rec[3] != s_vkey[0]) return false;   // solar charger + key check
  uint16_t nonce = (uint16_t)rec[1] | ((uint16_t)rec[2] << 8);
  out.resize(len - 4);
  uint8_t nc[16] = {0}, sb[16] = {0};
  size_t off = 0;
  nc[0] = nonce & 0xFF; nc[1] = (nonce >> 8) & 0xFF;
  mbedtls_aes_context aes; mbedtls_aes_init(&aes);
  if (mbedtls_aes_setkey_enc(&aes, s_vkey, 128) != 0) { mbedtls_aes_free(&aes); return false; }
  int rc = mbedtls_aes_crypt_ctr(&aes, out.size(), &off, nc, sb, rec + 4, out.data());
  mbedtls_aes_free(&aes);
  return rc == 0;
}

static void parseVictron(const std::vector<uint8_t>& p, int rssi) {
  if (p.size() < 12) return;
  uint8_t state = bitsLE(p.data(), p.size(), 0, 8);
  uint8_t err   = bitsLE(p.data(), p.size(), 8, 8);
  int16_t bv = (int16_t)signExt(bitsLE(p.data(), p.size(), 16, 16), 16);
  int16_t bc = (int16_t)signExt(bitsLE(p.data(), p.size(), 32, 16), 16);
  uint16_t yd = bitsLE(p.data(), p.size(), 48, 16);
  uint16_t pv = bitsLE(p.data(), p.size(), 64, 16);
  uint16_t ld = bitsLE(p.data(), p.size(), 80, 9);
  teleLock();
  gTele.solState = state; gTele.solError = err;
  gTele.solBattV = (bv == 0x7FFF) ? NAN : bv * 0.01f;
  gTele.solBattA = (bc == 0x7FFF) ? NAN : bc * 0.1f;
  gTele.solYieldKwh = (yd == 0xFFFF) ? NAN : yd * 0.01f;
  gTele.solPvW  = (pv == 0xFFFF) ? NAN : (float)pv;
  gTele.solLoadA = (ld == 0x1FF) ? NAN : ld * 0.1f;
  gTele.solRssi = rssi; gTele.solValid = true; gTele.solLastMs = millis();
  teleUnlock();
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
  std::vector<uint8_t> payload;
  if (decryptVictron(raw + 6, m.size() - 6, payload))
    parseVictron(payload, dev->getRSSI());
}

// ---- scan callbacks ----
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (dev->getAddress().toString() == std::string(gSettings.victronMac.c_str()))
      handleVictronAdv(dev);
  }
};

// ---- JBD battery notification assembly ----
static std::vector<uint8_t> s_jbdBuf;
static volatile bool s_jbdFrameReady = false;

static void jbdNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  for (size_t i = 0; i < len; i++) s_jbdBuf.push_back(data[i]);
  if (s_jbdBuf.size() >= 4 && s_jbdBuf.front() == 0xDD && s_jbdBuf.back() == 0x77)
    s_jbdFrameReady = true;
}

static void parseJbdBasic(const std::vector<uint8_t>& d) {
  if (d.size() < 4 || d[0] != 0xDD || d[1] != 0x03) return;
  int ln = d[3];
  if ((int)d.size() < 4 + ln) return;
  const uint8_t* p = d.data() + 4;
  float volt = ((p[0] << 8) | p[1]) / 100.0f;
  int16_t rawCur = (int16_t)((p[2] << 8) | p[3]);
  float cur = rawCur / 100.0f;
  float resid = ((p[4] << 8) | p[5]) / 100.0f;
  float nom = ((p[6] << 8) | p[7]) / 100.0f;
  int cycles = (p[8] << 8) | p[9];
  int soc = (ln > 19) ? p[19] : -1;
  teleLock();
  gTele.battVolts = volt; gTele.battAmps = cur; gTele.battSoc = soc;
  gTele.battResidAh = resid; gTele.battNomAh = nom; gTele.battCycles = cycles;
  gTele.battValid = true; gTele.battLastMs = millis();
  teleUnlock();
}

static void pollBattery() {
  if (gSettings.batteryMac.isEmpty()) return;
  NimBLEClient* c = NimBLEDevice::createClient();
  c->setConnectTimeout(6 * 1000);
  bool ok = c->connect(NimBLEAddress(std::string(gSettings.batteryMac.c_str()), BLE_ADDR_PUBLIC));
  if (!ok) { NimBLEDevice::deleteClient(c); return; }
  NimBLERemoteService* svc = c->getService(JBD_SVC);
  if (svc) {
    NimBLERemoteCharacteristic* notify = svc->getCharacteristic(JBD_NOTIFY);
    NimBLERemoteCharacteristic* write = svc->getCharacteristic(JBD_WRITE);
    if (notify && write && notify->canNotify()) {
      s_jbdBuf.clear(); s_jbdFrameReady = false;
      notify->subscribe(true, jbdNotify);
      const uint8_t req[] = {0xDD, 0xA5, 0x03, 0x00, 0xFF, 0xFD, 0x77};
      write->writeValue((uint8_t*)req, sizeof(req), false);
      uint32_t t0 = millis();
      while (!s_jbdFrameReady && millis() - t0 < 3000) vTaskDelay(pdMS_TO_TICKS(20));
      if (s_jbdFrameReady) parseJbdBasic(s_jbdBuf);
    }
  }
  c->disconnect();
  NimBLEDevice::deleteClient(c);
}

static void bleTask(void*) {
  NimBLEDevice::init("Huckleberry");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  // load Victron key
  s_vkeyOk = parseHexBytes(gSettings.victronKey, s_vkey, 16) == 16;

  NimBLEScan* scan = NimBLEDevice::getScan();
  static ScanCB cb;
  scan->setScanCallbacks(&cb);
  scan->setActiveScan(true);
  scan->setInterval(80);
  scan->setWindow(60);

  uint32_t lastBatt = 0;
  for (;;) {
    if (!gSettings.bleEnabled) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
    // Scan window for Victron advertisements.
    if (!scan->isScanning()) scan->start(0, false);
    vTaskDelay(pdMS_TO_TICKS(6000));

    // Periodically poll the battery (pauses scan while connected).
    if (millis() - lastBatt > 12000) {
      lastBatt = millis();
      if (scan->isScanning()) scan->stop();
      pollBattery();
    }
  }
}

void begin() {
  xTaskCreatePinnedToCore(bleTask, "ble", 8192, nullptr, 1, nullptr, 0);
}

} // namespace ble
