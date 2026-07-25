# Release Notes - v0.2.0

Date: 2026-07-25

## Summary
`v0.2.0` is the official M2 field release for Huckleberry. It establishes the
display/background foundation, OTA workflow, richer EcoWorthy battery telemetry,
and a cleaned-up on-device Power/Climate/Status layout over trailer-specific
background images.

## Firmware
- Version string: `v0.2.0`.
- USB firmware upload verified before OTA.
- Wi-Fi OTA verified with:
  `pio run -e huckleberry_ota -t upload`
- OTA target: `huckleberry.local`.
- OTA password: `huckleberry`.

## Verified Hardware State
After the final OTA reboot, `/api/state` returned:
- Firmware: `v0.2.0`
- Battery: live, SOC `35`, voltage about `13.26 V`, idle/near-zero current.
- EcoWorthy cells: four cell voltages around `3315 mV`.
- Victron: live, `Bulk`, about `8 W` against an `8 W` observed boot peak, RSSI
  about `-46 dBm`.
- Wi-Fi: `Rhinestone`, IP `192.168.1.146` during validation.

## Included Features
- Multi-network Wi-Fi memory.
- Always-on `Huckleberry` AP.
- Browser and PlatformIO OTA.
- SPIFFS JPEG backgrounds under `data/bg`.
- Day display page background selection.
- Per-page theme and data-box controls.
- Position/scale layout controls for Clock / Climate / Power / Status.
- EcoWorthy/JBD basic and cell-voltage telemetry in `/api/state`.
- Victron Instant Readout telemetry in `/api/state`.
- Observed Victron PV max and percent-of-max.
- BLE retry/settle behavior after OTA/reconnect so basic battery stats and cells
  populate together.

## Known Limitations
- The web app is functional but transitional. M2.1 should replace the current
  single-page utility with Dashboard, Power, Display Settings, Networking, BLE
  Manager, and Firmware sections.
- Victron "PV max" is currently an observed live peak since boot, not the deeper
  VictronConnect historical max.
- BLE discovery/pick UI is not implemented yet.
- Gidrox AC integration is still pending.
- Display presets are planned but not implemented.

## Next Work
See `docs/WEB_APP_OVERHAUL.md` and the prompt in `docs/HANDOFF.md`.
