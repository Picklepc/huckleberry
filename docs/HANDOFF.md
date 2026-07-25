# Huckleberry - Session Handoff

Use the prompt at the bottom to continue development from another machine. Keep
private BLE keys and local-only credentials out of this file.

## Project
Command-center firmware for the **Huckleberry** horse trailer on a QMSD
**ZX2D80CE02S** (ESP32-S3-WROOM-1 N8R2). Repo:
`https://github.com/Picklepc/huckleberry`.

The device is a calm LED clock plus power, climate, and trailer-control hub.

## Current release
- Official release: **v0.2.0**.
- Branch/tag target: `main` / `v0.2.0`.
- Hardware verified on trailer Wi-Fi at `huckleberry.local`
  (`192.168.1.146` during release testing).
- OTA verified with:
  `pio run -e huckleberry_ota -t upload`
- OTA password: `huckleberry`.
- Background filesystem was previously uploaded and includes:
  `bg_charlie_01.jpg`, `bg_creek_01.jpg`, `bg_flower_01.jpg`,
  `bg_indie_01.jpg`, `bg_indie_02.jpg`.

## Release state
- M0 Clock: done.
- M1 Connectivity/BLE: done.
- M2 Settings/display foundations: done in `v0.2.0`.
- Night clock is locked: black face, white seven-segment LEDs, dimmest
  brightness. Do not redesign night mode except through the existing brightness
  setting.
- Day/display pages are background-driven and still open for design tuning.
- Current web app works but is transitional and should be overhauled next.

## Verified live telemetry at release
Post-OTA `/api/state` returned:
- Battery: live, 35%, about 13.26 V, idle/near-zero current, EcoWorthy basic
  stats present.
- Cells: four cell voltages present around 3315 mV.
- Solar: Victron live, Bulk, about 8 W against an 8 W observed boot peak, RSSI
  about -46 dBm.
- Firmware: `v0.2.0`.

## Build and flash
Install PlatformIO and dependencies:
```
pip install platformio esptool bleak pycryptodome
```

Build:
```
pio run -e huckleberry
```

Flash firmware over USB:
```
pio run -e huckleberry -t upload
```

Flash firmware over Wi-Fi OTA:
```
pio run -e huckleberry_ota -t upload
```

Upload SPIFFS backgrounds after `data/bg` changes:
```
pio run -e huckleberry -t uploadfs
```

Secrets:
- Copy `include/secrets.example.h` to `include/secrets.local.h` if needed.
- Fill Victron key and BLE MACs locally, or use the web Settings page.
- Never commit `include/secrets.local.h`.

## Key references
- `README.md`: current release and setup.
- `CHANGELOG.md`: release history.
- `docs/DESIGN.md`: display and UX goals.
- `docs/ROADMAP.md`: milestone status.
- `docs/WEB_APP_OVERHAUL.md`: next web app plan.
- `docs/HARDWARE.md`: board and wiring notes.
- `RELEASE_CHECKLIST.md`: run before each official release.

## Next work
Primary next task: **M2.1 web app overhaul**.

Implement a clearer web app with:
- Dashboard: simple battery status, charging/solar summary, climate controls,
  Wi-Fi/BLE health, firmware version.
- Power page: all EcoWorthy/JBD and Victron fields currently available, plus
  room for richer Victron data once the BLE protocol/connection improves.
- Display Settings page: one selected page/card at a time, with background,
  position, scale, color theme, data box, approximate preview, reset, save/load
  seasonal presets, and safe save/refresh behavior while switching targets.
- Networking card: hostname (default `huckleberry`, shown as `Huckleberry`),
  Wi-Fi manager, STA/AP state.
- BLE Manager card: battery, Victron, and Gidrox bindings; later scan/list/pick.
- Firmware card: current version, browser OTA upload, PlatformIO OTA guidance.
- Web theme controls so the orange accent can be changed.

## Copy-paste continuation prompt

> I am continuing development of Huckleberry, ESP32-S3 firmware for my horse
> trailer, from `https://github.com/Picklepc/huckleberry`. Please read
> `README.md`, `CHANGELOG.md`, `docs/DESIGN.md`, `docs/ROADMAP.md`,
> `docs/WEB_APP_OVERHAUL.md`, `docs/HARDWARE.md`, and `RELEASE_CHECKLIST.md`
> before editing. Current release is official `v0.2.0`, verified on trailer
> Wi-Fi with OTA via `pio run -e huckleberry_ota -t upload` targeting
> `huckleberry.local` with password `huckleberry`. The night clock is locked:
> black face, white seven-segment LEDs, dimmest brightness; do not redesign it.
> Next work is M2.1: overhaul the ESP32 web app into Dashboard, Power, Display
> Settings, Networking, BLE Manager, and Firmware sections. The Display Settings
> page should use a dropdown-selected page/card editor with background, X/Y,
> scale, color theme, data-box toggle, approximate live preview, reset, and
> save/load presets for seasonal displays. Preserve existing NVS keys where
> possible (`bgN`, `ptN`, `pbN`, `lXN`, `lYN`, `lSN`) and verify settings update
> the live UI and survive target/page switching. Keep secrets out of git; create
> `include/secrets.local.h` from `include/secrets.example.h` only locally if
> needed. Build, OTA flash, and verify `/api/state` after changes.
