#include "AppState.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

Telemetry gTele;
NetStatus gNet;
volatile bool gUiApplyRequested = false;
volatile bool gBgReloadRequested = false;

static SemaphoreHandle_t s_mtx = nullptr;

void teleLock() {
  if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
  xSemaphoreTake(s_mtx, portMAX_DELAY);
}
void teleUnlock() {
  if (s_mtx) xSemaphoreGive(s_mtx);
}
