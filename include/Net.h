#pragma once
#include <Arduino.h>
namespace net {
void begin();
void loop();
void reconnect();   // restart the saved-network join sequence
// Set the RTC from a Unix epoch pushed by a browser (off-grid time source).
void setTimeFromEpoch(uint32_t epoch, const char* source);
bool timeIsValid();
}
