# Victron BLE Reverse Engineering (M6 stretch)

Working notes for the SmartSolar MPPT connected-BLE effort. **Update at the end
of every session.** This is intended to be resumable after gaps.

## Current source-of-truth overview (2026-07-29)

This section is the short, non-chronological reference. Later sections retain
the full reverse-engineering trail, including false starts that are explicitly
marked as superseded.

### Shipped in Huckleberry v0.5.5

| capability | implementation |
|------------|----------------|
| Passive live solar data | Decodes Victron Instant Readout advertisements with the configured 16-byte advertisement key. No connection or pairing is required for these broadcasts. |
| Authenticated connected reads | Uses BLE SMP passkey pairing with the charger's six-digit label PIN, then the `306b` VeSmart GATT service and CBOR/VREG messages. There is no extra app-layer login cipher. |
| Native 31-day history | Reads charger-owned daily records `0x1050..0x106E`, including yield, load, battery range, PV maximum, charge stages, and errors. |
| Native stored trends | Reads charger-owned intraday samples from the `0xEC00..0xEC5F` trend block, condenses them to 30-minute bins, and stores about 31 days in SPIFFS. No five-minute or live-history ring remains in RAM. |
| Power-page charts | Shows daily and click-a-day intraday charts, fixed left/right axes with a scrolling plot, hover/tap values, full-screen expansion, and synchronized older/newer day navigation. |
| Data exports | Streams daily-history, selected-day intraday, and all-intraday CSV without allocating a second complete history buffer. |
| SQL collection | A Windows-side collector pulls only charger-owned daily and intraday API data into `dbo.DailyHistory` and `dbo.IntradaySample`. The ESP32 never connects to SQL Server and no live-poll history is stored. |
| Reliability controls | Uses a passive 10% scan window, polls the EcoWorthy BMS once per minute, delays connected Victron work until Wi-Fi settles, performs bounded trend backfill, and normally refreshes native history every six hours. |
| Temperature display | Shows Victron battery temperature only when a real remote-temperature source is present. EcoWorthy temperature remains in the BMS section and is not presented as charger data. |
| VE.Smart external sense | Broadcasts fresh EcoWorthy voltage, probe temperature, and shunt current as authenticated VE.Smart Vsense/Tsense/Isense records. Live SmartSolar readback proves all three are accepted from Huckleberry. The ESP32 never writes charge parameters. |

The collector's Windows scheduled task still requires one administrator run of
`tools/victron-collector/register-task.ps1`. That task performs API ingestion;
it is not a SQL Server database-backup job. Existing SQL Server backup policy is
separate from Huckleberry.

### Proven protocol layers

| layer | proven behavior |
|-------|-----------------|
| Victron manufacturer data | BLE AD type `0xFF`, Victron company ID `0x02E1` encoded as bytes `E1 02`. |
| Instant Readout | Passive encrypted telemetry decoded in `src/BleManager.cpp`; it uses the per-device advertisement key stored by VictronConnect. |
| Connected authentication | Standard BLE encrypted/bonded link using the six-digit PIN. `97580003` changes the PIN; `97580006` is the PUK/reset path rather than a session-key challenge. |
| Connected application transport | VeSmart service `306b0001`; control `0002`, lastData `0003`, data `0004`; concatenated CBOR with credit/chunk flow control. |
| Connected values | Opcode `0x05` reads VREGs, `0x06` writes VREGs, `0x08` returns values, and `0x09` reports unavailable VREGs. Production history/trend work is read-only except the required transient trend AskData control write. |
| BLE firmware update | VictronConnect downloads XML/base64 BUP packages, concatenates their firmware blocks unchanged, and sends them through the BLE bootloader/DFU path. Smart Battery Sense packages additionally apply the recovered repeating XOR transport mask documented below. |
| VE.Smart receive acceptance | SmartSolar only queues legacy `ADV_NONCONN_IND` advertisements for this receiver. A type-1 CCM-valid sequence record registers the source; type-2 compact values then apply. Vsense/Tsense use priority 8; Isense is rejected below priority 12. |

### Future feature leads

- **VE.Smart provisioning:** VictronConnect's exact Create/Join/Leave writes are
  recovered and Huckleberry can read the selected ID/key/name from the charger.
  Full native recognition of Huckleberry as a configurable Smart Battery Sense
  remains a larger optional GATT identity/emulation layer.
- **VE.Smart stale-source validation:** source registration, live V/T/I
  acceptance, cadence, and sequence persistence are proven. A controlled
  stale-input test can still measure the exact charger fallback timeout.
- **Additional connected VREGs:** rejected VREG reporting and the extracted
  `vregs.json` make it inexpensive to investigate other charger-owned fields
  later without reintroducing continuous polling or RAM history.
- **Firmware tooling:** the app's public release catalog covers charger and BLE
  updater families. The reproducible BUP extractor and Ghidra projects make
  future protocol recovery possible without changing production firmware.

### Safety boundaries

- Stop any emulated VE.Smart sensor broadcast immediately when EcoWorthy voltage
  or temperature is unavailable, stale, or outside a validated range.
- Never advertise the no-data sentinels as measurements, and never synthesize a
  temperature when the battery probe is absent.
- Keep only one voltage/temperature sense source active in a VE.Smart network.
- Treat Isense as measurement input only. Never advertise charge targets,
  synchronized-charging control VREGs, or other write-equivalent control data.

## Calls, syntax, and specifications quick reference

### Huckleberry HTTP API

Base URL on the current device is `http://huckleberry.local`.

| method | path/body | result |
|--------|-----------|--------|
| `GET` | `/api/state` | Current display, Wi-Fi, battery, BLE, and passive Victron telemetry. Live fields are current state only and are not retained as history. |
| `GET` | `/api/victron/history` | `{"days":[...]}` with charger daily records. Day fields are `age`, `seq`, `yield`, `consumed`, `bmax`, `bmin`, `peak`, `imax`, `pvmax`, `bulk`, `abs`, `float`, `errors[4]`, and `intraday`. |
| `GET` | `/api/victron/day?age=N` | One stored intraday day, where `N=0..30`. Returns `age`, local `date` as `YYYYMMDD`, `intervalSeconds=1800`, `availableAges`, and samples containing `slot`, `ts`, `outA`, `pvV`, `pvW`, `tempC`, `battV`, and `chargeA` when available. |
| `GET` | `/api/victron/history.csv` | Streams all available native daily-history rows as CSV. |
| `GET` | `/api/victron/trends.csv?age=N` | Streams one stored intraday day as CSV; returns 404 if that day is not in SPIFFS. |
| `GET` | `/api/victron/trends.csv` | Streams every stored intraday day as one CSV. |
| `POST` | `/api/ble` form fields `bMac`, `vMac`, `vKey`, `vPin`, `gMac`, `en` | Saves BLE settings. `vKey` is 16 bytes as 32 hexadecimal digits; a non-empty `vPin` must be exactly six digits. Omitting/blanking `vPin` preserves the saved PIN. |
| `POST` | `/api/vs` form fields `name`, `id`, `key`, `en` | Saves the VE.Smart emulator network override and enable state. A blank key preserves the saved 16-byte network key. |
| `POST` | `/api/vs/read` | Queues a bounded connected read of the charger's VE.Smart network and accepted-source diagnostics. |

Useful direct calls:

```powershell
Invoke-RestMethod http://huckleberry.local/api/victron/history
Invoke-RestMethod 'http://huckleberry.local/api/victron/day?age=1'
Invoke-WebRequest http://huckleberry.local/api/victron/history.csv -OutFile daily.csv
Invoke-WebRequest 'http://huckleberry.local/api/victron/trends.csv?age=1' -OutFile intraday-day-1.csv
Invoke-WebRequest http://huckleberry.local/api/victron/trends.csv -OutFile intraday-all.csv
```

### Connected VeSmart BLE calls

The connected service is `306b0001-b081-4037-83dc-e59fcc3cdfd0`; its
characteristics are control `306b0002-...`, lastData `306b0003-...`, and data
`306b0004-...`. The test SmartSolar leaf instance was `3`, but clients must
discover it rather than hard-code it. Requests are written to lastData; the
device notifies lastData for final/small replies and data for intermediate
chunks.

The management/DFU service is `97580001-ddf1-48be-b73e-182664615d8e`:
`97580002-...` reads device information, `97580003-...` changes the six-digit
PIN, `97580004-...` accepts DFU commands, and `97580006-...` carries the
PUK-based PIN-reset challenge. Ordinary telemetry does not use that challenge.

Notation: `<cbor X>` means the normal CBOR encoding of X; all response messages
may be concatenated in one characteristic notification.

| bytes | direction | call |
|-------|-----------|------|
| `01` | client → device | `getDevices`; response opcode `02` contains the device list. |
| `03 <cbor instance>` | client → device | Subscribe to an instance. |
| `05 <cbor instance> <cbor array[vreg...]>` | client → device | Read one or more VREGs. |
| `06 <cbor instance> [<cbor vreg>, <cbor bytes>]` | client → device | Write a VREG value. Production uses this only for the transient stored-trend AskData request. |
| `07 ...` | device → client | Write accepted/result. |
| `08 <instance> <vreg> <byte-string>` | device → client | VREG value. |
| `09 <instance> <vreg> <signed reason>` | device → client | Unknown or unavailable VREG. |
| `0A <cbor instance>` | client → device | `getPathList`; acknowledged but not needed for this charger. |
| `0B <cbor instance> <cbor indices>` | client → device | `getPathValues`; not used by the working trend implementation. |

Control-characteristic flow control:

| bytes | meaning |
|-------|---------|
| `FA <size> FF` | Set CBOR chunk size; the proven probe used `FA 80 FF`. |
| `F9 <credits>` | Grant receive credits; the proven probe used `F9 80`, while the device grants transmit credit with values such as `F9 01`. |
| `F7 <u16 size LE> ...` | Device chunk/result announcement; large payloads are reassembled before CBOR parsing. |

### Daily and stored-trend VREG syntax

- Totals history is `0x104F`; daily records are `0x1050..0x106E`, where
  `0x1050` is today and the remaining registers are progressively older days.
- Trend discovery reads `0xEC5D` AvailableVregs, `0xEC52` TimeTuples,
  `0xEC5A` LastTimeRef, `0xEC4A+i` per-series metadata, and `0xEC5F` the active
  time tuple.
- AskData writes VREG `0xEC5B` with six bytes
  `[u8 seriesIndex][u32 timeRef LE][u8 maxSamples]`.
- The immediate same-session read of `0xEC5B` returns
  `[u8 indexEcho][u32 TrefReply LE][u8 SamplesInReply]`
  `[u16 intervalSeconds LE][typed samples...]`.
- Sample `i` maps to `TrefReply - i*intervalSeconds`; the active time tuple maps
  that reboot-relative value to Unix time.
- Fine tiers are `120×1 s`, `239×30 s`, `288×300 s`, and `2160×1800 s`.
- Production retains only 48 half-hour slots per stored day in
  `/victron-trends.bin`; raw fine-tier arrays are discarded immediately.

Trend series and scaling:

| index | VREG | wire type | engineering value | no-data |
|------:|-----:|-----------|-------------------|---------|
| 0 | `0xEC89` | `u8` | DC output current = raw × 0.1 A | `0xFF` |
| 1 | `0xEDBB` | `u16 LE` | PV voltage = raw × 0.01 V | `0xFFFF` |
| 2 | `0xEC8A` | `u16 LE` | PV power = raw W | `0xFFFF` |
| 3 | `0xEC88` | `i8` | Battery temperature = raw °C | `0x7F` |
| 4 | `0xED8D` | `i16 LE` | Battery voltage = raw × 0.01 V | `0x7FFF` |
| 5 | `0xED8F` | `i16 LE` | Charge current = raw × 0.1 A | `0x7FFF` |

### VE.Smart sensor advertisement syntax

`Networking::Core::setConfig` consumes a 16-byte network key and two-byte
network address. AES-128-CCM parameters are nonce length 13, tag length 4, L=2,
and no AAD.

```text
nonce = [u8 messageType]
        [u48 sequence LE]
        [u32 stable sourceAddress LE]
        [u16 networkAddress LE]

compact AD = [1E] [FF E1 02]
             [02] [networkAddress low byte]
             [u32 sourceAddress LE] [u32 sequence low bits LE]
             [13-byte ciphertext] [4-byte CCM tag]

sequence AD = [13] [FF E1 02]
              [01] [networkAddress low byte]
              [u32 sourceAddress LE] [u48 full sequence LE]
              [4-byte CCM tag for empty plaintext]
```

The first byte is the BLE AD-element length and therefore excludes itself.
Compact plaintext is exactly 13 bytes and uses little-endian VREG IDs:

```text
record A = [08]
           [0x0100 u16 LE] [04] [product 0x0000A3A5 u32 LE]
           [0xED8D u16 LE] [02] [battery voltage i16 LE, 0.01 V]

record B = [08]
           [0x0102 u16 LE] [04] [firmware 0x000115FF u32 LE]
           [0xEDEC u16 LE] [02] [battery temperature u16 LE, 0.01 K]

record C = [0C]
           [0xED8C u16 LE] [04] [battery current i32 LE, 0.001 A]
```

The first plaintext byte is the source-priority nibble. SmartSolar accepts
Vsense/Tsense at priority 8 but `FUN_000390f6` rejects Isense below priority 12,
so record C must begin with `0x0C`. `0xED8F` is the charger's local/trend charge
current (`i16`, 0.1 A); network Isense is `0xED8C` (`i32`, 0.001 A).

Huckleberry emits eight leading type-1 sequence records, then rotates
`sync -> A -> sync -> B -> sync -> C` at a 1200 ms dwell. Every packet gets a
fresh 48-bit sequence. Voltage no-data is `0x7FFF`, temperature no-data is
`0xFFFF`, and current no-data is `0x7FFFFFFF`; the safe behavior is to stop the
affected measurement instead of transmitting stale/no-data values.

The receiver only sees the exact legacy PDU used by Victron peers. NimBLE must
use non-connectable, non-discoverable advertising with scan response disabled:

```cpp
adv->setConnectableMode(BLE_GAP_CONN_MODE_NON);
adv->setDiscoverableMode(BLE_GAP_DISC_MODE_NON);
adv->enableScanResponse(false);
```

### VictronConnect Create, Join, and Leave syntax

The extracted `PageSettingsBleNetworking.qml`, `ProductVBusItems.qml`,
`Utils.js`, `item_list.json`, and `vregs.json` establish the complete
provisioning contract:

| path | VREG | type | purpose |
|------|------|------|---------|
| `/BleNetwork/NetworkId` | `0xEC12` | two-byte array | Network address/ID used by the CCM nonce and clear-header low byte. |
| `/BleNetwork/NetworkKey` | `0xEC13` | 16-byte array | AES-128 VE.Smart network key. |
| `/BleNetwork/NetworkName` | `0xEC14` | zero-terminated `string32` | Human-readable name; the UI limits creation to 30 characters. |
| `/BleNetwork/Transmitting/NrOfTransmittedVregs` | `0xEC15` | `u8` | Number of VREGs this product is broadcasting. |
| `/BleNetwork/Receiving/NrOfReceivedVregs` | `0xEC16` | `u8` | Number of unique VREGs received since startup. |
| `/BleNetwork/Rssi` | `0xEC42` | `u8` | VE.Smart network RSSI/status field. |

VictronConnect behavior:

1. **Create Network** generates 16 random key bytes and two random ID bytes
   (each generated byte is `0x00..0xFE`), retries the ID while it already exists
   in the phone database, then writes name, ID, and key to the connected product.
   It stores the same hex ID/key/name through `ProductDB.syncNetwork`.
2. **Join Existing** does not discover networks over BLE. It calls
   `ProductDB.getNetworks()`, displays networks previously saved by this
   VictronConnect installation, converts the selected hex ID/key back to byte
   arrays, then writes ID, key, and name to the connected product.
3. **Leave Network** writes `FF FF` to `0xEC12`, sixteen `FF` bytes to `0xEC13`,
   and an empty name to `0xEC14`. A byte array is considered configured if any
   byte differs from `0xFF`.
4. A five-second press on the network icon opens the 16-byte key as hex. The
   network ID is already displayed next to its name. This provides a practical
   manual handoff from a VictronConnect-created charger network to Huckleberry
   without emulating the full Smart Battery Sense configuration service.

For Huckleberry's first broadcaster, VictronConnect can therefore create the
network on the SmartSolar and remain responsible for its charger writes.
Huckleberry only needs local settings for the copied two-byte ID, 16-byte key,
and a stable source address. Making Huckleberry itself appear in VictronConnect
as a Smart Battery Sense would additionally require its product identity,
pairing/PIN behavior, connected `306b` service, and writable `0xEC12..0xEC14`
VREGs.

Offline packet checks:

```powershell
python tools/victron_re/vesmart_packet.py --self-test
python tools/victron_re/vesmart_packet.py `
  --key 00112233445566778899aabbccddeeff `
  --network-address 0x1234 --source-address 0x89abcdef `
  --sequence 1 --voltage 13.27 --temperature-c 24.5 --current-a -4.321
```

### Firmware and reverse-engineering calls

VictronConnect's release request is `GET
https://vrmapi.victronenergy.com/v2/firmwares` with JSON
`{"feedChannel":"release","victronConnectVersion":"6.33"}`. Download URLs and
MD5 values are returned in `remoteFirmwares.json`.

```powershell
python tools/victron_re/extract_bup.py --decode-ble input.bup decoded.bin

& 'C:\Users\Roto Router\.platformio\penv\Scripts\pio.exe' run -e huckleberry
& 'C:\Users\Roto Router\.platformio\penv\Scripts\pio.exe' run -e huckleberry_ota -t upload
```

Ghidra 12.1.2 uses JDK 21. Import the app or firmware once into a persistent
project with `-noanalysis`; subsequent `-process` calls must also include
`-noanalysis` unless a deliberately bounded analysis pass is being run.

Important VictronConnect 6.33 ARM64 entry points:

| address | function | relevance |
|---------|----------|-----------|
| `0x045B51D8` | `Networking::aes_ccm_encrypt` | Thin encryption entry; standard AES-128-CCM, four-byte tag. |
| `0x045B585C` | `Networking::Core::setConfig` | Stores the 16-byte VE.Smart network key and two-byte address. |
| `0x045B5E70` | `Networking::Core::getTxData` | Builds compact/type-1 Victron manufacturer advertisements. |
| `0x0489E980` | `TrendsManager::fetchData` | Creates and connects the trend items. |
| `0x048A9388` | `TrendsManager::updateData` | Reassembles and types trend replies. |
| `0x04863060` | `QuickConnectFromQR::decryptAesCcm` | QR credential path; not connected BLE authentication. |
| `0x048E4934` | `VeService::characteristicChanged` | Connected VeSmart notification handling. |
| `0x048ECDD0` | `VeSmartService::characteristicChanged` | Chunk/control/application dispatch. |

Important Smart Battery Sense v7 v1.15 functions after loading the decoded image
at Nordic application base `0x26000`:

| address | recovered role |
|---------|----------------|
| `0x0002EE50` | VREG provider for product `0x0100`, firmware `0x0102`, voltage `0xED8D`, and temperature `0xEDEC`. |
| `0x00030A24` | Converts voltage to signed 0.01 V and temperature to unsigned 0.01 K. |
| `0x00030EC4` | Calculates/clamps temperature, adds 27315 centi-kelvin, and also exposes integer °C on `0xEC88`. |
| `0x00031634` | Installs an advertising-payload callback; the directly observed caller builds Instant Readout, while VE.Smart uses an indirect/virtual path. |

### SQL collector calls

Run these from `tools/victron-collector/`:

```powershell
python collector.py --dry-run
python collector.py --once
python collector.py
.\register-task.ps1
```

`--dry-run` fetches and prints without writing SQL; `--once` is one ingestion
pass; invoking without either option loops at `poll_interval_seconds` (currently
1800). Task
registration requires Administrator PowerShell. The collector upserts only
`dbo.Device`, `dbo.DailyHistory`, and `dbo.IntradaySample`; it never writes the
legacy `dbo.LiveSample` table and never deletes history that has aged out of the
charger.

## Stored Trends spike (2026-07-28) — intraday history feasibility

Goal: can we get the VictronConnect **Trends** (intraday) charts onto the ESP32?

> **Current answer (v0.5.4): yes, implemented.** The definitive wire format is
> in “Final sample pull” below. Earlier path-protocol, firmware-2.53, EC5E, and
> live-probe passages are retained only as the chronological RE log and are
> superseded by the 2026-07-28 final findings.

- **YES, the data is on the device and BLE-readable.** VictronConnect's "Stored
  Trends" are held in the **non-volatile memory of the charger's Bluetooth
  module** (persists across power loss), read **only over BLE** (not VE.Direct
  serial). Supported on **SmartSolar chargers** (our 75/15), BMV-712,
  SmartShunts, Smart Battery Sense.
- **Resolution tiers** (charger): `2m@1s` (RAM only) · `2h@30s` · `1d@5m` ·
  **`45d@30m`**. The user's screenshot (~30-min buckets over a past day) is the
  `45d@30m` tier. So 31 days is well within device retention.
- **No public decoder** exists for the *connected* stored-trends fetch. All
  public work (keshavdv/victron-ble, birdie1, politi, ChrisJ7903, vvvrrooomm)
  decodes the **passive advertisement** or basic connected telemetry — not
  stored trends. This is original RE.
- **Mechanism**: ordinary VeSmart VREG opcode 5/6 traffic on the same encrypted,
  paired session as daily history. AskData and its transient reply both use
  `0xEC5B`; path enumeration is not involved.
- **Storage**: Huckleberry has no usable PSRAM in this board configuration.
  Production stores only compact 30-minute results in SPIFFS
  (`/victron-trends.bin`, about 27 KB for 31×48 bins). Native fine-resolution
  reply arrays are decoded immediately and discarded.

### Historical live probe + first results (removed before v0.5.2)

Added a **safe, HTTP-driven RE probe** (temporary scaffolding) that runs one
bounded request *inside* the proven `pollVictronConnected` session — no second
connection, reusing the 4 KB-capped capture + credit flow-control + clean
teardown. This is the fix for the old probe that wedged Wi-Fi (that one opened
its own connection). Controls:
- `POST /api/victron/probe?mode=1&start=0x1050&count=8` → VREG scan (opcode 5)
- `POST /api/victron/probe?mode=2` → getPathList (opcode 0x0A)
- `GET  /api/victron/probe` → last result as JSON (raw hex + heapBefore/After)
- Decoder: `scratchpad/vprobe.py` (drives probe + decodes `08 <inst> <vreg> <bytestr>`).
- Firmware formerly used `requestProbe`/`probeResult`/`runProbeIfPending` and
  `/api/victron/probe`; all of this scaffolding was removed before v0.5.2.

Results (heap stayed ~32–34 KB every time — no crash, Wi-Fi never dropped):
- **Validated** against known daily history: `0x1050/0x1051/0x1052` each return a
  34-byte record, decoded correctly.
- **getPathList (0x0A)** returned no path tree — only the subscription's periodic
  live pushes. This MPPT is **VREG-based, not path-based**; paths are a dead end.
- **Neighborhood scans**: `0x1040–0x104E` unknown (op9); `0x104F` = lifetime
  yield; daily block starts `0x1050` (our unit has only 3 days so far);
  `0x1070–0x108F` all unknown. **No trends register in these ranges.**
- **Undocumented pushed VREGs** seen in subscription stream: `0xED8C` (4-byte
  incrementing counter), `0xEC5A` (4-byte incrementing), `0xEC8A`, `0xED8D`,
  `0xED8F`, plus `0x010E`, `0x0200`, `0x0202`. Not trends, but worth cataloguing.
- **Conclusion**: stored trends (2,160 records) use a dedicated fetch mechanism,
  not a plain neighboring VREG. Finding it efficiently now needs **Route B (Ghidra
  dive)** — locate the trends-fetch function in the `.so` to learn its
  opcode/VREG/paging — then the live probe can confirm it on hardware.
- **Caveat**: Huckleberry's charger has only ~3 days of history right now
  (recently commissioned), so validating a trends decode will be limited until
  more days accumulate. (Mervyns' unit has 31 days but is a separate device.)

### Ghidra dive — TrendsManager found (2026-07-28)

Symbol/string mining of `libVictronConnect...so` (no analysis needed — C++
mangling embeds names) revealed the **entire stored-trends subsystem**: a class
**`TrendsManager`** driven over **VeQItem `/Trends/...` paths** (NOT plain
VREGs — which is exactly why the VREG scans found nothing).

Paths exchanged:
`/Trends/AskData` (request) · `/Trends/PushData` (device reply) ·
`/Trends/AvailableVregs` · `/Trends/SamplesInterval` · `/Trends/SamplesInReply`
(a.k.a. "Max Push Samples") · `/Trends/TimeTuple` · `/Trends/ActiveTimeTuple` ·
`/Trends/LastTimeRef` · `/Trends/TrefReply` · `/Trends/VregReply` ·
`/Trends/RebootList` · `/Trends/ClearData` · `{NumOfSubTrends, NumSamplesSubTrend,
TimeStepSubTrend}`.

Protocol flow (from `TrendsManager` method names):
1. `requestRebootList`/`onRebootList` — read `/Trends/RebootList` (timestamps are
   relative to device boot sessions, so you need the reboot list first).
2. `requestTimeTupleList`/`onTimeTuplesList` — available time ranges (tuples).
3. `onActiveTimeTuple`, `onAvailableVregs`, `getLastTimeRef` — config: which
   VREGs have trend data + the time reference.
4. `setMaxPushSamples` → `/Trends/SamplesInReply`.
5. `askData(u8 vreg-idx, u8 subtrend)` → writes `/Trends/AskData`.
6. Device pushes `/Trends/PushData`; `updateData`/`onPushTrendStateChanged` parse
   it. Samples are a **QByteArray → `convertByteArrayToVector<T>`** where T is
   per-VREG typed (i8/u8/i16/u16/i32/u32). Spacing = `/Trends/SamplesInterval`.
   Sub-trends chunk a VREG's series (`getSubTrends`, `checkStartSubTrends`).
- **Guards** (error strings): "Active time tuple is invalid, can't request
  trends"; "Tuple list is still empty…"; "Reboot list is still pending…"; "No
  available VeRegs for stored Trends".
- **Decompiled** (124 functions) and **persisted** to
  `../Pack Rat/apk-analysis/victron/ghidra/decompiled/trends/` — key TARGETs:
  `askData` (31 KB request builder), `requestTrends`, `updateData`,
  `onPushTrendStateChanged`, `onAvailableVregs`, `onActiveTimeTuple`,
  `getTimeTupleList`, and `convertByteArrayToVector<{i8,u8,i16,u16,i32,u32}>`.
  These confirm TrendsManager drives the flow via **VeQItem setValue/getValue on
  the `/Trends/*` paths**, and the pushed samples deserialize via `QDataStream`
  (`writeRawData`) into typed vectors.
- **`askData(u8 vregIdx, u8 subTrend)` payload decoded** (from the decompile):
  builds a `QDataStream` and serializes the vreg index, a raw time reference
  (`writeRawData`), and then the **actual VREG number** pulled from the
  available-vregs array (`this+0xc0`, an int[] filled from `/Trends/AvailableVregs`),
  and sends it via **`VeSmartDevice::setRequest(VeIf::Message)`**. So the trend
  transport is **VREG-based** (not the path/getPathList protocol, which the MPPT
  ignored in probes): a fixed "AskData" control VREG carries a payload naming the
  target data VREG + time window; the device replies on a "PushData" control VREG.
- **`VeSmartDevice` holds a `QHash<QString, QFlags<VregItemProperty>>`** — the
  path→VREG registry — and there is a `trend_vregs` key + `TrendsManager::
  {saveVregs, getTimestampsForVregId}`. The concrete **control-VREG numbers**
  (AskData / AvailableVregs / PushData / RebootList / TimeTuple) live in that
  registration, being resolved via `FindStringXrefs` on the `/Trends/*` strings.
- `fetchData` @ 0489e980 (references both `/Trends/AskData` and `/Trends/PushData`)
  creates the trend VeQItems via `VeQItem::itemGetOrCreate(root, "<base>/AskData"…)`
  and stores them at member offsets (this+0x1a8/0x1b0/0x1b8). So the items are
  **path-based**; their numeric VREG is resolved by the VeSmart producer, not here.
- `getTranslationFromVreg` @ 0467fa9c is a **name/localization helper** (compares
  "venus_network_version", "cbor-list", …) — NOT the control-VREG table. Dead end.

### Historical handoff (superseded by final EC5B decode)

**Conclusively established this session:**
- Intraday trends ARE on the charger (BT-module NVM, 45d@30min) and BLE-readable.
- Handled by `TrendsManager` over `/Trends/*` VeQItem paths; full method/flow map
  above; `askData` payload = QDataStream(vregIdx, timeRef, actual-VREG-from-
  AvailableVregs) → `VeSmartDevice::setRequest`; samples = typed QByteArray
  (`convertByteArrayToVector<T>`), chunked into sub-trends.
- A **safe live probe** is in firmware (`/api/victron/probe`) — never crashes Wi-Fi.

**The one remaining unknown blocking implementation:** the concrete **control-VREG
numbers** (which VREG carries AskData / PushData / AvailableVregs / TimeTuple /
RebootList) and the **PushData chunk framing**. These live in the `VeSmartService`
producer that maps these path VeQItems to VREG wire messages — NOT yet decompiled.

**Next session, do exactly this:**
1. Decompile the `VeSmartService`/`VeSmartDevice` producer for these items — target
   `VeSmartDevice::setRequest`, the `VregItemProperty` QHash population, and
   whatever consumes `this+0x1a8/0x1b0/0x1b8`. The literal VREG numbers are there.
2. With the AskData/PushData VREGs known, use the **existing probe** (add a mode to
   write the AskData VREG payload and capture PushData) to confirm on hardware.
3. Then build the ESP32 state machine + storage + `/api/victron/day` + charts + a
   SQL `IntradaySample` table.

**Caveats:** our test charger has only ~3 days of history, limiting validation;
and the probe scaffolding in `src/BleManager.cpp`/`WebApp.cpp` must be removed
before a real release (currently uncommitted).

### Historical false wall (superseded; fw 2.52 works)

Confirmed on the wire (via the probe) that trends use the **path protocol**
(`setPathValue`/`getPathValues` by numeric index; opcodes `0x0A` getPathList /
`0x0B` getPathValues), the app pulls **on demand** when Trends is opened (user
confirmed), and transfer is credit-flow-controlled + chunked.

**The empirical wall:** this charger (charger fw 1.74 / **BT fw 2.52**) answers
`getPathList` (`0a <inst>`) with a **control-channel `f7 <inst> 00`** ack but
returns an **empty path tree** — no `newPath`/path entries on any of the three
characteristics. Without the path list we can't get the trend path indices →
can't form the `AskData` write → can't pull data. Observed also: VREG values
arrive on **lastData (0003)**, not data (0004); control (0002) carries `f9`
(credits) and `f7` (chunk/result).

**Most likely cause:** the stored-trends BLE path tree isn't populated at BT fw
**2.52**; **2.53 is available** for this unit. Or there's an on-demand trigger
sequence the app sends that we haven't replicated.

**Do next (in priority order):**
1. **Update the charger's Bluetooth firmware 2.52 → 2.53** in VictronConnect, then
   re-run `POST /api/victron/probe?mode=4&raw=0a03` — if a path tree appears, the
   rest (getPathValues the indices → AskData → collect chunked PushData → parse
   typed samples → store → charts → SQL) is a straightforward build.
2. If still empty, **BLE-sniff VictronConnect opening Trends** (nRF Sniffer / an
   Android HCI snoop log) for the exact request bytes — that's ground truth.
3. Decode control opcode **`0xf7`** fully (`VeSmartService::writeCborChunkSize`,
   `writeReadyToReceive` @ `scratchpad/trends_wire/`).
4. Confirm what the screenshot app "StockTralr" is — if it self-logs rather than
   reading device trends, 2.52 may genuinely not serve them.

### Historical probe recipe (not present in release firmware)

Temporary diagnostic in `src/BleManager.cpp` (`requestProbe`, `requestProbeRaw`,
`runProbeIfPending`, `runProbeEarlyIfPending`, `s_probeCaptureAll` in the notify
CB) + two routes in `src/WebApp.cpp` (`/api/victron/probe`). Runs INSIDE the
proven `pollVictronConnected` session (never crashes Wi-Fi). Driver/decoder:
`scratchpad/vprobe.py`. Modes:
- `POST ?mode=1&start=0x1050&count=8` — VREG scan (opcode 5).
- `POST ?mode=2` — getPathList (0x0A), data-char capture.
- `POST ?mode=3&raw=<hex>` — write arbitrary request bytes (late in session).
- `POST ?mode=4&raw=<hex>` — write bytes right after subscribe + **capture all 3
  characteristics tagged** `C0`=control/`1D`=lastData/`DA`=data, len-prefixed.
- `GET /api/victron/probe` — last result (raw hex + heap).
**Remove all of it before cutting a release** (keep this doc as the recipe).

### COMPLETE trends protocol (extracted from Ghidra, 2026-07-28)

**Transport** — VeSmart GATT service `306b0001`, chars: control `0002`, lastData
`0003` (carries value/app messages *and* is where VREG value pushes arrive),
data `0004`. All app messages are CBOR, **opcode-first**.

**App message opcodes** (outgoing req / incoming resp):
| op | direction | meaning |
|----|-----------|---------|
| `0x01` | → | getDevices (resp: opcode 2 device list) |
| `0x03` | → | subscribe(instance) |
| `0x05` | → | getValues(instance, [vregs]) |
| `0x08` | ← | itemValue(instance, vreg, bytes) — a VREG value |
| `0x09` | ← | unknownVreg |
| `0x0A` | → | **getPathList(instance)** |
| `0x0B` | → | **getPathValues(instance, [indices])** |
| — | ← | **NewPath(instance, index, pathString)** — defines a path's wire index |
| — | ← | **PathValue(instance, index, QVariant)** — a path's value |
| — | → | **setPathValue(instance, index, value)** — write a path |

**Chunk flow-control** (on control char `0002`, handled by
`VeSmartService::processControlData`): `0xfa <size> 0xff` = writeCborChunkSize;
`0xf9 <credits>` = writeReadyToReceive (grant N receive credits, app re-sends when
its running total crosses 0x40); `0xf7 <u16 size> …` = device chunk announcement.
On any inbound control frame the app answers `writeCborChunkSize(0x80)` +
`writeReadyToReceive(0x80)` (= `fa 80 ff` + `f9 80`) to keep large transfers
flowing. Large messages are reassembled in `writeChunkToStack`.

**Trends sequence** (`TrendsManager`): items live under a device base path as
`/Trends/{AskData, PushData, AvailableVregs, SamplesInterval, SamplesInReply
(MaxPushSamples), TimeTuple, ActiveTimeTuple, LastTimeRef, RebootList,
NumOfSubTrends}`. Flow: read RebootList → TimeTuple list → ActiveTimeTuple →
AvailableVregs → LastTimeRef (all `getPathValues` by index) → write
`AskData` via `setPathValue`, payload = `QDataStream`(BigEndian) of
`{u8 vregIdx, rawTimeRef, u32 actualVreg-from-AvailableVregs}` → device streams
`PushData` chunks → parse each VREG's samples with
`convertByteArrayToVector<T>` (T per-vreg: i8/u8/i16/u16/i32/u32), spaced by
`SamplesInterval` (30 min), chunked into sub-trends. **Path indices are learned
from `NewPath` messages the device sends** (populate the producer's
path→index map; `PathItemProducer::requestValue` queues unknown paths pending).

### The empirical gap (what's blocking the last step)

Implemented the flow-control ack (fa/f9) in the probe and re-ran
`getPathList`: the device now handshakes on control (`f7`↔`f9`, 26 frames) but
still emits **only `0x08` VREG value pushes on lastData — zero `NewPath`
messages**, so no path→index map, so no way to form the getPathValues/AskData
requests. i.e. this unit (charger fw 1.74 / **BT fw 2.52**) does not surface its
VeQItem path tree over BLE in response to getPathList.

**Remaining unknowns (need one of):**
1. **BT fw 2.53** may gate path-tree/trends exposure — update it and re-run
   `POST /api/victron/probe?mode=4&raw=0a03`; watch for non-`08` frames.
2. **A BLE HCI sniff** of VictronConnect (or StockTralr) opening Trends on this
   charger = ground-truth byte sequence (the exact trigger + the NewPath frames).
   This is now the highest-value next step — everything else is mapped.
3. Decompile who drives `getPathList`/flushes the pending-path set on the
   producer (vtable-dispatched; xrefs are `COMPUTED_CALL` via vtables
   `04f93758`/`04f93768`) to find the precise discovery trigger the app uses.

### Definitive on-hardware result (2026-07-28)

Confirmed the pull is **parameterized** (per-day), not a blanket dataset — so
tested both path-protocol requests with correct chunk flow-control:
- `0x0A` getPathList (enumerate), instance 3 and 0 → device replies control
  `f7 03 00`, **no PathList/NewPath**, only `0x08` value pushes + `f9 01` credit
  grants.
- `0x0B` getPathValues by index [0..15], instance 3 and 0 → **identical**
  `f7 03 00`, no PathValue responses.

So this charger (**BT fw 2.52**) acknowledges path-protocol messages but returns
**no path tree/values under any request** — it does not expose the VeQItem path
layer (hence no trends) over BLE at this firmware. This is now a device/firmware
fact, not an RE gap: the protocol is fully mapped above; the unit won't serve it.
**Resolution = BT fw 2.53 (available) or a BLE sniff of the app.** No further
software change on our side can extract data the charger isn't sending.

### CORRECTION / BREAKTHROUGH — trends ARE VREG-based at fw 2.52 (2026-07-28)

The path-protocol conclusion above was WRONG. Trends are served over plain
**VREG reads (opcode 5)** — the VeSmartService maps the `/Trends/*` VeQItem paths
to a **VREG block around `0xEC4x`–`0xEC5x`**. Found by scanning that range with
the existing probe (mode 1). No path protocol, no fw 2.53 needed. Confirmed
registers (SmartSolar 75/15, fw 1.74):
- `0xEC52`/`0xEC53` → 32 bytes = **4× uint32 LE timestamps** (uptime-seconds,
  reboot-relative): `0x00049ace 0x00049a56 0x00047e50 0x00032c1c` then `ffffffff`
  padding → the **time-tuple / reboot boundaries**.
- `0xEC5A` → 4 bytes = a single uint32 timestamp (LastTimeRef / now).
- `0xEC4A`/`0xEC4B` → 34-byte structured records (`04 3878 0001 00ef 001e 0020
  012c 0170 0808 07 ff…`) = per-series **trend metadata/config**.
- `0xEC41` → `ffffffff`.
Full `0xEC00–0xEC5F` map in `scratchpad/trendmap.txt` (0xEC60+ = unknown/op9).
**Decoded trend registers (all read via getValues opcode 5):**
| VREG | bytes | meaning |
|------|-------|---------|
| `0xEC5D` | 16 | **AvailableVregs** = uint16 LE list of trend series: `0xEC89, 0xEDBB(PVv), 0xEC8A, 0xEC88, 0xED8D, 0xED8F` (ffff-padded) |
| `0xEC52`–`57` | 32 | **TimeTuples** = 4× uint32 LE uptime-sec boundaries (mirror regs) |
| `0xEC5A` | 4 | **LastTimeRef** = uint32 uptime-sec "now" (live-increments each read) |
| `0xEC4A`–`4F` | 34 | per-series **metadata** (one per available vreg; contains `1e`=30-min interval) |
| `0xEC30`–`3C` | 33 | **sample pages** (mostly 0xff — only ~3 days of data yet) |
| `0xEC5F` | 8 | config (`e00d0000 0736646a`) |
| `0xEC01`/`02` | 2/4 | counts/config (`0300`, `0174`) |

### ★ COMPLETE WORKING TREND-PULL PROTOCOL (VREG-based, fw 2.52) ★

All over the VeSmart connected session (same as the 31-day history). Opcodes:
**`0x05` = getValues (read)**, **`0x06` = setValues (write)**. Message framing:
- read:  `05 <cbor inst> <cbor-array of vregs>`  → responses `08 <inst> <vreg> <bytestr>`
- write: `06 <cbor inst> [<cbor vreg>, <cbor bytestr>]`  → result `07 …`

**Discovery reads:**
- `0xEC5D` → AvailableVregs: uint16 LE list of series (here `EC89, EDBB(PVv),
  EC8A, EC88, ED8D, ED8F`, ffff-terminated).
- `0xEC52` → TimeTuples: 4× uint32 LE uptime-sec boundaries.
- `0xEC5A` → LastTimeRef (uint32 uptime-sec "now", live).
- `0xEC4A+i` → per-series metadata (34 B, contains 30-min interval).

**Final sample pull — verified on the live fw-2.52 charger:**
1. **WRITE `0xEC5B`** via setValues with the 6-byte AskData payload
   `[u8 seriesIndex][u32 timeRef LE][u8 maxSamples]`. The device returns opcode
   `0x07` accepted.
2. **READ `0xEC5B` immediately in the same connected session breath.** The
   byte-string is the complete transient reply cluster:
   `[u8 indexEcho][u32 TrefReply LE][u8 SamplesInReply]`
   `[u16 SamplesIntervalSeconds LE][typed samples…]`.

Example, series 1 at tref 216000 requesting 28 samples:
`01 c04b0300 1c 0807 ...`; this means index 1, tref 216000, 28 samples,
1800-second interval, followed by 28 little-endian u16 values. The earlier
`0xEC5E`/12-byte result was a descriptor-like dead end caused by the wrong
payload model. `0xEC5B` is the production path.

`trend_config` (`0xEC4A+i`) decodes as:
`[u8 NumOfSubTrends][u8 MaxPushSamples]`, followed by eight
`[u16 NumSamples LE][u16 TimeStepSeconds LE]` pairs. This charger advertises four
active tiers for every series: 120×1 second, 239×30 seconds, 288×300 seconds,
and 2160×1800 seconds (45 days). MaxPush is 56 for byte-valued series and 28
for 16-bit series.

Series order and typed decoding:

| index | VREG | value | no-data |
|---:|---:|---|---|
| 0 | `0xEC89` | unsigned u8 × 0.1 A, DC output current | `0xFF` |
| 1 | `0xEDBB` | unsigned u16 LE × 0.01 V, PV voltage | `0xFFFF` |
| 2 | `0xEC8A` | unsigned u16 LE W, PV power | `0xFFFF` |
| 3 | `0xEC88` | signed i8 °C, battery temperature | `0x7F` |
| 4 | `0xED8D` | signed i16 LE × 0.01 V, battery voltage | `0x7FFF` |
| 5 | `0xED8F` | signed i16 LE × 0.1 A, charge current | `0x7FFF` |

Samples run backward from `TrefReply`: sample `i` has
`sampleTref = TrefReply - i * SamplesIntervalSeconds`. `0xEC5F` is the active
time tuple `[u32 tupleTref LE][u32 UnixEpoch LE]`; therefore
`sampleEpoch = tupleEpoch + sampleTref - tupleTref`. Live values from all six
series matched VictronConnect and the charger's current telemetry.

**Implementation status:** complete in Huckleberry v0.5.4. Each transient reply
is decoded immediately. Fine tiers are averaged into half-hour bins in one
small per-series accumulator, then written to `/victron-trends.bin` on SPIFFS.
No raw 1-second, 30-second, or 5-minute arrays are retained in RAM. The API is
`GET /api/victron/day?age=0..30`; each response also lists `availableAges` from
the persistent SPIFFS store, so navigation and SQL backfill do not depend on the
volatile connected-read history after a reboot. CSV downloads are streamed by
`GET /api/victron/history.csv`, `GET /api/victron/trends.csv?age=0..30`, and
`GET /api/victron/trends.csv` (all stored intraday days). The Power page displays
all stored charts for the selected daily-history bar. Their plot areas scroll
without moving the left/right axes, mouse hover and touch reveal exact values,
and an Expand/Close control opens each chart full-screen on desktop and mobile.
Expanded charts add a centered older/newer day navigator that updates all stored
charts; all 48 intraday bins fit at once. The SQL collector
upserts only charger-owned records into `dbo.DailyHistory` and
`dbo.IntradaySample`; it does not store live poll snapshots. Connected reads run
after Wi-Fi stabilization in bounded three-series groups every 15 minutes until
the oldest available daily-history day is covered, then no more often than
every six hours.

### VE.Smart external-sense emulator (broadcast-only proven; native pairing optional)

Implemented as a **broadcast-only VE.Smart external-sense peer**. Huckleberry
advertises EcoWorthy voltage, probe temperature, and shunt current into a
VictronConnect-created VE.Smart network. It never uses advertisements to send
charge targets, never connects to push measurements, and never writes charger
configuration or charge parameters.

**Settings (NVS namespace `huck`), never reuse the Instant Readout key:**
`vsEn` (enable), `vsId` (network ID, 4 hex digits = 2 bytes), `vsKey` (network
key, 32 hex digits = 16 bytes; never returned by the API), `vsName` (label only,
not transmitted), `vsSrc` (stable u32 source address, derived once from the MAC).
The 48-bit anti-replay sequence lives in its own namespace `huckvs` key `seq`.

**Auto-adopt network from the charger (primary path):** the VE.Smart network is
created on the SmartSolar in VictronConnect, so Huckleberry reads it back over the
PIN-authenticated connected session — `readVsNetworkFromCharger()` reads `0xEC12`
NetworkId, `0xEC13` NetworkKey, `0xEC14` NetworkName during `pollVictronConnected`.
`vsApplyChargerNetwork()` then adopts id/name (and the 16-byte key when `0xEC13`
is readable) into settings idempotently. This removes the manual key copy: the
user just creates the network on the charger. Whether `0xEC13` is readable is
charger/firmware dependent; if not, the manual override fields remain the fallback.
`POST /api/vs/read` forces an immediate connected read; otherwise adoption happens
on each scheduled connected read.

**Isense (current) is proven.** Official SmartShunt BLE firmware shows that local
`0xED8F` (`i16`, 0.1 A) is converted to network `0xED8C` (`i32`, 0.001 A), with
`0x7FFFFFFF` as no-data. SmartSolar's receiver knows `0xED8C` as a four-byte VREG,
but its current-specific apply function `FUN_000390f6` rejects compact priority
classes below 12. Huckleberry therefore sends V/T at class 8 and current at class
12. Product identity is not consulted by the per-field receive dispatcher, so the
existing Smart Battery Sense product record `0xA3A5` can remain while the current
tuple accurately represents a shunt measurement. Live readback reports the
Huckleberry source for all three senses.

**Endpoints / API:** `POST /api/vs` fields `name`, `id`, `key`, `en` (all validated
server-side; blank `key` preserves the saved one). `POST /api/vs/read` queues a
charger network read. `GET /api/state` `ble` object adds `vsEnabled`, `vsId`,
`vsName`, `vsKeySet` (key withheld), `vsBattFresh`, `vsSolFresh`, `vsBroadcasting`,
`vsSrcV`, `vsSrcT`, `vsSrcA`, and charger-read status `vsChargerRead`,
`vsChargerIdOk`, `vsChargerKeyReadable`, `vsChargerId`, `vsChargerName`. The web
card also reports charger acceptance for Vsense, Tsense, and Isense. It appears
in `#ble` (below Battery and Solar) only while both `vsBattFresh` and
`vsSolFresh` are true; settings inputs never wipe a value being typed.

**Advertising lifecycle** (`src/BleManager.cpp`, `src/VeSmartEmu.cpp`): a fresh
session starts with eight type-1 sequence-sync packets, then rotates
`sync -> identity/V -> sync -> status/T -> sync -> current/I` at a 1200 ms dwell,
one fresh sequence per packet. The crucial radio contract is legacy
`ADV_NONCONN_IND`: non-connectable, non-discoverable, scan response disabled.
Connectable (`ADV_IND`) and scannable (`ADV_SCAN_IND`) variants were both visible
to generic scanners but discarded before SmartSolar's VE.Smart receiver callback.
Advertising pauses before connected trend reads and whenever BLE is disabled.

**Sequence persistence:** the working 48-bit sequence runs in RAM but a
high-water mark is reserved a block (`VS_SEQ_BLOCK = 2048`) at a time and written
to NVS *before* those numbers are used. A reboot resumes from the reserved mark,
so recently transmitted sequence numbers can never be reused; NVS is written only
once per block (~1/hour at this cadence), not per advertisement.

**Safety / freshness:** broadcasting requires `vsEnabled`, a valid ID+key, and
battery voltage that is fresh (`battLastMs` within 90 s) and encodable.
Temperature is included only when the BMS reports a real probe; if no probe is
present, the temperature phase becomes another sync rather than synthesizing a
value. Current is included only while the BMS current is valid. Broadcasting
pauses immediately if required battery data goes stale/invalid; `0x7FFF`,
`0xFFFF`, and `0x7FFFFFFF` no-data sentinels are never transmitted.
The card visibility also requires the charger to be seen within 60 s
(`solLastMs`). Freshness uses live timestamps under `teleLock()`, not the
persistent `valid` flags.

**Confirmed live test results (SmartSolar 75/15, BLE firmware 2.52):** offline
vectors match `tools/victron_re/vesmart_packet.py`; the ESP32 verifies the
charger's own CCM traffic with the adopted key; and charger diagnostics report
Huckleberry source `0xAF5E8B08` as accepted for Vsense class 8, Tsense class 8,
and Isense class 12. Connected readback returned plausible live values. No
additional VictronConnect sense-enable option was required. Direct
VictronConnect pairing of Huckleberry is not supported or needed.

**Remaining native-GATT pairing work:** to make VictronConnect list and configure
Huckleberry itself as a Smart Battery Sense would additionally require product
identity `0x0000A3A5`, SMP pairing/PIN, the connected `306b` service, and writable
network VREGs `0xEC12..0xEC14` (NetworkId/Key/Name). Not implemented and out of
scope for the broadcaster.

The target architecture is now confirmed: Huckleberry should be a
**broadcast-only VE.Smart sensor peer**, not a process that periodically writes
temperature or voltage registers into the SmartSolar. Huckleberry reads the
EcoWorthy/JBD battery voltage and probe temperature, encrypts them as a
VE.Smart-compatible sensor advertisement, and lets the SmartSolar consume those
broadcasts. VictronConnect remains responsible for creating/selecting the
VE.Smart Network and configuring the SmartSolar to join it.

Victron's supported workflow is to create the network on a Smart Battery Sense,
BMV, or SmartShunt first and then use VictronConnect's **Join Existing** action on
the charger. Smart Battery Sense transmits Vsense and Tsense; BMV-712 and
SmartShunt can also transmit Isense. The SmartSolar uses received voltage and
temperature to alter charging behavior, so stale or invalid emulated values must
stop transmitting immediately. Only one battery-sense source should be active in
the network; Victron applies a source-priority rule if multiple sources appear.

- https://www.victronenergy.com/media/pg/VE.Smart_Networking/en/step-by-step-instructions.html
- https://www.victronenergy.com/media/pg/VE.Smart_Networking/en/ve-smart-networking-product-compatibility.html
- https://www.victronenergy.com/media/pg/VE.Smart_Networking/en/voltage%2C-temperature-and-current-sense---further-details.html
- https://www.victronenergy.com/media/pg/VE.Smart_Networking/en/limitations.html

There are two provisioning scopes. A broadcast-only emulator needs the same
network ID/key as the charger. The extracted QML proves the app can create the
network on the charger, displays the two-byte ID, and reveals the 16-byte key
after a five-second press on the network icon; Huckleberry can accept those
values in its own settings. `JOIN EXISTING` is a selection from VictronConnect's
local `ProductDB` networks table, not over-air discovery. Making VictronConnect
itself recognize and configure Huckleberry as a native Smart Battery Sense would
additionally require emulating product identity, pairing, the connected `306b`
service, and writable network VREGs `0xEC12..0xEC14`. That larger compatibility
layer is optional and must not be conflated with writing sensor measurements to
the charger.

Firmware and decompiler evidence now pins the sender format:

- VictronConnect keeps distinct `ble-firmware`, `vedirect-firmware`,
  `vebus-firmware`, `xup-firmware`, and `vup-firmware` updater paths. Its public
  catalog request is `GET https://vrmapi.victronenergy.com/v2/firmwares` with
  `{"feedChannel":"release","victronConnectVersion":"6.33"}`; the response is
  cached as `remoteFirmwares.json`. This is the authoritative charger and BLE
  firmware source used by the app.
- The release catalog contains Smart Battery Sense v1.15 for product `0xA3A4`
  (`smartbatterysense_v3_v1.15.bup`, MD5
  `5a7e8e33b250fbf66f2da1766742b64e`) and product `0xA3A5`
  (`smartbatterysense_v7_v1.15.bup`, MD5
  `5f772f46bbe911fbf897dc90c562dfde`). A BUP is XML containing base64 firmware
  blocks. VictronConnect concatenates and sends those blocks unchanged.
- BLE BUP payloads use a repeating 32-byte XOR mask
  `f6ecd9b367cf9e3d7af4e8d1a3478e1d3b76eddbb66ddab56ad5ab57af5ebd7b`.
  Removing it yields valid Nordic images: v3 is 84,580 bytes; v7 is 84,716
  bytes, uses the S132 v7 application base `0x26000`, and has reset vector
  `0x0002BE8D`. `tools/victron_re/extract_bup.py --decode-ble` reproduces this.
- Both decoded images end with two exact 12-byte `Networking::Core::TxMessage`
  records. Record A is `ffffffff080000018ded0000`; record B is
  `ffffffff08000201eced0000`. The layout is `{u32 address, u8 compactSource,
  u8 pad, u16 vreg1, u16 vreg2, u16 vreg3}`. Address `0xFFFFFFFF` selects compact
  message type 2 and compact source `8`.
- Firmware VREG providers confirm record A carries product ID `0x0100` as four
  bytes plus battery voltage `0xED8D` as signed 0.01 V. Record B carries firmware
  version `0x0102` as four bytes plus battery temperature `0xEDEC` as unsigned
  0.01 K. The v7 identity values are product `0x0000A3A5` and firmware
  `0x000115FF`. Voltage no-data is `0x7FFF`; temperature no-data is `0xFFFF`.
- `Networking::Core::getTxData` produces manufacturer AD elements for company
  ID `0x02E1`. Each compact plaintext is exactly 13 bytes:
  `[08][u16 VREG LE][u8 len][value]...`. AES-128-CCM uses a 13-byte nonce
  `[type][u48 sequence LE][u32 source LE][u16 network address LE]`, no AAD, and
  a 4-byte tag. The clear data header contains type, the network-address low
  byte, source address, and sequence low 32 bits. After both compact records,
  message type 1 publishes the full six-byte sequence with an empty plaintext
  and CCM tag.
- `tools/victron_re/vesmart_packet.py` now builds and decrypts this exact packet
  cycle offline. It does not access BLE or enable a transmitter.

The remaining blockers are provisioning and live behavioral validation. A
broadcast-only emulator needs the configured network's 16-byte key and 2-byte
address. Huckleberry can accept them in settings, while native VictronConnect
pairing would require emulating Smart Battery Sense identity and configuration
GATT services. A known-network capture or cautious charger acceptance test must
still confirm packet cadence, sequence persistence, source identity behavior,
and stale-data timeout. Until then, v0.5.4 omits the Victron battery-temperature
row/chart when no real remote source is present and transmits no emulated
charge-control data.

Device under test: a **SmartSolar MPPT 75/15** (identifiers redacted), reached at
its random-static BLE address. Live at RSSI ~-45 to -47.

## What's already shipped (working today)

Huckleberry passively decodes the encrypted **Instant Readout** BLE
advertisements. See `src/BleManager.cpp:45` `decryptVictron`.

- Uses `mbedtls_aes_crypt_ctr` with the 16-byte hex `victronKey`.
- Manufacturer data layout: `[CID 0x02E1][0x10][model][0xA0][type][nonce_lo][nonce_hi][key_check][ciphertext...]`.
- Product-type byte `0x01` = solar charger (only case handled).
- Yields: state, error, battery V/A, PV W, yield today, load A, and RSSI.
  Huckleberry samples these advertisements in short passive scan bursts.
- The Victron code uses AES-CCM (see "Decompilation findings" below: L=2,
  M=4, 13-byte nonce) but the community historically decodes with plain
  AES-CTR because the MAC is skipped. Both give the same plaintext for our
  purposes. (Earlier notes said "L=4, M=2" — that was a guess; the compiled
  B0 flags byte 0x09 proves L=2, M=4.)

## What we want that we don't have

Connected mode. Specifically: the coarse ~30-day history the MPPT stores
internally, and the ability to poll registers on demand (product ID, exact
model, firmware version, etc.). Access requires the 6-digit PIN.

**Shipped 2026-07-26**: authenticated connected mode now reads device metadata,
extended live VREGs, and the charger's persistent daily history. The web app
charts the native 31-day records; no five-minute samples are retained in RAM.

## Probe findings (2026-07-25)

Two probes were run against the MPPT via NimBLE GATT (see `runVictronProbe`
in `src/BleManager.cpp`). Both used address type **`BLE_ADDR_RANDOM`** — the
first probe tried `BLE_ADDR_PUBLIC` and failed to connect at all. The random
address type is required.

### Services + characteristics enumerated

```
SVC 0x1800  (Generic Access)
  0x2a00 [R]  "<device name>"          device name
  0x2a01 [R]  0000                       appearance
  0x2a04 [R]  0600060000009001           preferred conn params
  0x2aa6 [R]  01                         central addr resolution

SVC 0x1801  (Generic Attribute)
  0x2a05 [I]  service changed

SVC 68c10001-b17f-4d3a-a290-34ad6499937c   (unknown / secondary channel)
  68c10002-...   [WN]
  68c10003-...   [w]

SVC 97580001-ddf1-48be-b73e-182664615d8e   (device info / PIN management / DFU)
  97580002-...   [R]      16-byte record, mostly 0xff prefixed
  97580003-...   [WN]     change Bluetooth PIN (ASCII six digits)
  97580004-...   [W]      DFU command
  97580006-...   [RWN]    PUK-based PIN reset challenge

SVC 306b0001-b081-4037-83dc-e59fcc3cdfd0   (VeSmart connected-data channel)
  306b0002-...   [RwN]    flow-control capabilities
  306b0003-...   [wN]     final CBOR chunk / small request
  306b0004-...   [wN]     intermediate CBOR chunks
```

### Initial nonce observation (superseded as an authentication theory)
`97580006` returns different bytes on each read:
- Probe 1: `d56b70168c3f0340`
- Probe 2: `daf889988ec01133`

That's a **freshly-generated 8-byte nonce**, not a data value.

### Behavior with unauthenticated writes
- All writes to writable characteristics on `9758…` and `68c1…` returned
  `write=FAIL`, regardless of write-with-response or write-no-response.
- All notify subscribes reported `subscribe=ok` locally, but **zero
  notifications** were received in a 3s listen window — implying the CCCD
  write also silently fails or the device only notifies once authenticated.
- VE.Direct HEX `:154\n` ping was rejected on every writable target.

### Interpretation (superseded — incorrect)
**Historical dead end; do not implement this flow.**
The MPPT allows **unauthenticated BLE connection** (no bonding / no PIN
pairing at the BLE layer) but enforces authentication at the **application
layer**:
1. Client reads a fresh nonce from `97580006`.
2. Client computes `f(PIN, nonce)` — algorithm unknown.
3. Client writes the response back to `97580006`.
4. Only then are commands accepted on `97580003`/`97580004` and
   notifications delivered.

BLE-layer PIN pairing is NOT needed. This is a nice property — no bonding
storage, no LE-Secure-Connections dance. The whole handshake is in
application-layer messages.

### Corrected interpretation (2026-07-26)
The MPPT permits an unencrypted connection and service discovery, but protected
CCCD and characteristic writes require normal **BLE SMP passkey pairing and
bonding**. VictronConnect explicitly waits for the operating system's Bluetooth
pairing dialog and enters the six-digit label PIN. There is no PIN-to-AES
application handshake.

`97580006` belongs to the PUK reset flow: the app reads eight random bytes,
appends the PUK, computes the reset checksum, and writes a three-byte response.
It is not used for routine login. The prior probes failed because their links
were neither encrypted nor authenticated.

## Symbol mining from libVictronConnect_arm64-v8a.so (79 MB Qt/C++ binary)

Strings dump: `../Pack Rat/apk-analysis/victron/reports/libVictronConnect_arm64-v8a.so.strings.txt`

### Named function symbols with high signal

Crypto primitives:
- `Networking::aes_ccm_encrypt(cipher_fn, ..., Nonce&, ...)`
- `Networking::aes_ccm_decrypt(...)`
- `Encryption::generic_aes_ccm_encrypt<4, 2, 0>` — template params are
  <M=4, L=2, adata=0> (tag=4 bytes, length-field=2 bytes → nonce=13 bytes).
  Confirmed by decompilation, see below.
- `Encryption::generic_aes_ccm_decrypt<4, 2, 0>` — matching decrypt

VE.Direct HEX protocol (full command set present):
- `VeDirectHex::sendPing()`
- `VeDirectHex::get(uint16_t reg)`
- `VeDirectHex::set(uint16_t reg, const void*, size_t)`
- `VeDirectHex::getAppVersion()` / `getDeviceId()`
- `VeDirectHex::sendRestart()` / `sendEnterBoot()` / `sendErase()` / `sendProgram()`
- `VeDirectHex::buildMessage(InputCommands, ...)`
- `VeDirectHex::hexRxEvent(uint8_t)` — RX byte handler
- `vedirect::HexProto::msgSetNonce(HexMsg&, vector<uint8_t>&)`
- `vedirect::HexProto::setNonce(vector<uint8_t>&, bool)`
- `vedirect::Firmware::setNonce(string)` / `getNonce()`

**Best Ghidra targets (small, self-contained):**
- `QuickConnectFromQR::decryptAesCcm(QString, QString, QString)` — 3 args, likely `(key, ciphertext, nonce)`. Small wrapper; fastest window into the algorithm.
- `QuickConnectFromQR::encryptAesCcm(QString, QString, QString, bool)` — 4th bool is probably `padding` or `raw-output`.
- `PinManager::encryptPin(QString)` / `decryptPin(QString)` — PIN → storage form.
- `Networking::aes_ccm_encrypt` — nonce construction is here.

Storage:
- `ProductDb::KnownProductEncryptedPin` — PIN lookup table entries.
- `SimpleCrypt::encryptToByteArray` / `decryptToByteArray` — Qt SimpleCrypt (known algorithm, PIN-at-rest wrapping).
- `QKeychain::javax.crypto.Cipher.ENCRYPT_MODE` — Android Keychain integration.

BLE stack:
- `QtVeBleStack`, `VeBleInterface`, `VeBleStorageInterface`
- Uses Qt Bluetooth (`QBluetoothLocalDevice`, `QBluetoothDeviceDiscoveryAgent`)
- No detailed method symbols exported for these classes — probably only their
  constructors and a few slots leaked to the dynamic symbol table; the rest
  will need Ghidra's static analysis to recover.

Signals (Boost.Signals2):
- `signal<void(const vedirect::HexMsg&)>` slots exist — that's the
  "HEX message received" fanout. In connected mode we'd hook into this after
  the handshake to receive responses.

### Not present (confirms scope)
- No symbols matching `hmac`, `sha256`, `pbkdf2`, `bcrypt`, `argon`. So the
  PIN → key derivation is probably a small custom step, not standard PBKDF.
- No obvious `SecureChannel` / `Handshake` / `Session` classes named — the
  handshake is probably ad-hoc inline code in whatever class calls
  `Networking::aes_ccm_encrypt` with the nonce.

## Community landscape

- **[keshavdv/victron-ble](https://github.com/keshavdv/victron-ble)** — passive Instant Readout decoder in Python. Same scope as our current code.
- **[Fabian-Schmidt/esphome-victron_ble](https://github.com/Fabian-Schmidt/esphome-victron_ble)** — ESPHome components. `victron_ble` (passive) and `victron_ble_connect` (SmartShunt only, uses "ble_passkey" but for a different device; not applicable to MPPTs).
- **[Olen/VictronConnect](https://github.com/Olen/VictronConnect)** — "Decompiling the VictronConnect app". Author explicitly notes Victron kept BLE closed. Repo has a Phoenix inverter script that reads unauthenticated data — does **not** cover authenticated MPPT.
- **[KinDR007/VictronMPPT-ESPHOME](https://github.com/KinDR007/VictronMPPT-ESPHOME)** / **[syssi/esphome-victron-vedirect](https://github.com/syssi/esphome-victron-vedirect)** — VE.Direct over the **physical serial dongle** (UART), not BLE.

**Conclusion**: no public reversal of the authenticated MPPT BLE handshake
exists. We're breaking new ground.

## Ghidra plan

Binary: `../Pack Rat/apk-analysis/victron/arm64-apk/lib/arm64-v8a/libVictronConnect_arm64-v8a.so` (79 MB, ARM64 ELF, Qt/C++).

**Not** running full auto-analysis (would be 2-6 hours). Instead:
1. Install JDK 21 (Temurin) + Ghidra 12.x (546 MB zip).
2. Create Ghidra project, import .so with **default loader, no auto-analysis**.
3. Run only the analyzers needed for decompilation: Auto Analyze -> unchecked
   most, keep: Disassemble Entry Points, Function Start Search, Stack, ASCII
   Strings, Demangler GNU. Skip: Decompiler Parameter ID, Aggressive Instr
   Finder, Non-Returning Functions Discovered, Reference (heavy).
4. Symbol table -> jump to `QuickConnectFromQR::decryptAesCcm` first.
   Ghidra's decompile-on-demand will handle just that function's neighborhood.
5. Read decompiled C, capture algorithm.
6. Verify against `Networking::aes_ccm_encrypt` — the nonce structure there
   should show how the 8-byte GATT nonce becomes the AES-CCM 13-byte nonce.
7. Reimplement in `src/BleManager.cpp` using mbedTLS's `mbedtls_ccm_*` API.

## Reimplementation notes (superseded)

**Historical dead end; the PIN is a BLE SMP passkey, not an AES key source.**

- **mbedTLS provides**: `mbedtls_ccm_init/setkey/encrypt_and_tag/auth_decrypt`. Already in the ESP32 IDF. No new dependency.
- **Target flow on ESP32**:
  1. Connect to MPPT (random address).
  2. Read 8-byte nonce from `97580006`.
  3. Compute AES-CCM(key = derived_from_PIN, nonce = expand(nonce8), plaintext = handshake_msg).
  4. Write response to `97580006`.
  5. Subscribe to `97580003` (notify).
  6. Send VE.Direct HEX queries on `97580004` (or `97580003`), collect responses.
- **PIN storage**: reuse the existing web-only `victronKey` slot? No — that's
  the 16-byte broadcast key. Add a new setting `victronPin` (6-digit string),
  stored in NVS, entered from the BLE tab.
- **Do NOT** implement `set` / `restart` / `enter boot loader` / `erase` /
  `program` commands. Read-only queries only.

## Current connected-data implementation

- `victronPin` is a separate six-digit secret stored in NVS. The state API only
  exposes whether a valid PIN is saved, never the PIN itself.
- NimBLE uses keyboard-only I/O, bonding, and MITM protection. The passkey is
  injected from the setting when the MPPT requests it.
- The diagnostic connects with the random address type, calls
  `secureConnection(false)`, verifies encryption, subscribes to `306b0002`,
  `306b0003`, and `306b0004`, then reads the control characteristic.
- The client then writes `FA 80 FF` (CBOR chunk configuration) and `F9 80`
  (receive credit), waits for the charger's `F9 01` send credit, and only then
  sends a request. These bytes are confirmed from `writeCborChunkSize` and
  `writeReadyToReceive` ARM instructions, not guessed.
- The probe first sends standard CBOR unsigned integer `1` (`0x01`) on
  `306b0003`, VictronConnect's read-only `getDevices` command. After decoding
  the returned instance/parent pairs, it requests the first non-root device's
  compressed read-only path list with `10 + deviceId`. Intermediate response
  chunks arrive on `306b0004`; the final chunk arrives on `306b0003`.
- No PIN-change, PUK-reset, DFU, settings, restart, or charger-control command is
  implemented.

Known read-only request opcodes recovered from `VeSmartService`: `1`
getDevices, `3 + deviceId` subscribe, `5 + deviceId + item-list` getValues,
`10 + deviceId` getPathList, and `11 + deviceId + path-list` getPathValues.
The first successful hardware response was `02 9f 00 00 01 00 03 01 ff`:
response opcode `2`, followed by the indefinite array of instance/parent pairs
`(0,0)`, `(1,0)`, and `(3,1)`. `VeSmartDevice::getChildMap` confirms the pair
order: the first value is the instance and the second is its parent.

## Decompilation findings (2026-07-25 night, Ghidra 12.1.2)

Decompiled the crypto cluster headlessly (see "Ghidra workflow" below).
Raw decompiled C is archived in the scratchpad output dirs; the algorithm is
captured here.

### 1. The CCM primitive — fully pinned down

`Networking::aes_ccm_encrypt` is a thin tail-call into
`Encryption::generic_aes_ccm_encrypt<4,2,0>`, which is **textbook AES-128-CCM**:

- **AES-128** block cipher (key baked into a `cipher_fn(ctx, in16, out16)`).
- **Nonce = 13 bytes**, **L = 2** (2-byte message-length field), **M = 4**
  (4-byte tag), **no associated data**.
- Proven by the compiled flags bytes: **B0 flags = `0x09`** (Adata=0,
  M'=(4-2)/2=1, L'=L-1=1) and **CTR flags = `0x01`** (L'=1).
- B0 block = `[0x09] [nonce(13)] [len_be(2)]`. CBC-MAC over B0 + message
  blocks; CTR encryption with A_i = `[0x01] [nonce(13)] [ctr_be(2)]`;
  tag = `CBC_MAC[0..3] XOR S0[0..3]` (4 bytes).
- This is standard `mbedtls_ccm_encrypt_and_tag` / `mbedtls_ccm_auth_decrypt`
  with `tag_len = 4`. **No custom crypto** — reimplementation is trivial once
  we know key + nonce.

`QuickConnectFromQR::decryptAesCcm(key, nonce, ciphertext)` is a generic CCM
utility for the QR "quick connect" feature. It hex-decodes **key → 16 bytes**,
**nonce → 13 bytes**, splits the **last 4 bytes as the tag**, and runs the same
CCM. In the QR path key+nonce arrive pre-formed (the QR carries them), so this
function does NOT reveal any PIN→key or nonce-expansion step. It only confirms
the CCM parameters.

### 2. `Networking::Core` = the encrypted **advertisement** builder (NOT the connected handshake)

Big course-correction. The `Networking::Core` / `aes_ccm` cluster is the code
that builds/parses the **Instant Readout advertisement broadcast**, i.e. the
same data our passive decoder already handles — not the connected GATT
handshake. Evidence: the `Core` ctor seeds `obj+0x60 = [len,0xFF,0xE1,0x02]`
and `getTxData` writes the same header, where **`0x02E1` is Victron's BLE
company ID** — this is the manufacturer-specific-data header, not a GATT frame.

`Networking::Core` object layout (from ctor @ `0x045b576c` + setConfig):
- `+0x20` RadioInterface\*, `+0x28` SeqNrProvider\*, `+0x30` cipher_fn (AES
  block-encrypt), `+0x4c..0x5b` **16-byte key**, `+0x5c..0x5d` 2-byte address.
- `setConfig(key[16], addr[2])` stores key@+0x4c, addr@+0x5c, then
  `activateConfig()` (AES key schedule).

Advertisement **nonce (13 bytes)**, reconstructed from `getTxData` @ `0x045b5e70`:
```
nonce[0]      = message-type byte (1 = keep-alive/empty, 2 or 3 = data record)
nonce[1..6]   = 6-byte sequence number from SeqNrProvider
nonce[7..10]  = 4-byte source address  (Core+0x08)
nonce[11..12] = 2-byte dest/config address (Core+0x5c, set by setConfig)
```
`getTxData` calls `aes_ccm_encrypt(cipher_fn@+0x30, key@+0x4c, nonce, payload,
len, tag_out)`. The 16-byte key comes from
`VeBleInterface::getStoredAdvertisementKey(mac[6], key[16])` — i.e. **the same
bindkey we already store as `victronKey`.** So the advertisement is genuinely
AES-CCM with a seqnr nonce; our AES-CTR passive decode is the tag-skipping
approximation, as suspected.

`Networking::Nonce` and `SeqNrProvider` have **no standalone symbols** (fully
inlined) — the layout above (from `getTxData`) is the definitive source.

### 3. `PinManager` = storage only

`PinManager::encryptPin/decryptPin` just wrap `SimpleCrypt::encrypt/decrypt`
(Qt SimpleCrypt, PIN-at-rest). Not key derivation.

### 4. Earlier connected-mode conclusion (superseded)

**Historical dead end; later `VeService`/`VeSmartService` analysis corrected it.**

The connected history readout does **not** go through `Networking::Core`. The
connected path is the `9758…` VE.Direct-over-BLE service with the 8-byte
random challenge on `97580006`, and it is handled by **`VeSmartDevice` /
`VeBleDevice`** (Qt classes, GATT wiring). Those are where PIN's role and the
8-byte-challenge → session-key/nonce mapping live. They are decompiled and
archived (VeSmartDevice ctor ~105 KB C, VeBleDevice ctor ~32 KB C) but **not
yet analyzed**. **This is the next-session target.**

Because of this, the BLE PIN is **not yet actionable** — we don't have the
connected handshake algorithm to test it against. Hold the PIN until the
`VeSmartDevice` analysis produces a candidate key/nonce derivation.

### 5. Final connected-mode conclusion

Routine authentication is BLE SMP pairing with the six-digit label PIN. The
`9758` service handles device information, PIN changes, DFU, and PUK reset. The
`306b` VeSmart service carries concatenated CBOR requests and responses after
the BLE link is encrypted. CCM belongs to advertisement and QR-related paths,
not this GATT login.

## Ghidra workflow (reproducible, headless, no GUI)

JDK 21 at `C:\Program Files\Eclipse Adoptium\jdk-21.0.11.10-hotspot`.
Ghidra 12.1.2 at `C:\Users\Roto Router\ghidra-tools\ghidra_12.1.2_PUBLIC`.
Scripts + runners in the session scratchpad (`scratchpad/ghidra-scripts`,
`scratchpad/run_*.ps1`). Key gotchas learned:

- Ghidra 12 **removed Jython**; scripts must be Java (or PyGhidra). We use a
  Java `GhidraScript` `DumpVictronFuncs.java`.
- Import once with **`-noanalysis`** to a **persistent project** (`run_import.ps1`),
  then iterate with `-process … -noanalysis` (`run_dump.ps1`). **`-process`
  without `-noanalysis` triggers a full multi-hour auto-analysis** — always
  pass `-noanalysis` on the process step too.
- The postscript finds targets by **mangled-name substring** (C++ mangling
  embeds identifiers literally, e.g. `13decryptAesCcm`), disassembles +
  `createFunction` on demand, decompiles the target + its direct callees one
  level deep, and writes C files. Target substrings are passed as a script arg
  (cmd.exe splits bare commas into separate args — the script handles both).
- Re-dump example: `run_dump.ps1 out4 "VeSmartDevice,VeBleDevice"`.
- **Limit of the -noanalysis approach:** small/medium functions decompile fine,
  but **large Qt functions (e.g. `VeSmartService::characteristicChanged`,
  `getCharacteristics`) time out the decompiler** — with no analysis, callees
  are undefined and the decompiler recursively analyses each one, exploding.
  Once one decompile times out it tends to wedge the subprocess (restart it).
  Fix: run a **bounded auto-analysis** first (`run_analyze.ps1`, uses
  `-analysisTimeoutPerFile 1800` to cap at 30 min) so functions + callees are
  defined and named; then decompiles are fast. This is the prerequisite for the
  connected-mode (`VeSmartService`) work.
- `DumpVictronFuncs.java` also supports **callee-only mode** (env
  `DUMP_CALLEES_ONLY=1`): writes each target's resolved call-target list and
  skips decompilation. Only reliable once analysis has defined the bodies.

## Session log

**2026-07-29 — VE.Smart Vsense, Tsense, and Isense accepted live; receiver contract complete.**

- **Root cause of the original V/T rejection:** SmartSolar BLE firmware
  `FUN_00034068` only forwards legacy advertising PDU type 2
  (`ADV_NONCONN_IND`) into `FUN_00029322`. Generic scanners saw Huckleberry's
  connectable/scannable experiments, but the VE.Smart receiver never did. Exact
  NimBLE settings are non-connectable + non-discoverable + no scan response.
- **Live V/T proof:** charger diagnostics reported two in-range products,
  Huckleberry source `0xAF5E8B08`, priority class 8, and fresh accepted voltage
  and temperature. No separate VictronConnect "sense input" switch or config
  VREG was needed; the earlier `obj+0x3a`/`obj+0x1c` guard hypothesis was a dead
  end once the radio PDU prefilter was found.
- **EC20/EC30/EC31 diagnostics:** `0xEC20` supplies the accepted VREG/source
  tuples and is the reliable source-attribution fallback. Firmware/product and
  age/class data are exposed through the related network diagnostic blocks;
  parser differences on `0xEC31` no longer hide a valid accepted source.
- **Official SmartShunt firmware:** downloaded
  `smartshunt52_v7_ble_v2.52.bup`, verified MD5
  `a91ef544b3dca70df82c8f2f8a809195`, decoded to a valid nRF52 image, and loaded
  at base `0x26000` in `scratchpad/ghidra-smartshunt/ss`.
  `FUN_000377a2` converts local `0xED8F` (`i16` 0.1 A) to network `0xED8C`
  (`i32` mA); `FUN_0003770e` exposes `0xED8C` as a four-byte network value.
- **Isense acceptance gate:** SmartSolar `FUN_000390f6` rejects current records
  whose compact priority nibble is below 12. Huckleberry now emits current as
  `[0C][8C ED][04][i32 mA LE]`, while V/T remain class 8. Live charger readback
  then reported `currentAccepted=1`, class 12, age 3, source `0xAF5E8B08`, and a
  plausible selected current value. The receiver's per-VREG dispatch does not
  require a SmartShunt product identity.
- **Final cadence:** eight leading type-1 syncs, then
  `sync -> V -> sync -> T -> sync -> I`, 1200 ms per advertisement. Connected
  charger reads temporarily pause advertising and restart with the sync burst.
- **Release cleanup:** removed the temporary `/api/vs/debug` and
  `/api/vs/validate` routes, raw packet captures, and CCM brute-force validator.
  The production API never returns the VE.Smart network key; normal connected
  diagnostics retain only accepted-source/status fields used by the settings UI.
- **Firmware artifacts:** SmartSolar receiver project
  `scratchpad/ghidra-smartsolar/ss`; current path decompiles under
  `scratchpad/ghidra-smartsolar/isense`; SmartShunt provider project
  `scratchpad/ghidra-smartshunt/ss` and decompiles under
  `scratchpad/ghidra-smartshunt/isense`. The encrypted main-controller DUP was
  not needed for this BLE-network contract.

**2026-07-29 — Historical unresolved checkpoint (superseded by the completed result above).**

All results below are on real hardware (SmartSolar 75/15 + ESP32).

- **Our packets are byte-perfect and radiating well.** nRF Connect (at 250 ft!) shows the
  connectable device "Huckleberry" emitting rotating Victron `0x02E1` records: type-1 sync
  + type-2 compact V/T, netId `0x88`, our source `0x33B3B668`. TX power is already max
  (`ESP_PWR_LVL_P9`); the −90 dBm RSSI was purely the 250 ft distance.
- **Network key + netId are PROVEN correct (round-trip).** New on-device validator
  `ble::vsValidateKey()` / `GET /api/vs/validate` CCM-verifies the *charger's own* broadcast
  with the key we read from `0xEC13`: `{"verified":1,"verifiedFrom":"sync",
  "keyEqualsInstantReadoutKey":0,"chargerRecordTypesSeen":[1,2]}`. Decrypting the charger's
  real traffic is definitive proof that key + netId + nonce + CCM are all correct, and that
  we did **not** grab the per-device Instant Readout key by mistake.
- **The charger is an active network member.** It broadcasts type-1 (sync) + type-2
  (compact). Its compact record (source `0xa0faea02`) decodes to VREG `0x2001` = **13.50 V**
  (its own terminal voltage) + VREG `0x2007` (u32 counter). So the charger *shares its own
  status* on the `0x20xx` regs — distinct from the sense-INPUT regs `0xED8D`/`0xEDEC` that a
  Battery Sense provides. No direct register conflict; the charger just isn't consuming ours.

**Conclusion:** nothing is wrong with what Huckleberry transmits. The SmartSolar is in the
network, holds the right key, hears us (proven radiating), yet does not consume our
`0xED8D`/`0xEDEC` sense. The blocker is entirely charger-side (config/behavior).

**Ruled-out / dead-end paths (do not re-litigate):**
- Packet format / nonce / CCM / netId / key — all proven correct via round-trip decode of
  the charger's own traffic.
- TX not radiating — disproven (nRF Connect sees it clearly at 250 ft).
- Weak signal — disproven (max power; −90 dBm was distance).
- Wrong key (Instant Readout vs network) — disproven (`keyEqualsInstantReadoutKey:0`).
- Isense/current record causing rejection — removed; no change (still correct to omit).
- Sync too infrequent — added an 8-packet up-front sync burst + sync-every-other rotation;
  no change (keep it anyway; it's correct behavior).
- Non-connectable PDU — switched to connectable+scannable+named "Huckleberry"; no change
  (keep: matches a real sensor and makes it visible to scanners).
- Network name — irrelevant (RX matches the numeric netId + key, never the name).
- Rename / recreate network / let Victron "join" Huckleberry — no benefit: both directions
  only require a shared netId+key (which we have, verified), and "join" would need a full
  connectable Victron GATT-server impersonation (306b service, pairing) we don't have.

**Open hypotheses (charger-side, UNRESOLVED):**
1. **Sense INPUT not enabled on the SmartSolar (most likely).** The RX handler
   `FUN_00029322` is gated on `obj+0x3a != 0` and `obj+0x1c != 0`. These plausibly map to
   "Battery voltage sense" / "temperature sense" being enabled to *consume* network sense —
   a VictronConnect setting separate from "VE.Smart networking = on". If off, incoming
   `0xED8D`/`0xEDEC` are decrypted-then-ignored (or not processed).
2. **Source-registration timing.** Type-2 is dropped until a type-1 sync from our source is
   CCM-verified; our adverts are sparse/irregular (dual-role: 500–1500 ms, interrupted by
   the ESP32's own scan + battery connection) vs a real Sense's fast continuous advertising.
   Lower probability over minutes, but real.
3. **Arbitration / single-source.** Charger prefers its own measurement or another source
   (user reports none, and never locked).

**Next-method options (ranked):**
- **RE the guard flags `obj+0x3a` / `obj+0x1c`** in `FUN_00029322`'s object: find what
  writes them → identifies the exact VictronConnect setting (or a writable config VREG) that
  turns on sense *consumption*. Highest-value RE path. Ghidra project ready at
  `scratchpad/ghidra-smartsolar/ss`; see the RX-handler decode below.
- **Toggle sense in VictronConnect**: SmartSolar → Settings → Battery → set "Battery voltage
  sense" and "Battery temperature sense" to On/Network. Simplest possible test.
- **Reduce dual-role advert starvation**: give advertising dedicated continuous airtime
  (suspend the ESP32's own Victron passive scan while broadcasting, or shorten the advert
  interval), to guarantee the type-1 sync lands during a charger scan window.
- **Write the enable VREG over the PIN connection** (Huckleberry already has connected GATT
  access) — ONLY if RE finds a safe config VREG. NOTE: this crosses the project's
  broadcast-only / never-write-config boundary; get explicit sign-off first.
- **Ask Victron community/support**: "What must be enabled on a SmartSolar MPPT for it to
  *consume* external VE.Smart voltage/temperature sense, beyond joining the network?"

Diagnostics/tools added: `/api/vs/validate` (round-trip key proof), `/api/vs/debug` now
includes `rxSync`/`rxCompact`/`rxTypeMask` (charger's captured records);
`tools/victron_re/FindSvcAndCallers.java`, `ResolvePointers.java`.

**2026-07-28 (later) — Removed Isense; obtained the receiver (SmartSolar) firmware to RE the accept path.**
- Emulator now rotates only identity(V) → status(T) → sequence-sync, byte-identical
  to a real Smart Battery Sense; the `0xED8F` current record was removed (a Battery
  Sense has no shunt — current is a SmartShunt/BMV field). Both envs rebuilt, OTA
  flashed; on-air captures decode byte-perfect against the charger's own key and
  `advActive=1`, yet the SmartSolar still shows nothing received. The remaining
  unknown is receiver-side, so we pulled the receiver's firmware.
- **Victron firmware catalog:** `POST/GET https://vrmapi.victronenergy.com/v2/firmwares`
  with body `{"feedChannel":"release","victronConnectVersion":"6.33"}` returns 454
  products with presigned S3 `downloadUrl`s (time-limited). The BLE/VE.Smart stack is
  a **shared "52" module firmware** across the Smart line:
  `smartsolar52_v7_ble_v2.53.bup` (`0xA197`/`0xA19B`, the SmartSolar's BLE interface =
  our **receiver**), `smartshunt52_v7_ble_v2.52.bup` (`0xA19C`/`0xA19D`, the Isense
  **sender**), `smartbmv52_v7_ble`, `smartinverter52_v7_ble`. One image likely holds
  both RX-accept and Isense-TX paths. (`v7` = nRF52/S132, same as the Battery Sense;
  a `v1` nRF51 variant `smartsolar52_ble_v2.52` exists as `0xA191`/`0xA195`.)
- **Decode:** unlike the Battery Sense, this BUP is **not** XOR-masked — the plain
  `extract_bup.py` transport payload *is* the image. 114312 bytes, valid Nordic
  vector table at offset 0 (`sp=0x2000fff8`, `reset=0x0002d895`), load base `0x26000`.
  Saved as `scratchpad/firmware/smartsolar/smartsolar52_v7_ble_v2.53.decoded.bin`.
- **Recon (byte search, +0x26000):** company id `E1 02` at `0x2d3fb`, `0x3cebf`;
  sense VREGs `0xED8D` (5×, incl. `0x40e30`/`0x41e08` tables + `0x3c108`/`0x3c46c`
  code), `0xEDEC` (`0x40e84`/`0x41e20`), `0xED8F` (`0x3c480`/`0x40e3c`); network
  VREGs `0xEC12/13/14` consecutively at `0x3be20` (config table). Strings confirm
  the shared image ("SmartSolar", "SmartBMV", "SmartShunt", "Smart Dongle").
- Imported to Ghidra project `scratchpad/ghidra-smartsolar/ss` (`ARM:LE:32:Cortex`,
  base `0x26000`, BinaryLoader); bounded auto-analysis done. `FindConstantReferences`
  (`0x02E1`/`0xED8D`/`0xEDEC`/`0xED8F`/`0xEC13`/`0xEC12`) → `scratchpad/ghidra-smartsolar/refs/`.
- **Findings so far (receiver image):**
  - **TX advert builders:** `FUN_000328b0` and `FUN_0003e8c0` both set company `0x02E1`
    and build two ≤31-byte adverts via the serializer twins `FUN_0002d3d6` /
    `FUN_0003ce9a`. `FUN_000328b0` builds the manufacturer element `04 FF E1 02 10 …`
    (record type `0x10` = Instant Readout) and appends a **record-provider callback**
    (`*DAT_00032ab4`) that returns the encrypted VE.Smart payload + its length. These
    are the charger's *own* broadcasts, not the receive path.
  - **Networking object layout** (`FUN_00033b90`, the `0xEC12/13/14` write handler):
    per-instance base `p`, **netId `p+0x5fc` (2 B)**, **key `p+0x5e4` (16 B)**, **name
    `p+0x5c4` (≤32 B)** + name-len `p+0x5fe`; every write calls `FUN_0003381c(p+0x604)`
    ("config changed"). Writes are first offered to three sub-handlers at `p+0x50`,
    `p+0xe4`, `p+0x174` (vtable method `+4`).
  - `FUN_00038838` = VREG→width table: `0xED8D`/`0xEDEC`/`0xED8F` → 2 bytes, `0x100`/
    `0x102` → 4; so the sense regs are first-class known regs.
  - The RX company-ID check is a **byte compare** (`e1`,`02`), so it did not surface in
    the 16-bit-constant search — the scan/decode function is still unlocated.
- **RX trace (Networking subsystem mapped; per-advert decrypt line not yet pinned).**
  Tooling: `FindConstantReferences --operands-only` on config struct offsets;
  `tools/victron_re/FindSvcAndCallers.java` (new) enumerates every SVC + dumps
  wrapper/callers. Outputs under `scratchpad/ghidra-smartsolar/{cfgusers,rxusers,svc}/`.
  - **Networking object layout** (base `p`): **shadow** cfg `p+0x5c4…+0x603` (name/…,
    key@`+0x5e4`, netId@`+0x5fc`), **active** cfg `p+0x3c4…+0x403` (key@`+0x3e4`,
    netId@`+0x3fc`); `FUN_00033cf8` commits shadow→active; ctor `FUN_000339f8` loads a
    0x200-byte blob from flash (`FUN_0002cf4c`) and arms a periodic timer (`+0x604/+0x60c`,
    handler `DAT_00033af8`). Config persists to flash.
  - **Stored-networks table:** `FUN_00033cf8` iterates entries `p+0x404 … p+0x5c4` in
    **0x40-byte strides (~7 slots)**, comparing each with `FUN_00033cb6` — the device
    holds a *list of joined networks*, not just one.
  - **Per-source sense tracker:** `FUN_0003387c` constructs an array of source slots
    **initialised to no-data sentinels** (`0x7f00`, `0xffff`, `0xff`/`0x7f`) — the
    receiver tracks sense per source and arbitrates among them (single-source selection).
  - **TX scheduler** `FUN_000396b4` (state `p+0xe6`) encodes outgoing records with the
    active key at `p+0x3c8`. TX builders `FUN_000328b0`/`FUN_0003e8c0`; serializers
    `FUN_0002d3d6`/`FUN_0003ce9a`; adv-start = GAP SVC `0x6d`.
  - **Crypto:** no AES tables / CCM-peripheral constants → AES is `sd_ecb_block_encrypt`
    (SVC). Its exact SVC number isn't yet isolated (0x3c/0x3d are softdevice-mgmt, not
    ECB; the ECB SVC + its CCM caller are the next hop). Full SVC map saved at
    `scratchpad/ghidra-smartsolar/svc/svc_summary.txt`.
  - **Interim conclusion:** nothing in the packet *format* explains rejection — the sense
    regs are first-class and the subsystem is built to accept exactly what we send. The
    likely gate is **receive-side state/arbitration** (is the charger scanning for &
    configured to consume external sense? is another source selected?), not our bytes.
- **RX handler FOUND & fully decoded: `FUN_00029322`** (the VE.Smart receive-and-apply
  method; reached only via vtable/observer, which is why direct-caller tracing failed —
  located instead by grepping decompiles for the `0xFF/0xE1/0x02` byte compares).
  Signature `(obj, advData, advLen, srcCtx)`. Accept criteria, in order:
  1. Guard: `*(obj+0x3a)!=0` (**networking enabled**) and `*(obj+0x1c)!=0` (**ready**).
  2. Walk AD elements; only elements with **len > 0x11**; require `[+1]=0xFF [+2]=0xE1
     [+3]=0x02` (manufacturer / company `0x02E1`).
  3. `recType = advData[+4]`; **must be 1, 2, or 3** (`if (2 < recType-1) return`). 1 =
     sequence-sync, 2 = compact, 3 = a source-`2` variant. (Instant Readout `0x10` is a
     different, non-network path.)
  4. **`advData[+5]` (netId low byte) must equal `*(obj+0x38)`** (the charger's own netId
     low) — else drop.
  5. `FUN_0002914a` reads the 4-byte **source** at `+6`; a method on `obj+0xc` (the
     **per-source table**) looks it up, returning its last sequence (or `0xffffffff/0xffff`
     if unknown).
  6. **KEY GATE (type 2):** `if (storedSeqHi==0xffff && storedSeqLo==0xffffffff) return;`
     → a compact V/T record is **dropped unless the source already has an established
     sequence from a prior type-1 sync.**
  7. Anti-replay: reconstruct full seq from clear low bits + stored high bits; reject if
     not strictly greater than stored.
  8. **Nonce = `[recType][seq×6 LE][source×4 LE][netId×2 LE]` (13 B)**; `FUN_00029016`
     (AES-CCM) verifies the 4-byte tag with the **key at `obj+0x28`**; tag fail → drop.
     This matches `tools/victron_re/vesmart_packet.py` / our emulator **byte-for-byte**.
  9. Type 2: parse plaintext VREG TLVs, update per-source seq, then apply each `(vreg,
     value)` to the sense sink `obj+8` (compact-source index = `plaintext[0] & 0xf`).
     Type 1: empty plaintext; on tag-OK, **register source+full-sequence** into the
     per-source table (`obj+0xc` method `+4`, lines 186–192).
- **Conclusion — our packets are spec-correct; the gate is the type-1 sync.** There is
  **no pairing/whitelist**: any source on the matching netId is accepted *once its type-1
  sync is CCM-verified*, but every type-2 V/T record is dropped until then. We send the
  sync only 1-of-3 packets and BLE scan is duty-cycled → the charger can easily miss it
  and then drop all V/T. **Fix (firmware-derived): emit the type-1 sync far more often —
  an up-front burst, then sync every other packet.** Secondary: the charger must be in
  the enabled/ready receive state (`obj+0x3a`/`+0x1c`), and netId+key must match (we read
  both from the charger, so they should).
  - **IMPLEMENTED (2026-07-29) + OTA-flashed.** `vsTick()` in `src/BleManager.cpp`: on a
    fresh broadcast session (enable, or resume after stale) it seeds `VS_SYNC_BURST = 8`
    leading type-1 syncs, then runs a period-4 rotation `sync → V → sync → T` (sync in
    half of packets). Verified on-air: all three records decode against the charger key.
    Awaiting the on-hardware reception check in VictronConnect.
- New reusable Ghidra scripts: `tools/victron_re/FindSvcAndCallers.java` (SVC map +
  wrapper/callers), `tools/victron_re/ResolvePointers.java` (resolve vtable/descriptor
  pointers to functions). Note: `FUN_000359d0`/SVC `0x47` is the SoftDevice **RNG**
  (`sd_rand_*`), not ECB — it seeds an LFSR advert-jitter object (`FUN_00029150`); don't
  re-chase it as crypto.

**2026-07-28 — VE.Smart Battery Sense broadcast emulator implemented.**
- Added `src/VeSmartEmu.cpp` / `include/VeSmartEmu.h`: a byte-for-byte C++ port of
  `tools/victron_re/vesmart_packet.py` using `mbedtls_ccm_*` (AES-128-CCM, 13-byte
  nonce, 4-byte tag, no AAD). Records: identity (product `0x0000A3A5` + voltage
  `0xED8D` i16 0.01 V), status (firmware `0x000115FF` + temperature `0xEDEC` u16
  0.01 K), and the type-1 full-sequence sync.
- `src/BleManager.cpp`: non-connectable manufacturer-only advertising rotated in
  the BLE idle window, freshness-gated on `battLastMs`/`solLastMs` under
  `teleLock()`, paused for connected reads and when BLE is off. 48-bit sequence
  reserved a block at a time in NVS namespace `huckvs`.
- `Settings` + `POST /api/vs` + `#ble` card (shown only when battery and charger
  are both fresh); the 16-byte key is validated server-side and never returned.
- Docs updated (see the emulator section). Built `huckleberry` and
  `huckleberry_ota`.
- **Live network-join acceptance test (run this on hardware before relying on it):**
  1. In VictronConnect, connect to the SmartSolar and **Create** a VE.Smart Network
     (or open an existing one). Note the 2-byte **Network ID** shown by the name;
     long-press the network icon ~5 s to reveal the 16-byte **key**.
  2. Confirm the charger has **joined** that network (VE.Smart icon lit) and that no
     other voltage/temperature sense source is active in it (single-source rule).
  3. In Huckleberry's BLE settings → *VE.Smart Battery Sense Emulator*, enter the
     ID and key exactly as shown, Save, then **Enable**. The card only appears when
     both EcoWorthy and the charger are live.
  4. Watch the card: `Broadcast` should read *broadcasting* with plausible
     `Source voltage`/`Source temperature` matching the EcoWorthy readings.
  5. In VictronConnect, open the SmartSolar's battery/VE.Smart details and confirm
     it now shows **shared** battery voltage and temperature from the network
     (source name/ID matching Huckleberry's stable source address), and that the
     values track the EcoWorthy pack.
  6. Stale-stop test: disable the EcoWorthy link (or let it drop) and confirm the
     card flips to *paused* within ~90 s and the charger reverts to its own sensing
     — Huckleberry must never send the `0x7FFF`/`0xFFFF` no-data sentinels.
  7. If step 5 fails to match, the only likely ambiguity is Network-ID byte order:
     the ID is parsed as the entered hex in array order (`b0 b1` → u16 `b0|b1<<8`);
     if the charger rejects it, retry with the two hex bytes swapped.
- **Direct VictronConnect pairing of Huckleberry remains unsupported** (broadcast
  only; no Smart Battery Sense identity/SMP/`306b`/`0xEC12..0xEC14` GATT server).

**2026-07-25 evening — Stages 1 & 2 done, symbol recon done.**
- Stage 1 probe: enumeration succeeded, no PIN needed at BLE layer.
- Stage 2 probe: writes rejected, nonce discovery on `97580006`.
- String/symbol mining: found `QuickConnectFromQR::decryptAesCcm`,
  `Networking::aes_ccm_encrypt`, `PinManager::*`, `VeDirectHex::*` — enough
  named entry points to skip full auto-analysis.
- Installed JDK 21. Ghidra 12.1.2 zip downloading in background.
- Next session: extract Ghidra, minimal import, decompile the 3-5 target
  functions, capture the algorithm.

**2026-07-25 night — Ghidra done; CCM primitive fully captured; scope corrected.**
- Extracted Ghidra 12.1.2, JDK 21 wired in. Built a reproducible headless
  decompile workflow (see "Ghidra workflow"). Jython is gone in Ghidra 12 →
  ported the dump script to Java.
- Decompiled the crypto cluster (`aes_ccm_encrypt`, `generic_aes_ccm_*`,
  `decryptAesCcm`/`encryptAesCcm`, `PinManager::*`, all of `Networking::Core`,
  `VeBleInterface` key storage). See "Decompilation findings".
- **Result: the CCM primitive is standard AES-128-CCM, nonce=13, L=2, M=4
  (tag=4), no AAD.** Corrected the earlier "L=4/M=2" guess (B0 flags = 0x09).
- **Scope correction: `Networking::Core` is the encrypted *advertisement*
  builder (company ID 0x02E1 header), not the connected handshake.** Its key =
  the stored advertisement bindkey; nonce = [type][seqnr6][srcAddr4][dstAddr2].
- `PinManager` is just SimpleCrypt at-rest storage.
- **Next session: analyze `VeSmartDevice` / `VeBleDevice`** (already decompiled
  & archived) to map the connected `9758…` handshake: how the 8-byte
  `97580006` challenge + (maybe) the PIN produce the session key/nonce. Only
  then is the BLE PIN actionable for a reimplementation test.

**2026-07-26 — connected authentication corrected; safe probe and charts implemented.**
- Fully analyzed `VeService` and `VeSmartService`. Confirmed that ordinary
  access uses BLE SMP passkey pairing; `97580003` changes the PIN and
  `97580006` is the PUK reset path.
- Mapped the `306b` VeSmart channel and its concatenated CBOR request opcodes.
- Replaced the broad write-to-every-characteristic probe with an authenticated,
  read-only `getDevices` probe and raw chunk capture.
- Added separate secret PIN storage without returning the PIN from `/api/state`.
- Prototyped a 24-hour RAM history, then removed it at the owner's request in
  favor of the charger's persistent native history.
- Built and flashed the flow-control implementation over OTA. Hardware pairing
  succeeded with an encrypted, authenticated, bonded connection. The charger
  accepted `FA 80 FF`, `F9 80`, granted `F9 01` send credit, and returned the
  device list `(0,0)`, `(1,0)`, `(3,1)` for `getDevices`.
- Extended the diagnostic to consume send credit per request, decode the device
  list, and request instance `1`'s compressed read-only path catalog next.

**2026-07-26 late — direct VREG reads and native history proven on hardware.**
- `getPathList` returned control error `3` for both instances `1` and `3`, even
  after subscribing to every instance. This charger does not expose the path
  catalog; the path-based branch is not required for telemetry.
- `VeSmartDevice::handleDeviceList` showed VictronConnect immediately requesting
  VREGs `0xEC65` and `0xEC66` from root instance `0`. That established that the
  connected CBOR service uses the normal VE.Direct HEX/VREG register space.
- Confirmed `getValues` encoding as CBOR `5, instance, [vreg...]`. Responses are
  concatenated CBOR messages: opcode `8, instance, vreg, byte-string` for values
  and opcode `9, instance, vreg, signed-reason` for unavailable registers.
- Hardware identification resolved the device tree: leaf instance `3` returned
  product `0xA075` and `SmartSolar Charger MPPT 75/15 rev2`; instance `1` is an
  intermediate node.
- Read-only live VREGs returned correct values for charger/battery state,
  battery voltage/current, PV power/voltage, load output, yield, and errors.
- Read-only history VREG `0x104F` returned a valid 34-byte totals record. VREG
  `0x1050` returned a valid 34-byte today record with battery min/max, phase
  minutes, peak power/current/panel voltage, and day sequence number. The full
  daily range is `0x1050..0x106E` (today through 30 days ago).
- Implemented an automatic authenticated read every 15 minutes. It discovers the
  leaf instance, performs only `getValues` requests, parses all 31 native daily
  records, disconnects cleanly, and releases capture buffers.
- Added extended SmartSolar fields and a streamed `/api/victron/history` endpoint
  plus a VictronConnect-inspired 31-day dashboard: stacked yield bars colored
  by charge stage, tap-to-view daily statistics, and always-visible charts for
  load consumption, peak power, PV voltage, battery range, charge current, and
  stage duration. Daily error codes are included in the history endpoint.
- Removed the five-minute RAM sampler, ring buffer, `/api/history` endpoint, and
  rolling chart. Only SmartSolar's native persistent history is retained.
- Diagnostic reports are now released after delivery; retaining the large raw
  register report had reduced free heap enough to drop Wi-Fi on the probe build.
- Production cleanup removed the diagnostic probe, raw notification logs,
  minute retry loop, per-advertisement heap buffers, observed-since-boot peak,
  and duplicate web history array. Live advertisements now use a passive 10%
  scan window, the BMS is read once per minute, and native Victron history is
  read only after Wi-Fi stabilizes and then every six hours (30-minute retry on
  failure). The web app no longer polls the history endpoint every minute; it
  reloads when the Power page opens or a connected read completes. The history
  day count is derived from valid decoded records rather than the unrelated
  totals-record byte. No five-minute telemetry series is retained.
- OTA validation with BLE re-enabled held Wi-Fi continuously through passive
  solar reads, repeated BMS polls, and the first authenticated history refresh.
  Free heap settled near 33 KB after the connected read, and both state and the
  history API reported the same two native charger days.
- Solar percent-of-peak is derived on demand from the highest charger-recorded
  peak power in the available 31-day native history. Today's peak remains a
  separate value; no rolling or learned maximum is stored by Huckleberry.

**2026-07-26 — Connected auth confirmed = BLE passkey; deepened live VREGs.**
- Confirmed from the decompiled app that connected mode is **not** app-layer
  AES-CCM: `VeSmartService::characteristicChanged` decompresses (`qUncompress`)
  CBOR but never decrypts, `processControlData` is flow-control (chunkSize /
  maxAttLength / maxFreeChunks), and the only `aes_ccm` caller is
  `Networking::Core` (the advertisement builder). Auth is **BLE-layer passkey
  pairing with the 6-digit PIN** — which matches the working firmware
  (`injectPassKey` in `pollVictronConnected`). No custom handshake to reproduce.
- Firmware already reads connected VREGs over the `306b…` service (CBOR opcode
  5 get / 8 value). Extended `liveRegisters` with candidate VREGs: serial
  `0x010A`, firmware `0x0102`, yield/peak yesterday `0xEDD1`/`0xEDD0`, battery
  temp `0xEDEC`, panel current `0xEDBD`. Added parsing → `gTele`, `/api/state`,
  and Power-page rows.
- Added an **unknown-VREG diagnostic**: VREGs the charger rejects (CBOR opcode
  9) during the live read are captured to `gTele.solUnknownVregs`, logged to
  serial, and shown on the Power page — so invalid candidates are identified on
  hardware without a reflash-guess loop. Confirm which of the above stick on the
  75/15 and prune the rest.

**2026-07-28 — stored trends decoded, implemented, and probe removed.**
- Extracted the embedded Qt `vregs.json` and decompiled the exact `trends_push`
  and `trend_config` translators. This pinned the transient reply fields to
  `0xEC5B` and established the 6-byte little-endian AskData request.
- Validated all six series and all four time-resolution tiers on the live
  SmartSolar 75/15 with BT firmware 2.52. No firmware update or path protocol is
  involved.
- Added bounded progressive backfill in the existing authenticated VeSmart
  session. Raw native samples are consumed one reply at a time, condensed to
  30-minute bins, and persisted to SPIFFS rather than a RAM history ring.
- Added `/api/victron/day`, five always-visible click-a-day Power-page charts,
  and `dbo.IntradaySample` collector storage. Current solar percentage continues
  to use the highest charger-recorded peak in the available 31-day history.
- Split the bootstrap into bounded three-series sessions that run every 15
  minutes only until every available daily-history day is covered. Complete
  installations refresh every six hours, with a 30-minute retry only after
  failure. Removed `requestProbe`, `runProbe*`, all `s_probe*` state, tagged
  captures, and `/api/victron/probe` routes.
- OTA hardware validation on the 75/15 retained SPIFFS data across reboot,
  restored all four available intraday days, and filled the oldest day's
  battery-voltage and charge-current charts in the first three-minute session.
  Wi-Fi stayed connected; free heap remained above 29 KB during the pull and
  recovered above 31 KB afterward.
- Applied the idempotent `dbo.IntradaySample` migration to the configured local
  SQL Server and completed a real collector pass: 167 Huckleberry half-hour
  rows spanning 2026-07-25 through 2026-07-28 were present afterward.

**2026-07-28 — v0.5.3 exports, expanded charts, and stored-only SQL.**
- Added streamed daily-history, selected-day intraday, and all-intraday CSV
  downloads at the bottom of the Power page. The ESP32 reads one stored day at
  a time and does not allocate another history buffer.
- Reworked every canvas chart so the plot scrolls between fixed left/right
  axes. Added exact-value mouse hover and touch/tap tooltips. Each chart now has
  an explicit Expand/Close control instead of an in-window magnifier; desktop
  and 412x915 mobile validation showed all 48 intraday bins at once.
- Hid the Victron battery-temperature row and chart when the charger has no
  actual remote-temperature source. EcoWorthy temperature remains visible only
  in the BMS section. Corrected the battery-current gauge labels so charge is on
  the green/left side and use is on the orange/right side.
- Removed SQL `LiveSample` writes and new-schema creation. The collector now
  upserts only charger-owned `DailyHistory` and `IntradaySample` data, and its
  default/active interval is 30 minutes. A real collector pass left the legacy
  live row count unchanged at 10 while retaining 4 Huckleberry daily rows and
  167 stored intraday rows. Existing legacy live rows were not deleted.
- Applied the non-destructive SQL migration, which removes the obsolete
  `vLatestLive` view. Windows Task Scheduler registration was attempted but the
  non-administrator session was denied; no `VictronCollector` task exists yet.
  Run `tools/victron-collector/register-task.ps1` once from an Administrator
  PowerShell to enable automatic PC-side collection.
- Built and flashed v0.5.3 over OTA. Live API validation returned 29 of 48 bins
  for today and 174 all-days CSV rows during testing. Desktop/mobile checks
  confirmed expansion, fixed axes, hover/tap values, hidden unavailable
  temperature, all three export links, corrected current labels, and no browser
  console warnings or errors.

**2026-07-28 — v0.5.4 persisted day navigation and VE.Smart sender path.**
- Added a centered older/newer selector to expanded charts. It changes the
  shared selected day, so every daily/intraday chart and the selected-day CSV
  stay synchronized. Desktop and 412x915 mobile checks confirmed no control
  overlap and no browser warnings or errors.
- Added `availableAges` to `/api/victron/day` by scanning the persistent trend
  file once. Day navigation and SQL intraday discovery now survive a reboot even
  before the next authenticated 31-day read repopulates RAM. The collector still
  stores only charger-owned daily and intraday records.
- Confirmed the VE.Smart battery bridge should advertise as a sensor peer; it
  must not push voltage or temperature values into the charger. No emulator is
  enabled until sender tuple IDs, cadence, source identity, and stale-data
  behavior are verified.
- Decompiled the VictronConnect firmware manager/request path. The app keeps
  separate BLE and VE.Direct firmware directories, caches `remoteFirmwares.json`,
  and requests its catalog through MQTT topic `/firmwares`. The next RE target is
  the current Smart Battery Sense BLE firmware, followed by a live known-network
  advertisement capture for independent validation.
- Built and flashed v0.5.4 over OTA. The live ESP32 reported stored ages 0–4 and
  30 current-day bins after reboot; navigating to age 1 returned all 48 bins and
  updated the intraday CSV link to `age=1`.

**2026-07-28 — Smart Battery Sense firmware and packet format recovered.**
- Followed VictronConnect's charger/BLE firmware catalog path and downloaded the
  official Smart Battery Sense v1.15 BUP images for products `0xA3A4` and
  `0xA3A5`. Recovered the BLE BUP XOR mask and decoded valid Nordic S132 images.
- Decompiled the v7 image at base `0x26000`. Its VREG providers and embedded
  `TxMessage` records confirm the exact `0xED8D` voltage and `0xEDEC` temperature
  fields, units, no-data sentinels, product identity, and two-message rotation.
- Reconciled the firmware records with VictronConnect's `Networking::Core` to
  pin the clear header, 13-byte nonce, compact tuple encoding, four-byte CCM tag,
  and full-sequence synchronization message.
- Added offline BUP extraction and VE.Smart packet round-trip tools under
  `tools/victron_re/`. No ESP32 broadcaster is enabled; network provisioning,
  cadence, sequence persistence, and live charger acceptance remain safety
  gates.
- Reworked the top of this document into a current capability matrix plus a
  calls/syntax/specifications quick reference covering HTTP APIs, connected
  CBOR messages, flow control, VREG layouts, VE.Smart advertisements, firmware
  tools, Ghidra entry points, OTA, and SQL collector commands.
- Extracted VictronConnect's complete QML resource bundle and recovered the
  native VE.Smart provisioning UI. Create writes a random two-byte ID, random
  16-byte key, and name to VREGs `0xEC12..0xEC14` and saves them in ProductDB;
  Join reuses that local database rather than discovering a network over air;
  Leave writes all-`FF` ID/key values. A five-second press reveals the key.

## Where the source-of-truth artifacts live
- Decompiled/extracted APK contents: `../Pack Rat/apk-analysis/victron/`
- Strings dumps: `../Pack Rat/apk-analysis/victron/reports/*.strings.txt`
- **Ghidra decompiled C + scripts: `../Pack Rat/apk-analysis/victron/ghidra/`**
  - `scripts/` — `DumpVictronFuncs.java`, `run_import.ps1`, `run_dump.ps1`
    (paths inside the runners point at a session scratchpad; update the
    `$SCRATCH`/`$PROJDIR` vars before reuse).
  - `decompiled/crypto_core/` — aes_ccm + decrypt/encryptAesCcm + PinManager
  - `decompiled/networking_core/` — Networking::Core cluster (advertisement)
  - `decompiled/ble_interface/` — VeBleInterface + VeSmartDevice/VeBleDevice
    ctors (**VeSmartDevice ctor = next session's starting point**)
- `.so` binary: `../Pack Rat/apk-analysis/victron/arm64-apk/lib/arm64-v8a/libVictronConnect_arm64-v8a.so`
- Current passive decoder: `src/BleManager.cpp` (`decryptVictron`, `parseVictron`)
- Production stored-trend implementation: `src/BleManager.cpp`,
  `src/VictronTrends.cpp`, and `include/VictronTrends.h`
- Smart Battery Sense firmware catalog/downloads, decoded images, and Ghidra
  project: `scratchpad/firmware/smart-battery-sense/` and
  `scratchpad/ghidra-sbs-final/`
- SmartSolar receiver and SmartShunt Isense firmware projects:
  `scratchpad/ghidra-smartsolar/ss`, `scratchpad/ghidra-smartsolar/isense/`,
  `scratchpad/ghidra-smartshunt/ss`, and `scratchpad/ghidra-smartshunt/isense/`
- Reproducible firmware/packet tools: `tools/victron_re/extract_bup.py` and
  `tools/victron_re/vesmart_packet.py`
- Extracted VictronConnect QML used to recover Create/Join/Leave behavior:
  `scratchpad/qml-extracted/qtquickcontrols2.conf/qml/`
