# SEC2 + factory reset — implementation plan

Status: **plan, 2026-11-11.** For the implementation agents that follow. Normative behaviour is
in [SECURITY_V2.md](SECURITY_V2.md) and [FACTORY_RESET.md](FACTORY_RESET.md); where this plan and
those disagree, they win. Nothing here has been built or measured on hardware; every size below
is an estimate with its basis stated.

## 1. Order of work

```
WP0 spec gate (expert review, protocol-repo values)
 |
WP1 py-opendisplay reference crypto + test vectors   <- everything else checks against these
 |                       \
WP2 firmware HAL          WP9a website crypto (vectors cross-check)
 |
WP3 shared od_sec2 (replaces od_session v1)
 |
WP4 config policy (key field, secrets-need-a-key, migration, UNCLAIM scrub)
 |
WP5 claim-code + recovery screens           WP8 py-opendisplay client features (claim, rotate, CLI)
 |                                            |
WP6 factory reset (shared)                  WP9b website UI + /r/ page     WP10 Home Assistant
 |                                            |                               |
WP7 target integration, size measurement, DFU gating                          |
 |                                                                            |
WP11 docs, checklist rows, release ordering  <---------------------------------+
```

**Release ordering** (from the encryption review's option C hazard): py-opendisplay with SEC2,
then HA and the website, then 3.0 firmware. HA must not offer the 3.0 OTA to a device unless its
bundled py-opendisplay speaks SEC2 (WP10).

Rule for every WP: host-tested ≠ hardware-verified. Every hardware row goes into
`docs/HARDWARE_VERIFICATION_CHECKLIST.md` as **open**.

## 2. WP0 — spec gate

- Expert review of SECURITY_V2 § 15 (E1-E14). Changes to the KDF, commitment or record format
  must land in the spec before WP1 generates vectors.
- Protocol-repo entries (needed from the protocol owner — firmware never hand-edits
  `shared/protocol/*.h`): opcodes 0x0054-0x0057 and RESP_ 0x54-0x57; `AUTH_STATUS_V2_REQUIRED
  0x05`; the `OD_SEC_ERR_*` namespace (0x01-0x0C) and sealed NACK codes 0xF1-0xF4; CONFIG_WRITE
  NACK bytes 0x10 SECRET_NEEDS_CLAIM / 0x11 KEY_MISMATCH; READ_IDENT flag bits 2-5; 0x27 flag bits
  0 and 1 renamed reserved; `session_timeout_seconds == 0` means 3600 s on 3.x; MSD bit 3 meaning on
  3.x; header text for "ENCRYPTED ENVELOPE" / "AUTH GATING" / "CHUNK BUDGETS" rewritten for SEC2.
  Record each in `docs/PROTOCOL_CHANGES_3.0.md` as **provisional**.
- Acceptance: maintainer answers § 9 Q5-Q12 or accepts the recommendations.

## 3. Firmware

### WP2 — HAL additions (`shared/hal/`, per-target implementations)

| Item | Files | Content |
|---|---|---|
| ECDH | `shared/hal/od_hal_crypto.h`; `targets/esp32-idf/hal/od_hal_crypto.c`, `targets/nordic-zephyr/src/od_hal_crypto.c`, `targets/efr32bg22-slc/od_hal_crypto.c` | `od_hal_crypto_ecdh_keygen(od_hal_crypto_eph_t *h, uint8_t pub[64])`, `od_hal_crypto_ecdh_agree(od_hal_crypto_eph_t *h, const uint8_t peer[64], uint8_t z[32])` (validates the peer point, destroys the private key on every outcome, returns `OD_HAL_CRYPTO_BAD_POINT` new status), `od_hal_crypto_ecdh_discard(h)`. PSA on all three: `psa_generate_key` (ECC_KEY_PAIR(SECP_R1), 256, usage DERIVE, alg ECDH, volatile), `psa_export_public_key` (strip the 0x04), `psa_raw_key_agreement`, `psa_destroy_key` |
| CCM with empty plaintext | same files | lift "at least one ciphertext byte" (`od_hal_crypto.h:64`); PSA handles zero-length CCM |
| Slots | `od_hal_crypto.h` `OD_HAL_CRYPTO_KEY_SLOTS` → 2 (h2d, d2h); BG22 `SL_PSA_KEY_USER_SLOT_COUNT` 1 → 3 (2 CCM + 1 ephemeral ECC) in `opendisplay-bg22.slcp`/config | |
| Secret records | new `shared/hal/od_hal_secret.h`; per-target `od_hal_secret.c` | `od_hal_secret_read/write/erase(uint8_t id, ...)` for ids KEY (28 B) and RESET_MARKER (12 B); same atomicity contract as `od_hal_nvs_write`; ENOENT vs EIO vs "present but corrupt" distinguishable (SECURITY_V2 § 9.1). ESP32 NVS namespace `od_sec`; Nordic a separate NVS/settings id (must survive the config-replace -ENOSPC retire path); BG22 NVM3 keys outside the config object, counted in the page-capacity budget |
| Reset cause + retained window | new `shared/hal/od_hal_reset.h` | `od_hal_reset_cause()` → {POWER_ON, BROWNOUT, PIN, BUTTON_WAKE, SOFTWARE, WATCHDOG, TIMER_WAKE, OTHER}; `od_hal_retained_write/read/clear(16 B)`. ESP32 `esp_reset_reason` + `esp_sleep_get_wakeup_cause` + `RTC_NOINIT_ATTR`; Zephyr `hwinfo_get_reset_cause` + `__noinit`; BG22 `RMU_ResetCauseGet`/EMU + BURAM |
| GPIO input | promote `targets/esp32-idf/hal/od_hal_gpio.h` to `shared/hal/od_hal_gpio.h` (`od_hal_gpio_config_input(pin, pullup, pulldown)`, `od_hal_gpio_read(pin)`), implement on Nordic (via `od_pin_codec`) and BG22 | for the reset pin |
| Test seam | host-only `tests/host/fake_hal_crypto.c` using OpenSSL (host gate already builds C with system libs — check; else a vendored host-only P-256) with injectable "random" and ephemeral keys | reproduces vectors |

Acceptance: host test `tests/host/hal_crypto_contract_test.c` runs V0 (CMAC, CCM incl. empty, ECDH
CAVP and invalid points) against the fake HAL; each target builds; on hardware (open rows) V0 and
V7-invalid-point run through the real HAL on one board per target (expert item E7).

### WP3 — shared `od_sec2` (replaces `od_session` v1)

| Delete (v1) | Add / change |
|---|---|
| `shared/core/od_session.c/.h` (handshake step 1/2, v1 KDF, 16-byte envelope nonce, inner length byte, `od_session_derive_tls_psk`), v1 constants in `od_session.h`; `od_gate_authenticate`; v1 vectors/tests in `tests/host` (`session_auth` fuzz target etc.) | `shared/core/od_sec2.c/.h`: KDF, key hierarchy, claim and auth state machines, fragment reassembly/emission, SEC_STATUS, SEC_KEY, record open/seal with per-direction keys and 32-bit counters, keeping `od_nonce_window.h` for RX; `od_sec2_app.h` (replaces `od_session_app.h`): `uid`, `dflags`, `now_ms`, connection handle, report hook |
| `od_dispatch.c:296-353` v1 gate | the gating table of SECURITY_V2 § 6 as one table-driven function; plaintext set {0x43, 0x44, 0x46, 0x50, 0x54, 0x55, 0x56}; 0x0050 answers NOT_CONFIG / V2_REQUIRED; sealed D->H for every response while a session is live (`od_reply`/`od_txq` path seals at emit) |
| — | `od_cmd_ident.c` flag bits 2-5; `od_advert` bit 3 from "claimed"; od_txq flush of a disconnected connection's frames (verify it exists; add if not) |
| — | rate limiter (10 starts / 60 s), timeouts of SECURITY_V2 § 13 |

Acceptance (host): `tests/host/sec2_test.c` reproduces V1-V7, V9 byte for byte; every
`OD_SEC_ERR_*` path covered; the -Wswitch exhaustiveness ratchet of `od_gate.h` kept for the new
result enums; fuzz targets `sec2_fragments`, `sec2_record_open`, `sec2_claim`, `sec2_auth` replace
`session_open_raw`, `session_open_sealed`, `session_auth` in `tools/check.sh` `FUZZ_TARGETS`;
`tests/vectors/dispatch.json` gains plaintext-set and FE cases; `tools/check.sh` host gate green.
Shared boundary checks pass (no vendor headers in `shared/`).

### WP4 — config policy

Files: `shared/core/od_config.c`, `od_config_tlv.c`, `od_config_read.c`, `od_config_store.c`;
`targets/nordic-zephyr/src/od_cmd_config.c` (delete the REWRITE_ALLOWED exemption, lines
~142-155 and ~373-388); target config parsers where they read 0x27.

- Secret detection over the assembled blob (all instances of 0x26/0x27/0x2D, SECURITY_V2 § 9.3);
  NACK 0x10 on unclaimed devices, 0x11 key mismatch on claimed ones; stored 0x27 always has key 0
  and enabled 0.
- CONFIG_READ synthesis: 0x27 `encryption_enabled`/key filled from state; inserted when absent on a
  claimed device; `total_len` adjusted.
- Boot migration (§ 9.4 option B1 by default, A1 for secrets without a key) — two idempotent steps;
  ROTATION_REQUIRED allow-list in the gate; `LEGACY_SECRETS_OPEN` flag.
- UNCLAIM scrub-then-erase ordering. Factory-embedded config check (ESP32
  `tryProvisionFactoryEmbed`, Nordic `factory_config.c`).

Acceptance (host): V8, V10, V11; power-cut injection between the two migration steps and between
scrub and erase converges; the corpus runner's three composition profiles still pass.

### WP5 — claim-code and recovery screens

Files: `shared/core/od_boot_screen.c/.h` (screen kind NORMAL / CLAIM_CODE / RECOVERY; text lines
of SECURITY_V2 § 7.5 and FACTORY_RESET.md § 4.1), `shared/core/od_boot_payload.c/.h` (delete key
formatting and the key bytes of the payload; add `od_boot_recovery_url_build()`),
`shared/core/od_display.c/.h` (`od_display_boot()` takes the kind; still mode 0),
`targets/efr32bg22-slc/opendisplay_display.cpp` (its own renderer: same three kinds, key lines
removed), `targets/esp32-idf/src/boot_screen.cpp` (IT8951/parallel sink path).

The claim-code render must start **after** the NONCE reply is queued and flushed; on targets where
the refresh blocks the loop (BG22), the reply must already be in the stack's TX buffer.

Acceptance (host): render tests for 152x152, 200x200, 128x296, 800x480 in mono and one colour
scheme per kind; QR decoded back (host QR decoder or bit-compare against a reference) equals the
URL; URL vectors (FACTORY_RESET.md § 4.2: all fields, omitted fields, maxima = 79 B). Hardware:
open rows (scan on a 1.54in).

### WP6 — factory reset (shared)

Files: new `shared/core/od_factory_reset.c/.h` (sampling/debounce state machine, R0-R7 stage
machine, marker format, LED/buzzer pattern calls through `od_led`/`od_buzzer`);
`targets/esp32-idf/src/encryption.cpp` (delete `checkResetPin`, move `secureEraseConfig` behind
the shared erase), `config_parser.cpp:310` call site; Nordic `src/main.c`; BG22 `app.c`/`main.c`.
Boot order per FACTORY_RESET.md § 3.1 — the check and any resume run **before** the BLE stack and
advertising start.

Acceptance (host): FACTORY_RESET.md § 9 host list (pin traces, power-cut after every write/erase,
draw at most twice). Presets (WP9/WP8): `securityConfig` blocks for the boards with buttons,
strapping-pin rule enforced by the website's preset validator
(`.github/scripts/validate-simple-config-presets.py`). Hardware: open rows of FACTORY_RESET.md § 9.

### WP7 — target integration, measurement, DFU gating

- Wire WP2-WP6 into esp32-idf, nordic-zephyr (nRF52840, nRF54L15, nRF54LM20A), efr32bg22-slc.
- Bind the session to the connection handle; teardown on that handle's disconnect.
- DFU (SECURITY_V2 § 6.1, conservative default pending Q6): SMP unlock bound to the unlocking
  connection, relocked on its disconnect or after 300 s (`targets/esp32-idf/src/od_smp.cpp:445-448,
  542-545`, `targets/nordic-zephyr/src/opendisplay_ble.c:296-327, 375-387`; Nordic must stop
  disconnecting the unlocker first).
- LAN: TLS-PSK from K_lan (esp32-idf LAN code).
- Measure: BG22 `text+data` vs the 253,952 B app region; nRF slot use; RAM/heap; **benchmark ECC**
  keygen + ECDH per target (hardware, open row) — SECURITY_V2 § 8.1 latency claims are estimates.
- Acceptance: `tools/check.sh --targets` green; sizes recorded in `targets/*/README.md`.

## 4. Flash / RAM estimates

Basis: `v3/seq-streaming` build outputs in sibling worktrees, read-only (no build was run for this
plan): BG22 `fw-seq-streaming/targets/efr32bg22-slc/cmake_gcc/build/base/opendisplay-bg22.map`,
nRF52840 `fw-seq-streaming/targets/nordic-zephyr/build-xiao_ble/zephyr/zephyr/zephyr.map` and
`partitions.yml`; BG22 free flash 5,660 B (impl_seq-streaming.md, after 8075018).

**What v1 costs today (deleted by WP3/WP5):**

| | BG22 | nRF52840 (xiao_ble) |
|---|---|---|
| `od_session.c` text | 2,886 B (`od_session_authenticate` 1,256, `_open` 932, `_seal` 370, rest ~330) | ~3,500 B (`_authenticate` 1,440, `_open` 1,028, `_seal` 496, strings ~464, rest ~120) |
| boot-payload key formatting (`od_boot_key_*`, key hex) | ~130 B of `od_boot_payload.c`'s 721 | similar |
| v1 part of `od_gate.c` (400 B total) | ~100-150 B | similar |
| **v1 deletable** | **~3.1-3.2 KB** | **~3.7 KB** |

The review's "-1 KB for deleting v1" undercounted: the whole of `od_session.c` goes, though most of
its record-layer logic is re-implemented in `od_sec2`.

**Already placed, no new cost:** BG22 `psa_raw_key_agreement` (206 B), `psa_generate_key_custom`
(434), `psa_export_public_key` (+internal 174), CRYPTOACC key agreement (344) and export (240),
CMAC, CCM, TRNG; nRF52840 `psa_raw_key_agreement` (122), `psa_generate_key` (240),
`psa_export_public_key` (186), `CONFIG_BT_ECC=y`. `qrcode.c` (3,134 B on BG22) is already linked.

**New code (estimates, by analogy with the measured v1 functions):**

| Item | BG22 estimate | Basis |
|---|---|---|
| `od_sec2` handshake + claim + fragments + status + SEC_KEY | 2.2-3.0 KB | v1 `od_session_authenticate` is 1,256 B for a 2-step machine; SEC2 has two machines of 2-3 steps plus fragmentation |
| records open/seal | 0.9-1.2 KB | v1 open+seal 1,302 B, minus the inner-length handling |
| HAL ECDH wrappers + secret store + reset cause/retained | 0.5-0.9 KB | thin PSA/NVM3/RMU wrappers; v1 `od_hal_crypto.c` is 1,170 B for 8 functions |
| config policy (secret scan, key match, read synthesis, migration, scrub) | 0.5-0.8 KB | TLV walks already exist (`od_config_tlv.c`) |
| factory reset module | 0.5-0.8 KB | two small state machines |
| screen kinds + recovery URL (BG22 renderer) | 0.3-0.5 KB | strings + branches; `od_boot_url_build` is 168 B |
| **new total** | **4.9-7.2 KB** | |
| **net (new − v1 deleted)** | **+1.7 to +4.1 KB** of 5,660 B free | leaves 1.5-4.0 KB; the upper end is tight — measure after WP3 before WP5/WP6 land, and keep the factory-reset module free of logging strings on BG22 |

RAM, BG22: v1 `struct od_session` is <= 128 B (static-asserted). SEC2 session ~80-100 B (two slot
ids, two 32-bit counters, 32 B RX bitmap, 16 B sid, timestamps, strike/rate counters) plus a
pending claim/handshake union of ~200 B (Qc, Qd, Nc, Nd, K_new, ephemeral handle, timers; the
handshake keeps only derived tags and keys); fragment reassembly writes straight into that union.
Static delta **+150 to +250 B**. Stack peak in a handshake ~450 B (179-B transcript + ~195-B KDF
input; PSA ECDH stack on CRYPTOACC **not measured**). PSA volatile slots 1 → 3: **not measured**,
`psa_crypto_slot_management.c` currently holds 204 B of RAM for its single slot. Heap after
8075018: 10,120 B.

nRF52840: app 365,112 B in a 450,048 B MCUboot primary app slot (`mcuboot_primary_app`
0xd200-0x7b000) → ~85 KB free; the "98.6 % full" figure in older notes does not match this build
(not re-investigated). Flash is not a constraint; estimate +2-4 KB net. nRF54: not measured, same
code, ample flash. ESP32: not measured; mbedTLS ECDH is already enabled (`MBEDTLS_ECDH_C=y`);
expected negligible relative to the partition.

## 5. py-opendisplay (`v3/py`) — the reference implementation

### WP1 — crypto core and vectors (first)

Files: new `src/opendisplay/sec2.py` (primitives over `cryptography`: AES-CMAC, AESCCM with
tag_length=12, ECDH P-256; KDF, key hierarchy, Tc/Ta, commitment, code, record seal/open, fragment
encode/decode — pure functions, no I/O); new `scripts/gen_sec2_vectors.py` (writes
`tests/fixtures/sec2_vectors.json` with the groups of SECURITY_V2 § 14, fixed inputs, every
intermediate); `tests/unit/test_sec2_primitives.py` (RFC 4493, CAVP CCM/ECDH),
`tests/unit/test_sec2_vectors.py` (regenerates and compares). Keep `crypto.py` (v1) untouched.

Acceptance: `pytest tests/unit` green; vectors file committed and copied (by the firmware agent)
to `Firmware_Unified/tests/vectors/sec2_vectors.json`.

### WP8 — client features

| Area | Files | Behaviour |
|---|---|---|
| protocol | `protocol/commands.py`, `protocol/responses.py`, constants | SEC_STATUS/CLAIM/AUTH/KEY builders and parsers, error enums, READ_IDENT flag bits |
| device | `device.py` | choose SEC2 when firmware major >= 3 (or READ_IDENT SEC_V2); per-connection session state cleared in `_on_disconnect` (exists) and never reused; HELLO on every connect; SESSION_REQUIRED → one re-HELLO + one retry for idempotent ops; 90 % renewal; replace the `< 31 bytes = plaintext` read rule with SECURITY_V2 § 8.5 client rules; strictly increasing device counter |
| claim | `device.py`, new `claim.py` | `claim(code_callback, no_panel_ack_callback)` → returns K_dev; persists via a caller-supplied `on_key(uid, key, confirmed)` **before** CONFIRM; REBOOT_REQUIRED → `ClaimWindowClosedError` with the user text; mismatch → ABORT + `ClaimCodeMismatchError` |
| keys | `device.py`, `cli.py` | `rotate_key()`, `unclaim()`, `KeyRotationRequired` (legacy B1) unless `on_key_rotated` given |
| config | `config_serializer.py` / write path | before writing: if the blob carries a secret (same rule as § 9.3) and the device is unclaimed → raise `SecretsNeedClaimError` (and map NACK 0x10 to it); zero the 0x27 key/enabled when restoring to another device; map 0x11 |
| exceptions | `exceptions.py` | the errors above + `DeviceAlreadyClaimedError`, `RateLimitedError` |
| CLI | `cli.py` | `opendisplay claim`, `key rotate`, `key export`, `unclaim`, `status` (SEC_STATUS) with interactive code prompt |

### 5.2 Version pinning (all clients)

A client stores, per device (keyed by uid where known, else address/name): "seen SEC2". Once set it
never sends v1 to that device and never uses a SEC2 key in a v1 handshake, even if a later
FIRMWARE_VERSION (plaintext, forgeable) reports 2.x; the user must clear the pin explicitly
(e.g. after deliberately downgrading firmware).

### 5.3 v1 client mitigations (for 2.x and legacy devices)

Kept from the encryption review § 4 item 5, since new clients keep v1: before disconnecting from a
v1 device, send AUTH step 1 (tears the session down on every 2.x firmware and Firmware_NRF); check
session id and a per-direction counter window on v1 responses; refuse plaintext frames during a
v1 session except the exact `[00|FF][cmd][FE|FF]` refusal shapes.

Acceptance (WP8): unit tests with a simulated device built on the WP1 vectors (claim happy path,
lost CONFIRM reply, window closed, already claimed, code mismatch, no panel, reconnect after
disconnect, SESSION_REQUIRED retry, rotation with lost reply, legacy B1 flow, secrets-need-claim).

## 6. Website (`v3/website`)

### WP9a — crypto (parallel to WP2)

New `httpdocs/js/od-sec2.js`: WebCrypto ECDH P-256 (`generateKey`, `exportKey('raw')` → drop the
0x04, `deriveBits` 256), the existing hand-rolled CMAC/CCM from `ble-common.js` generalised to a
12-byte tag and empty plaintexts, KDF, transcripts, records, fragments. `tests/sec2.test.js`
reproduces **every** vector of `sec2_vectors.json` (an independent second implementation —
mandatory before firmware relies on the vectors).

### WP9b — UI and pages

- Toolbox (`firmware/toolbox/index.html`, `js/toolbox-v3.js`, `js/od-v3.js`): SEC2 path by
  firmware major; claim wizard (window check, REBOOT_REQUIRED prompt with reconnect/poll, code
  **entry** field — SHOULD per spec, recommended MUST, no-panel acknowledgement); "claim first"
  prompt when a config with WiFi password / FindMy keys is about to be written to an unclaimed
  device, and on NACK 0x10; legacy B1 rotation; `LEGACY_SECRETS_OPEN` banner; keys stored in
  `localStorage['opendisplay.deviceKeys']` keyed by uid for SEC2 devices; full backup/restore
  including the key (decided); zero the 0x27 key when restoring onto another device.
- Reconnect (opendisplay.org#95): clear session state on `gattserverdisconnected`; HELLO on every
  connect.
- `?key=` stays (decided). Optional hardening of SECURITY_V2 § 9.7 (strip from the address bar,
  never in generated links unless asked, also accept `#key=`, `Referrer-Policy`).
- Presets: `simple-config-presets.json` `securityConfig` defaults (FACTORY_RESET.md § 2.1) +
  validator rule (no strapping pins, pin must be one of the board's buttons).
- New recovery page `httpdocs/r/index.html` (FACTORY_RESET.md § 4.3) using
  `simple-config-id-registry.json`; toolbox accepts `?board=&display=&power=` preselection.

Acceptance: `node --test tests/*.test.js` (new tests pass; the 5 pre-existing failures noted in
impl_integration.md are not this WP's); manual browser checks are open rows.

## 7. Home Assistant (`v3/ha`) — WP10

Files: `custom_components/opendisplay/config_flow.py`, `strings.json` + `translations/*`,
`repairs.py`, `coordinator.py`, `update.py`, `manifest.json` (py-opendisplay requirement bump),
`diagnostics.py` (redact the key).

| Flow step | Behaviour |
|---|---|
| discovery → `async_step_bluetooth_confirm` | read SEC_STATUS: UNCLAIMED → offer "Claim (recommended)" or "Use without a key"; CLAIMED → `async_step_encryption_key` (existing; user pastes a key exported from another client) |
| `async_step_claim` | runs BEGIN/NONCE via py-opendisplay; REBOOT_REQUIRED → `async_step_reboot_required` (text + "I have power-cycled it" button → retry); ALREADY_CLAIMED → abort with "claimed elsewhere — enter its key or factory-reset it" |
| `async_step_claim_code` | text field for the 6 digits ("can take up to a minute to appear"); mismatch → abort with the warning text of SECURITY_V2 § 7.5; no panel → `async_step_claim_no_panel` acknowledgement checkbox |
| entry creation | the key is written to `entry.data` **before** CONFIRM is sent (py `on_key` callback), marked unconfirmed until the reply |
| reauth | AUTH_FAILED → existing reauth flow; KEY_CORRUPT → repair issue "factory reset required" |
| repairs | "legacy key must be rotated" (B1; HA rotates automatically on first connect after the OTA and updates `entry.data`, raising the issue only on failure); "WiFi/FindMy secrets on an unclaimed display" (A1); "config with secrets needs a claim" (NACK 0x10 from a service call) |
| options flow | "Show key" (for moving the key to the website / CLI), "Rotate key" |
| coordinator / transport | session per connection; HELLO on every connect through the proxy; no session reuse across `bleak` reconnects (py-opendisplay#141) |
| `update.py` | refuse to offer/push 3.0 firmware unless the installed py-opendisplay supports SEC2; after the OTA of an encrypted 2.x device, run the B1 rotation immediately |

Acceptance: HA unit tests for every step above with a mocked py-opendisplay; translations present;
ESPHome-proxy end-to-end is a hardware row (open).

## 8. WP11 — docs and status

- `docs/OD_SESSION.md`: mark superseded by SECURITY_V2.md (keep for 2.x reference).
- `docs/FOLLOWUPS.md` § 5 (nonce reuse) and § 4 (unauthenticated CONFIG_WRITE recovery): closed by
  SEC2 / factory reset once WP3/WP6 land.
- `CLAUDE.md` status bullet on the bidirectional nonce reuse: update when WP3 lands.
- `docs/PROTOCOL_CHANGES_3.0.md`: rows for every value of WP0.
- `docs/HARDWARE_VERIFICATION_CHECKLIST.md`: rows for ECC benchmark, V0/V7 on real HAL, claim with
  code on a panel, claim via ESPHome proxy at MTU 23, reconnect after deep sleep, factory reset rows.

## 9. Questions only the maintainer can answer

Decided on 2026-11-11 and folded into the spec (listed so nobody re-asks): **Q1** secrets readable
in a session — yes, full read-back, no write-only fields; **Q2** `?key=` in website URLs — kept,
optional hardening only; **Q3** unclaimed devices refuse to store secrets — yes; **Q4** Nordic
REWRITE_ALLOWED — removed on every target, bit reserved, recovery = factory reset.

| # | Question | Options | Recommendation |
|---|---|---|---|
| Q5 | Firmware image signing in 3.x (BG22 GBL and ESP32 OTA unsigned; nRF debug MCUboot key accepted for 3.0) | (a) nothing in 3.0, revisit 3.1; (b) ESP32 signed-app verification (no eFuse) in 3.0; (c) project MCUboot keys in 3.0 (breaks SMP from NRF54 1.1.x); BG22 needs a new bootloader for any of it | (a) for 3.0 with (b) in 3.1, and new BG22 units shipped with a signing bootloader; state the limitation in the release notes — every SEC2 guarantee ends at the DFU path |
| Q6 | Bind the DFU/SMP unlock to the authenticated session | (a) as today: unlock until reboot, any peer; (b) bound to the unlocking connection, relock on its disconnect or after 300 s; (c) additionally require SMP frames to be tunnelled through sealed records | (b), the spec's implementation default. Cheap, removes "next connector wins". (c) breaks standard mcumgr tools |
| Q7 | Factory reset for boards without a button | (a) none: USB/SWD mass erase only; (b) power-cycle counter (e.g. 5 power-ons, each < 5 s apart, triggers the reset); (c) a jumper/pad defined per board as reset pin | (a) for 3.0 plus (c) in presets where a free pad exists; (b) costs a flash write per boot and is easy to trigger by flaky power — only if users ask |
| Q8 | Require a button press on the device to accept a claim CONFIRM (closes the device-side takeover gap of SECURITY_V2 § 7.6, E8) | (a) no; (b) yes when a reset pin/button is configured, else window-only; (c) always optional via a 0x27 flag | (b) if the expert review agrees; it is the only thing that turns takeover from "detected" into "prevented". Adds one UX step for button boards |
| Q9 | A secret record that is present but corrupt | (a) fail closed: LOCKED until factory reset (spec default); (b) fail open: device unclaimed | (a): with secrets now stored on claimed devices, fail-open would publish the WiFi password |
| Q10 | Upgrade from 2.x: (i) stored WiFi/FindMy secrets without a key; (ii) a v1 key | (i) A1 keep working + flag / A2 disable until claimed / A3 erase; (ii) B1 migrate + forced rotation / B2 migrate, rotation optional / B3 lock until factory reset / B4 drop key (not acceptable) — SECURITY_V2 § 9.4 | (i) **A1**, (ii) **B1** |
| Q11 | Factory reset also erases the image store and BLE bonds | (a) yes (spec); (b) config + key only | (a): stored images may be private, and a "factory reset" that keeps them surprises users |
| Q12 | Claim of devices that report no panel | (a) allowed after an explicit acknowledgement (spec); (b) refused, such devices stay unclaimed or need a code via another channel (LED blink code) | (a); an LED-blinked code is possible later (6 digits as blink groups) but slow and error-prone |
| Q13 | The website's code check: entry (MUST) or compare-and-click (SHOULD, as specified) | (a) MUST enter, as HA and py; (b) allow compare | (a); "click OK" invites confirmation without comparing (§ 1.4 item 6) |

## 10. What this plan does not cover

OTA signing implementation (Q5), BLE privacy/RPA (decision 4), the LAN TLS ciphersuite (SECURITY_V2
§ 11), and any change to the legacy Firmware_NRF fleet (cannot be updated).
