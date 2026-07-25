# Huckleberry — Session Handoff

Paste the prompt at the bottom into Claude Code on another machine to continue.
This file is public — **no secrets here**.

## Project
Command-center firmware for the **Huckleberry** horse trailer on a QMSD
**ZX2D80CE02S** (ESP32-S3-WROOM-1 N8R2). Repo: **github.com/Picklepc/huckleberry**.
A calm LED clock that is also the power / climate / control hub.

## Where things stand (M-status)
- **M0 Clock** ✅ + **M1 Connectivity/BLE** ✅ (verified on hardware).
- **M2 in progress**: multi-STA network memory ✅, comprehensive settings ✅,
  **OTA** ✅ (browser `/update` + `pio -e huckleberry_ota`). Pending: BLE
  discovery UI, MQTT.
- Device on home Wi-Fi at its DHCP IP, plus always-on AP **`Huckleberry`**.
- Night clock is **LOCKED** (black face / white LEDs). Day-mode redesign is the
  next big push — see [DESIGN.md](DESIGN.md).

## Build & flash
- PlatformIO (`pip install platformio esptool bleak pycryptodome`).
- USB: `pio run -e huckleberry -t upload` (COM port in platformio.ini).
- **Over Wi-Fi (no USB)**: `pio run -e huckleberry_ota -t upload` (targets
  `huckleberry.local`, password `huckleberry`) — or the browser at `/update`.
- **Secrets**: copy `include/secrets.example.h` → `include/secrets.local.h`
  (gitignored) and fill the Victron key + BLE MACs (Victron key is in your
  VictronConnect app). Or leave blank and set them from the web Settings page.

## Key references (in repo)
- [DESIGN.md](DESIGN.md) — UI/UX design goals (night clock locked; day-mode
  redesign; layout system; timeouts; weather-on-climate).
- [HARDWARE.md](HARDWARE.md) — GPIO/debug pinout, 5V power input, USB-host
  camera provision.
- [ROADMAP.md](ROADMAP.md) — milestones. [RELEASE_CHECKLIST.md](../RELEASE_CHECKLIST.md)
  — run every milestone.

## What's next
1. Day-mode home redesign: pink/purple theme → greeting + hearts → position/scale
   layout → background-image upload (see DESIGN.md).
2. BLE device discovery UI, MQTT.
3. Thermostat simulator + scheduler (Gidrox = Tuya/AllLink; arrives ~2026-07-31).

---

## Copy-paste continuation prompt

> I'm continuing development of **Huckleberry**, ESP32-S3 firmware for my horse
> trailer, at github.com/Picklepc/huckleberry (also on this machine under
> `…/ESP32/Huckleberry`). Please read README.md, docs/DESIGN.md, docs/HARDWARE.md,
> docs/ROADMAP.md, and RELEASE_CHECKLIST.md first. Current state: M0 clock + M1
> connectivity/BLE + M2 (multi-STA, comprehensive settings, OTA) are done and on
> hardware; the device is on my Wi-Fi and exposes a `Huckleberry` AP; flash over
> Wi-Fi with `pio run -e huckleberry_ota -t upload`. The **night clock is locked
> (do not change it)**. Next up is the **day-mode home redesign** per DESIGN.md
> (pink/purple theme, "Good Morning" greeting + heart dividers, a general
> position+scale layout for widgets, and a user-uploadable 320×240 background
> image), then the BLE discovery UI and MQTT. If `include/secrets.local.h` is
> missing, help me recreate it from secrets.example.h. Continue from there.
