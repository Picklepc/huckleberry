# Changelog

## Unreleased (M2)
- Multi-STA network memory: save multiple Wi-Fi networks (home + campsites);
  auto-join a known network in range; add/remove from the web settings page;
  migrates the previous single-SSID setting. `Huckleberry` AP stays up off-grid.
- Comprehensive settings: configurable night window + night brightness + auto-night
  toggle, per-mode brightness, return-to-clock timeout, scheduled display-off
  (wake on touch), and storing protection limits — all in web Settings.
- BLE robustness: 90% scan duty for weak adverts, battery connect retries,
  case-insensitive MAC match, rate-limited health logging.
- OTA firmware updates: browser uploader at `/update` + PlatformIO-over-Wi-Fi
  (`pio run -e huckleberry_ota -t upload`, ArduinoOTA). Firmware version in
  `/api/state`. Enables remote development without USB.
- Docs: DESIGN.md (UI/UX goals; night clock locked; day-mode redesign; layout
  system), HARDWARE.md (GPIO/debug pinout, 5V power input, USB-host camera
  provision), HANDOFF.md (resume-on-another-device prompt).

## v0.2.0-b1 — Connectivity & live telemetry (M1)
- Wi-Fi STA + always-on `Huckleberry` AP, captive DNS, mDNS.
- Web app: dashboard (power/climate/status) + settings; browser + NTP time sync.
- BLE task: Eco-Worthy battery (JBD FF00) live; Victron MPPT Instant Readout
  decoded on-device (mbedtls AES-CTR, ported from Mervyns).
- Live telemetry wired into on-device Power/Status tiles.
- Clock: narrowed the hour-tens "1" digit; AM/PM moved beside the minutes.
- Settings persisted to NVS; device secrets moved to gitignored `secrets.local.h`.

## v0.1.0-a1 — Bring-up & clock (M0)
- LovyanGFX driver for ZX2D80CE02S (ST7789 8080 8-bit + FT5x06), verified.
- Seven-segment LED clock; auto day/night (night = black/white, home only);
  1-minute return-to-clock; tap→thermostat; swipe Clock/Climate/Power/Status.
- Stock 8 MB firmware backed up.
