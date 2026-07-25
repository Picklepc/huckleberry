# Changelog

## Unreleased
- Web app overhaul: split the current single-page app into Dashboard, Power,
  Settings, Networking, BLE Manager, and Firmware sections. See
  `docs/WEB_APP_OVERHAUL.md`.
- Display presets: save/restore seasonal clock, climate, power, and status page
  layouts/themes/backgrounds.

## v0.2.0 - Official M2 field release (2026-07-25)
- Multi-STA network memory: save multiple Wi-Fi networks (home + campsites);
  auto-join a known network in range; add/remove from the web settings page;
  migrates the previous single-SSID setting. `Huckleberry` AP stays up off-grid.
- Comprehensive settings: configurable night window + night brightness + auto-night
  toggle, per-mode brightness, return-to-clock timeout, scheduled display-off
  (wake on touch), and storing protection limits - all in web Settings.
- Background display system: SPIFFS JPEG backgrounds, background upload/selection,
  per-page background/theme/data-box settings, and position/scale controls for
  Clock / Climate / Power / Status.
- Day clock cleanup: removed flower blooms and instructional text, added heart
  glyph font support, restored the day clock after layout edits, and added
  Christmas/holiday background support with frameless darker text.
- Power page cleanup: compact on-device SOC/solar/net-power view that stays
  inside the background image, plus web API exposure for EcoWorthy battery stats
  and Victron instant-readout data.
- EcoWorthy/JBD battery telemetry: SOC, voltage, current, watts, remaining and
  nominal Ah, cycles, status, FET/protection/software flags, temperature sensors,
  cell count, and per-cell millivolts.
- Victron telemetry: encrypted Instant Readout decode, PV watts, charger state,
  battery V/A, load A, yield today, RSSI, observed PV max, and percent of max.
- BLE robustness: 90% scan duty for weak adverts, battery connect retries,
  case-insensitive MAC match, rate-limited health logging, and retry/settle logic
  so EcoWorthy basic stats and cell-voltage frames populate together after OTA.
- OTA firmware path: `huckleberry_ota` environment targets `huckleberry.local`
  with password `huckleberry`; verified on trailer Wi-Fi after USB flash.

## v0.2.0-b1 - Connectivity & live telemetry (M1)
- Wi-Fi STA + always-on `Huckleberry` AP, captive DNS, mDNS.
- Web app: dashboard (power/climate/status) + settings; browser + NTP time sync.
- BLE task: Eco-Worthy battery (JBD FF00) live; Victron MPPT Instant Readout
  decoded on-device (mbedtls AES-CTR, ported from Mervyns).
- Live telemetry wired into on-device Power/Status tiles.
- Clock: narrowed the hour-tens "1" digit; AM/PM moved beside the minutes.
- Settings persisted to NVS; device secrets moved to gitignored `secrets.local.h`.

## v0.1.0-a1 - Bring-up & clock (M0)
- LovyanGFX driver for ZX2D80CE02S (ST7789 8080 8-bit + FT5x06), verified.
- Seven-segment LED clock; auto day/night (night = black/white, home only);
  1-minute return-to-clock; tap to thermostat; swipe Clock/Climate/Power/Status.
- Stock 8 MB firmware backed up.
