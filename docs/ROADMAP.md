# Huckleberry Roadmap & Milestones

Command center for the Huckleberry horse trailer, on a QMSD ZX2D80CE02S
(ESP32-S3, ST7789 8080, FT5x06 touch). A calm LED clock that is also the power,
climate, and control hub. Reuses architecture from `picklepc/r48display` and
Victron code from `picklepc/mervyns-esp32-offgrid-manager`.

Each milestone ends by running `RELEASE_CHECKLIST.md`, tagging a release, and
updating this file.

## M0 — Bring-up & clock ✅ DONE
- LovyanGFX driver (ST7789 8080 8-bit + FT5x06), verified on hardware.
- Seven-segment LED clock, landscape, auto day/night (night = black/white, home
  page only), 1-minute return-to-clock, tap→thermostat, swipe pages.
- Stock 8 MB firmware backed up.

## M1 — Connectivity & live telemetry ✅ DONE
- Wi-Fi STA + always-on `Huckleberry` AP (off-grid), captive DNS, mDNS.
- Web app (dashboard/settings), browser + NTP time sync.
- BLE: Eco-Worthy battery (JBD FF00) live; Victron MPPT Instant Readout decoded
  on-device (mbedtls AES-CTR).

## M2 — Comprehensive settings & profiles
- **Multi-STA network memory** (home + campsites): store several SSIDs, auto-join
  the best known network; add/remove from web.
- **BLE device discovery UI**: scan, list, pick + bind battery/solar/AC.
- Day & night **theme selection**, per-mode **brightness**, screen **timeouts**,
  **display off** schedule, **camping/storage** profiles.
- **OTA firmware** management (web uploader + GitHub self-update, like R48).
- **MQTT** publishing (Home Assistant), opt-in.
- Settings page with descriptions + animation controls.

## M3 — Power center & maintenance
- Rolling on-device **history graphs** (SOC, battery V/A, PV watts) sampled from
  live reads — graphs without needing deep Victron history.
- At-home (home STA) **battery-maintenance scheduler** to keep the pack healthy
  while parked (storage float target, periodic top-ups, alerts).
- **Exterior 12V lights**: "Auto" control on a GPIO for the physical 3-way
  ON-AUTO-OFF switch (ON/OFF bypass the ESP32); schedule + dusk logic.

## M4 — Thermostat & climate (tasks #1–#7)
- Tuya-BLE (AllLink) backend for the Gidrox AC; simulator until it arrives.
- Boost-then-decay to the power budget; **overnight comfort ramp** (high-cool →
  power-save, tied to night + SOC); IR-remote reconciliation; camping/storing
  protection; web scheduler UI.

## M5 — Delight & theming
- Recreated **landscape weather art** (clean, original assets — not the stock
  Chinese firmware's code); 7-day forecast on web.
- **Bigger flowers**, Central-Valley natives, **Indie** the horse motifs,
  **holiday themes** (Valentine/Christmas/Halloween/spring), tasteful animation.

## M6 — Victron deep data (stretch)
- Mine the decompiled VictronConnect app (already in `../Pack Rat/apk-analysis`)
  for a richer BLE protocol to add fuller graphs beyond Instant Readout, IF the
  complexity is justified (Mervyns notes live-reads usually beat on-device
  history).

## Ongoing — Release process
- Run `RELEASE_CHECKLIST.md` every major milestone; keep GitHub history; update
  README/CHANGELOG; keep secrets out of git.
