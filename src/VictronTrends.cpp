#include "VictronTrends.h"

#include "AppState.h"

#include <SPIFFS.h>
#include <algorithm>
#include <cstring>
#include <limits.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {

static constexpr char STORE_PATH[] = "/victron-trends.bin";
static constexpr uint32_t STORE_MAGIC = 0x31525456;
static constexpr uint16_t STORE_VERSION = 1;

#pragma pack(push, 1)
struct StoreHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t dayCount;
  uint32_t backfillCursor[HUCK_VICTRON_TREND_SERIES];
};

struct StoredSample {
  uint32_t timestampUtc;
  uint8_t validMask;
  int16_t outputCurrentDeciA;
  uint16_t pvVoltageCentiV;
  uint16_t pvPowerW;
  int16_t batteryTempDeciC;
  uint16_t batteryVoltageCentiV;
  int16_t chargeCurrentDeciA;
};

struct StoredDay {
  uint32_t dateKey;
  StoredSample samples[HUCK_VICTRON_INTRADAY_SLOTS];
};
#pragma pack(pop)

static SemaphoreHandle_t s_mutex = nullptr;
static bool s_ready = false;

static size_t expectedStoreSize() {
  return sizeof(StoreHeader) + HUCK_VICTRON_HISTORY_DAYS * sizeof(StoredDay);
}

static void ensureMutex() {
  if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
}

static bool initializeStoreLocked() {
  SPIFFS.remove(STORE_PATH);
  File file = SPIFFS.open(STORE_PATH, FILE_WRITE);
  if (!file) return false;
  StoreHeader header = {};
  header.magic = STORE_MAGIC;
  header.version = STORE_VERSION;
  header.dayCount = HUCK_VICTRON_HISTORY_DAYS;
  if (file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
    file.close();
    return false;
  }
  StoredDay emptyDay = {};
  for (size_t i = 0; i < HUCK_VICTRON_HISTORY_DAYS; i++) {
    if (file.write(reinterpret_cast<const uint8_t*>(&emptyDay), sizeof(emptyDay)) != sizeof(emptyDay)) {
      file.close();
      return false;
    }
  }
  file.close();
  return true;
}

static bool ensureStoreLocked() {
  if (s_ready) return true;
  File file = SPIFFS.open(STORE_PATH, FILE_READ);
  StoreHeader header = {};
  bool valid = file && file.size() == expectedStoreSize() &&
               file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
               header.magic == STORE_MAGIC && header.version == STORE_VERSION &&
               header.dayCount == HUCK_VICTRON_HISTORY_DAYS;
  if (file) file.close();
  if (!valid && !initializeStoreLocked()) return false;
  s_ready = true;
  return true;
}

static size_t dayOffset(size_t index) {
  return sizeof(StoreHeader) + index * sizeof(StoredDay);
}

static bool readDayAt(File& file, size_t index, StoredDay& day) {
  return file.seek(dayOffset(index), SeekSet) &&
         file.read(reinterpret_cast<uint8_t*>(&day), sizeof(day)) == sizeof(day);
}

static bool writeDayAt(File& file, size_t index, const StoredDay& day) {
  return file.seek(dayOffset(index), SeekSet) &&
         file.write(reinterpret_cast<const uint8_t*>(&day), sizeof(day)) == sizeof(day);
}

static bool findDayLocked(File& file, uint32_t dateKey, size_t& index, StoredDay& day,
                          bool create) {
  size_t emptyIndex = HUCK_VICTRON_HISTORY_DAYS;
  size_t oldestIndex = 0;
  uint32_t oldestDate = UINT32_MAX;
  StoredDay candidate = {};
  for (size_t i = 0; i < HUCK_VICTRON_HISTORY_DAYS; i++) {
    if (!readDayAt(file, i, candidate)) return false;
    if (candidate.dateKey == dateKey) {
      index = i;
      day = candidate;
      return true;
    }
    if (candidate.dateKey == 0 && emptyIndex == HUCK_VICTRON_HISTORY_DAYS) emptyIndex = i;
    if (candidate.dateKey && candidate.dateKey < oldestDate) {
      oldestDate = candidate.dateKey;
      oldestIndex = i;
    }
  }
  if (!create) return false;
  index = emptyIndex < HUCK_VICTRON_HISTORY_DAYS ? emptyIndex : oldestIndex;
  day = {};
  day.dateKey = dateKey;
  return true;
}

static uint32_t dateKeyForAge(uint8_t ageDays) {
  time_t now = time(nullptr);
  if (now < 1600000000) return 0;
  now -= static_cast<time_t>(ageDays) * 24 * 60 * 60;
  struct tm local = {};
  localtime_r(&now, &local);
  return static_cast<uint32_t>(local.tm_year + 1900) * 10000UL +
         static_cast<uint32_t>(local.tm_mon + 1) * 100UL + local.tm_mday;
}

static int16_t signedScaled(float value, float scale) {
  float scaled = value * scale;
  if (scaled > INT16_MAX) scaled = INT16_MAX;
  if (scaled < INT16_MIN) scaled = INT16_MIN;
  return static_cast<int16_t>(lroundf(scaled));
}

static uint16_t unsignedScaled(float value, float scale) {
  float scaled = value * scale;
  if (scaled > UINT16_MAX) scaled = UINT16_MAX;
  if (scaled < 0) scaled = 0;
  return static_cast<uint16_t>(lroundf(scaled));
}

static void storeValue(StoredSample& sample, uint8_t series, float value) {
  sample.validMask |= static_cast<uint8_t>(1U << series);
  switch (series) {
    case VICTRON_TREND_OUTPUT_CURRENT: sample.outputCurrentDeciA = signedScaled(value, 10.0f); break;
    case VICTRON_TREND_PV_VOLTAGE: sample.pvVoltageCentiV = unsignedScaled(value, 100.0f); break;
    case VICTRON_TREND_PV_POWER: sample.pvPowerW = unsignedScaled(value, 1.0f); break;
    case VICTRON_TREND_BATTERY_TEMP: sample.batteryTempDeciC = signedScaled(value, 10.0f); break;
    case VICTRON_TREND_BATTERY_VOLTAGE: sample.batteryVoltageCentiV = unsignedScaled(value, 100.0f); break;
    case VICTRON_TREND_CHARGE_CURRENT: sample.chargeCurrentDeciA = signedScaled(value, 10.0f); break;
  }
}

static bool fallbackDateKeyLocked(File& file, uint8_t ageDays, uint32_t& dateKey) {
  uint32_t keys[HUCK_VICTRON_HISTORY_DAYS] = {};
  size_t count = 0;
  StoredDay day = {};
  for (size_t i = 0; i < HUCK_VICTRON_HISTORY_DAYS; i++) {
    if (!readDayAt(file, i, day)) return false;
    if (day.dateKey) keys[count++] = day.dateKey;
  }
  if (ageDays >= count) return false;
  std::sort(keys, keys + count, std::greater<uint32_t>());
  dateKey = keys[ageDays];
  return true;
}

}  // namespace

bool victronTrendsBegin() {
  ensureMutex();
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  bool ready = ensureStoreLocked();
  xSemaphoreGive(s_mutex);
  return ready;
}

bool victronTrendMergeSeriesDay(uint32_t dateKey, uint8_t series,
                                const VictronTrendBin bins[HUCK_VICTRON_INTRADAY_SLOTS]) {
  if (!dateKey || series >= HUCK_VICTRON_TREND_SERIES || !bins) return false;
  ensureMutex();
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  bool success = false;
  if (ensureStoreLocked()) {
    File file = SPIFFS.open(STORE_PATH, "r+");
    if (file) {
      size_t index = 0;
      StoredDay day = {};
      if (findDayLocked(file, dateKey, index, day, true)) {
        for (size_t slot = 0; slot < HUCK_VICTRON_INTRADAY_SLOTS; slot++) {
          if (!bins[slot].valid || isnan(bins[slot].value)) continue;
          day.samples[slot].timestampUtc = bins[slot].timestampUtc;
          storeValue(day.samples[slot], series, bins[slot].value);
        }
        success = writeDayAt(file, index, day);
      }
      file.close();
    }
  }
  xSemaphoreGive(s_mutex);
  return success;
}

bool victronTrendReadDayByAge(uint8_t ageDays, VictronIntradayDay& output) {
  output = {};
  ensureMutex();
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  bool success = false;
  if (ensureStoreLocked()) {
    File file = SPIFFS.open(STORE_PATH, FILE_READ);
    if (file) {
      uint32_t dateKey = dateKeyForAge(ageDays);
      if (!dateKey) fallbackDateKeyLocked(file, ageDays, dateKey);
      size_t index = 0;
      StoredDay day = {};
      if (dateKey && findDayLocked(file, dateKey, index, day, false)) {
        output.dateKey = day.dateKey;
        for (size_t slot = 0; slot < HUCK_VICTRON_INTRADAY_SLOTS; slot++) {
          const StoredSample& stored = day.samples[slot];
          VictronIntradaySample& sample = output.samples[slot];
          sample.timestampUtc = stored.timestampUtc;
          sample.validMask = stored.validMask;
          if (stored.validMask & (1U << VICTRON_TREND_OUTPUT_CURRENT)) sample.outputCurrentA = stored.outputCurrentDeciA * 0.1f;
          if (stored.validMask & (1U << VICTRON_TREND_PV_VOLTAGE)) sample.pvVoltageV = stored.pvVoltageCentiV * 0.01f;
          if (stored.validMask & (1U << VICTRON_TREND_PV_POWER)) sample.pvPowerW = stored.pvPowerW;
          if (stored.validMask & (1U << VICTRON_TREND_BATTERY_TEMP)) sample.batteryTempC = stored.batteryTempDeciC * 0.1f;
          if (stored.validMask & (1U << VICTRON_TREND_BATTERY_VOLTAGE)) sample.batteryVoltageV = stored.batteryVoltageCentiV * 0.01f;
          if (stored.validMask & (1U << VICTRON_TREND_CHARGE_CURRENT)) sample.chargeCurrentA = stored.chargeCurrentDeciA * 0.1f;
        }
        success = true;
      }
      file.close();
    }
  }
  xSemaphoreGive(s_mutex);
  return success;
}

bool victronTrendHasDayByAge(uint8_t ageDays) {
  VictronIntradayDay day;
  if (!victronTrendReadDayByAge(ageDays, day)) return false;
  for (const VictronIntradaySample& sample : day.samples) {
    if (sample.validMask) return true;
  }
  return false;
}

uint32_t victronTrendAvailableAgeMask() {
  ensureMutex();
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  uint32_t mask = 0;
  if (ensureStoreLocked()) {
    File file = SPIFFS.open(STORE_PATH, FILE_READ);
    uint32_t keys[HUCK_VICTRON_HISTORY_DAYS] = {};
    size_t count = 0;
    StoredDay day = {};
    if (file) {
      for (size_t index = 0; index < HUCK_VICTRON_HISTORY_DAYS; index++) {
        if (!readDayAt(file, index, day)) break;
        bool hasData = false;
        for (const StoredSample& sample : day.samples) {
          if (sample.validMask) {
            hasData = true;
            break;
          }
        }
        if (day.dateKey && hasData) keys[count++] = day.dateKey;
      }
      file.close();
    }
    if (dateKeyForAge(0)) {
      for (uint8_t age = 0; age < HUCK_VICTRON_HISTORY_DAYS; age++) {
        uint32_t expected = dateKeyForAge(age);
        for (size_t index = 0; index < count; index++) {
          if (keys[index] == expected) {
            mask |= 1UL << age;
            break;
          }
        }
      }
    } else {
      std::sort(keys, keys + count, std::greater<uint32_t>());
      for (size_t age = 0; age < count; age++) mask |= 1UL << age;
    }
  }
  xSemaphoreGive(s_mutex);
  return mask;
}

bool victronTrendSeriesHasData(uint8_t series) {
  if (series >= HUCK_VICTRON_TREND_SERIES) return false;
  ensureMutex();
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  bool found = false;
  if (ensureStoreLocked()) {
    File file = SPIFFS.open(STORE_PATH, FILE_READ);
    StoredDay day = {};
    if (file) {
      for (size_t index = 0; index < HUCK_VICTRON_HISTORY_DAYS && !found; index++) {
        if (!readDayAt(file, index, day)) break;
        for (const StoredSample& sample : day.samples) {
          if (sample.validMask & (1U << series)) {
            found = true;
            break;
          }
        }
      }
      file.close();
    }
  }
  xSemaphoreGive(s_mutex);
  return found;
}

bool victronTrendSeriesHasDayByAge(uint8_t series, uint8_t ageDays) {
  if (series >= HUCK_VICTRON_TREND_SERIES) return false;
  VictronIntradayDay day;
  if (!victronTrendReadDayByAge(ageDays, day)) return false;
  for (const VictronIntradaySample& sample : day.samples) {
    if (sample.validMask & (1U << series)) return true;
  }
  return false;
}

uint32_t victronTrendBackfillCursor(uint8_t series) {
  if (series >= HUCK_VICTRON_TREND_SERIES) return 0;
  ensureMutex();
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  uint32_t cursor = 0;
  if (ensureStoreLocked()) {
    File file = SPIFFS.open(STORE_PATH, FILE_READ);
    StoreHeader header = {};
    if (file && file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header)) {
      cursor = header.backfillCursor[series];
    }
    if (file) file.close();
  }
  xSemaphoreGive(s_mutex);
  return cursor;
}

void victronTrendSetBackfillCursor(uint8_t series, uint32_t timeRef) {
  if (series >= HUCK_VICTRON_TREND_SERIES) return;
  ensureMutex();
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  if (ensureStoreLocked()) {
    File file = SPIFFS.open(STORE_PATH, "r+");
    StoreHeader header = {};
    if (file && file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header)) {
      header.backfillCursor[series] = timeRef;
      file.seek(0, SeekSet);
      file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
    }
    if (file) file.close();
  }
  xSemaphoreGive(s_mutex);
}
