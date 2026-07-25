# Web App Overhaul Plan

This is the next major workstream after the official `v0.2.0` field release.
The current web app works and exposes the data needed for tuning, but it is still
a compact single-page utility. The goal is to turn it into a clean trailer
control surface that is easy to use from a phone near the trailer.

## Goals
- Keep the first page operational, not decorative: battery status, climate
  setpoint controls, and connection health should be visible immediately.
- Split high-detail data away from daily-use controls so the dashboard stays
  calm and quick.
- Make display/background tuning safe: every page/card gets size, position,
  color theme, and data-box controls with a useful preview before saving.
- Let good seasonal display layouts become presets so the trailer can quickly
  switch between Christmas, spring, Valentine, camping, storage, and other
  seasonal looks.
- Keep processing and memory overhead modest. A dropdown that selects one display
  page/card and reuses the same controls is preferred over rendering many live
  editors at once.

## Proposed Navigation
- **Dashboard**: simple battery status, solar/charging summary, climate controls,
  Wi-Fi/BLE health, and firmware version.
- **Power**: all available EcoWorthy and Victron data, including fields that are
  not important enough for the dashboard.
- **Display Settings**: one card selector/dropdown with preview and controls for
  page/card background, position, scale, color theme, and data box.
- **Networking**: hostname, Wi-Fi manager, STA/AP state.
- **BLE Manager**: battery, Victron, and Gidrox device bindings; later BLE scan,
  pick, and pairing flows.
- **Firmware**: OTA browser upload, OTA status, version, and release notes.

## Dashboard Page
- Top status row: battery percent, charging/discharging/idle, solar watts, and
  Wi-Fi status.
- Battery summary: SOC and rough power budget, not every raw field.
- Climate controls: setpoint, mode/preset, AC paired/unpaired state.
- Links/buttons to Power, Settings, and Firmware.
- Theme: should have its own web theme palette controls later. The current orange
  accent can remain as a default, but it must be replaceable.
- Header cleanup: keep the flower mark if desired, but spacing must not overlap
  the `Huckleberry` title.

## Power Page
Show all live data available today:
- EcoWorthy/JBD: SOC, total voltage, total current, total watts, charging status,
  remaining Ah, nominal Ah, estimated working time, temperature sensors, cycles,
  FET bits, protection bits, software version, cell count, and per-cell mV/V.
- Victron Instant Readout: PV watts, observed PV max, percent of observed max,
  charger state, solar battery voltage/current, load current, yield today, RSSI,
  and error state if decoded.
- Make clear that the current solar max is an observed live peak since boot, not
  the deeper VictronConnect historical max yet.
- Future Victron deep data: add detailed history once the richer BLE protocol is
  understood and the Bluetooth connection is stable enough to justify it.

## Display Settings Page
Use one editor card with a dropdown selector. Suggested selectable targets:
- Clock page
- Clock date
- Climate page/card
- Power page/card
- Status page/card

For the selected target, expose:
- Background image
- X position
- Y position
- Scale
- Color theme
- Data box on/off
- Optional text contrast/shadow mode if frameless text needs help
- Reset selected target
- Save preset
- Load preset

Preview requirements:
- Show an approximate 320x240 preview using the selected background and current
  target rectangle/text overlay.
- Preview does not need perfect LVGL parity, but it should make crowding,
  contrast, and off-screen placement obvious before saving.
- Saving must update NVS and live UI state immediately.
- Switching between targets must preserve unsaved edits only if the UI clearly
  indicates they are pending. The safer first implementation is to save on every
  control change, then refresh the selected target from `/api/state`.

Preset requirements:
- Presets should include page background, page theme, data-box flag, and layout
  slots for that target or for the full display set.
- Start with named full-display presets in NVS if there is room. If NVS gets
  tight, use SPIFFS JSON under `/presets/`.
- Presets should be quick to apply for seasonal changes.
- Suggested initial presets: `Spring`, `Christmas`, `Valentine`, `Camping`,
  `Storage`, and `Minimal`.

## Networking Card
- Hostname field, default `huckleberry`, shown to the user as `Huckleberry`.
- Wi-Fi manager: saved networks, connected SSID/IP, add/remove known networks.
- Always-on AP state and SSID.
- Reconnect/apply button when hostname or Wi-Fi changes.
- Persist hostname to existing `Settings::hostname`.

## BLE Manager Card
- Battery binding: MAC, connected/waiting/live status, last basic frame, last cell
  frame.
- Victron binding: MAC, encryption key entry/status, RSSI, last instant readout.
- Gidrox binding: placeholder now; BLE scan/pairing later.
- Later: scan nearby BLE devices and choose battery/solar/AC from a list.

## Firmware Card
- Current firmware version.
- Browser OTA upload.
- PlatformIO OTA instructions:
  `pio run -e huckleberry_ota -t upload`
- SPIFFS upload reminder when backgrounds change:
  `pio run -e huckleberry -t uploadfs`
- Show last OTA result if feasible.

## Implementation Notes
- Keep the first refactor simple: routes can remain served from `WebApp.cpp`, but
  the embedded HTML/JS should be split into clearer sections or static assets if
  the file keeps growing.
- Prefer `/api/state` for readback and `/api/settings` or new focused endpoints
  for writes.
- Keep existing settings keys compatible: `bgN`, `ptN`, `pbN`, `lXN`, `lYN`,
  `lSN`.
- Avoid rendering all preview editors at once on mobile. Use the dropdown
  selected target model to conserve memory and reduce accidental saves.
