# Target: ESP32-S3 / C3 / C6 / classic (ESP-IDF)

Source repo: `Firmware` (https://github.com/OpenDisplay/Firmware.git)

## Status: **runs on hardware** (ESP32-S3; re-verified after phase C on 2026-08-05)

All ten boards build, and the S3 has been flashed and exercised. This section used to say
"Phase B in progress — configures, partially compiles, does not link" and carried a census of
six missing third-party headers; every one of those is resolved. The census is dropped rather
than updated because a list of solved problems reads as a list of open ones.

```bash
./build.sh                     # every board -> ../../release/, merged images
./build.sh s3-n16r8 c6-n4      # some
./build.sh --list --clean
```

`build.sh` sources ESP-IDF itself if it is not on `PATH` (it never is — the install is
activated per shell). Each board produces one merged image flashed at offset 0; see
[docs/BBEPAPER_IO_BACKENDS.md](../../docs/BBEPAPER_IO_BACKENDS.md) for the panel layer and
`release/MANIFEST.txt` for the per-board chip and flash command.

### Verified on hardware (s3-n16r8-extuart-debug, 2026-08-05) — after phase C

Second and larger verification event, from a device log covering a full session on a **Seeed
reTerminal E1001** (800x480, panel IC `0x003C`, bb_epaper path, no FastEPD). This is the run
that retires most of phase C's hardware debt: steps 1-15 had **not** been on hardware before it.

| | |
|---|---|
| Boot + config load | NVS (`od_hal_nvs`), full config dump, 1.0 |
| Encrypted session | `0x0050` challenge/response, all later traffic encrypted |
| Read config `0x0040` | 9 chunks, encrypted |
| Write config `0x0041`/`0x0042` | 5 chunks, reloaded from storage |
| Image push `0x0080`-`0x0082` | **4 pushes**, 96 000 B each, zlib 3.37-38.05x, 23.3-39.1 KB/s |
| Panel refresh | 1.01 s full, x4 |
| Panel cold bring-up | 1319 ms x4, no BUSY timeouts (899 rail + 201 initIO + 41 wake + 178 initSeq) |
| I2C sensor probe | `od_hal_i2c_probe` — 0x44 (SHT40) and 0x51 present, three absent, correct |
| Die temperature | `od_hal_adc_die_temp_c()` installs, range -10..80 C |
| LED `0x0073`/`0x0075` | activate + stop |
| Reboot `0x000F` | LAN/WiFi teardown, BLE deinit, clean `RTC_SW_CPU_RST` |
| Deep sleep | entered on idle (40 s hold), 360 s configured |
| **Button wake** | **buttons 1/2/3 and wake-from-deep-sleep all work** |

**What this covers.** The BLE + panel + GPIO arm: `od_hal_{nvs,log,gpio,time,adc}`, the 19 + 7
`pinMode`+`digitalWrite` glitch removals of steps 12/14, the deep-sleep pin pass, and the GPIO
interrupt path behind the buttons. On I2C it covers `od_hal_i2c_init`, `od_hal_i2c_probe`
(five addresses, correct present/absent) and the SHT40 sensor reads.

**The AXP2101 PMIC path is NOT covered, despite being the bulk of step 14.** The path is gated
on the *configuration* declaring a sensor of type `OD_SENSOR_TYPE_AXP2101` (`pwrmgm()` scans
`globalConfig.sensors` for it). This unit declares one sensor, type `0x0004` — an SHT40 — so
`initAXP2101()` is never reached and `pwrmgm(on)` takes the plain-GPIO rail branch instead,
which is what *"rail + 800 ms settle"* in the log is. Consistent with that, the PMIC's address
0x34 is never probed; the probes are 0x44, 0x45, 0x51, 0x55 and 0x6A.

Note the claim carefully: **this says nothing about whether an AXP2101 is fitted to the board.**
A log cannot show that, and the gate is a config check, not a hardware detection. What is
certain is that the ~20 rewritten `Wire` transactions setting DCDC/ALDO enables and voltage
set-points **have never executed**. Verifying them needs a unit whose config declares an
AXP2101 sensor, so until then step 14 is only partly verified.

**What it does NOT cover.** The **entire WiFi/LAN arm** — this unit has `WiFi: disabled` in
`communication_modes`, so steps 9b-ii (station, six formerly-dead behaviours), 9b-iii (LAN
sockets, TLS-PSK) and 9b-iv (mDNS) are still unexercised. Also untested: the E1004 dual-CS
path (compiled out on every board) and FastEPD (this panel uses bb_epaper).

#### GAP EVENT COVERAGE IS A PORT REGRESSION (found 2026-08-05)

`NimBLEServer::handleGapEvent` in NimBLE-Arduino — the wrapper the shipped firmware uses —
handles **14** GAP events. Phase B's rewrite against the raw NimBLE C API
(`ble/od_ble_nimble.cpp`) covered **6**. The eight that were dropped:

| Event | What the wrapper did |
|---|---|
| `REPEAT_PAIRING` | **deleted the stale bond and returned `RETRY`** |
| `ENC_CHANGE` | observed the encryption result |
| `PASSKEY_ACTION` | drove passkey exchange during pairing |
| `CONN_UPDATE` | connection-parameter updates |
| `NOTIFY_TX` | notification transmit completion |
| `NOTIFY_RX`, `IDENTITY_RESOLVED`, `SCAN_REQ_RCVD` | central-role, privacy, diagnostics |

`REPEAT_PAIRING` is the one that reached the user, as the connect stutter: with a stale bond on
the client, the wrapper deleted it and re-paired, while this port kept the mismatched keys and
failed identically on every attempt — **permanently**, because nothing on the device side could
clear it.

**All eight are now implemented and coverage is at parity: 14 of 14.** "Equivalent" means the
same information reaches the same place, not the same shape — the wrapper dispatched to
`NimBLEServerCallbacks` virtuals this firmware has no counterpart for. What each became:

| Event | Here |
|---|---|
| `REPEAT_PAIRING` | deletes the stale bond, returns `RETRY` — behaviour, not logging |
| `ENC_CHANGE` | logged; nothing depends on link encryption (`SM_LVL=0`, no `_ENC` flags) |
| `PASSKEY_ACTION` | logged; cannot fire with no IO capability, but will not stall silently |
| `CONN_UPDATE` | routed to the existing link reporter, so interval/MTU/PHY renegotiations all read alike — this is what answers "why did throughput drop mid-transfer" |
| `NOTIFY_TX` | **failures only.** A non-zero status means the notification never went out, which the BLE TX queue could not previously see. Success is not logged: one line per frame per transfer would bury the failures |
| `IDENTITY_RESOLVED` | logged — it is the event that says "this peer IS bonded to us" |
| `NOTIFY_RX` | explicit, and warns: peripheral-only build, so it cannot occur |
| `SCAN_REQ_RCVD` | deliberately silent — every scanning phone in range emits these several times a second |

Two are accounted for rather than acted on (`NOTIFY_RX`, `SCAN_REQ_RCVD`), and that is the
point: an unhandled case returns 0 and is indistinguishable from success, which is exactly how
eight missing events went unnoticed until one of them broke connections.

#### CORRECTION 2026-08-05: the SM defaults were NOT the root cause

Hardware disproved it. The device was reflashed at `c5119eb` (which includes the SM restoration)
and **`status=26` still occurs on most connections**. `sm_bonding = 0` did not remove it, so the
bonding story below is wrong as a *cause*.

**What 26 actually is.** It travels 1:1 with these, on every affected connection:

```
GAP CONNECT h=1 status=26
NimBLE: ogf=0x08, ocf=0x0032, hci_err=0x21A : BLE_ERR_UNSUPP_REM_FEATURE
2M PHY request rejected (rc=538, staying at 1M)
DLE 251 request rejected (rc=538)
```

`538 = 0x21A = BLE_HS_HCI_ERR(0x1A)`, and **`26 = 0x1A`**. Both are the same HCI error —
**Unsupported Remote Feature** — surfacing once raw in `connect.status` and once wrapped in the
PHY/DLE rejections. It is a link-layer feature negotiation the peer refuses, and has nothing to
do with encryption, bonding or key size. `BLE_HS_EENCRYPT_KEY_SZ` is 26 as well, which is what
made the wrong reading plausible; the co-occurrence with `0x21A` is what settles it.

Affected connections come up at 1M PHY and are refused DLE; unaffected ones negotiate 2M and a
251-byte PDU. So this is peer-dependent, not device state.

**The SM restoration stands anyway** — it returns the device to the shipped security posture and
stops it offering bonding it cannot persist — but it fixed a latent divergence, not this bug.
**What actually fixed the connect stutter is `1c2e2f5`**, adopting a link that exists whatever
the status says: confirmed on hardware, `link_exists=1` followed by `CLIENT CONNECTED` with an
epoch, and no `Dropped write from non-owner` anywhere in the capture.

#### (superseded) the security-manager defaults

Found 2026-08-05 by a second review specifically comparing behaviour against the wrapper, after
the event census was already closed. `NimBLEDevice::init()` sets six `ble_hs_cfg.sm_*` fields
explicitly (`NimBLE-Arduino/src/NimBLEDevice.cpp:991-997`); the C-API rewrite set **none** of
them and silently inherited ESP-IDF's defaults, **which enable bonding and Secure Connections**:

| | wrapper (shipped) | this port, before the fix |
|---|---|---|
| `sm_bonding` | **0** | 1 (IDF default) |
| `sm_sc` | **0** | 1 (IDF default) |
| `sm_mitm` | 0 | 0 |
| `sm_io_cap` | `NO_INPUT_OUTPUT` | unset |

So the shipped firmware **never bonded**, and this port began advertising bonding capability the
moment it was flashed. The failure chain is exact: a client accepts and bonds → nothing calls
`ble_store_config_init()` and NVS persistence is off in every baseline, so the bond dies with the
boot → the client keeps its half → the next connection presents keys the device no longer has →
NimBLE reports the connect with `BLE_HS_EENCRYPT_KEY_SZ` (26). That is why it reproduced on the
first connection after **every deep-sleep wake**.

Nothing here wants bonding: no characteristic carries an `_ENC`/`_AUTHEN` flag, `SM_LVL=0`, and
authentication and confidentiality are the application's job (`0x0050` challenge/response plus
per-frame AES-CCM, which work identically over an unencrypted link). The wrapper's values are
restored field for field — a restoration of shipped behaviour, not a new security policy.

**The general lesson, worth more than the individual events:** re-implementing a vendor wrapper
means inheriting its whole surface, not just its dispatch. A partial `switch` fails silently —
every unhandled event returns 0 and looks like success. **Unset configuration fails even more
silently**: the port did not choose IDF's defaults, it simply never mentioned them, and the
result was a security posture the shipped product never had.

#### One defect and one warning the log exposes

1. **A GPIO reservation conflict on the buzzer pin — noisy, but NOT a failure.**
   `W (41054) ledc: GPIO 45 is not usable, maybe conflict with others`, repeating roughly every
   100 ms for the duration of each `0x0077`. **The buzzer works** (confirmed on hardware); this
   was first recorded here as "the buzzer does not sound", which was wrong. The warning is
   informational: IDF's `_ledc_set_pin()` calls `esp_gpio_reserve()`, logs this line if the pin
   was already reserved by something else, and then connects the output signal and returns
   `ESP_OK` regardless (`esp_driver_ledc/src/ledc.c:809-820`). So `ledc_channel_config()`
   succeeds and the tone plays.

   What is real: **something else already holds GPIO 45's output reservation**, so two drivers
   believe they own that pin, and the message repeats because the buzzer re-runs
   `ledc_channel_config()` per note rather than attaching once. Neither breaks output today.
   Worth finding the other owner before it does — a `LOG_LOCAL_LEVEL` bump is not the fix.
2. **`ble_gap_adv_start failed: 6` is EXPECTED, and logged at the wrong level.** Six is
   `BLE_HS_ENOMEM`, not `BLE_HS_EALREADY` (2), which is why the existing `rc != BLE_HS_EALREADY`
   guard does not suppress it. But it is not a fault: **`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`**,
   and connectable undirected advertising needs a free connection object. With the single slot
   already taken by a live client, NimBLE has nothing to allocate and returns ENOMEM. It fires
   because the MSD-publish path re-starts advertising to refresh the manufacturer data, which
   happens while connected.
   
   So: nothing is broken, and the device is not left invisible — it cannot accept a second
   connection anyway. What is wrong is that a routine, unavoidable condition is reported with
   `ESP_LOGE`. The tolerance list should include ENOMEM-while-connected, and the remaining
   genuine failures should stay loud. (First recorded here as "a real failure ... nothing
   retries"; that was wrong.)

Also visible and worth a look: **five consecutive connections were refused ownership** at the
start of the log ("Dropped write from non-owner h=1", each ending in a remote-terminated
disconnect ~8 s later) before one succeeded with epoch `e=1` — the first epoch ever issued. A
client that connects and cannot claim the slot wastes an 8 s session; the arbitration is doing
what R3 says, but why the first five could not claim is unexplained.

### Verified on hardware (S3, 2026-08-03)

Measured, not inferred — from a device log:

| | |
|---|---|
| Compressed image push | 4x 96 000 B, zlib 1.52–43.8x, 15–35 KB/s |
| Config read round-trip | `0x0040`, 805 B in 9 chunks |
| Config write round-trip | `0x0041` + `0x0042`, reloaded from storage |
| Encrypted path | `0x0050` challenge/response, session established, all traffic encrypted |
| Panel power cycle | 4 cold bring-ups, **1398 ms** each, no BUSY timeouts |
| Panel refresh | 0.71 s, full |
| Reboot (`0x000F`) | BLE deinit + controller release, clean `RTC_SW_CPU_RST` |
| Deep-sleep button wake | works |

Cold bring-up breakdown, which is worth keeping because the parts sum exactly to the total
(898 + 201 + 49 + 250 + 0 = 1398) — so there is no unaccounted time on this path:

```
[EPD cold] pwrmgm(on)      898 ms   <- 800 ms of it a hardcoded delay(800)
[EPD cold] bbepInitIO      201 ms   <- almost entirely the two delay(100) reset pulses
[EPD cold] bbepWakeUp       49 ms
[EPD cold] initSeq (full)  250 ms
[EPD cold] alignRamMode      0 ms
```

**Still unexercised**, so MIGRATION.md Gate 2 is *mostly* met rather than met — and Gate 2 is
what licenses retiring the source repo:

- an **uncompressed** full image push
- an **interrupted transfer that recovers**. One `[DROP: 1]` was observed mid-transfer (the
  transfer completed, 337/337 chunks), so this is worth testing deliberately rather than
  assumed.

### Known defects, measured on hardware

1. ~~**FastEPD writes no pixels to an IT8951 panel.**~~ **FIXED 2026-08-04, NOT YET
   HARDWARE-VERIFIED.** All three `it8951WriteFramebuffer{1,2,4}Bit` functions had unpatched
   `#ifdef ARDUINO` guards (9 sites) that fell through to nothing under IDF: no data preamble,
   and the row loop built each line and discarded it. Commands, `LD_IMG_END` and `DisplayArea`
   all still ran, so the panel refreshed whatever was already in its RAM and every call
   reported success. The nine guards now carry `OD_FASTEPD_IDF_SPI` like the six on the
   command path; confirmed in preprocessed output, where all three writers emit live
   `SPI.writeBytes` calls. **A green build proves nothing here** — the defect's signature was
   that every call already reported success, so this stays listed until an IT8951 panel is seen
   to render. S3-only, IT8951-over-SPI only; the parallel ED103 path never went through it.
2. **`bbepWaitBusy` blocks the loop task.** It polls with a bare `delay(20)` inside
   `bb_ep.inl`, so `serviceBleTx()` cannot run for the duration. A legitimate multi-second
   refresh therefore holds queued BLE responses in the TX ring until it finishes; this was the
   mechanism behind acks arriving 16 s late while the panel was faulty, and it survives the
   panel fix. `bbepLightSleep()` is the injection point for a pump.
3. **Every transfer pays a full cold bring-up** (1.4 s) because this unit has
   `Screen Timeout: 0 s`, i.e. keep-alive disabled. 800 ms of that is an unconditional
   `delay(800)` rail settle in `pwrmgm()`.
4. ~~**`s3-e1004` still has no board fragment**~~ — **RETIRED 2026-08-05**: the board variant is retired, so no fragment is wanted. See third_party/NOTICE.md.

### Phase C — the Arduino shim is GONE (2026-08-16)

The count went 22 → 0 across sixteen recorded steps and `compat/` was deleted.
[docs/ARCHIVE_esp32_arduino_shim.md](../../docs/ARCHIVE_esp32_arduino_shim.md) carries one dated
paragraph per step, including what each one found — several existed only because the shim was
hiding a defect.

**The gate was replaced, not retired.** `tools/check.sh`'s "esp32: arduino-free app code" checks
CALLS rather than includes. That distinction is not academic: the ratchet's include-count reached
0 while three call sites still reached shim primitives, because `delay(long)` and `millis()` are
declared by an OD-PATCH in `third_party/bb_epaper/src/bb_epaper.h` and not by any header it
grepped for. One of the three was app code (`main.cpp`'s `delay(100)`).

**The permanent piece is `vendor/fastepd/`.** It is one header plus its storage — an Arduino
`SPI` object over IDF's `spi_master`, plus `millis()` — because FastEPD's IT8951 transport is
written against that object. It is called an **adapter**, not a shim: in this repo "shim" means
scheduled demolition, and using the word for something permanent would have made the ratchet's
vocabulary meaningless. `delay(long)` sits in `panel/od_bbep_idf_io.inl` for the same reason on
bb_epaper's side. Both forward to `od_hal_time`.

> **Corrected 2026-08-04.** This section previously said the adapter would also have to own
> `delay()`, `delayMicroseconds()`, `millis()` and `ledc_compat.h` "for `bb_epaper.h`'s
> unmangled declaration". That was written before `panel/od_bbep.cpp` landed. **bb_epaper needs
> nothing from the shim** — it has our own IDF backend, its `arduino_io.inl` / `esphome_io.inl`
> are never compiled, and its `<Arduino.h>` sits behind an `#ifdef ARDUINO` this build does not
> define. FastEPD borrows exactly one loose symbol, `millis()`, for `arduino_io.inl`'s 19 call
> sites; it defines `delay()` and `delayMicroseconds()` itself.
>
> `ratchet.sh`'s `third_party/` report was also described here as "that adapter's
> specification". It is not — it is a text grep that does not evaluate `#ifdef`, and most of
> what it lists is unreachable. The script now says so itself.

**Containment.** `third_party/bb_epaper/src`, `third_party/FastEPD/src` and `vendor/fastepd` are
all **off the component include path**; `main/CMakeLists.txt` grants each to a named list of
translation units. Adding a consumer is a deliberate edit there. Verified by trying it: an
unlisted file including any of the three fails to compile.

**Still owed:** the `od_hal_panel` repoint. `hal/od_hal_panel.{h,c}` and the two backends under
`panel/` exist and compile, but **nothing calls them yet** — `display_service.cpp` still drives
`BBEPDISP` directly. That repoint is staged one path at a time per MIGRATION.md, and it is the
point where hardware stops being optional.

The count *can* still reach 0: the last three entries (`buzzer_hw.cpp`, `device_control.cpp`,
`encryption.cpp`) are counted only for their `TARGET_NRF` arms, which leave at migration
step 4.

### Toolchain translation findings

Recorded as they are found, since TOOLCHAINS.md's translation table was written from reading
rather than building:

- **`CONFIG_FREERTOS_WATCHDOG_TIMEOUT_S=120` does not translate.** `platformio.ini` passed it
  as a `-D`, which the Arduino core accepted. Under IDF it is a real Kconfig symbol,
  `CONFIG_ESP_TASK_WDT_TIMEOUT_S`, with an enforced range of **[1, 60]** — 120 is rejected and
  silently falls back to the default. `sdkconfig.defaults` uses 60, the closest legal value.
  Whether the original 120 was load-bearing is unverified; if it was, the watchdog must be
  reconfigured at runtime instead.
- **`ARDUINO_USB_MODE` / `ARDUINO_USB_CDC_ON_BOOT` have no define equivalent.** They become
  the sdkconfig console choice (`CONFIG_ESP_CONSOLE_USB_CDC`), which is why they appear in
  `sdkconfig.defaults.esp32s3` and not in the board fragment's define list.

## Phase A (complete) — sources imported unchanged

| | |
|---|---|
| Imported from | `upstream/main` @ `2e2131b9980c9a46f1c8e56ce2d3dcb4b7aa5bd3` |
| Commit | `feat(wifi/lan): WiFi/LAN transport + tinfl inflate; pin pioarduino 55.03.39 (#124)`, 2026-07-25 |
| Contents | `src/` `lib/` `tools/` `scripts/` — 67 files, verified byte-identical to the source tree |

**This does not compile, and that is the expected state.** Phase A exists for provenance and
reviewable blame, not for a working build (../../docs/MIGRATION.md § "The ESP32 import is
different"). The sources are Arduino/PlatformIO and this target is ESP-IDF; Phase B adds the IDF
skeleton plus the temporary `compat/arduino_compat.h` shim and aims to **link and boot on
hardware early**, so every later step is bisectable against a known-good baseline.

Until Phase B lands, build this target from the original repo.

### What was deliberately NOT imported

Step 1 says "import unchanged", and these are the exceptions — decided before the first commit,
the way the Silabs SDK exclusion was, because a file committed once is carried forever:

- **`include/opendisplay_protocol.h` and `include/opendisplay_structs.h`.** They are a vendored
  copy of the canonical wire contract, and this repo already holds exactly one copy at
  `shared/protocol/`. Importing them would make this the *tenth* copy in a workspace whose
  header-sync mechanism is already measured at 1-in-sync/5-drifted/2-missing. Phase B points the
  include path at `shared/protocol/` instead. (Checked at import: the source copy differs from
  canonical by ten lines, all in a doc-only changelog entry, with no wire difference — see
  "Protocol header state" below.)
- **`platformio.ini`, `boards/`, `variants/`, `bin/`.** PlatformIO and Arduino board artifacts
  with no meaning under IDF; carrying them in would contradict the repo's "no PlatformIO, no
  Arduino" rule and leave dead files that look authoritative. The board *information* is still
  needed — it is the input to the sdkconfig translation table in ../../docs/TOOLCHAINS.md — and
  its source of truth remains the `Firmware` repo, which is not retired until migration step 4.
- The source repo's own `README.md`, `CLAUDE.md`, `AGENTS.md`, `docs/`, `LICENSE`.

### Two things to know before reading this code

**`src/` is an ESP32 *and* nRF52840 tree.** The two chip families are interleaved *within*
files — `ble_init.cpp`, `communication.cpp`, `device_control.cpp` and `display_service.cpp` all
carry Bluefruit/nRF52 code behind `#ifdef` — not separated into per-chip files. So this
directory legitimately contains Nordic code that does not belong to this target. It leaves at
migration **step 4**, when the nRF52840 half becomes a board on `targets/nordic-zephyr/`; that
is why `Firmware` is not retired until step 4 completes.

**The portable resumable zlib inflater is `shared/core/od_zlib_inflate.{c,h}`.** ESP32's former
target-local and third-party copies were retired in C14. It remains pure C with no vendor headers;
Wi-Fi builds retain the existing tinfl API remap as their target-specific backend.

### Protocol header state at import

The risk recorded in MIGRATION.md § "Risks to watch" — that the import would land a header stale
at protocol 2.1, missing all of SECTION 9 (LAN) — **did not materialise, and the underlying
claim was itself stale.** `upstream/main` is at 2.2 *because* `#124` is the LAN feature. The
source copy differs from canonical only by a ten-line doc-only changelog entry describing the
`0x43` trailing patch byte. Nothing needs re-syncing; DIVERGENCE_MATRIX §8.2 should be corrected.

First target in the migration order; see ../../docs/MIGRATION.md.

**This target changes framework on import.** The source repo builds with PlatformIO + Arduino;
this target is **ESP-IDF, no Arduino**. The rationale, the per-API replacement census, and the
PlatformIO-knob → sdkconfig translation table are in ../../docs/TOOLCHAINS.md. The import is
therefore a three-phase sequence (unchanged import → temporary `compat/arduino_compat.h` shim →
shim demolition subsystem by subsystem), not the plain "import unchanged" used for other
targets — see the ESP32 section of ../../docs/MIGRATION.md.

## Boards

**Eleven board fragments build** under ESP-IDF. The former `s3-e1004` variant is retired
(2026-08-05); current fragments include the two `s3-n32r8-extuart` release/debug profiles.

| Board | Chip | Flash / PSRAM | FastEPD | Image | Slot free | Notes |
|---|---|---|---|---|---|---|
| `s3-n16r8` | ESP32-S3 | 16 MB + 8 MB OPI | yes | 1,260,992 | 81% | reference board |
| `s3-n8r8` | ESP32-S3 | 8 MB + 8 MB OPI | yes | 1,260,992 | 62% | |
| `s3-n32r8` | ESP32-S3 | 32 MB + 8 MB OPI | yes | 1,260,992 | 81% | |
| `s3-n16r8-extuart` | ESP32-S3 | 16 MB + 8 MB OPI | yes | 1,284,256 | 80% | console on CH343P UART (GPIO43/44) |
| `s3-n16r8-extuart-debug` | ESP32-S3 | 16 MB + 8 MB OPI | yes | 1,296,224 | 80% | `OD_LOG_LEVEL=DEBUG`; +12 KB. Debug build, not a shipping one |
| `s3-n32r8-extuart` | ESP32-S3 | 32 MB + 8 MB OPI | **no** | 1,269,248 | 81% | reTerminal Sticky; panel via bb_epaper |
| `c3-n4` | ESP32-C3 | 4 MB, no PSRAM | no | 1,327,536 | 32% | |
| `c3-n16` | ESP32-C3 | 16 MB, no PSRAM | no | 1,327,536 | 80% | DIO flash to free GPIO12/13 |
| `c6-n4` | ESP32-C6 | 4 MB, no PSRAM | no | 1,518,032 | 23% | IDF ≥ 5.1. **Shipped.** `min_spiffs_4MB.csv` — see below |
| `esp32-n4` | classic ESP32 | 4 MB, no PSRAM | no | **695,104** | 65% | no WiFi; reduced PIPE reorder window |
| ~~`s3-e1004`~~ | ESP32-S3 | 32 MB + 8 MB | — | — | — | **RETIRED 2026-08-05.** Board variant retired; no fragment wanted. The old blocker (wrong bb_epaper fork) also dissolved — upstream merged E1004 support at `5dccfbb` |

Two things the table makes obvious that were not obvious before:

* **The classic ESP32 is by far the smallest image at 695 KB — 45% smaller than the S3.**
  It is the one board that does not compile the WiFi/LAN transport. NEXT_STEPS.md item 3
  expected this board to be the troublesome one; the reduced PIPE window
  (`PIPE_SMALL_DRAM_WINDOW`) was already in `structs.h` from the import and it built first try.
* **The C6 is the tightest fit by a wide margin** — 23% slot headroom against 62-81% for
  everything else, and it is a *shipped* board. It is the one to watch as `shared/core` lands.

### ~~`s3-e1004` is blocked, not forgotten~~ — RETIRED 2026-08-05

**The E1004 board variant is retired.** No fragment exists and none is wanted; the entry stays
in the table above so the absence reads as a decision rather than an oversight.

The blocker this section described also dissolved, independently and on the same day, so neither
half is outstanding:

- `bitbank2/bb_epaper` merged E1004 support upstream (`5dccfbb`), retiring the `limengdu` fork
  requirement. The re-vendor brought `EPD_SEEED_E1004` / `EP133_SPECTRA_1200x1600` in, and the
  core drives the dual-CS refresh itself via `cs_mode = CMD_CS1_CS2`.
- `BBEP_T133A01` and `e1004_write_stream_bytes()` no longer exist upstream, so the
  `SPI.writeBytes()`-without-`SPI.begin()` defect has no code left to fix.

Recorded for anyone who revives the variant: it differed from `s3-n32r8-extuart` by exactly one
build flag, `OPENDISPLAY_ZLIB_WINDOW_BITS=15`. Panel pins, controller type and the second
chip-select all come from the runtime config blob (`DisplayConfig.cs_pin_2` exists for
"panels split over 2 drivers"), so reviving it is a three-line fragment, not a driver port.

### Original decision (2026-07-25), superseded above

C6 units ship today on `huge_app.csv`: a single 3 MB app slot and **no OTA capability at all**.
Since the deployed-fleet migration is flash-and-reconfigure (MIGRATION.md § Deployed fleet
status), every C6 is on the bench once — and that is the only opportunity to give it a dual-slot
layout. **`default.csv`** is the decision:

```
# default.csv (4 MB)
app0,   app,  ota_0,  0x10000,  0x140000    # 1.25 MB
app1,   app,  ota_1,  0x150000, 0x140000    # 1.25 MB
spiffs, data, spiffs, 0x290000, 0x160000    # 1.4 MB
```

**Verify before rollout: the C6 IDF image must fit in 1.25 MB.** That is the tighter dual-slot
option — chosen for its 1.4 MB filesystem — and far less headroom than the 3 MB the app enjoys
today. Measuring it is a Phase B gate, not a rollout-time discovery.

- If it exceeds 1.25 MB → `min_spiffs.csv` gives 1.875 MB per slot, costing the filesystem
  (128 KB remains). Viable if nothing needs bulk storage, since config lives in NVS.
- If it exceeds 1.875 MB → C6 cannot do A/B on 4 MB at all, and is bench-only permanently.
  Record that as a finding rather than rediscovering it per unit.

The IDF image is expected to be *smaller* than today's Arduino build — dropping the Arduino core
and NimBLE-Arduino's wrapper typically shrinks it — so 1.25 MB is plausible. Plausible is not
measured.

The S3 boards need no change: `default_8/16/32MB.csv` already carry `app0` + `app1`.

## Layout

```
build.sh                        ./build.sh [board...]  -- all boards if none given
sdkconfig.defaults              common to all boards
sdkconfig.defaults.<idf_target> per-chip, auto-selected by IDF
sdkconfig.baselines/*.sdkconfig the reviewed effective config, gated by tools/
boards/<board>.cmake            per-board fragment (NOT .conf -- CMake, set by build.sh)
partitions/*.csv                per flash size
main/CMakeLists.txt             the app component; also the per-source include grants
src/                            imported application sources (from Firmware)
hal/                            od_hal_{nvs,log,gpio,time,i2c,adc,panel} -- this target's HALs
panel/                          od_bbep*.{cpp,inl} bb_epaper IDF backend; od_panel_* HAL backends
ble/                            NimBLE C-API transport
vendor/fastepd/                 PERMANENT FastEPD adapter -- an adapter, NOT a shim
tools/                          host tests, sdkconfig baseline gate
```

`compat/`, the temporary Arduino shim, was deleted on 2026-08-16. `vendor/fastepd/` was always a
separate directory so that "delete `compat/`" would stay an unambiguous instruction rather than a
judgement call about which files inside it were permanent.

## Toolchain

ESP-IDF, pinned to one explicit release. Floors: **≥ 5.1** for ESP32-C6, **≥ 5.2** for
`driver/i2c_master.h` (do not port onto the deprecated `driver/i2c.h`).

**Pinned to ESP-IDF `v5.5.4`** in [`.idf_version`](.idf_version) — one line, checked in, and
enforced by `build.sh`, which compares it against `idf.py --version` after activation and
refuses to build on a mismatch. `release/MANIFEST.txt` records both the pin and the active
version, so an image built off-pin is identifiable later.

```bash
./build.sh                              # enforce (default)
OD_IDF_VERSION_CHECK=warn ./build.sh    # build anyway, but say so
OD_IDF_VERSION_CHECK=off  ./build.sh    # silence the check
```

Installed on the primary dev box at `~/esp/esp-idf`, activated with
`source ~/esp/esp-idf/export.sh` — it is **not** on `PATH`, so `which idf.py` finding nothing is
the normal state, not a missing install. `build.sh` sources the export script itself unless the
caller already did.

**Adopting a newer IDF means editing `.idf_version` in the same commit** as whatever the bump
requires (sdkconfig defaults, bootloader, startup code all move with it). The override exists
for testing a candidate release or bisecting a toolchain regression — not for getting past a
red build.

### The sdkconfig baseline

`sdkconfig.baselines/<board>.sdkconfig` records the **effective** configuration of every board,
reviewed and checked in. `tools/sdkconfig_baseline.sh` diffs the live build against it:

```bash
tools/sdkconfig_baseline.sh              # every board that has been built
tools/sdkconfig_baseline.sh --update     # re-record, in the SAME commit as the change
```

It exists because 77 behaviour-relevant settings differed from the Arduino build this target
reproduces, and only **six** were ones the project had declared — the rest were IDF defaults
inherited by never naming the symbol. Four of those were restored on 2026-08-04 (CPU frequency
on the S3, tick rate, optimisation level, watchdog panic); the remainder are recorded in
[docs/TOOLCHAINS.md](../../docs/TOOLCHAINS.md) as reviewed divergences.

A setting you did not write does not appear in any file you can review, which is why this gates
the whole config rather than a curated subset. CI checks one board per chip; the other six are
gated only locally.

**`sdkconfig.defaults` edits need a config regeneration.** IDF applies the defaults *only* when
creating `build/<board>/sdkconfig`; once that file exists it is authoritative and edits to the
defaults are silently ignored — the build succeeds and produces a binary with the old config.
`build.sh` now deletes the generated file when any input is newer, so this is handled, but it is
worth knowing when invoking `idf.py` directly.
