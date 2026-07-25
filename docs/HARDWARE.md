# Huckleberry — Hardware & Wiring

Board: **QMSD / Smart Panlee ZX2D80CE02S** ("SC05_X"), ESP32-S3-WROOM-1 **N8R2**
(8 MB flash, 2 MB PSRAM). ST7789 240×320 over an 8080 8-bit parallel bus, FT5x06
capacitive touch, LEDC backlight. USB-C = native ESP32-S3 USB-Serial/JTAG.

## GPIO usage (from the vendor PanelLan library — authoritative signals)

**Free / broken out (use these):** `GPIO 10, 11, 12, 13, 14, 21`.
**Also free if RS485 is unused:** `GPIO 4 (RXD), 5 (RTS), 6 (TXD)`.

**In use — do NOT reuse:**
| Function | GPIO |
|---|---|
| Display 8080 data D0–D7 | 16, 40, 15, 7, 41, 42, 2, 1 |
| Display WR / DC(RS) / RST / TE | 17 / 18 / 3 / 38 |
| Backlight (LEDC) | 47 |
| Touch + base I²C (SDA / SCL / INT) | 8 / 9 / 48 |
| USB D+ / D− (native) | 19 / 20 |

## Debug header (7-pin) — Smart Panlee standard
Used by the vendor **ZXACC-ESPDB** flashing adapter. **Confirm pin-1 orientation
against the board silkscreen before wiring.**

| Pin | Signal | Notes |
|---|---|---|
| 1 | **+5V** | 5V rail (VBUS) — see power-input below |
| 2 | +3.3V | regulated 3.3V |
| 3 | TXD0 (GPIO43) | UART0 debug/console out |
| 4 | RXD0 (GPIO44) | UART0 debug/console in |
| 5 | EN | reset |
| 6 | BOOT (GPIO0) | hold low at reset for download mode |
| 7 | GND | ground |

## Powering via 5V (to free the USB-C port)
To use the USB-C port for **USB host** (camera capture) instead of power, feed a
**regulated 5V** into the board's 5V rail:
- **Debug header Pin 1 (+5V)** and **Pin 7 (GND)** — the simplest tap. The debug
  +5V is the same VBUS rail the USB-C would supply, so injecting 5V here powers
  the whole board (LCD backlight included).
- Source it from the trailer 12V via a **12V→5V buck converter** (size for the
  backlight load — budget ~500 mA+). Do **not** also power from USB-C at the same
  time (avoid back-feeding two 5V sources).
- Verify the exact 5V pin on the silkscreen; some units also expose 5V/GND on the
  GPIO header.

## FUTURE PROVISION — USB-host trailer camera (not started)
Goal: watch the horses from the dashboard and/or a web portal.

```
Composite camera → USB capture module (UVC/MJPEG) → ESP32-S3 USB Host → { LCD "Camera" page ; MJPEG web stream }
```

- **Power**: board runs from 5V-on-debug-header (above) so the **USB-C is free**
  to act as USB **Host** (native USB, GPIO19/20). The ESP32 supplies VBUS to the
  UVC capture dongle.
- **Software**: ESP-IDF USB Host + a **UVC** driver pulls **MJPEG** frames from
  the capture module. Serve them as an MJPEG stream on a web route
  (`/camera`) and optionally decode+draw frames on a device **Camera** tile
  ("composite-only path" = view on the dashboard without a phone).
- **Constraints**: ESP32-S3 UVC is limited to low resolution / low fps (e.g.
  ~320×240–640×480 MJPEG). Good enough for a horse-check feed, not HD.
- **Build note**: USB Host + Wi-Fi + LCD + BLE together is memory/CPU heavy —
  may need to gate BLE or camera behind a mode. Provision only for now; see the
  camera task in the roadmap.
