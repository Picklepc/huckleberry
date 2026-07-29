#pragma once

#include <Arduino.h>

static constexpr size_t HUCK_VICTRON_INTRADAY_SLOTS = 48;
static constexpr size_t HUCK_VICTRON_TREND_SERIES = 6;

enum VictronTrendSeries : uint8_t {
  VICTRON_TREND_OUTPUT_CURRENT = 0,
  VICTRON_TREND_PV_VOLTAGE = 1,
  VICTRON_TREND_PV_POWER = 2,
  VICTRON_TREND_BATTERY_TEMP = 3,
  VICTRON_TREND_BATTERY_VOLTAGE = 4,
  VICTRON_TREND_CHARGE_CURRENT = 5,
};

struct VictronTrendBin {
  bool valid = false;
  uint32_t timestampUtc = 0;
  float value = NAN;
};

struct VictronIntradaySample {
  uint32_t timestampUtc = 0;
  uint8_t validMask = 0;
  float outputCurrentA = NAN;
  float pvVoltageV = NAN;
  float pvPowerW = NAN;
  float batteryTempC = NAN;
  float batteryVoltageV = NAN;
  float chargeCurrentA = NAN;
};

struct VictronIntradayDay {
  uint32_t dateKey = 0;
  VictronIntradaySample samples[HUCK_VICTRON_INTRADAY_SLOTS];
};

bool victronTrendsBegin();
bool victronTrendMergeSeriesDay(uint32_t dateKey, uint8_t series,
                                const VictronTrendBin bins[HUCK_VICTRON_INTRADAY_SLOTS]);
bool victronTrendReadDayByAge(uint8_t ageDays, VictronIntradayDay& day);
bool victronTrendHasDayByAge(uint8_t ageDays);
uint32_t victronTrendAvailableAgeMask();
bool victronTrendSeriesHasData(uint8_t series);
bool victronTrendSeriesHasDayByAge(uint8_t series, uint8_t ageDays);
uint32_t victronTrendBackfillCursor(uint8_t series);
void victronTrendSetBackfillCursor(uint8_t series, uint32_t timeRef);
