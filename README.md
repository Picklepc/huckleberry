# Huckleberry

A calm LED-clock command center for the **Huckleberry** horse trailer, built on a
QMSD **ZX2D80CE02S** board (ESP32-S3-WROOM-1 N8R2, ST7789 240×320 over an 8080
8-bit parallel bus, FT5x06 capacitive touch).

The home screen is a dim, sleep-friendly seven-segment clock (black face, digital
LEDs; automatically black/white at night). Tap for climate; swipe for power and
status. Off-grid it runs fully independently; at home it joins Wi-Fi for time,
updates, and battery maintenance.

## Features (current)
- Seven-segment LED clock, landscape, **auto day/night** (night = black/white,
  home page only), 1-minute return-to-clock, swipeable Clock / Climate / Power /
  Status pages.
- **Wi-Fi STA + always-on `Huckleberry` AP** simultaneously; captive portal; mDNS.
- **Web app** (dashboard, settings) served on the device; browser + NTP time sync.
- **Live BLE telemetry**: Eco-Worthy 280 Ah battery (JBD/Xiaoxiang FF00) and
  Victron SmartSolar MPPT (encrypted *Instant Readout*, decoded on-device).
- Themeable (holiday skins); flower + Indie-the-horse motifs.

See [docs/ROADMAP.md](docs/ROADMAP.md) for what's next and
[RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md) for the per-milestone review.

## Build & flash
```
pio run -e huckleberry            # build
pio run -e huckleberry -t upload  # flash over USB (COM port in platformio.ini)
```
Device secrets (BLE keys/MACs) go in `include/secrets.local.h` (gitignored; copy
from `include/secrets.example.h`) or are entered from the web Settings page.

## Hardware
ESP32-S3 N8R2 · ST7789 8080 8-bit · FT5x06 touch · LEDC backlight. Pin map and
driver config in `include/LGFX_Huckleberry.hpp`. Related projects:
`picklepc/r48display` (architecture) and `picklepc/mervyns-esp32-offgrid-manager`
(Victron BLE).
