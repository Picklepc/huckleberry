#pragma once
// NimBLE task: passively decodes the Victron SmartSolar "Instant Readout"
// advertisements (AES-CTR, ported from picklepc/mervyns-esp32-offgrid-manager)
// and polls the Eco-Worthy JBD/FF00 BMS for SOC/volts/amps.
#include <Arduino.h>
namespace ble {
void begin();   // starts the background BLE task
}
