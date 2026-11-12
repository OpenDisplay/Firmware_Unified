# SEC2, claim mode and factory reset — implementation plan

Status: **plan, revision 2, 2026-11-11.** This plan is for the implementation agents that follow.
The normative behaviour is in [SECURITY_V2.md](SECURITY_V2.md) and
[FACTORY_RESET.md](FACTORY_RESET.md); where this plan disagrees with them, they win. Nothing here
has been built or measured on hardware. Every size below is an estimate, with its basis stated.

**Implementation base: branch `v3/decisions`** (`v3_worktrees/fw-decisions`, read-only for this
plan). Its BG22 image is 248,516 B against a 253,952 B ceiling, which leaves **5,436 B free**
(`impl_decisions.md`).

## 0. What `v3/decisions` changed that this work must build on

| `v3/decisions` change | Consequence for SEC2 |
|---|---|
| READ_IDENT 3.0 tail `[flags][max_config:2][max_seq_steps:2][escapes]`, with flags bit0 CONFIG_DRIVEN, bit1 NEEDS_MIGRATION, bit2 **BUILTIN_DRIVER** | the SEC2 flag bits move to **3-6** (SECURITY_V2 § 5.2); revision 1's bit 2 SEC_V2 collided. `od_cmd_ident.c` gains the bits; the tail stays readable by length |
| one `fw_target` per MCU family (0x01 ESP32, 0x02 NRF54, 0x03 SILABS, 0x04 NRF52811, 0x05 NRF52840) | the setup URL's `t` is the decimal of this byte (SECURITY_V2 § 7.8) |
| the mode-0 convention with old-client fallback, and the NEEDS_MIGRATION flag | setup screens draw in mode 0 like the boot screen; a NEEDS_MIGRATION device has HAS_PANEL = 0 |
| ESP32 native drivers selected **only** by 0xB3 | a native board (M5Paper, S3 parallel) without 0xB3 cannot draw its SETUP screen; the wizard must write 0xB3 (panel DB `native_controllers`) in the fresh config. Native-driver panels are not exposed to the drive-recipe vector (SECURITY_V2 § 1.4 item 5) |
| PANEL_BUSY NACKs (0x0085 /08, START /09) and CONFIG_WRITE BUSY `[FF][41\|42][01]` | while the device draws a SETUP or CODE screen, config writes may get BUSY 0x01: clients retry (they already must). SEC_* ops that hit a running pass answer `OD_SEC_ERR_BUSY`. The SEC2 CONFIG_WRITE bytes 0x10 / 0x11 sit in the same 4g namespace, clear of 0x00 / 0x01 |
| ACTION_INFO / FIRE moved to shared `od_cmd_action.c` | the claim-mode gate refuses 0x0047-0x004D in one place; the action runner's "off in claim mode" hook goes into the shared runner |
| bounded SPI_RAW hold, released on detach / close / disconnect | the claim-mode FD refusal covers every SPI_RAW sub; no hold can exist at claim-mode entry, because ENTER is before the radio starts |

## 1. Order of work

```
WP0 spec gate (cryptographer: E1-E3 + K0; hardware facts H8, H11; protocol-repo values)
 |
WP1 py-opendisplay reference crypto + vectors V0-V13      <- everything else checks against these
 |                       \
WP2 firmware HAL          WP9a website crypto (independent cross-check of every vector)
 |
WP3 shared od_sec2 (replaces od_session v1)  --- BG22 size checkpoint (mandatory, § 4) ---
 |
WP3b shared od_claim_mode (gesture, state machine, allow-list)
 |
WP4 config policy (boot decision, 0x27 at rest, secrets, migration B2, sticky reset pin, scrub)
 |
WP5 setup screens (SETUP / CODE / RESET) + setup URL      WP8 py-opendisplay client features
 |                                                         |
WP6 factory reset (shared)                                WP9b website wizard + /s/ page     WP10 Home Assistant
 |                                                         |                                   |
WP7 target integration, measurement, one-connection, LAN                                       |
 |                                                                                             |
WP11 docs, checklist rows, release ordering  <-------------------------------------------------+
```

**Release ordering** follows the encryption review's option C hazard:
1. py-opendisplay with SEC2;
2. then HA and the website;
3. then 3.0 firmware.

HA must not offer the 3.0 OTA to a device unless its bundled py-opendisplay speaks SEC2 (WP10).

Rule for every WP: host-tested ≠ hardware-verified. Every hardware row goes into
`docs/HARDWARE_VERIFICATION_CHECKLIST.md` as **open**.

BG22 RAM rule for every WP (docs/BG22_RAM_BUDGET.md): anything that grows BG22 static RAM, or
allocates from the heap at runtime (PSA key slots, ECDH contexts, advertising sets created on
demand), re-runs the heap budget, and the image passes the build's heap gate before any OTA. A
test or trace build must not grow RAM. Below the floor, a BG22 unit still boots and advertises,
but its GATT server is dead, and it can only be recovered by SWD.

## 2. WP0 — spec gate

- **Cryptographer** (SECURITY_V2 § 19): E1 (the extractor assumption), **E2: adopt the
  SP 800-108r1 K0 form or not**, and E3 (a formal model of claim + CONFIRM recommended). A change
  to the KDF, the commitment or the record format must land in the spec before WP1 generates
  vectors.
- **Hardware facts before vectors and before the gesture ships**: H11 (Nordic
  `hwinfo_get_device_id` bytes) and H8 (reset causes per board, incl. nRF54 POR/BOR). These are
  read-only measurements on one board each.
- **Protocol-repo entries.** They are needed from the protocol owner: firmware never hand-edits
  `shared/protocol/*.h`. Record each in `docs/PROTOCOL_CHANGES_3.0.md` as **provisional**.
  - **Opcodes**: 0x0054-0x0057 and RESP_ 0x54-0x57; `AUTH_STATUS_V2_REQUIRED 0x05`.
  - **Error codes**:
    - the `OD_SEC_ERR_*` namespace 0x01-0x0F (with 0x04 renamed NOT_IN_CLAIM_MODE, and the new
      0x0D ATTEMPTS_EXHAUSTED, 0x0E LOCKED, 0x0F WRONG_TRANSPORT);
    - the sealed NACK codes 0xF1-0xF3, with 0xF4 reserved;
    - the dispatcher refusal `[FF][cmd][FD]` CLAIM_MODE;
    - the CONFIG_WRITE 4g bytes 0x10 SECRET_NEEDS_CLAIM / 0x11 KEY_MISMATCH.
  - **Status and advertising bits**:
    - READ_IDENT tail flag bits 3-6;
    - the SEC_STATUS layout (19 B);
    - MSD bit 2 = "in claim mode" (in addition to its existing triggers), and MSD bit 3 =
      claimed/locked on 3.x.
  - **0x27 semantics**:
    - `reserved[0] = 0x02` as the at-rest marker, and the at-rest semantics (enabled 1 + K_v1);
    - flag bits 0 and 1 renamed reserved;
    - `session_timeout_seconds == 0` means 3600 s on 3.x.
  - **The closed secret list** marked in the packet table.
  - **Header text** rewritten for SEC2: "ENCRYPTED ENVELOPE", "AUTH GATING", "CHUNK BUDGETS".
    The auth-required shape there must also be corrected to `[00][cmd][FE]` (the code's shape,
    `od_gate.c:15-22`).
- **Acceptance**: the maintainer answers § 9 Q16, or accepts the recommendation (Q14 and Q15 are decided).

## 3. Firmware

### WP2 — HAL additions (`shared/hal/`, per-target implementations)

| Item | Files | Content |
|---|---|---|
| ECDH | `shared/hal/od_hal_crypto.h`; `targets/esp32-idf/hal/od_hal_crypto.c`, `targets/nordic-zephyr/src/od_hal_crypto.c`, `targets/efr32bg22-slc/od_hal_crypto.c` | `od_hal_crypto_ecdh_keygen(od_hal_crypto_eph_t *h, uint8_t pub[64])`; `od_hal_crypto_ecdh_agree(h, const uint8_t peer[64], uint8_t z[32])`, which destroys the private key on every outcome; `od_hal_crypto_ecdh_discard(h)`. PSA on all three: `psa_generate_key` (ECC_KEY_PAIR(SECP_R1), 256, usage DERIVE, alg ECDH, volatile), `psa_export_public_key` (strip the 0x04), `psa_raw_key_agreement`, `psa_destroy_key`. **Point validation is done before this call, in shared code** (`od_sec2_point_valid()`: X, Y < p, curve equation; SECURITY_V2 § 2). The HAL also returns the new `OD_HAL_CRYPTO_BAD_POINT` if the backend refuses |
| CCM with empty plaintext | same files | lift "at least one ciphertext byte" (`od_hal_crypto.h`); PSA handles zero-length CCM |
| Slots | `od_hal_crypto.h` `OD_HAL_CRYPTO_KEY_SLOTS` → 2 (h2d, d2h); BG22 `SL_PSA_KEY_USER_SLOT_COUNT` 1 → 3 (2 CCM + 1 ephemeral ECC) in `opendisplay-bg22.slcp` / config | |
| Secret records | new `shared/hal/od_hal_secret.h`; per-target `od_hal_secret.c` | `od_hal_secret_read/write/erase(id, …)` for ids KEY (OS2K, 28 B), STATE (OS2S, 16 B), MARKER (OS2R, 12 B), with the `od_hal_nvs_write` atomicity contract. Reads report only VALID / NOT VALID, deliberately (SECURITY_V2 § 10.1). Media: ESP32 NVS namespace `od_sec`; **Nordic a separate NVS/settings area, not the 2 x 4 kB config NVS** (H13); BG22 NVM3 keys outside the config object, counted in the page-capacity budget that gates the 3120 B config (three small objects plus the OS2S churn from gestures) |
| Reset cause + retained RAM | new `shared/hal/od_hal_reset.h` | `od_hal_reset_cause()` → {POWER_ON, PIN, BROWNOUT, POWER_ON_OR_BROWNOUT, SOFTWARE, WATCHDOG, LOCKUP, SLEEP_WAKE, DEBUG, OTHER}, read **and cleared** once per boot. The shared function `od_claim_cause_counted()` maps it per SECURITY_V2 § 7.2. `od_hal_retained_write/read/clear(16 B)` for OS2C. ESP32 `esp_reset_reason` + `RTC_NOINIT_ATTR`; Zephyr `hwinfo_get_reset_cause` / `hwinfo_clear_reset_cause` + `__noinit`; BG22 `RMU_ResetCauseGet` / EMU + BURAM (4 of its retention registers; check which are free) |
| GPIO input | promote `targets/esp32-idf/hal/od_hal_gpio.h` to `shared/hal/od_hal_gpio.h` (`od_hal_gpio_config_input(pin, pullup, pulldown)`, `od_hal_gpio_read(pin)`); implement it on Nordic (via `od_pin_codec`) and BG22 | for the reset pin |
| Test seam | host-only `tests/host/fake_hal_crypto.c` using OpenSSL, or a vendored host-only P-256; a fake secret store with power-cut injection; a fake reset cause / uptime | reproduces the vectors and the V12 traces |

**Acceptance**:
- the host test `tests/host/hal_crypto_contract_test.c` runs V0 (CMAC, CCM incl. empty, ECDH
  CAVP and invalid points) against the fake HAL;
- each target builds;
- on hardware (open rows): V0 and V7 invalid points run through the real HAL on one board per
  target (H1).

### WP3 — shared `od_sec2` (replaces `od_session` v1)

| Delete (v1) | Add / change |
|---|---|
| `shared/core/od_session.c/.h` (handshake step 1/2, v1 KDF, 16-byte envelope nonce, inner length byte, `od_session_derive_tls_psk`), the v1 constants in `od_session.h`; `od_gate_authenticate`; the v1 vectors/tests in `tests/host` (`session_auth` fuzz target etc.) | `shared/core/od_sec2.c/.h`: KDF, key hierarchy (incl. K_v1), claim and auth state machines with the error table of SECURITY_V2 § 8.7, fragment reassembly/emission per § 3.3 (deterministic D->H split), SEC_STATUS (19 B), SEC_KEY with persist-before-reply, record open/seal with per-direction keys and counters 1..0xFFFFFFFE, no-ack handling (§ 9.5), and the ring rate limiter. It keeps `od_nonce_window.h` for RX. `od_sec2_app.h` (replaces `od_session_app.h`): `uid`, `dflags`, `now_ms`, owning connection handle, report hook |
| `od_dispatch.c` / `od_gate.c` v1 gate | the gating table of SECURITY_V2 § 6 as one table-driven function, including the claim-mode allow-list and `[FF][cmd][FD]`; the plaintext set {0x43, 0x44, 0x46, 0x50, 0x54, 0x55, 0x56}; 0x0050 answers NOT_CONFIG / V2_REQUIRED; every response is sealed at emit while a session is live (the `od_reply` / `od_txq` path); WRONG_TRANSPORT for SEC_* on a LAN origin |
| — | `od_cmd_ident.c` flag bits 3-6; `od_advert` bit 3 from "claimed or locked"; od_txq flush of a disconnected connection's frames (verify it exists; add it if not) |

**Acceptance (host)**:
- `tests/host/sec2_test.c` reproduces V1-V8 byte for byte, and every `OD_SEC_ERR_*` path is
  covered;
- the `-Wswitch` exhaustiveness ratchet of `od_gate.h` is kept for the new result enums;
- the fuzz targets `sec2_fragments`, `sec2_record_open`, `sec2_claim` and `sec2_auth` replace
  `session_open_raw`, `session_open_sealed` and `session_auth` in `tools/check.sh`
  `FUZZ_TARGETS`;
- `tests/vectors/dispatch.json` gains the plaintext-set, FE and FD cases;
- the host gate is green, and the shared boundary checks pass.

**BG22 size checkpoint (mandatory)**: build BG22 after WP3 and record the free bytes before WP3b,
WP4, WP5 or WP6 start (§ 4, Q16).

### WP3b — shared `od_claim_mode` (new)

Files: new `shared/core/od_claim_mode.c/.h`, plus hooks in the gate, `od_advert`, the action
runner, the boot path, ESP32 `od_smp.cpp` (predicate `claimed ? unlocked : !claim_mode`), and
the target WiFi start.

- Entry (SECURITY_V2 § 7.1): FRESH / GESTURE / CONTINUE, evaluated before the radio starts; the
  virgin detection, including the factory-embedded config case.
- The gesture counter (§ 7.2): `od_claim_cause_counted()`; writes at T_ARM = 2 s and T_CLEAR =
  10 s; clearing before sleep or reboot; no counter on CLAIMED / LOCKED devices.
- The state machine (§ 7.3): deadline, renders (≤ 3), restarts (≤ 3), `config_locked`, the END
  reasons, the OS2C write before planned reboots, FRESH clearing at END, the reply flush, and a
  software reset without OS2C.
- Signalling: MSD bit 2; READ_IDENT bit 5; SEC_STATUS flags, `window_s` and `renders_left`; the
  LED/buzzer patterns of § 7.7.
- Suppressed while in claim mode: the action runner, image-store rotation, deep sleep, WiFi start,
  SMP.

**Acceptance (host)**:
- V12 traces: every counted and uncounted cause per target mapping; the on-period edges 1.9, 2.0,
  9.9 and 10.0 s; deep sleep after arming; a claimed device writing nothing;
- restart cap, attempt cap and timeout paths;
- the gate allow-list against every opcode in the header, including an unknown one.

### WP4 — config policy

Files: `shared/core/od_config.c`, `od_config_tlv.c`, `od_config_read.c`, `od_config_store.c`;
`targets/nordic-zephyr/src/od_cmd_config.c` (delete the REWRITE_ALLOWED exemption); the target
config parsers where they read 0x27; ESP32 `src/wifi_service.cpp`.

- **The boot decision table** (SECURITY_V2 § 10.1, eight rows) with its repairs, run after the
  factory-reset resume and before claim-mode entry. This includes **migration B2** (row 4: OS2K
  LEGACY gen 1, OS2S, 0x27 at rest) and the stale-key zeroing.
- **0x27 at rest**: enabled 1, K_v1, marker 0x02 on claimed devices. The read view is truthful
  (K_dev, marker 0). Insertion on writes uses the "before the first type > 0x27" rule.
- **Write rules** (§ 10.3): 0x10 / 0x11 in the 4-byte 4g shape; the **sticky reset pin**
  (§ 10.6); claimed CONFIG_CLEAR becomes an atomic replace with a 0x27-only config.
- **Secret scan as a callback in the loader's own walk** (§ 10.4); an unclean walk gets 0x00; the
  identical-secret rule for LEGACY_SECRETS devices.
- **The scrub procedure**, shared by UNCLAIM and factory-reset R1b. On ESP32,
  `esp_wifi_set_storage(WIFI_STORAGE_RAM)` before any `esp_wifi_set_config`, and `nvs.net80211` is
  erased once at the first 3.0 boot and in every scrub.
- The factory-embedded config check (ESP32 `tryProvisionFactoryEmbed`, Nordic `factory_config.c`).

**Acceptance (host)**:
- V9, V10, V11;
- power-cut injection after every write of migration, claim, rotation, UNCLAIM and CONFIG_CLEAR
  converges to a row of § 10.1 that is never "unclaimed with secrets";
- the corpus runner's three composition profiles still pass, and the corpus contains only clean
  blobs.

### WP5 — setup screens

Files:
- `shared/core/od_boot_screen.c/.h`: screen kind NORMAL / SETUP / CODE / RESET; for the three new
  kinds, no 0x2C header, no footer and only fixed strings (SECURITY_V2 § 7.8);
- `shared/core/od_boot_payload.c/.h`: delete the key formatting and the key bytes of the payload;
  add `od_setup_url_build(kind, …)` (`/s/?k=…`);
- `shared/core/od_display.c/.h`: `od_display_boot()` takes the kind, still in mode 0;
- `targets/efr32bg22-slc/opendisplay_display.cpp`: its own renderer, with the same kinds and the
  key lines removed;
- `targets/esp32-idf/src/boot_screen.cpp`: the IT8951 / parallel sink path.

The CODE render starts only **after** the NONCE reply has been queued and flushed. On BG22, where
the refresh blocks the loop, the reply must already be in the stack's TX buffer.

**Acceptance (host)**:
- render tests for 152x152, 200x200, 128x296 and 800x480, in mono and one colour scheme per kind;
- a config whose 0x2C strings are set renders identically for SETUP / CODE / RESET;
- the QR decodes back (with a host QR decoder, or a bit-compare against a reference) to the URL;
- V13.

Hardware: open rows (the QR scanned on a 1.54in, H10).

### WP6 — factory reset (shared)

Files:
- new `shared/core/od_factory_reset.c/.h`: the sampling/debounce state machine, the R0-R8 stage
  machine with **R1b** and stage 4 LOCKED, the marker format, and the LED/buzzer pattern calls;
- `targets/esp32-idf/src/encryption.cpp`: delete `checkResetPin`, and move `secureEraseConfig`
  behind the shared erase; also its `config_parser.cpp:310` call site;
- Nordic `src/main.c`; BG22 `app.c` / `main.c`.

The boot order is FACTORY_RESET.md § 3.1: the check and any resume run **before** the BLE stack
and advertising start. R6 writes OS2S FRESH with the key_gen tombstone, and R8 ends in fresh claim
mode.

**Acceptance (host)**:
- FACTORY_RESET.md § 9: pin traces; a power cut after every write and erase; the A4 property ("no
  point leaves an unclaimed device with secrets", including with the marker deleted); a draw at
  most twice; a failing key erase reaches LOCKED without a loop.

**Presets** (WP8 / WP9): `securityConfig` blocks are generated per board later (maintainer). The
generator and validator enforce FACTORY_RESET.md § 2.1.

Hardware: the open rows of FACTORY_RESET.md § 9.

### WP7 — target integration, measurement, one connection, LAN

- Wire WP2-WP6 into esp32-idf, nordic-zephyr (nRF52840, nRF54L15, nRF54LM20A) and efr32bg22-slc.
- **One connection** (SECURITY_V2 § 5.4):
  - BG22: set the stack connection limit to 1 and resolve the conflicting
    `SL_BT_CONFIG_MAX_CONNECTIONS` definitions (`config/sl_bluetooth_config.h:44` (0) vs
    `config/sl_bluetooth_connection_config.h:38` (4));
  - every target: SEC2 state owned by a connection handle, other handles closed;
  - verify on hardware (H7).
- **DFU as today** (decided). Revision 1's connection binding of the SMP unlock is **removed**
  from this plan. The only DFU change is the claim-mode refusal (the SMP predicate in WP3b).
- **LAN** (SECURITY_V2 § 12):
  - TLS-PSK from K_lan, only when claimed;
  - plaintext frames on a claimed device are gated (FE);
  - SEC_* frames get WRONG_TRANSPORT;
  - WiFi is not started in claim mode or LOCKED.
- **Measure**:
  - BG22 `text+data` against the 253,952 B ceiling;
  - nRF slot use; RAM / heap;
  - **benchmark ECC** keygen + ECDH per target (H2, open row; the SECURITY_V2 § 9.1 latencies
    are estimates);
  - claim-mode battery (H12).
- **Acceptance**: `tools/check.sh --targets` green; sizes recorded in `targets/*/README.md`.

## 4. Flash / RAM estimates (BG22 is the constraint)

Basis:
- BG22 free flash **5,436 B on `v3/decisions` (a594ce6)** (`impl_decisions.md`);
- the measured v1 function sizes from the `v3/seq-streaming` BG22 map (`opendisplay-bg22.map`),
  which `v3/decisions` did not change (its BG22 delta of +224 B is in other files);
- no build was run for this plan.

**What v1 costs today (deleted by WP3 / WP5):**

| | BG22 | nRF52840 (xiao_ble) |
|---|---|---|
| `od_session.c` text | 2,886 B (`od_session_authenticate` 1,256, `_open` 932, `_seal` 370, rest ~330) | ~3,500 B |
| boot-payload key formatting (`od_boot_key_*`, key hex) | ~130 B of `od_boot_payload.c`'s 721 | similar |
| v1 part of `od_gate.c` (400 B in total) | ~100-150 B | similar |
| **v1 deletable** | **~3.1-3.2 KB** | **~3.7 KB** |

**Already placed, no new cost:**
- BG22: `psa_raw_key_agreement` (206 B), `psa_generate_key_custom` (434), `psa_export_public_key`
  (+174 internal), CRYPTOACC key agreement (344) and export (240), CMAC, CCM, TRNG, and `qrcode.c`
  (3,134 B);
- nRF52840: `psa_raw_key_agreement`, `psa_generate_key`, `psa_export_public_key`, `CONFIG_BT_ECC=y`.

**New code (estimates, by analogy with the measured v1 functions):**

| Item | BG22 estimate | Basis |
|---|---|---|
| `od_sec2`: handshake + claim (+EXIT) + fragments + status + SEC_KEY + error table + rate limiter | 2.3-3.1 KB | v1 `od_session_authenticate` is 1,256 B for a 2-step machine; SEC2 has two machines of 2-3 steps plus fragmentation |
| records open/seal (+ no-ack rules) | 0.9-1.2 KB | v1 open + seal is 1,302 B, minus the inner-length handling |
| explicit point validation (P-256 curve check with big-number arithmetic) | **0.4-0.8 KB** | new: the PSA raw API exposes no point check. **Alternative**: if `psa_import_key` of the public point validates on CRYPTOACC (H1), the check reuses it for ~50 B. Decided by H1 |
| HAL: ECDH wrappers, 3 secret records, reset cause, retained RAM | 0.6-1.0 KB | thin PSA / NVM3 / RMU wrappers; v1 `od_hal_crypto.c` is 1,170 B for 8 functions |
| config policy: boot table, 0x27 at rest + K_v1, read view, key match, sticky pin, CONFIG_CLEAR variant, scan-in-walk, identical-secret rule, scrub | 0.8-1.2 KB | the TLV walks already exist (`od_config_tlv.c`) |
| `od_claim_mode`: entry, gesture, state machine, allow-list, hooks, LED patterns | 0.5-0.9 KB | small state machines and one table |
| factory reset | 0.5-0.8 KB | two small state machines |
| setup screen kinds + setup URL (BG22 renderer) | 0.4-0.6 KB | strings and branches; `od_boot_url_build` is 168 B |
| **new total** | **6.4-9.6 KB** | |
| **net (new − v1 deleted)** | **+3.2 to +6.5 KB** against 5,436 B free | **leaves between about +2.2 KB and −1.1 KB** |

**This does not fit with margin.** The `v3/decisions` stream worked to a 4,000 B free-flash
stop line, and SEC2 cannot hold it even at the low estimate. The upper estimate does not fit at
all. Hence the mandatory checkpoint after WP3, and **Q16**. Savings that do not change behaviour,
to be measured, not assumed:
- (s1) no log strings in the new BG22 modules;
- (s2) point validation through PSA import if CRYPTOACC validates (above);
- (s3) the reduced setup-screen layout only on BG22's small panels;
- (s4) BG22-only: the claim-mode LED/buzzer patterns reduced to one pattern;
- (s5) compiler and linker options (`-Os` is assumed; check LTO and `--gc-sections` coverage of
  vendor code).

**RAM, BG22:**
- v1 `struct od_session` is ≤ 128 B (static-asserted).
- The SEC2 session is ~80-100 B: two slot ids, two 32-bit counters, a 32 B RX bitmap, a 16 B
  sid, timestamps, strike counters, and the rate-limiter ring of 10 x 4 B = 40 B.
- A pending claim/handshake union of ~200 B: Qc, Qd, Nc, Nd, K_new, the ephemeral handle and
  timers. The handshake keeps only the derived tags and keys. Fragment reassembly writes straight
  into the union.
- The claim-mode session is ~12 B, and OS2C is 16 B of BURAM.
- Static delta: **+200 to +300 B**.
- Stack peak in a handshake: ~450 B (a 179-B transcript plus a ~195-B KDF input). The PSA ECDH
  stack on CRYPTOACC is **not measured**.
- PSA volatile slots 1 → 3: **not measured**. The heap after 8075018 was 10,120 B; it was **not
  re-measured on `v3/decisions`**.

**Other targets:**
- nRF52840: xiao_nrf52840 text 361,028 B on `v3/decisions`, in a 450,048 B MCUboot primary slot,
  so about 89 KB is free. Flash is not a constraint. Estimate +3-5 KB net.
- nRF54: not measured; same code, ample flash.
- ESP32: not measured. mbedTLS ECDH is already enabled (`MBEDTLS_ECDH_C=y`), so the cost is
  negligible relative to the partition.

## 5. py-opendisplay (`v3/py`) — the reference implementation

### WP1 — crypto core and vectors (first)

Files:
- new `src/opendisplay/sec2.py`: primitives over `cryptography` (AES-CMAC, AESCCM with
  `tag_length=12`, ECDH P-256 plus the explicit point check), the KDF (with the WP0 K0 decision),
  the key hierarchy incl. K_v1, Tc / Ta, the commitment, the code, record seal/open, fragment
  encode/decode with the deterministic split, and the setup URL. These are pure functions, no
  I/O;
- new `scripts/gen_sec2_vectors.py`: writes `tests/fixtures/sec2_vectors.json` with V0-V13 of
  SECURITY_V2 § 18, from fixed inputs, with every intermediate value;
- `tests/unit/test_sec2_primitives.py` (RFC 4493, CAVP CCM/ECDH) and
  `tests/unit/test_sec2_vectors.py` (regenerates and compares).

Keep `crypto.py` (v1) untouched.

**Acceptance**: `pytest tests/unit` is green, and the vectors file is committed and copied (by the
firmware agent) to `Firmware_Unified/tests/vectors/sec2_vectors.json`.

### WP8 — client features

| Area | Files | Behaviour |
|---|---|---|
| protocol | `protocol/commands.py`, `protocol/responses.py`, constants | builders and parsers for SEC_STATUS (19 B) / CLAIM (incl. EXIT) / AUTH / KEY; error enums incl. 0x0D-0x0F and the FD refusal; READ_IDENT bits 3-6; MSD claim-mode candidate (`status & 0x0C == 0x04`) |
| device | `device.py` | choose SEC2 when the firmware major is ≥ 3 (or READ_IDENT SEC_V2); per-connection session state cleared in `_on_disconnect` and never reused; HELLO on every connect; SESSION_REQUIRED → one re-HELLO + one retry for idempotent ops; 90 % renewal; the SECURITY_V2 § 9.5 client receive rules (replacing `< 31 bytes = plaintext`); counter rules incl. REPLAY handling |
| claim | `device.py`, new `claim.py` | `claim(code_callback, no_panel_ack_callback, blank_panel_ack_callback)` → returns K_dev; persists via the key store **and verifies** before CONFIRM (§ 8.8); NOT_IN_CLAIM_MODE → `ClaimModeRequiredError` carrying the gesture text; ATTEMPTS_EXHAUSTED; a mismatch → ABORT + `ClaimCodeMismatchError`; `exit_claim_mode()`; refuses the no-panel path if the key store has a display record for the uid |
| key store | new `keystore.py` | per uid: one confirmed key + candidates (SECURITY_V2 § 8.8); default file in the user config directory, mode 0600, fsync; or a caller callback; **never deletes on negative results** |
| keys | `device.py`, `cli.py` | `rotate_key()`; **default rotation of LEGACY keys on the first SEC2 session** into the key store or the `on_key_rotated` callback (opt-out `rotate_legacy_key=False`); `unclaim()` |
| config | `config_serializer.py` / write path | before writing: if the blob carries a new secret and the device is unclaimed → raise `SecretsNeedClaimError` (and map NACK 0x10 to it); the identical-secret rule for LEGACY_SECRETS devices; zero the 0x27 key / enabled when restoring to another device; map 0x11; write the preset's 0x27 reset-pin defaults |
| exceptions | `exceptions.py` | the errors above + `DeviceAlreadyClaimedError`, `DeviceLockedError`, `RateLimitedError` |
| CLI | `cli.py` | `opendisplay claim`, `key rotate`, `key export` / `import` (the P8 format, *proposal*), `unclaim`, `status` (SEC_STATUS), `setup [--url]` (the wizard, *proposal* P5) with an interactive code prompt |

### 5.2 Version pinning (all clients)

A client stores, per device (keyed by uid where known, else by address/name), "seen SEC2". Once
set, it never sends v1 to that device and never uses a SEC2 key in a v1 handshake, even if a later
FIRMWARE_VERSION (plaintext, forgeable) reports 2.x. The user must clear the pin explicitly, e.g.
after deliberately downgrading the firmware. After such a downgrade, the client uses **K_v1**
(SECURITY_V2 § 10.2) as the v1 key, or K_dev itself if the device's key was never rotated from a
migrated v1 key.

### 5.3 v1 client mitigations (for 2.x and legacy devices)

These are kept from the encryption review § 4 item 5, since new clients keep v1:
- before disconnecting from a v1 device, send AUTH step 1 (it tears the session down on every 2.x
  firmware and on Firmware_NRF);
- check the session id and a per-direction counter window on v1 responses;
- refuse plaintext frames during a v1 session, except the exact `[00|FF][cmd][FE|FF]` refusal
  shapes.

**Acceptance (WP8)**: unit tests with a simulated device built on the WP1 vectors, covering: the
claim happy path, a lost CONFIRM reply (the key is kept), NOT_IN_CLAIM_MODE, ALREADY_CLAIMED, a
code mismatch, no panel (with and without a display record), a blank panel, ATTEMPTS_EXHAUSTED,
EXIT, a reconnect after disconnect, a SESSION_REQUIRED retry, rotation with a lost reply (both
keys kept), the default legacy rotation, secrets-need-claim, the identical-secret rule, a forged
AUTH_FAILED / UNCLAIMED (no key deleted), and LOCKED.

## 6. Website (`v3/website`)

### WP9a — crypto (parallel to WP2)

New `httpdocs/js/od-sec2.js`:
- WebCrypto ECDH P-256: `generateKey`, `exportKey('raw')` → drop the 0x04, `deriveBits` 256;
- the explicit point check (BigInt);
- the existing hand-rolled CMAC/CCM from `ble-common.js`, generalised to a 12-byte tag and empty
  plaintexts;
- the KDF, transcripts, records, fragments and the setup URL.

`tests/sec2.test.js` reproduces **every** vector of `sec2_vectors.json`. This independent second
implementation is mandatory before firmware relies on the vectors.

### WP9b — UI and pages

- **Toolbox** (`firmware/toolbox/index.html`, `js/toolbox-v3.js`, `js/od-v3.js`):
  - the SEC2 path by firmware major;
  - the setup wizard of SECURITY_V2 § 13.1. The step order is decided; the shared texts (P1) are
    a *proposal*;
  - the gesture prompt on NOT_IN_CLAIM_MODE;
  - **typed code entry (decided)**;
  - the no-panel and blank-panel acknowledgements;
  - "Skip setup" (EXIT).
- **Key persistence check** before CONFIRM / ROTATE_CONFIRM: a read-back, and a key-file download
  when `navigator.storage.persisted()` is false or in a private window (§ 8.8).
- **Key store** keyed by uid (`localStorage['opendisplay.deviceKeys']`), with confirmed and
  candidate keys; never deleted on negative results.
- **Default legacy rotation** (setting "Do not rotate migrated keys").
- **Secrets**: the "claim first" prompt with the gesture text before writing WiFi / FindMy to an
  unclaimed device, and on NACK 0x10. The `LEGACY_SECRETS_OPEN` banner, plus a "change your WiFi
  password" reminder after the claim.
- **Backup and restore**: full backup/restore including the key; the 0x27 key is zeroed when
  restoring onto another device.
- **Errors**: FD / 0x0D / 0x0E / 0x0F mapped to user texts.
- **Reconnect** (opendisplay.org#95): clear the session state on `gattserverdisconnected`; HELLO on
  every connect.
- **`?key=`** stays (decided). The hardening of SECURITY_V2 § 10.8, with at least items 1, 3 and 4.
- **Presets**: the generated `securityConfig` defaults (FACTORY_RESET.md § 2.1), with validator
  rules.
- **New page `httpdocs/s/index.html`** per SECURITY_V2 § 7.8 rules 1-6 and FACTORY_RESET.md § 4.3,
  using `simple-config-id-registry.json`. The toolbox accepts the preselection (*proposal* P2 for
  the wizard hand-off).

**Acceptance**: `node --test tests/*.test.js`. The new tests must pass; the 5 pre-existing failures
noted in `impl_integration.md` are not this WP's. Manual browser checks are open rows.

## 7. Home Assistant (`v3/ha`) — WP10

Files: `custom_components/opendisplay/config_flow.py`, `strings.json` + `translations/*`,
`repairs.py`, `coordinator.py`, `update.py`, `manifest.json` (py-opendisplay requirement bump),
`diagnostics.py` (redact the key).

| Flow step | Behaviour |
|---|---|
| discovery | **decided behaviour**: read SEC_STATUS after connecting. **Proposal P3**: a separate discovery card for the MSD claim-mode candidate (bit 2 = 1, bit 3 = 0), *"OpenDisplay OD1A2B3C is waiting to be set up"* |
| `async_step_bluetooth_confirm` | UNCLAIMED in claim mode → "Claim (recommended)" / "Skip setup"; UNCLAIMED in normal mode → "Claim (needs the power-cycle gesture)" / "Use without a key"; CLAIMED → `async_step_encryption_key` (a key exported from another client, imported as a candidate); LOCKED → abort with "factory reset required" |
| `async_step_gesture` | the gesture text; waits for claim mode (it rescans and reconnects) |
| `async_step_claim` / `async_step_claim_code` | BEGIN / NONCE via py-opendisplay; a text field for the 6 digits ("can take up to a minute to appear"); a mismatch → abort with the SECURITY_V2 § 8.4 warning; no panel / blank panel → acknowledgement steps |
| entry creation | the key is written to `entry.data` **and read back** before CONFIRM is sent (py `on_key` callback), marked as a candidate until the reply |
| secrets | after the claim, in the same session: the WiFi / FindMy options, if the user configures them |
| reauth | AUTH_FAILED → the existing reauth flow, **without deleting the stored key** (§ 8.8); LOCKED → a repair issue "factory reset required" |
| repairs | "WiFi/FindMy secrets on an unclaimed display" (LEGACY_SECRETS, with the gesture text); "config with secrets needs a claim" (NACK 0x10 from a service call); "change your WiFi password" after claiming a flagged device |
| options flow | "Show key" (for moving the key to the website or CLI), "Rotate key", "Keep the migrated key" (the opt-out of default rotation) |
| coordinator / transport | a session per connection; HELLO on every connect through the proxy; no session reuse across `bleak` reconnects (py-opendisplay#141); PANEL_BUSY / CONFIG_WRITE_BUSY / FD treated as transient |
| `update.py` | refuse to offer or push 3.0 firmware unless the installed py-opendisplay supports SEC2; after the OTA of an encrypted 2.x device, the first SEC2 session runs the **default rotation** (unless opted out) |

**Acceptance**: HA unit tests for every step above, with a mocked py-opendisplay; translations
present. The ESPHome-proxy end-to-end test is a hardware row (open).

## 8. WP11 — docs and status

- `docs/OD_SESSION.md`: mark it superseded by SECURITY_V2.md (keep it for 2.x reference).
- `docs/FOLLOWUPS.md` § 5 (nonce reuse) and § 4 (unauthenticated CONFIG_WRITE recovery): closed by
  SEC2 and the factory reset, once WP3 / WP6 land.
- `CLAUDE.md` status bullet on the bidirectional nonce reuse: update it when WP3 lands. Only the
  user can authorise CLAUDE.md edits.
- `docs/PROTOCOL_CHANGES_3.0.md`: rows for every value of WP0.
- `docs/HARDWARE_VERIFICATION_CHECKLIST.md`: rows for H1-H13 of SECURITY_V2 § 19, the claim with a
  code on a panel, the gesture on each target, the claim via an ESPHome proxy at MTU 23, a
  reconnect after deep sleep, and the factory-reset rows.
- Release notes: the plain-language § 1.5 of SECURITY_V2, and the old-client note of § 13.6.

## 9. Remaining questions for the maintainer

**Decided** and folded into the spec, listed so that nobody asks again:

| # | Decision |
|---|---|
| Q1-Q4 | full read-back in a session; `?key=` kept; secrets need a key; REWRITE_ALLOWED removed |
| Q5 | no firmware signing in 3.0 |
| Q6 | DFU/SMP unlocked for any peer until reboot after a sealed ENTER_DFU, as today |
| Q7 | the factory-reset pin is the one configured in 0x27, with a per-preset default generated later; no gesture reset (option only, SECURITY_V2 Appendix B.1) |
| Q8 | no button press to accept a claim |
| Q9 | a corrupt key record locks the device until factory reset |
| Q10 | 2.x secrets without a key keep working and are flagged; a v1 key migrates as the device key; firmware rotation is optional; clients rotate by default with an opt-out |
| Q11 | factory reset erases config, key, image store and bonds |
| Q12 | a device without a panel may be claimed after an explicit warning |
| Q13 | the website requires typing the code |
| Q14 | a Core build without the SEC2 module (`OD_SEC2=0`) refuses WiFi / FindMy secrets with 0x10, like every unclaimed device; no build flag allows storing them (SECURITY_V2 § 13.7) |
| Q15 | a FRESH claim-mode session that ends without a claim (timeout, skip, 3 codes) clears FRESH; the device is then an ordinary open device, and claiming it later needs the gesture (§ 7.3) |
| new | claim mode by a deliberate gesture (two quick power-cycles), or fresh / factory-reset; panel writes refused; 10 minutes; the screen shows it; reboot into normal mode at the end |

**Open** (each has a default in the spec that can be flipped without touching the wire format):

| # | Question | Options | Recommendation |
|---|---|---|---|
| Q16 | BG22 flash. SEC2 + claim mode + factory reset are estimated to leave between about +2.2 KB and −1.1 KB of the 5,436 B free on `v3/decisions` (§ 4); the 4,000 B stop line cannot hold | (a) accept a lower floor for 3.0 (e.g. 1,024 B), decided at the WP3 checkpoint; (b) BG22-only UI cuts (savings s3 / s4: the reduced screen layout, one LED pattern); (c) find space elsewhere on BG22 (logging, LTO, s1 / s5), measured; (d) defer part of the scope on BG22 only (e.g. the setup URL QR on the CODE / RESET screens) | **Measure at the WP3 checkpoint first**, then (c) + (b). (a) only for what remains. None of these changes the wire format |

## 10. What this plan does not cover

- OTA signing (decided: not in 3.0);
- BLE privacy / RPA (decision 4);
- the LAN TLS ciphersuite (SECURITY_V2 § 12);
- generating the per-preset reset-pin defaults (maintainer: later);
- any change to the legacy Firmware_NRF fleet, which cannot be updated.
