# Huckleberry

A calm LED-clock command center for the **Huckleberry** horse trailer, built on a
QMSD **ZX2D80CE02S** board (ESP32-S3-WROOM-1 N8R2, ST7789 240×320 over an 8080
8-bit parallel bus, FT5x06 capacitive touch).

The home screen is a dim, sleep-friendly seven-segment clock (black face, digital
LEDs; automatically black/white at night). Tap for climate; swipe for power and
status. Off-grid it runs fully independently; at home it joins Wi-Fi for time,
updates, and battery maintenance.

## Current release
**v0.5.5 — stored Victron trends and VE.Smart external sense.** Huckleberry reads
the SmartSolar's charger-owned 31-day daily and intraday history, provides
interactive day charts plus CSV exports, and lets the PC-side SQL collector retain
only those native records. It also broadcasts fresh EcoWorthy voltage,
temperature, and shunt current into the charger's VE.Smart network; live
SmartSolar diagnostics confirm Vsense, Tsense, and Isense acceptance without any
charge-setting writes. See **[docs/VICTRON_RE.md](docs/VICTRON_RE.md)** for the
complete protocol and hardware-validation record.

Built on **v0.3.0** (M2.1): the web app in six sections (Dashboard, Power,
Display, Network, BLE, Firmware) with a per-page display editor, live preview,
per-page reset, seasonal presets, a page-contrast helper, and a configurable web
accent color. Prior field release **v0.2.0** was USB-flashed and OTA-verified on
the trailer at `huckleberry.local`.

## Features (current)
- Seven-segment LED clock, landscape, **auto day/night** (night = black/white,
  home page only), 1-minute return-to-clock, swipeable Clock / Climate / Power /
  Status pages.
- **Wi-Fi STA + always-on `Huckleberry` AP** simultaneously; captive portal; mDNS.
- **Web app** (dashboard, settings) served on the device; browser + NTP time sync.
- **Live BLE telemetry**: Eco-Worthy 280 Ah battery (JBD/Xiaoxiang FF00) and
  Victron SmartSolar MPPT (encrypted *Instant Readout*, decoded on-device).
- **Native Victron history**: charger-owned daily and half-hour intraday records,
  interactive charts, CSV exports, and optional stored-only SQL collection.
- **VE.Smart bridge**: broadcast-only EcoWorthy Vsense, Tsense, and Isense input
  accepted by the SmartSolar; no charge-control writes.
- **Background-driven display pages**: SPIFFS JPEG backgrounds, per-page
  background selection, per-page theme, data-box toggle, and position/scale
  layout controls for Clock / Climate / Power / Status.
- Power data includes EcoWorthy SOC, V/A/W, remaining/nominal capacity, status,
  temperature, cycles, per-cell voltages, Victron PV watts, observed PV max,
  percent-of-max, charger state, yield, and RSSI.
- Themeable (holiday skins); flower + Indie-the-horse motifs. Night clock remains
  locked as the approved black/white seven-segment clock.

See [docs/ROADMAP.md](docs/ROADMAP.md) for what's next and
[RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) for the per-milestone review.

## Build & flash
```
pio run -e huckleberry            # build
pio run -e huckleberry -t upload  # flash over USB (COM port in platformio.ini)
pio run -e huckleberry_ota -t upload  # flash over Wi-Fi to huckleberry.local
```
Device secrets (BLE keys/MACs) go in `include/secrets.local.h` (gitignored; copy
from `include/secrets.example.h`) or are entered from the web Settings page.

SPIFFS assets live in `data/bg`. If background files change, run:
```
pio run -e huckleberry -t uploadfs
```

## Next development focus
The web app needs a full structure/design pass. See
[docs/WEB_APP_OVERHAUL.md](docs/WEB_APP_OVERHAUL.md) for the target dashboard,
power page, display preset editor, networking card, BLE manager, firmware card,
and web-theme controls.

## Hardware
ESP32-S3 N8R2 · ST7789 8080 8-bit · FT5x06 touch · LEDC backlight. Pin map and
driver config in `include/LGFX_Huckleberry.hpp`. Related projects:
`picklepc/r48display` (architecture) and `picklepc/mervyns-esp32-offgrid-manager`
(Victron BLE).
