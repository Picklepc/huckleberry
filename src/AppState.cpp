#include "AppState.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

Telemetry gTele;
NetStatus gNet;
volatile bool gUiApplyRequested = false;
volatile bool gBgReloadRequested = false;

static SemaphoreHandle_t s_mtx = nullptr;
static VictronDay s_victronDays[HUCK_VICTRON_HISTORY_DAYS];

static void ensureMutex() {
  if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
}

void teleLock() {
  ensureMutex();
  xSemaphoreTake(s_mtx, portMAX_DELAY);
}
void teleUnlock() {
  if (s_mtx) xSemaphoreGive(s_mtx);
}

void victronDayStore(size_t ageDays, const VictronDay& day) {
  if (ageDays >= HUCK_VICTRON_HISTORY_DAYS) return;
  ensureMutex();
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  s_victronDays[ageDays] = day;
  s_victronDays[ageDays].ageDays = (uint8_t)ageDays;
  xSemaphoreGive(s_mtx);
}

bool victronDayCopy(size_t ageDays, VictronDay& out) {
  if (ageDays >= HUCK_VICTRON_HISTORY_DAYS) return false;
  ensureMutex();
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  out = s_victronDays[ageDays];
  xSemaphoreGive(s_mtx);
  return true;
}

size_t victronValidDayCount() {
  ensureMutex();
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  size_t count = 0;
  for (const VictronDay& day : s_victronDays) {
    if (day.valid) count++;
  }
  xSemaphoreGive(s_mtx);
  return count;
}

float victronHistoryPeakPowerW() {
  ensureMutex();
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  float peakPowerW = NAN;
  for (const VictronDay& day : s_victronDays) {
    if (day.valid && !isnan(day.peakPowerW) &&
        (isnan(peakPowerW) || day.peakPowerW > peakPowerW)) {
      peakPowerW = day.peakPowerW;
    }
  }
  xSemaphoreGive(s_mtx);
  return peakPowerW;
}
