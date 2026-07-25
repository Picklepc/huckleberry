# Huckleberry — Design Goals

Calm LED-clock command center for the Huckleberry horse trailer. This document
captures the *intent* behind the UI and hardware so any contributor (or a future
session) can build to it. See also [ROADMAP.md](ROADMAP.md) and the persisted
project memory.

## Display modes

### Night clock — LOCKED, do not change
8pm–8am, home page only: **black face, white seven-segment LEDs**, dimmest
backlight. The user has explicitly approved this and it must not change except
the **night-brightness** setting. Night styling applies to the **clock tile
only** — swiping to Thermostat/Power/Status shows those fully day-themed even at
night.

### Day-mode home — redesign target
Inspired by a Lenovo-Smart-Clock "Good Morning" style (reference image:
`docs/day-clock-reference.png` — a big rounded 10:48 clock with a pink "Good
Morning!" greeting, heart dividers, date, and a floral photo background of pink
gerbera + purple lavender + blue hydrangea on black). Goals:

- **Time-of-day greeting**: "Good Morning / Afternoon / Evening!" in a script/
  accent color.
- **Heart dividers** above and below the greeting, drawn from a font glyph.
- **Large rounded time** (not seven-seg) with a pink **AM/PM**, and the full date
  ("Wed, July 23, 2025").
- **Pink/purple theme** selectable in settings (replacing the orange accents).
- **User-uploadable 320×240 background image** (compressed/JPEG) via the settings
  page; stored in SPIFFS and drawn behind the clock. The reference floral image
  is a perfect example of such a background.
- **No Wi-Fi indicator** on the clock — Wi-Fi status lives on the Settings/Status
  page.
- **No weather on the clock.** Weather moves to the **Climate page** and is shown
  **only when weather data is available** (Open-Meteo over STA); if unavailable,
  show nothing.
- **No battery/solar/temp/humidity data strip** on the day clock — keep it clean.

### Generalized position + scale layout
Positioning is not clock-specific: provide a **layout system** to **position AND
scale** the main widgets across pages — day clock, date, thermostat card, power
widgets — with per-widget `{x, y, scale}` stored in NVS and edited from the web
settings page. (Night clock excluded.) Scaling approach: seven-seg via a DW/DH/T
factor; labels via font-size tiers; cards via a size factor.

## Interaction
- **Clock is home.** Tap the clock → Thermostat. Swipe between
  Clock / Thermostat(Climate) / Power / Status.
- **Return-to-clock inactivity timeout**: after N seconds of no touch on any
  non-home page, snap back to the clock. **Configurable** in settings
  (`homeTimeoutSec`, default 60s).
- **Scheduled display-off**: optional window where the backlight blanks after ~15s
  of inactivity and **wakes on touch** (`dispOffEnable`, `dispOffStart/End`).

## Theming
- Baseline: black face + digital LEDs. Holiday skins (Valentine, Christmas,
  Halloween, spring) recolor all screens. Add a **pink/purple** palette.
- Per-mode brightness (`dayBrightness`, `nightBrightness`); auto night window
  (`nightStartHour`/`nightEndHour`, `autoNight`).
- Central-Valley native flowers + **Indie** the horse motifs in day mode.

## Web app direction
The `v0.2.0` web app is functional but still transitional. The next pass should
be a full web restructure, not more incremental stacking inside one long page.
See [WEB_APP_OVERHAUL.md](WEB_APP_OVERHAUL.md).

Design goals:
- Dashboard first: simple battery/charging state, climate controls, link health,
  and firmware version.
- Dedicated Power page: all EcoWorthy/JBD and Victron data, with future space for
  deeper Victron history once the richer BLE protocol is understood.
- Display Settings page: one selected display target/card at a time, with
  background, position, scale, color theme, data-box toggle, and an approximate
  live preview. Save changes safely as targets are switched.
- Seasonal presets: save and restore complete display looks for Christmas,
  spring, Valentine, camping, storage, and other seasonal setups.
- Networking card: hostname (default `huckleberry` / shown as `Huckleberry`),
  Wi-Fi manager, STA/AP state.
- BLE card: battery, Victron, and Gidrox bindings; later scan/pick/pair.
- Firmware card: current version and OTA upload.
- Web theme controls: the web UI accent palette, including the current orange,
  should become configurable.

## Off-grid / connectivity
- STA (multiple saved networks: home + campsites, auto-join) **plus** an
  always-on `Huckleberry` AP so a phone can connect directly off-grid. All core
  functions work with zero internet.
- Time chain: NTP on STA → browser time-push → free-running RTC.

## Hardware provisions
See [HARDWARE.md](HARDWARE.md) for the debug/GPIO pinouts, the 5V power-input
plan, and the future **USB-host trailer camera** provision.
