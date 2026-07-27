# Changelog

## Unreleased

## v0.5.0 - Normalized gauges
- **Combined SOC gauge**: the first round gauge now shows battery **percentage**
  (real SOC from the JBD BMS) as the main value, with **voltage as subtext**
  below — matching the shared design used across the Huckleberry and mervyns
  apps.
- **Bidirectional battery-current gauge**: the battery-flow dial now fills from
  center — right for charge, left for discharge — with a leaf (charge) / bolt
  (discharge) icon that follows net flow.
- **Dashboard gauges**: the four Power-page header gauges (SOC / solar /
  battery current / load) now also appear on the main dashboard, between the
  Climate and Health cards.
- **Power page order**: the Victron SmartSolar detail card now comes first, with
  the EcoWorthy battery detail card moved below it.

## v0.4.0 - Victron power center
- **Connected Victron SmartSolar over BLE** (6-digit PIN via BLE passkey
  pairing; no app-layer crypto). Adds exact model, serial number, firmware
  version, yesterday's yield/peak, battery temperature, plus the charger's
  31-day history. Extended data refreshes every 15 minutes. An unknown-VREG
  diagnostic reports which registers a given model actually serves.
- **Power page redesigned**: round dashboard gauges (SOC / solar / battery flow
  / load) up top, then a VictronConnect-style **daily-history table** (stacked
  yield bars segmented by charge stage, tap a day for its detail card), then
  30-day trend charts, then EcoWorthy and MPPT detail cards at the bottom. Day
  order is consistent (today at left) across the table and charts.
- **Inside temperature** derived from the EcoWorthy pack sensor, shown on the
  clock page, web dashboard, and thermostat tile.
- **Contrast helper reworked** into a consistent dark-backdrop scrim (0 off /
  1 medium / 2 solid) that works on any theme or background photo.
- New `docs/VICTRON_INTEGRATION.md`: instant vs. PIN data, GATT/CBOR protocol,
  VREG list, and storage math for anyone adding Victron support.
- Reverse-engineering effort concluded (`docs/VICTRON_RE.md`): connected mode is
  BLE passkey auth over a CBOR VREG protocol, not a custom crypto handshake.

## v0.3.0-b1 - Web app overhaul (M2.1)
- Web app restructured into six sections (Dashboard, Power, Display, Network,
  BLE, Firmware) served as a hash-routed SPA from the device. Existing NVS keys
  (`bgN`, `ptN`, `pbN`, `lXN`, `lYN`, `lSN`) preserved.
- Display Settings page: single dropdown selects Clock / Climate / Power /
  Status; controls for background, X/Y, scale, color theme, data box, contrast
  helper, plus an approximate 320x240 live preview. Reset-this-page and
  Reset-all-pages restore background, theme, box, contrast, and layout for the
  chosen page(s) - the night clock stays locked.
- Seasonal presets: save the current display state (backgrounds, themes, boxes,
  contrast, layout, day theme) as a named preset on SPIFFS `/presets/`, then
  load or delete it. Suggested names: Spring, Christmas, Valentine, Camping,
  Storage, Minimal.
- Contrast helper (new `pcN` NVS key, per page): 0 none, 1 forces dark text on
  the day clock, 2 also forces the data-box panel visible. Prevents the failure
  mode where a light theme + no data box washed out the clock text on busy
  JPEG backgrounds.
- Web accent color (new `wAcc` NVS key): change the web UI accent from the
  Network tab. On-device UI uses its own display color theme.
- BLE Manager card: view + set battery MAC, Victron MAC + key, Gidrox MAC, and
  the BLE-enabled toggle from the web (previously in `secrets.local.h` only).
- Firmware card: version, free heap, uptime, browser OTA, and CLI hints.
- New endpoints: `POST /api/reset?page=N`, `GET /api/presets`,
  `POST /api/preset/{save,load,delete}`, `POST /api/hostname`, `POST /api/ble`.
- Cleanup: removed nine `-Chaffee` AI-artifact duplicates from src/ and include/
  (they were already excluded from the build).

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
