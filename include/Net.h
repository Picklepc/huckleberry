#pragma once
#include <Arduino.h>
namespace net {
void begin();
void loop();
// Set the RTC from a Unix epoch pushed by a browser (off-grid time source).
void setTimeFromEpoch(uint32_t epoch, const char* source);
bool timeIsValid();
}
