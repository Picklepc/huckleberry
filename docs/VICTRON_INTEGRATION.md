# Victron SmartSolar MPPT integration

How Huckleberry reads a Victron SmartSolar MPPT over BLE, what data is
available, how to request it, and what it costs to store. Written so anyone
cloning this repo can add Victron support to their own build.

Reference implementation: [`src/BleManager.cpp`](../src/BleManager.cpp)
(`decryptVictron`, `parseVictron`, `pollVictronConnected`,
`applyConnectedVictronValue`), data model in
[`include/AppState.h`](../include/AppState.h).

---

## Two data tiers — instant vs. PIN

Victron exposes **two completely separate** data paths. Know which one you need.

### Tier 1 — Instant Readout (advertisement) · **no PIN, passive, always on**

- Comes from the MPPT's BLE **advertisements** — no connection, no bonding, no
  PIN. If the charger is in range you get it.
- Manufacturer data, Bluetooth company ID **`0x02E1`** (Victron).
- Encrypted; decrypt with the device's **16-byte encryption key** ("bindkey"),
  found in VictronConnect → the product → ⚙ → *Product info* → *Show encryption
  data*. Stored here as the `victronKey` setting (32 hex chars).
- Cipher: **AES-128-CTR** (`mbedtls_aes_crypt_ctr`). The IV is the 2-byte
  advertisement nonce, zero-padded to 16. (The app actually uses AES-CCM with a
  4-byte tag; skipping the tag and treating it as CTR yields identical
  plaintext — see [`VICTRON_RE.md`](VICTRON_RE.md).)
- Manufacturer-data layout:
  `[CID 0x02E1][0x10][model(2)][0xA0][record type][nonce_lo][nonce_hi][key_check][ciphertext…]`.
  Record type `0x01` = solar charger.
- Fields decoded (solar): charger **state**, **error**, **battery V/A**,
  **PV watts**, **yield today**, **load A**. Refreshed on every advertisement
  (~1 Hz).
- Code: `decryptVictron()` + `parseVictron()`.

### Tier 2 — Connected / VREG data · **requires the 6-digit PIN**

- Requires a **BLE connection** and the device's **6-digit PIN** (VictronConnect
  PIN; factory default `000000`). Gives you exact model/serial/firmware, live
  registers, and the charger's own **31-day history**.
- **Auth is plain BLE-layer passkey pairing** — *not* an application-layer
  crypto handshake. NimBLE supplies the PIN as the pairing passkey
  (`injectPassKey` in the client callback); once `secureConnection()` reports an
  encrypted link, VREG traffic flows in cleartext CBOR. (This was the key
  finding of the reverse-engineering effort: there is no custom handshake to
  reproduce.)
- Transport: a **CBOR-framed VE.Direct-over-BLE** protocol on GATT service
  **`306b0001-b081-4037-83dc-e59fcc3cdfd0`**:
  | Characteristic | Props | Role |
  |---|---|---|
  | `306b0002-…` | Write-no-resp, Notify | control: chunk config, flow-control credits |
  | `306b0003-…` | Read, Write, Notify | request/response ("last data") channel |
  | `306b0004-…` | Write, Notify | bulk data channel |
- Code: `pollVictronConnected()`, `sendConnectedGetValues()`,
  `parseConnectedVictronValues()`, `applyConnectedVictronValue()`.

---

## Connected call sequence (how to poll VREGs)

1. **Connect** to the MPPT MAC as a **random** static address (fall back to
   public). `NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY)` and supply
   the PIN via `injectPassKey`.
2. `secureConnection()` → confirm `getConnInfo().isEncrypted()`.
3. Subscribe (write CCCD, notify) on control / lastData / data.
4. On **control**, write chunk config `{0xFA,0x80,0xFF}` then a receive-credit
   `{0xF9,0x80}`; wait for a TX credit from the peer.
5. **Enumerate devices**: write `{0x01}` to lastData → CBOR list of
   `{instance, parent}` records. Pick the **leaf** instance (the one no other
   record lists as its parent) — that's the charger.
6. **Subscribe to the instance**: write `{0x03, <instance>}` to lastData.
7. **Request registers**: write `{0x05, <instance>, <CBOR array of u16 VREGs>}`
   to lastData.
8. **Parse the CBOR reply stream** (notifications on data/lastData):
   - opcode **8** → `{instance, vreg, bytestring}` — a register **value**.
   - opcode **9** → `{instance, vreg, reason}` — register **unknown/unsupported**
     on this model (harmless; used by Huckleberry's unknown-VREG diagnostic).
   - opcode **7** → `{instance, reqOpcode, result}` — request acknowledgement.

The CBOR used is minimal: unsigned ints (major 0, `0x18`/`0x19` prefixes for
1/2-byte follow-ons), byte strings (major 2), negative ints (major 1). See
`readCborUnsigned` / `readCborByteString` / `appendCborUnsigned`.

---

## Available VREGs (parsed by this firmware)

Requested in `liveRegisters` and decoded in `applyConnectedVictronValue()`.
Some are model-dependent — the **unknown-VREG diagnostic** (serial log
`[BLE] Victron unknown VREGs:` and the Power page) shows which your unit serves.

| VREG | Meaning | Encoding |
|---|---|---|
| `0x0100` | Product ID | u16 |
| `0x0102` | **Charger** firmware version* | hex-BCD, `0x017400` → v1.74 |
| `0x010A` | Serial number | ASCII |
| `0x010B` | Model name | ASCII |
| `0x0201` | Device state | u8 |
| `0xEDD5` | Battery voltage | u16 ×0.01 V |
| `0xEDD7` | Battery current | u16 ×0.1 A |
| `0xEDDA` | Charger error | u8 |
| `0xEDD3` / `0xEDD2` | Yield / max power **today** | u32 ×0.01 kWh / u32 W |
| `0xEDD1` / `0xEDD0` | Yield / max power **yesterday** | u32 ×0.01 kWh / u32 W |
| `0xEDEC` | Battery temperature* | u16 ×0.01 K − 273.15 (N/A w/o sensor) |
| `0xEDBC` / `0xEDBB` | Panel power / voltage | ×0.01 |
| `0xEDAD` / `0xEDA9` / `0xEDA8` | Load current / voltage / state | u16 ×0.1 A, u16 ×0.01 V, u8 |
| `0x104F` | Lifetime yield record | user + total yield (u32 ×0.01 kWh) |
| `0x1050`–`0x106E` | **31-day history** (age 0 = today) | 34-byte record, below |

*candidate on the 75/15; `0xEDBD` (panel current) is **rejected** by the 75/15 —
Huckleberry computes PV current as PV watts ÷ PV volts instead.

> **Note — two firmwares.** VictronConnect shows a *charger* firmware (e.g.
> v1.74, VREG `0x0102`) **and** a separate *SmartSolar Bluetooth* firmware (e.g.
> v2.52). The MPPT does **not** expose a standard GATT Device Information Service
> (`0x180A`) — its service list is `1800 / 1801 / 68c1… / 9758… / 306b…` only —
> so the BLE-module firmware is **not** readable over standard BLE; it lives
> behind Victron's proprietary protocol. Only the charger firmware is surfaced.

### Daily history record (VREG `0x1050+age`, 34 bytes)

| Offset | Field | Encoding |
|---|---|---|
| 1..4 | yield | u32 ×0.01 kWh |
| 5..8 | consumed (load) | u32 ×0.01 kWh |
| 9..10 | battery max | u16 ×0.01 V |
| 11..12 | battery min | u16 ×0.01 V |
| 14..17 | error codes | 4 × u8 |
| 18..19 | bulk minutes | u16 |
| 20..21 | absorption minutes | u16 |
| 22..23 | float minutes | u16 |
| 24..27 | peak power | u32 W |
| 28..29 | max battery current | u16 ×0.1 A |
| 30..31 | max PV voltage | u16 ×0.01 V |
| 32..33 | sequence number | u16 (`0xFFFF` = empty slot) |

---

## HTTP API

- `GET /api/state` — live snapshot. The `sol` object carries instant +
  last-connected fields (`serial`, `fw`, `yield`, `yieldYest`, `peakYest`,
  `battTemp`, `productId`, `model`, `unknownVregs`, …). `th.inside` is the
  derived inside/ambient temperature.
- `GET /api/victron/history` — streamed JSON:
  `{"days":[{"age","seq","yield","consumed","bmax","bmin","peak","imax","pvmax","bulk","abs","float","errors":[4]}, …]}`.

---

## Refresh cadence

- **Instant**: every advertisement (~1 Hz) during a low-duty passive scan window.
- **Connected**: first read ~3 min after boot (once Wi-Fi is stable), then
  **every 15 minutes** (15-min retry on failure). Each connected read re-fetches
  the live VREGs plus all 31 history days.

---

## Storage footprint (31 days)

- **On-device RAM**: history is a static array
  `VictronDay s_victronDays[31]` (`src/AppState.cpp`). `sizeof(VictronDay)`
  ≈ **44 bytes** (with 4-byte alignment) → **~1.36 KB RAM** for the full 31 days.
- **Not persisted** to flash/NVS — the charger keeps its own persistent history,
  so Huckleberry just re-reads it every 15 min.
- **Over-the-air**: history VREGs return 34 B each → **~1.05 KB** raw per full
  history fetch.
- **`/api/victron/history` JSON**: ~150 B/day → **~4.6 KB** streamed for 31 days.
- **NVS settings**: `victronKey` (16 B bindkey), `victronMac`, `victronPin`
  (6 digits).

---

## Adding Victron support to a clone

1. In Settings, enter the MPPT's **BLE MAC**, the **6-digit PIN**, and (for
   instant data) the **16-byte encryption key** from VictronConnect.
2. **Instant** data appears immediately from advertisements; **connected** data
   appears after the first 15-min connected read.
3. Watch the **unknown-VREG diagnostic** to see which candidate registers your
   specific model serves, and prune/extend `liveRegisters` accordingly.
4. Read-only by design: Huckleberry never issues `set`/restart/bootloader/erase
   VE.Direct commands.
