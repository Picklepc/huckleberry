# Huckleberry Roadmap & Milestones

Command center for the Huckleberry horse trailer, on a QMSD ZX2D80CE02S
(ESP32-S3, ST7789 8080, FT5x06 touch). A calm LED clock that is also the power,
climate, and control hub. Reuses architecture from `picklepc/r48display` and
Victron code from `picklepc/mervyns-esp32-offgrid-manager`.

Each milestone ends by running `RELEASE_CHECKLIST.md`, tagging a release, and
updating this file.

## M0 - Bring-up & clock - DONE
- LovyanGFX driver (ST7789 8080 8-bit + FT5x06), verified on hardware.
- Seven-segment LED clock, landscape, auto day/night (night = black/white, home
  page only), 1-minute return-to-clock, tap to thermostat, swipe pages.
- Stock 8 MB firmware backed up.

## M1 - Connectivity & live telemetry - DONE
- Wi-Fi STA + always-on `Huckleberry` AP (off-grid), captive DNS, mDNS.
- Web app (dashboard/settings), browser + NTP time sync.
- BLE: Eco-Worthy battery (JBD FF00) live; Victron MPPT Instant Readout decoded
  on-device (mbedtls AES-CTR).

## M2 - Comprehensive settings & display foundations - DONE in v0.2.0
- Multi-STA network memory (home + campsites): store several SSIDs, auto-join
  the best known network, add/remove from web.
- Day/night theme selection, per-mode brightness, screen timeouts, display-off
  schedule, and camping/storage settings.
- Day-home/display groundwork: SPIFFS JPEG backgrounds, page background
  selection, per-page theme, data-box toggle, and position/scale settings for
  Clock / Climate / Power / Status.
- Power center data: EcoWorthy/JBD basic and cell-voltage frames exposed to web;
  Victron Instant Readout, observed PV max, and percent-of-max exposed to web.
- OTA firmware management: browser uploader and `huckleberry_ota` PlatformIO
  environment targeting `huckleberry.local`.
- Official field verification: USB flash, OTA update, EcoWorthy + Victron live
  telemetry, and `/api/state` verified on trailer Wi-Fi.

## M2.1 - Web app overhaul - NEXT
- Split the web app into Dashboard, Power, Display Settings, Networking, BLE
  Manager, and Firmware sections.
- Dashboard: simple battery status, charge/solar summary, climate controls, and
  connection health.
- Power: all available EcoWorthy/JBD and Victron data, with room for deeper
  Victron data once the richer BLE connection/protocol is ready.
- Display Settings: one selected page/card at a time with background, position,
  scale, color theme, data-box toggle, approximate preview, and safe save/refresh
  behavior when switching targets.
- Display presets: save/load seasonal display looks for clock, climate, power,
  and status pages.
- Networking: hostname setting, Wi-Fi manager, STA/AP state.
- BLE Manager: battery, Victron, and Gidrox bindings; later BLE scan/list/pick.
- Firmware: OTA upload, current version, and release notes.
- Web theme settings so the orange accent can be changed.

## M2.2 - Device management backlog
- BLE device discovery UI: scan, list, pick + bind battery/solar/AC.
- MQTT publishing (Home Assistant), opt-in.

## M3 - Power center & maintenance
- Rolling on-device history graphs (SOC, battery V/A, PV watts) sampled from
  live reads - graphs without needing deep Victron history.
- At-home battery-maintenance scheduler to keep the pack healthy while parked
  (storage float target, periodic top-ups, alerts).
- Exterior 12V lights: "Auto" control on a GPIO for the physical 3-way
  ON-AUTO-OFF switch (ON/OFF bypass the ESP32); schedule + dusk logic.

## M4 - Thermostat & climate
- Tuya-BLE (AllLink) backend for the Gidrox AC; simulator until it arrives.
- Boost-then-decay to the power budget; overnight comfort ramp (high-cool to
  power-save, tied to night + SOC); IR-remote reconciliation; camping/storing
  protection; web scheduler UI.

## M5 - Delight & theming
- Recreated landscape weather art (clean, original assets - not the stock
  Chinese firmware's code); 7-day forecast on web.
- Bigger flowers, Central-Valley natives, Indie the horse motifs, holiday themes
  (Valentine/Christmas/Halloween/spring), tasteful animation.

## M6 - Victron deep data (stretch)
- Mine the decompiled VictronConnect app (already in `../Pack Rat/apk-analysis`)
  for a richer BLE protocol to add fuller graphs beyond Instant Readout, if the
  complexity is justified. Mervyns notes live reads usually beat on-device
  history.

## Ongoing - Release process
- Run `RELEASE_CHECKLIST.md` every major milestone.
- Keep GitHub history clean and keep secrets out of git.
- Update README, CHANGELOG, docs/HANDOFF.md, and this roadmap before tagging.
