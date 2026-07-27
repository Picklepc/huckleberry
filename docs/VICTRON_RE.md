# Victron BLE Reverse Engineering (M6 stretch)

Working notes for the SmartSolar MPPT connected-BLE effort. **Update at the end
of every session.** This is intended to be resumable after gaps.

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
- Probe implementation: `src/BleManager.cpp` (`runVictronProbe`)
