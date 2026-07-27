#pragma once
// Template for device secrets. Copy to include/secrets.local.h (gitignored) and
// fill in your own values, OR leave blank and set them from the web Settings page.
//
// Victron "Instant Readout" encryption key: VictronConnect -> gear/Settings ->
// Product info -> "Instant readout via Bluetooth" -> show encryption data.
#define HUCK_VICTRON_MAC ""   // e.g. "aa:bb:cc:dd:ee:ff" (random-static BLE address)
#define HUCK_VICTRON_KEY ""   // 32 hex chars (16 bytes)
#define HUCK_VICTRON_PIN ""   // 6 digits from the device label (BLE pairing)
#define HUCK_BATTERY_MAC ""   // JBD/Xiaoxiang FF00 pack MAC
