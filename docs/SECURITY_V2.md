# OpenDisplay Secure Session v2 ("SEC2") — normative specification

Status: **DRAFT revision 2 for maintainer and expert review, 2026-11-11.** Nothing here is
implemented or measured on hardware. Every opcode, status byte, error code, flag bit, record
layout and label is **provisional** until it is written into the canonical
`opendisplay-protocol` header. § 19 lists what still needs a cryptographer or a measurement on
a device; those points are not settled claims.

Companion documents: [FACTORY_RESET.md](FACTORY_RESET.md) (reset pin, erase ordering, reset
screen) and [SECURITY_V2_IMPLEMENTATION_PLAN.md](SECURITY_V2_IMPLEMENTATION_PLAN.md) (work
breakdown, cost, remaining questions).

The words MUST, MUST NOT, SHOULD and MAY are used in the RFC 2119 sense.

---

## Changelog

### Revision 2 (2026-11-11)

Inputs: the independent review (`review_security_v2.md`), the maintainer's answers to Q5-Q13,
and the maintainer's new central decision, **claim mode by a deliberate gesture**.
Appendix A answers every review finding, including the ones I disagree with.

| What changed | Why |
|---|---|
| The "first 10 minutes after any physical boot" claim window and `REBOOT_REQUIRED` are gone. They are replaced by **claim mode** (§ 7): entered only on a fresh or factory-reset device, or by two quick power-cycles. In claim mode the device refuses every command that can put pixels on the panel, and it shows that it is in claim mode. Claim mode ends after 10 minutes, on cancel, or after the claim, and the device then reboots into normal mode. Normal boots of an unclaimed device have no claim window and behave exactly as today | maintainer decision; closes review A1 (the attacker draws the code) and A8 (the window could be reopened remotely) |
| Claim attempts are capped at 3 code renders per claim-mode session (§ 7.5). G5 is restated per session (§ 1.3) | A7 |
| New firmware-drawn **setup screen family** (SETUP, CODE, RESET), with no config-supplied text on it. One setup URL `opendisplay.org/s/` replaces `/r/` (§ 7.8) | maintainer; A1 (0x2C strings were a drawing vector) |
| A fresh-install reference flow, its unhappy paths, and the 2.x upgrade, re-claim and old-client flows (§ 13). A table of every earlier decision the gesture touches (§ 14). A unified setup proposal for the website, HA and py-opendisplay, kept separate from the decisions (§ 15) | maintainer request |
| DFU: after a sealed ENTER_DFU the update service stays unlocked for any peer until reboot, **as today**. The connection binding is removed. No firmware signing in 3.0 (§ 6.1) | maintainer Q5/Q6; A6 |
| No button press to accept a claim. First claimer wins (§ 8.5, § 1.5) | maintainer Q8 |
| 2.x v1 key: **migrated as the device key (B2)**. Rotation is optional in firmware. Website, HA and py-opendisplay rotate by default on the first SEC2 connection, with an opt-out. Sealed NACK 0xF4 and the forced-rotation allow-list are deleted; 0xF4 is reserved (§ 10.5) | maintainer Q10; A6; residual R4 |
| 2.x secrets without a key keep working and are flagged (A1 option), so clients prompt to claim. Claiming them needs the gesture (§ 10.5) | maintainer Q10 |
| Storage is fail-closed by construction (§ 10.1). "Absent" and "corrupt" are no longer told apart. The claimed state has independent evidence: a SEC2 marker in config 0x27, plus a key generation number in a new state record. A key record that is unreadable while that evidence exists means **LOCKED until factory reset** | maintainer Q9; A3; C5 |
| 0x27 is "poisoned" at rest on a claimed device: `encryption_enabled = 1`, with a v1-compatible key derived from K_dev. A downgrade to 2.x then fails closed (§ 10.2) | A5 |
| Factory reset: scrub secrets (new step R1b) **before** erasing the key. The reset ends in fresh claim mode. It erases config, key, image store and bonds (FACTORY_RESET.md) | maintainer Q11; A4 |
| Reset pin: "the reset pin configured in 0x27; the default comes from the preset of the simple-config id". On a claimed device it is sticky-enabled: CONFIG_CLEAR and config writes preserve it (§ 10.6) | maintainer; A9 |
| SEC_KEY does the work, persists, and only then replies (§ 11) | A10 |
| Clients never delete or replace a stored key on unauthenticated or forged evidence (UNCLAIMED, AUTH_FAILED, a bad tag), and verify durable persistence before CONFIRM (§ 8.8) | A2 |
| One application connection per device is normative on every target (§ 5.4) | A11 |
| Plaintext LAN on a claimed device answers SESSION_REQUIRED. SEC_CLAIM, SEC_AUTH and SEC_KEY are BLE-only. TLS runs only on claimed devices. Claim mode does not start WiFi (§ 12) | A12 |
| The secret scan is part of the loader's own walk, and the secret list is closed (§ 10.4) | A13 |
| Decision 3 corrected: write-without-response exists on every target. Sealed handling of no-ack frames is specified, along with the counter-consumption and retry rules; counter 0xFFFFFFFF is unusable (§ 9.5) | A6, A14 |
| ESP32 WiFi credentials: `WIFI_STORAGE_RAM`, and `nvs.net80211` is erased in every scrub (§ 10.4) | A16 |
| The no-panel path is refused when the client has any record of a display. A new "blank panel" path covers a configured panel that does not draw (§ 8.6) | A15, maintainer Q12 |
| Every interop ambiguity of A18 is settled before vectors: label and context lengths, uid byte order, deterministic D->H fragment split, error-state table, rate-limiter algorithm, rounding, 0x27 insertion rule, point validation, dflags. Vectors renumbered (§ 2.3, § 3.3, § 4.3, § 8.7, § 17, § 18) | A18 |
| Wire values moved to avoid the `v3/decisions` assignments. READ_IDENT tail flag bits 0-2 are now CONFIG_DRIVEN / NEEDS_MIGRATION / BUILTIN_DRIVER, so the SEC bits move to **3-6**. `fw_target` now has five values (URL `t`). CONFIG_WRITE NACK byte 0x01 is BUSY; 0x10/0x11 are unaffected (§ 3, § 5) | `impl_decisions.md` |
| A plain-language section, "what this does not protect" (§ 1.5) | maintainer |
| SP 800-108r1 conformance is no longer claimed. The K0 question goes to the cryptographer before vectors (§ 2.1, § 19) | C3 |

### Revision 1 (2026-11-11)

First complete draft: SEC2 claim and session, factory reset, implementation plan.

---

## 0. Decisions this design is bound by

Made by the maintainer on 2026-11-10/11 and not re-argued here:

1. **Clean break.** 3.0 firmware speaks only SEC2. The v1 handshake (`CMD_AUTHENTICATE 0x0050`
   steps 1/2, `od_session.c`) and the v1 envelope are deleted from 3.0 firmware. Old clients
   cannot use a *claimed* 3.0 device. New clients keep v1 for 2.x firmware and for the
   un-updatable `Firmware_NRF` fleet.
2. **A device without a key is open, as today, in normal mode.** There is no physical-presence
   requirement for ordinary commands and no claim window after an ordinary boot. Claim mode
   (decision 6) is the only exception, and it is entered only physically.
3. **Works through ESPHome Bluetooth proxies.** Application layer only: no BLE pairing or
   bonding. Small MTU. Write-with-response, write-without-response and notify, as today. *(Revision
   1 said "write-with-response only"; that was wrong, see § 9.5.)* **One key per device.**
4. **Session timeout is 1 hour by default.** No change to advertising privacy.
5. **The device generates its key.** Provisioning is a one-time *claim* over an ephemeral ECDH
   exchange. The client must confirm with one authenticated message before the device marks
   itself claimed. Until then, a new claim restarts with a new key. Afterwards the key is never
   shown on the panel and never sent in clear; it is readable only inside an authenticated
   session (decision 8).
6. **Claiming happens only in claim mode.** Claim mode is entered only by:
   - (a) a fresh device (first boot after flashing: no config, never claimed) or a factory-reset
     device;
   - (b) a deliberate gesture: two quick power-cycles in a row.

   In claim mode:
   - external panel writes are refused;
   - a 10-minute limit applies;
   - the screen shows that the device is in claim mode;
   - when claim mode ends (claimed, timed out or cancelled), the device reboots into normal mode.

   A 6-digit code derived from the key-exchange transcript is shown on the e-paper and **typed**
   into the client. This applies to every client, the website included.
7. **No button press to accept a claim.**
8. **No write-only secrets** on a claimed device. A sealed CONFIG_READ returns the full config,
   including the 0x27 key, the WiFi password and the FindMy keys (§ 10.3).
9. **Secrets need a key.** An unclaimed device refuses to store a WiFi password or FindMy key
   material (§ 10.4). It is otherwise fully open.
10. **The device key is set only by the claim and by rotation.** The 0x27 key field of a config
    write must be zero or match (§ 10.3), and `encryption_enabled`, as read, is always truthful.
11. **DFU:** after a sealed ENTER_DFU the update service stays unlocked for any peer until
    reboot, as today. There is no firmware signing in 3.0.
12. **A corrupt key record locks the device until factory reset.**
13. **Factory reset** erases config, key, image store and bonds. It is triggered by the reset pin
    configured in 0x27; the default pin comes from the preset of the device's simple-config id
    (per-preset pin definitions are generated later).
14. **2.x upgrade.**
    - A device with stored WiFi/FindMy secrets and no key keeps them working, and is flagged so
      that clients prompt the user to claim it.
    - A v1 key migrates as the SEC2 device key. Rotation is optional in firmware.
    - Clients (website, py-opendisplay, HA) rotate to a device-generated key by default on the
      first SEC2 connection, with an opt-out.
15. **A device without a panel may be claimed** after an explicit warning that the code check is
    skipped.

Earlier decisions of the same dates are also folded in:
- REWRITE_ALLOWED is removed;
- `?key=` website URLs are kept;
- secrets are fully readable in a session.

Remaining questions are in
[SECURITY_V2_IMPLEMENTATION_PLAN.md § 9](SECURITY_V2_IMPLEMENTATION_PLAN.md#9-remaining-questions-for-the-maintainer).
Where this spec needs a choice the maintainer has not made, it picks one, says so, and makes
it easy to change.

---

## 1. Threat model

### 1.1 Assets

- **K_dev**, the 16-byte device key, and everything it protects: the ability to send gated
  commands (config write, upload, DFU, SPI_RAW, ...).
- Confidentiality of config contents on a claimed device (WiFi password, FindMy keys) and of
  images in transit.
- Integrity of commands and responses on a claimed device.
- The meaning of the claim: the client that completed it holds the key, and nobody else does.

### 1.2 Attackers in scope

| Id | Attacker | Capability |
|---|---|---|
| P | Passive sniffer in radio range | records every BLE PDU of any number of sessions, including the claim |
| A | Active peer in radio range without K_dev | connects to the device; impersonates the device to a client (address spoofing, relaying through a second radio); injects, drops, reorders and replays frames on a link it can reach; runs many handshakes; writes anything an open device accepts |
| M | Former client | once held K_dev; the key was rotated since |
| X | Attacker who learns K_dev *later* (a stolen HA backup, etc.) | plus P's recordings |

Out of scope: anyone with physical access to the device (they can power-cycle it, hold its
reset pin, or read its flash), and anyone who controls the user's client.

### 1.3 Security goals

| # | Goal | Holds against |
|---|---|---|
| G1 | A claimed device executes a gated command only if it was sealed under a live session established with K_dev | P, A, M (after rotation) |
| G2 | Every sealed frame is confidential and integrity-protected in both directions. No replay, no reflection, no reordering beyond the receive window, no cross-session splicing | P, A |
| G3 | Forward secrecy: recordings of past BLE sessions stay confidential if K_dev leaks later | X (session establishment uses fresh ECDH; under the extractor assumption of § 19 E1) |
| G4 | A passive attacker learns nothing about K_dev from a claim | P |
| G5 | See below | A |
| G6 | Offline guessing of K_dev from any transcript the device produces for an unauthenticated peer is impossible. For a device *impersonator* see § 9.3 and residual R4 | P, A |
| G7 | A stale or misrouted reply from an earlier connection or handshake cannot be accepted as part of a new one (py-opendisplay#141, opendisplay.org#95) | A, and plain bugs |

**G5**, revision 2. A client whose user typed the code shown on the device holds a K_dev that
no attacker knows, except with probability at most **3·10⁻⁶ per claim-mode session**: 3 code
renders per session (§ 7.5), each 10⁻⁶. Conditions:
- **Gesture claim mode** (§ 7.1 b): this holds against every radio attacker, provided the
  device's firmware was not replaced while it was open (residual R3). Nothing a radio peer can
  send in claim mode changes the panel (§ 7.4). A config written before claim mode cannot show
  a code the attacker learns only during the claim (§ 7.4, rationale).
- **Fresh claim mode** (§ 7.1 a), which allows config writes before the first code render: it
  holds against an attacker who does not also write the device's drive configuration during
  that claim mode. A two-radio man-in-the-middle present at first setup can defeat it (§ 13.2
  U3, § 1.5).

### 1.4 Explicitly out of scope / NOT protected (technical list)

This design does **not** protect against the following. Clients and docs must not imply that it
does. § 1.5 says the same in plain language.

1. **Unclaimed devices in normal mode** are fully open (decision 2): anyone in range can read and
   write config, draw on the panel, run SPI_RAW, enter DFU, and upload firmware. They cannot be
   given *new* secrets (decision 9). A device upgraded from 2.x may still hold a WiFi password or
   FindMy keys readable by anyone until it is claimed (decision 14, § 10.5).
2. **Firmware authenticity: this is the ceiling of the scheme.** BG22 GBL and ESP32 OTA images
   are unsigned, and nRF images use the public MCUboot debug key. After the owner (or HA's
   automatic OTA) sends a sealed ENTER_DFU, the update path is open to **any** peer until reboot
   (decision 11):
   - BG22: the Gecko AppLoader accepts any peer;
   - ESP32 / Nordic: SMP stays unlocked until reboot.

   Firmware pushed through it reads K_dev and the WiFi password from plaintext NVS/NVM3, answers
   SEC_AUTH correctly, and impersonates the device indefinitely. On an open device the update
   path is open anyway.
3. **Physical access.** This covers flash readout, SWD/JTAG, USB serial reflash, desoldering,
   power-cycling (the gesture), and holding the reset pin (the intended recovery path). K_dev is
   stored in plaintext flash. "Secure erase" on NVS / NVM3 / Zephyr NVS is best effort: old
   records, including pre-rotation keys and 2.x configs, can survive until garbage collection.
4. **First claimer wins.** Anyone in radio range while a device is in claim mode can claim it
   before the owner (decision 7). The owner *detects* this: their claim gets ALREADY_CLAIMED, or
   the code does not match, or a code appears that they did not ask for. Recovery is a factory
   reset. But a claiming attacker can install firmware that ignores the reset pin (item 2), so
   recovery from a takeover is guaranteed only by a USB/SWD reflash. A fresh device has no
   reset pin until a config with 0x27 is written.
5. **A two-radio MITM during fresh setup.** In fresh claim mode the drive configuration may be
   written, and the code is drawn through it. A man-in-the-middle who impersonates the device to
   the owner while also writing the real device's drive configuration can draw any code (§ 13.2
   U3). Devices whose panel is driven by a built-in driver (`BUILTIN_DRIVER`, 0xB3) or by factory
   panel data are not exposed to the recipe part of this. Gesture claim mode refuses config
   writes and is not affected. Mitigation: § 15 P4.
6. **Claims without a working panel** have no code check. They rely on claim mode alone, and a
   MITM present during that claim wins silently (§ 8.6).
7. **Users who type a code without looking at the panel.** The client asks the user to type
   the code, but it cannot make them read it from the display.
8. **Denial of service**: jamming; holding the single connection; flooding handshakes (the rate
   limit then locks the owner out for up to 60 s); forging SESSION_REQUIRED or error frames to
   force re-handshakes; forcing bad-tag teardowns; cancelling another user's claim mode with
   SEC_CLAIM EXIT; claiming an open device the owner chose to leave unclaimed (only after the
   owner's gesture, or on a fresh device).
9. **Traffic analysis**: the opcodes of host->device frames, frame lengths, timing, and the MSD
   advertisement are visible. That includes claim mode (MSD bit 2, § 7.7).
10. **Tracking / privacy**: static address and name, plaintext MSD, static FindMy keys
    (decision 4).
11. **Relay attacks** (distance bounding): an attacker relaying between a far-away device and the
    client is not detected. The session stays end-to-end protected.
12. **A compromised client** (HA host, browser, PC), or any client the owner gave the key to,
    until the key is rotated (§ 11). **Any session can read K_dev and every secret** (decision 8);
    there are no roles.
13. **2.x firmware and the legacy Firmware_NRF fleet.** They keep v1 and its known weaknesses
    (encryption review W4/W4b/W5/W6). New clients apply the v1 mitigations of
    [the plan § 5.3](SECURITY_V2_IMPLEMENTATION_PLAN.md#53-v1-client-mitigations-for-2x-and-legacy).
14. **Migrated 2.x keys** are as strong as they were: often typed, put in `?key=` links, or shown in
    the 2.x QR. A device impersonator gets one offline-guessing sample per client reconnect
    (§ 9.3). Default client rotation (decision 14) is the mitigation.
15. **The LAN (TLS-PSK) transport** beyond deriving its PSK from K_dev (§ 12). It has no forward
    secrecy unless an (EC)DHE-PSK suite is negotiated.
16. **Side channels and fault injection** on the device's ECC/AES implementations.
17. **RNG failure** beyond fail-closed behaviour. Every target's HAL refuses to produce randomness
    when its backend fails, but entropy quality (e.g. ESP32 with RF off) is not re-verified here.
18. **An attacker who induces two brown-outs with the right timing** on a weak battery of an
    nRF52 device (where brown-out and power-on cannot be told apart, § 7.2) can put an
    *unclaimed* device into claim mode remotely and claim it. Such a device was open, and so
    reflashable, anyway.

### 1.5 In plain words: what this does not protect

For release notes, the website and the HA documentation. Keep the wording simple.

- **The firmware update is the weak point.** When you (or Home Assistant) start a firmware
  update on a claimed display, anyone nearby can send it different firmware until it restarts.
  Firmware is not signed in 3.0. Bad firmware could read the display's key and your WiFi
  password. Update only when nobody you distrust is in Bluetooth range, and let the update
  finish.
- **Whoever claims first owns the display.** While a display is in claim mode (fresh, after a
  factory reset, or after you switched it off and on twice), anyone nearby with an OpenDisplay
  app can claim it before you. You will notice: your claim fails, or the code does not match, or
  a code appears before you pressed "Claim". Then factory-reset the display. If someone else
  claimed it and also updated its firmware, only re-flashing over USB or the debug port is
  certain to help.
- **Every app you give the key to can do everything.** Any connection that knows the key can
  read the key itself, your WiFi password and your FindMy keys. Backups contain them. Give the
  key only to apps you trust, and rotate it when you stop trusting one.
- **An unclaimed display is open.** Anyone nearby can change its picture, its settings and its
  firmware. It will not accept a WiFi password or FindMy keys until it is claimed. A display
  updated from 2.x may still hold them openly until you claim it. After claiming, change your
  WiFi password if the display was ever unclaimed with it stored.
- **The code only helps if you read it from the display.** Type the code that is on the
  display, not one you got from anywhere else. If you cannot see the display, the code check
  does not protect you.
- **First setup is the most exposed moment.** A determined attacker with two radios, present
  exactly while you set up a brand-new display, can defeat the code check. For the strongest
  setup, configure the display first, then switch it off and on twice and claim it (§ 15 P4).
- **Physical access wins.** Someone holding the display can reset it, re-flash it or read its
  memory.
- **Bluetooth is visible.** People nearby can see that a display exists, roughly what it does
  and when, and when it is in claim mode. They cannot read protected content.

---
## 2. Primitives

These are restricted to what **every** target already links:
- encryption review § 2.9;
- the BG22 map file of `v3/seq-streaming`, where `psa_raw_key_agreement`, `psa_generate_key`,
  `psa_export_public_key`, `psa_mac_compute`, `psa_aead_encrypt/decrypt` and the CRYPTOACC TRNG
  are placed;
- nRF52840 `CONFIG_PSA_WANT_ALG_ECDH=y`, `..._ECC_KEY_PAIR_GENERATE=y`, `..._ECC_SECP_R1_256=y`,
  `..._CMAC=y`, `..._CCM=y`;
- ESP-IDF mbedTLS ECDH/CMAC/CCM.

**No SHA-2, no HMAC, no HKDF**: they are not placed on BG22.

| Name | Definition | Reference |
|---|---|---|
| `AES` | AES-128 block cipher | FIPS 197 |
| `CMAC(k, m)` | AES-128-CMAC, full 16-byte output | NIST SP 800-38B, RFC 4493 |
| `CCM(k, n, a, p)` | AES-128-CCM, 13-byte nonce (L = 2), **12-byte tag** (M = 12); output `c \|\| t` | NIST SP 800-38C, RFC 3610 |
| `ECDH(d, Q)` | P-256 (secp256r1) ECC CDH primitive. The output Z is the 32-byte big-endian x-coordinate of d·Q | NIST SP 800-56A Rev 3 § 5.7.1.2 |
| `Q` on the wire | 64 bytes `X \|\| Y`, each 32 bytes big-endian: the SEC1 uncompressed form without its `04` prefix. Implementations prepend `04` for PSA / WebCrypto import | SEC1 § 2.3.3 |
| `RAND(n)` | n bytes from the target's CSPRNG/TRNG (`od_hal_crypto_random`, fail-closed) | — |
| `ct_equal` | constant-time comparison of equal-length byte strings | — |

**Point validation (normative, A18.9).** A received `Q` is valid only if all of these hold:
- `X < p`;
- `Y < p`;
- `Y² ≡ X³ − 3X + b (mod p)`, with the P-256 constants.

The identity has no `X || Y` encoding, and (0, 0) fails the equation. P-256 has cofactor 1, so
these checks are full public-key validation (SP 800-56A § 5.6.2.3.3). The check runs in
`shared/` code or in the HAL wrapper **before any vendor ECC call**, and fails with BAD_POINT. No
target relies on its vendor library to validate (§ 19 E7).

### 2.1 Key derivation function

**Expansion.** This is modelled on NIST SP 800-108 KDF in counter mode, with PRF = AES-128-CMAC,
8-bit counter and 16-bit length fields, and **always exactly one output block** (L = 128):

```
KDF(K_in, Label, Context) = CMAC(K_in, 0x01 || Label || 0x00 || Context || 0x00 0x80)
```

- `0x01` is [i]_8, the block counter. It is constant because L = 128 bits = one block.
- `Label` is an ASCII string from § 2.3. It never contains 0x00.
- `0x00 0x80` is [L]_16 = 128, big-endian.
- The input is injective: Labels are NUL-free and followed by 0x00, and for each Label the
  Context is one of the fixed-length forms listed in § 2.3.

**Conformance is not claimed (revision 2).** SP 800-108r1 recommends a variant for CMAC-based
KDFs that adds a value K0 = PRF(K_in, Label || 0x00 || Context || [L]) to each iteration. The
reason is *key control*: CMAC is invertible under a known key. That concern applies to one-block
outputs too; revision 1's argument that one block sidesteps it was wrong (review C3). In this
design, no party that knows K_in gains from steering an output:
- in SEC_AUTH, the last input block contains the device's fresh Nd;
- in a claim, a MITM already controls every input.

The cryptographer decides **before WP1 generates vectors**: either adopt the K0 form (one extra
CMAC per derivation, about 30 bytes of code), or keep this form with the conformance claim
dropped (§ 19 E2). Until then, this form is normative.

**Extraction.** This follows the NIST SP 800-56C Rev 2 § 5.1 two-step key derivation:
randomness extraction with MAC = AES-128-CMAC, where the 128-bit salt is the CMAC key.

```
PRK = CMAC(salt, Z)
```

- Claim: `salt = 0^16`.
- Session establishment: `salt = K_ext`, a secret derived from K_dev (§ 2.2). 800-56C allows a
  secret salt. This is the same shape as Bluetooth LE SC f5. The security argument is an
  ideal-cipher-model one (§ 19 E1).

Implementers must not "simplify" either construction. For example, they must not skip
extraction and key the expansion with Z directly.

### 2.2 Key hierarchy

K_dev is never used directly as a CMAC or CCM key. The device derives the keys below at boot,
and again on claim and rotation. The client derives them when it stores the key.

```
uid    = 8-byte device identity (§ 4.3)
K_ext  = KDF(K_dev, "OD2/k-ext",     uid)   salt for session extraction
K_conf = KDF(K_dev, "OD2/k-conf",    uid)   key-confirmation tags (claim, rotation)
K_lan  = KDF(K_dev, "OD2/k-lan",     uid)   TLS-PSK for the LAN transport (§ 12)
K_v1   = KDF(K_dev, "OD2/v1-compat", uid)   0x27 key at rest, for downgrade (§ 10.2); NOT a key any 3.0 code uses
```

### 2.3 Label registry (lengths are normative, A18.1-2)

All Labels and prefixes are ASCII with no NUL terminator. Each Label is used for exactly one
purpose.

| Label | Bytes | K_in | Context (length) | Output use |
|---|---|---|---|---|
| `OD2/k-ext` | 9 | K_dev | uid (8) | K_ext |
| `OD2/k-conf` | 10 | K_dev | uid (8) | K_conf |
| `OD2/k-lan` | 9 | K_dev | uid (8) | K_lan |
| `OD2/v1-compat` | 13 | K_dev | uid (8) | K_v1 |
| `OD2/wrap` | 8 | PRK_claim | Tc (179) | k_wrap, the one-shot CCM key that carries K_new |
| `OD2/h2d` | 7 | PRK_claim / PRK_auth | Tc (179) / Ta (178) | host->device record key |
| `OD2/d2h` | 7 | PRK_claim / PRK_auth | Tc (179) / Ta (178) | device->host record key |
| `OD2/sid` | 7 | PRK_claim / PRK_auth | Tc (179) / Ta (178) | 16-byte session id; it binds SEC_KEY and is never on the wire |
| `OD2/claim-c` | 11 | K_conf | Tc (179) | client's claim confirmation tag |
| `OD2/claim-d` | 11 | K_conf | Tc (179) | device's claim completion tag |
| `OD2/auth-c` | 10 | PRK_auth | Ta (178) | client's handshake proof |
| `OD2/auth-d` | 10 | PRK_auth | Ta (178) | device's handshake proof |
| `OD2/rotate-c` | 12 | K_conf_new | uid (8) \|\| sid (16) = 24 | client's rotation confirmation |

Three labels take two Context lengths: `OD2/h2d`, `OD2/d2h` and `OD2/sid`. This is harmless:
- the two forms are keyed by different PRKs;
- Tc and Ta begin with distinct prefixes, `OD2/claim` (9 bytes) and `OD2/auth` (8 bytes);
- Tc and Ta also differ in length.

Non-KDF strings:

| String | Bytes | Use |
|---|---|---|
| `OD2/claim` | 9 | prefix of Tc |
| `OD2/auth` | 8 | prefix of Ta |
| `OD2/commit` | 10 | commitment input |
| `OD2/code` | 8 | code input |
| `OD2/wrap` | 8 | AAD prefix of W (`"OD2/wrap" \|\| uid`) |

Two non-KDF uses of CMAC are copied in structure from Bluetooth LE Secure Connections numeric
comparison (Core Spec v5.x Vol 3 Part H § 2.2.6 f4 and § 2.2.9 g2):

```
Cd       = CMAC(Nd, "OD2/commit" || uid || Qd || Qc)          commitment, keyed by the secret Nd   (10+8+64+64 = 146 B input)
code_raw = CMAC(Nc, "OD2/code"   || uid || Qc || Qd || Nd)     confirmation-code source             (8+8+64+64+16 = 160 B input)
```

---

## 3. Wire framing overview

All SEC2 opcodes follow the protocol's universal framing (`opendisplay_protocol.h`, UNIVERSAL
FRAMING):
- requests are `[cmd_hi][cmd_lo][payload]`;
- responses are `[status][cmd_lo][data]`;
- multi-byte integer fields are little-endian unless stated.

Points `Q` and CMAC/KDF outputs are byte strings, sent in the byte order defined above.

### 3.1 New opcodes (provisional)

`0x0054..0x0057` are still free on `v3/decisions` (`fw-decisions/shared/protocol/
opendisplay_protocol.h`):
- 0x0053 CMD_DEEP_SLEEP is the last of the 0x005x family;
- no RESP_ value 0x54..0x57 exists;
- no shipped firmware or py-opendisplay uses them.

| Opcode | Name | Framing | Allowed when |
|---|---|---|---|
| `0x0054` | `CMD_SEC_STATUS` | plaintext, single frame | always |
| `0x0055` | `CMD_SEC_CLAIM` | plaintext, fragmented (§ 3.3), BLE only | claim mode (§ 7) |
| `0x0056` | `CMD_SEC_AUTH` | plaintext, fragmented, BLE only | claimed device |
| `0x0057` | `CMD_SEC_KEY` | **sealed only**, BLE only | claimed device, live session |

`CMD_AUTHENTICATE 0x0050` keeps its number on 3.0 only to give old clients a clean refusal
(§ 16.2). New status byte: `AUTH_STATUS_V2_REQUIRED = 0x05` (provisional).

### 3.2 SEC2 response and error shape

```
success   [0x00][cmd_lo][sub:1][frag:1][body chunk...]        (CLAIM / AUTH, § 3.3)
          [0x00][0x54][status body]                            (STATUS, § 5)
error     [0xFF][cmd_lo][sub:1][err:1]                         4 bytes, always plaintext
```

`sub` in an error is the request's sub byte, or 0x00 when the request was too short to carry one
(A18.5). `err` is scoped to opcodes 0x0054..0x0057, which share one namespace, `OD_SEC_ERR_*`:

| err | Name | Meaning / client action |
|---|---|---|
| 0x01 | MALFORMED | Wrong length, bad fragment, unknown version or sub. Fix the client. |
| 0x02 | BAD_STATE | A message out of order (§ 8.7 table). Restart from the first message. |
| 0x03 | RATE_LIMITED | Too many handshake starts (§ 17). Wait 60 s. |
| 0x04 | **NOT_IN_CLAIM_MODE** | (revision 1: REBOOT_REQUIRED) The device is unclaimed and in normal mode. Ask the user for the gesture (§ 7.1). |
| 0x05 | ALREADY_CLAIMED | The device has a key. Use SEC_AUTH. If the user has no key: factory reset. |
| 0x06 | NOT_CLAIMED | SEC_AUTH or SEC_KEY on a device without a key. |
| 0x07 | AUTH_FAILED | A proof or confirmation tag mismatch: wrong key (AUTH), or the code check was bypassed or a MITM is present (CLAIM). Unauthenticated: never delete a key because of it (§ 8.8). |
| 0x08 | EXPIRED | The step arrived after its timeout (§ 17). Restart. |
| 0x09 | BAD_POINT | The peer public key failed validation (§ 2). |
| 0x0A | CRYPTO_ERROR | A device engine failure (RNG, ECC). Retry later; if it persists, it is a hardware fault. |
| 0x0B | STORAGE_ERROR | Persisting failed. The device kept its previous state (§ 8.7, § 11). Retry. |
| 0x0C | BUSY | Another long operation (display pass, flash erase) holds the device. Retry after it. |
| 0x0D | ATTEMPTS_EXHAUSTED | (new) The third code of this claim-mode session was used. Claim mode ends and the device reboots into normal mode. Gesture again to retry. |
| 0x0E | LOCKED | (new) The key record is unreadable while the device has evidence of a claim (§ 10.1). Factory reset required. Unauthenticated, so treat it as advisory. |
| 0x0F | WRONG_TRANSPORT | (new) A SEC_CLAIM, SEC_AUTH or SEC_KEY frame arrived over LAN (§ 12). |

### 3.3 Handshake fragmentation (CLAIM and AUTH only)

Handshake messages are up to 97 bytes. A BLE notification at ATT MTU 23 carries 20 value bytes.
Each handshake message is therefore carried as fragments:

```
H->D  [0x00][cmd_lo][sub:1][frag:1][chunk]
D->H  [0x00][cmd_lo][sub:1][frag:1][chunk]
frag  bit 7 = LAST; bits 0-6 = fragment index 0, 1, 2, ... in order
```

Rules (normative, A18.4):

- The message **body** is the concatenation of the chunks, in index order. Every message has a
  **fixed body length** (§ 8.1, § 9.2). There is no length field.
- **H->D split: any.** Each non-LAST fragment carries **at least 1** byte; an empty non-LAST
  fragment is MALFORMED. A LAST fragment may carry 0 bytes only if the body is complete without
  it. The client may use any chunk size up to its usable write size: 20 bytes at MTU 23, up to
  249 at the protocol's 253-byte frame ceiling. Each fragment is one write-with-response.
- **D->H split: deterministic.** Every non-LAST fragment carries exactly `frag_max − 4` bytes,
  and the LAST fragment carries the rest. A message with an empty body (ABORT reply, EXIT reply)
  is one LAST fragment with no chunk. Vectors depend on this.
- `frag_max` is declared by the client in byte 1 of BEGIN or HELLO, in the range 20..244. Clients
  that do not know their MTU MUST send 20.
- **Validation point.** The device checks index order and the running length at every fragment:
  - an out-of-order index, a body that would exceed the fixed length, or a `sub` that differs
    from fragment 0's is MALFORMED at that fragment;
  - the content (`ver`, `frag_max`, points) is validated only once the body is complete, at the
    LAST fragment;
  - a LAST fragment that leaves the body short is MALFORMED.
- **Index limit.** Index 127 without LAST is MALFORMED; there is no wrap. The largest body (82
  bytes) needs at most 82 fragments.
- The device sends **no** notification for a non-LAST fragment; the ATT write response is the
  flow control. It answers the LAST fragment with the response message. It answers any malformed
  fragment with a 4-byte error, and the error discards the partial message.
- Reassembly state belongs to the one application connection (§ 5.4), and only one message is
  reassembled at a time. If the next fragment does not arrive within 5 s of the previous one, the
  reassembly is discarded without a reply. Because empty non-LAST fragments are refused, a peer
  holding reassembly open must send real bytes. The fixed length bounds that to 82 bytes per
  message.
- Fragmentation applies only to SEC_CLAIM and SEC_AUTH. Sealed records (§ 9) are never
  fragmented by this layer.

### 3.4 Refusal shapes (summary)

| Bytes | Meaning | Who sends it |
|---|---|---|
| `[00][cmd_lo][FE]` | SESSION_REQUIRED: gated command, claimed device, no live session | dispatcher gate (the existing `RESP_AUTH_REQUIRED` shape, `od_gate.c` `queue_status`) |
| `[00][cmd_lo][FF]` | v1 decrypt failure (existing, 2.x/v1 only; not sent by 3.0) | — |
| `[FF][cmd_lo][FE]` | frame too long (existing, `od_dispatch.c` `refuse_oversize`) | dispatcher |
| **`[FF][cmd_lo][FD]`** | **CLAIM_MODE: refused because the device is in claim mode** (new, provisional, `OD_GATE_CLAIM_MODE`) | dispatcher gate, plaintext on an unclaimed device and sealed after the claim |
| `[FF][cmd_lo][F1..F3]` inside a sealed D->H frame | RECORD / REPLAY / INTEGRITY (§ 9.5) | record layer |
| `[FF][cmd_lo][sub][err]` | SEC opcode error (§ 3.2) | SEC handlers |

`0xFD` follows the dispatcher-level precedent of `0xFE` (oversize). It is not a per-opcode error
byte, and no per-opcode namespace in the `v3/decisions` header uses `0xFD`. Old clients see a
3-byte NACK and report a failed command, which is loud, not silent.

The protocol header text says the auth-required frame is `[0xFE][cmd_echo]`. The code sends
`[00][cmd][FE]` (`od_gate.c:15-22`). The header text should be corrected when the SEC2 values
are adopted (plan WP0).

---

## 4. Device state and identity

### 4.1 Persistent and volatile state

Three small records live **outside** the config record, through the new secret-record HAL (plan
WP2):

| Record | Layout | Written |
|---|---|---|
| **OS2K** key | `"OS2K"(4) \| ver 0x02 \| origin (1: 0x01 CLAIMED, 0x02 LEGACY) \| gen (2 LE) \| K_dev (16) \| crc32 (4)` = 28 B | claim, migration, rotation; erased by UNCLAIM and factory reset |
| **OS2S** state | `"OS2S"(4) \| ver 0x01 \| flags (1) \| short_boots (1) \| reserved (1) \| key_gen (2 LE) \| reserved (2) \| crc32 (4)` = 16 B. flags: bit0 FRESH, bit1 LEGACY_SECRETS (accepted at the first 3.0 boot), bit2 MIGRATED (first 3.0 boot done), others 0 | first 3.0 boot; the gesture counter (§ 7.2); claim-mode end (clears FRESH); key_gen on claim/rotation; set FRESH by factory reset |
| **OS2R** reset marker | FACTORY_RESET.md § 5.2 | factory reset only |

crc32 is ISO-HDLC, as in `od_config_store`, over all preceding bytes of the record. A record
whose magic, version, length or CRC is wrong, or whose read fails, or that is missing, is **NOT
VALID**. These cases are not told apart (§ 10.1).

Volatile state (RAM; nothing here survives a power loss):

| State | Lifetime |
|---|---|
| claim-mode session `{kind FRESH/GESTURE, deadline, renders_used, restarts_used, config_locked}` | until claim mode ends. It is carried across a *planned software reset* in retained RAM as the 16-byte **OS2C** record `{"OS2C", kind, renders_used, restarts_used, config_locked, pad, crc32}` (ESP32 `RTC_NOINIT_ATTR`, Zephyr `__noinit`, BG22 BURAM). It is used only when the next reset cause is SOFTWARE (§ 7.3) |
| pending claim (Qc, Qd, ephemeral private key, Nc, Nd, K_new candidate) | until CONFIRM, disconnect, timeout, ABORT or a new BEGIN |
| pending handshake (expected auth-c tag, derived record keys) | until FINISH, disconnect, timeout or a new HELLO |
| session (k_h2d, k_d2h in HAL slots; counters; sid; start time) | until teardown (§ 9.6) |

### 4.2 Device security states

```
                 boot decision (§ 10.1)
                        |
     +---------+--------+-----------------+-------------------+
     v         v                          v                   v
  UNCLAIMED  UNCLAIMED+LEGACY_SECRETS   CLAIMED             LOCKED
     |  (normal mode; open)    |           |  (sessions)       | (refuses all gated; gesture
     |                         |           |                   |  ignored; only factory reset)
     +--- fresh / factory reset / gesture --+                   |
               v                            |                   |
          CLAIM MODE (§ 7)  ------- CONFIRM ok + persisted --> CLAIMED (+ claim mode
               |                                                until it ends) --reboot--> CLAIMED
               +-- timeout / EXIT / 3 attempts / restart cap --reboot--> UNCLAIMED (normal)

  CLAIMED --SEC_KEY UNCLAIM--> UNCLAIMED (normal mode; no claim mode)
  CLAIMED --factory reset----> UNCLAIMED, FRESH -> CLAIM MODE
  LOCKED  --factory reset----> UNCLAIMED, FRESH -> CLAIM MODE
```

### 4.3 Device identity `uid` (A18.3)

The uid is 8 bytes, stable for the life of the silicon, not secret, and visible on the wire in
SEC_STATUS. **It MUST NOT change between 3.x firmware versions on the same chip.** A changed uid
is a different device to every client and strands its stored key.

| Target | uid |
|---|---|
| efr32bg22-slc | `SYSTEM_GetUnique()` (uint64), as 8 bytes big-endian |
| nordic-zephyr (nRF52840, nRF54) | the first 8 bytes `hwinfo_get_device_id(buf, 8)` writes, in buffer order. The exact bytes per SoC are recorded from a real board before vectors are frozen (§ 19 H11), because the driver's byte order is not verified here |
| esp32-idf | `0x00 0x00` followed by the 6 bytes `esp_efuse_mac_get_default(mac)` writes, `mac[0]` first. Example: a MAC printed `24:6F:28:AB:CD:EF` has `mac[0] = 0x24` and gives uid `00 00 24 6F 28 AB CD EF` |

The uid binds every key and transcript to one device and fixes the roles: the device's uid is
always the one inserted. The client takes the uid from SEC_STATUS at claim time and stores it
with K_dev. It then uses the **stored** uid for every later handshake. If a device reports a
different uid, the handshake fails: that is a different device, or an attacker.

The 6-hex-digit display id `OD1A2B3C` (`od_boot_app_device_id24()`, derived from the hardware
address) is independent of the uid. It is the only identity drawn on setup screens (§ 7.8).

### 4.4 `dflags` — device capability byte, included in every transcript

| Bit | Name | Meaning |
|---|---|---|
| 0 | HAS_PANEL | a display is configured, its mode 0 is drawable (not NEEDS_MIGRATION), and the device will try to render the code |
| 1 | HAS_RESET_PIN | the reset pin is enabled in 0x27 |
| 2 | KEY_LEGACY | K_dev's origin is LEGACY, i.e. migrated from 2.x. **Always 0 in a claim transcript** |
| 3 | CLAIM_MODE_FRESH | in a claim transcript: this claim-mode session is FRESH (config writes were allowed). 0 in auth |
| 4-7 | reserved | 0 |

Both sides use the dflags byte **received in this handshake** (the device's reply) for Tc / Ta
(A18.11).

---

## 5. Status and discovery

### 5.1 `CMD_SEC_STATUS` 0x0054 (plaintext, always allowed)

```
request   [0x00][0x54]
response  [0x00][0x54][sec_ver:1][state:1][flags:1][window_s:2 LE][uid:8][timeout_s:2 LE][dflags:1][renders_left:1]
          19 bytes total (fits one notification at MTU 23)
```

| Field | Values |
|---|---|
| sec_ver | 0x02 |
| state | 0 UNCLAIMED, 1 CLAIM_PENDING (a claim was started on this connection and is not yet confirmed), 2 CLAIMED, 3 LOCKED |
| flags | bit0 CLAIM_MODE · bit1 SESSION_ACTIVE (this connection) · bit2 RESET_PENDING (never seen by a client in practice) · bit3 LEGACY_SECRETS_OPEN (unclaimed, but the stored config holds 2.x secrets, § 10.5) · bit4 ROTATION_SUGGESTED (origin LEGACY) · bit5 CLAIM_MODE_FRESH · bit6 CONFIG_LOCKED (claim mode refuses config writes now, § 7.4) · bit7 reserved |
| window_s | `floor(remaining_ms / 1000)` of the claim-mode deadline; 0 outside claim mode (A18.7) |
| timeout_s | the session lifetime a session opened now gets (§ 9.6); snapshotted when the session opens |
| dflags | § 4.4 |
| renders_left | codes left in this claim-mode session (3, 2, 1, 0); 0 outside claim mode |

SEC_STATUS is unauthenticated and therefore advisory. A client uses it to choose its next step.
Every security decision is made again inside the authenticated transcript (uid and dflags are in
Tc and Ta), and no client deletes a key because of it (§ 8.8).

### 5.2 READ_IDENT (0x0046) tail flags (provisional bits, moved for `v3/decisions`)

`v3/decisions` assigned bits 0-2 of the READ_IDENT 3.0 tail `flags` byte:
- 0x01 CONFIG_DRIVEN;
- 0x02 NEEDS_MIGRATION;
- 0x04 BUILTIN_DRIVER.

Revision 1 had put SEC_V2 on bit 2; that collision is fixed. The SEC2 bits are:

| Bit | Mask | Name | Meaning |
|---|---|---|---|
| 3 | 0x08 | SEC_V2 | the firmware has the SEC2 module (§ 13.6). 1 on every OpenDisplay 3.0 build |
| 4 | 0x10 | CLAIMED | state CLAIMED or LOCKED |
| 5 | 0x20 | CLAIM_MODE | in claim mode |
| 6 | 0x40 | LEGACY_SECRETS_OPEN | as SEC_STATUS flags bit 3 |
| 7 | 0x80 | reserved | 0 |

READ_IDENT stays plaintext, never gated (as on `v3/decisions`). A 2.x reader that stops before
the tail is unaffected.

### 5.3 Advertisement (MSD) and discovery without connecting

The 16-byte MSD has no free status bit:
- bit0 is battery bit 8;
- bit1 is REBOOT;
- bit2 is CONNECTION_REQUESTED;
- bit3 is ENCRYPTION_ENABLED;
- bits 4-7 are the loop counter;
- `dynamic[11]` is config-owned.

The 31-byte advertising PDU is full, with flags, the 8-character name and the MSD. The scan
response carries the 128-bit service UUID. A second MSD element with the same company id would
replace the first in bleak/HA's per-company dictionary.

| Signal | Meaning on 3.0 |
|---|---|
| status bit 3 `ENCRYPTION_ENABLED` | **1 = CLAIMED or LOCKED** (a session is required). It is computed from the security state, never from the 0x27 bytes at rest (§ 10.2). Clients behave as on 2.x ("needs a key") |
| status bit 2 `CONNECTION_REQUESTED` | **set continuously while in claim mode** (and still set by its existing triggers otherwise, which do not run in claim mode, § 7.4) |
| **"waiting to be claimed"** | `bit 2 = 1 && bit 3 = 0`, on a device that a client has seen report firmware ≥ 3 or that it does not know yet. This is a candidate only. The client confirms it after connecting with READ_IDENT bit 5 / SEC_STATUS |

Why bit 2: "the device wants a host to connect" is what claim mode means. No shipped client acts
on the bit: py-opendisplay only parses it (`models/advertisement.py:563`), and HA does not read
it. Changing the local name was rejected: it would overflow the 31-byte PDU, and HA matches on
the `OD` prefix (`config_flow.py:82-87`).

The signal is unauthenticated and spoofable. It only lists candidates; the code protects the
claim. Privacy: it reveals "being set up now" for at most the claim-mode duration (§ 1.4 item 9).

### 5.4 One application connection (normative, A11)

Every target MUST run **exactly one application BLE connection** at a time. All SEC2 state
belongs to that connection: pending claim, pending handshake, reassembly and session.

| Target | Mechanism | Status |
|---|---|---|
| esp32-idf | `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1` (`sdkconfig.defaults:9`, all baselines) | in place |
| nordic-zephyr | `CONFIG_BT_MAX_CONN=1` (`zephyr/prj_ncs.conf:14`) | in place |
| efr32bg22-slc | The application never restarts connectable advertising while `g_connection != 0xFF` (`opendisplay_ble.c:488, 1589`), and the stack stops it on connect (`:1961-1966`). But the stack limit is ambiguous: `config/sl_bluetooth_config.h:44` says `(0)` while `config/sl_bluetooth_connection_config.h:38` says `(4)`. WP7 MUST set it to 1 and verify (§ 19 H7) | **to do** |

As defence in depth, every target's SEC2 glue records the connection handle that owns the state.
A frame from any other handle gets no SEC2 processing, and that connection is closed.

---

## 6. Gating table — every opcode

Columns:
- **U**: unclaimed, normal mode;
- **CM**: claim mode, device still unclaimed;
- **C/no**: claimed, no live session on this connection;
- **C/s**: claimed with a live session;
- **L**: LOCKED.

After a successful claim the device is C/s while still in claim mode until the reboot. In that
state the CM refusals still apply, and come back sealed.

Values:
- "open" = plaintext, executed as today;
- "FE" = `[00][cmd_lo][FE]` SESSION_REQUIRED, not executed;
- "FD" = `[FF][cmd_lo][FD]` CLAIM_MODE refusal, not executed;
- "sealed" = must arrive sealed (§ 9); a plaintext frame gets FE.

| Opcode | Name | U | CM | C/no | C/s | L | Notes |
|---|---|---|---|---|---|---|---|
| 0x000F | REBOOT | open | open: a reboot *continues* claim mode (§ 7.3) | FE | sealed | FE | |
| 0x0040 | CONFIG_READ | open | open | FE | sealed | FE | A claimed device's config is readable only in a session, and that read is complete (§ 10.3) |
| 0x0041 | CONFIG_WRITE | open; secret → NACK 0x10 | **FRESH, before the first code render: open (secret → 0x10). Otherwise FD** | FE | sealed; key must be 0 or K_dev, else 0x11; reset pin sticky (§ 10.6) | FE | CONFIG_WRITE BUSY 0x01 (`v3/decisions`) applies unchanged |
| 0x0042 | CONFIG_CHUNK | as 0x0041 (checked at the last chunk) | as 0x0041 | FE | sealed | FE | |
| 0x0043 | FIRMWARE_VERSION | open | open | open | **plaintext** | open | unchanged |
| 0x0044 | READ_MSD | open | open | open | **plaintext** | open | made plaintext-exempt on every target |
| 0x0045 | CONFIG_CLEAR | open | as 0x0041 | FE | sealed | FE | preserves 0x27's reset-pin fields and the claim evidence (§ 10.6); never touches OS2K/OS2S |
| 0x0046 | READ_IDENT | open | open | open | **plaintext** | open | SEC bits (§ 5.2) |
| 0x0047..0x004D | FLASH_* / ACTION_* | open | **FD** | FE | sealed | FE | image store and actions can put pixels on the panel |
| 0x0050 | AUTHENTICATE (v1) | `[00][50][03]` NOT_CONFIG | `[00][50][03]` | `[00][50][05]` V2_REQUIRED | `[00][50][05]` (plaintext; no teardown) | `[00][50][05]` | the v1 machine is deleted |
| 0x0051 | ENTER_DFU | open | **FD**, and SMP is refused too (§ 6.1) | FE | sealed | FE | |
| 0x0052 | POWER_OFF | open | **FD** | FE | sealed | FE | claim mode stays awake |
| 0x0053 | DEEP_SLEEP | open | **FD** | FE | sealed | FE | |
| 0x0054 | SEC_STATUS | open | open | open | **plaintext** | open | |
| 0x0055 | SEC_CLAIM | NOT_IN_CLAIM_MODE | **open** (BLE only) | ALREADY_CLAIMED (EXIT: § 8.1) | ALREADY_CLAIMED (EXIT: § 8.1) | LOCKED | |
| 0x0056 | SEC_AUTH | NOT_CLAIMED | NOT_CLAIMED | open (handshake) | open; a HELLO tears the live session down (§ 9.6) | LOCKED | BLE only |
| 0x0057 | SEC_KEY | NOT_CLAIMED (4-byte error) | NOT_CLAIMED | FE | sealed | FE | rotation / unclaim, BLE only |
| 0x0070..0x0072 | DIRECT_WRITE_* | open | **FD** | FE | sealed | FE | |
| 0x0073 | LED_ACTIVATE | open | **FD** | FE | sealed | FE | the device owns the LED for its claim-mode pattern |
| 0x0075 | LED_STOP | open | **FD** | FE | sealed | FE | |
| 0x0076 | PARTIAL_WRITE_START | open | **FD** | FE | sealed | FE | |
| 0x0077 | BUZZER | open | **FD** | FE | sealed | FE | |
| 0x0080..0x0082 | PIPE_WRITE_* | open | **FD** | FE | sealed | FE | |
| 0x0083 | NFC_ENDPOINT | open | **FD** | FE | sealed | FE | not needed for setup |
| 0x0084 | SPI_RAW | open | **FD** (every sub) | FE | sealed | FE | can drive the panel directly |
| 0x0085 | DRIVE_WRITE_START | open | **FD** | FE | sealed | FE | |
| any other | unknown | unknown-opcode behaviour (Unified: no reply) | FD | FE | sealed, then unknown-opcode behaviour | FE | the gate runs before routing, so an unknown opcode cannot be used to probe |
| LAN | — | § 12 | WiFi not started | § 12 | § 12 | WiFi not started | |

Claim mode is an **allow-list**, not a deny-list:
- allowed: {0x000F, 0x0040, 0x0041/0x0042/0x0045 (FRESH, before the first render), 0x0043,
  0x0044, 0x0046, 0x0050 refusal, 0x0054, 0x0055};
- every other opcode, including any added later, is FD.

Rules:

1. The **plaintext set** on a claimed device is exactly {0x0043, 0x0044, 0x0046, the 0x0050
   refusal, 0x0054, 0x0055 (refusal or EXIT), 0x0056}. These are always sent and answered in
   plaintext, even inside a session. A client MUST NOT seal them, and the device MUST answer FE
   to a sealed one; otherwise it would be a second encoding of the same command.
2. A device MUST NOT execute any part of a gated handler before the frame's tag has verified.
   That includes partial parsing with side effects, such as arming a flash PRELOAD.
3. A claimed device answers FE, never a NACK or silence, to a plaintext gated command, so that a
   client always learns "session required" in one shape.
4. In claim mode, the device's **own** content sources are off as well:
   - the action runner (0xB2 triggers: buttons, timers, BOOT);
   - scheduled image-store rotation;
   - the post-config-write boot-screen redraw (replaced by the setup screen, § 7.8).

   The only pixels drawn in claim mode are the firmware's setup screens.

### 6.1 DFU (decided: as today)

- **Claimed device**: DFU is reachable only after a **sealed** ENTER_DFU.
  - nordic-zephyr SMP and esp32-idf SMP then stay unlocked for **any peer until reboot**, as
    today (`targets/esp32-idf/src/od_smp.cpp:445`, `od_smp_unlock` `:542`;
    `targets/nordic-zephyr/src/opendisplay_ble.c`).
  - efr32bg22-slc reboots into the Gecko AppLoader, which accepts any peer and any unsigned GBL.
  - No connection binding and no time limit (maintainer Q6 = (a)). Residual: § 1.4 item 2,
    review R2.
- **Unclaimed device in normal mode**: every DFU path is open, as today. ESP32 `od_smp.cpp:445`
  refuses SMP only when `od_smp_encryption_on()`. On 3.0 that predicate means "claimed", taken
  from the security state, never from the 0x27 bytes at rest.
- **Claim mode**: ENTER_DFU answers FD, and SMP requests are refused (`RC_DENIED`) as on a
  claimed device without unlock. The predicate becomes `claimed ? unlocked : !claim_mode`. A
  user who wants to update first cancels claim mode (SEC_CLAIM EXIT).
- No firmware signing in 3.0 (maintainer Q5 = (a)).

---
## 7. Claim mode

Claim mode is the only state in which a device can be claimed. It is a short, physically
started, visibly announced setup state. During it, no radio peer can change what the panel
shows.

### 7.1 Entry

Entry is evaluated once per boot:
- after the factory-reset resume (FACTORY_RESET.md § 5.3);
- after the boot decision (§ 10.1);
- **before** the BLE stack, WiFi or the display start.

It applies only if the security state is UNCLAIMED, with or without LEGACY_SECRETS. A CLAIMED
or LOCKED device never enters claim mode (§ 7.6).

| Kind | Condition |
|---|---|
| **FRESH** | `OS2S.FRESH = 1`, **or** the device is virgin. Virgin means: OS2S not valid, OS2K not valid, and either no config record or only a config provisioned this boot from factory data (ESP32 `tryProvisionFactoryEmbed`, Nordic `factory_config.c`). A virgin device first writes `OS2S {FRESH, MIGRATED}` |
| **GESTURE** | this boot's reset cause is counted (§ 7.2) and `OS2S.short_boots ≥ 1` |
| **CONTINUE** | the reset cause is SOFTWARE and a valid OS2C record is in retained RAM. This is the same session after a planned reboot (§ 7.3) |

Consequences:

- A **2.x device updated to 3.0** has a config, so it is not virgin. Its first 3.0 boot writes
  `OS2S {MIGRATED[, LEGACY_SECRETS]}` and runs in normal mode (§ 10.5). A 2.x device that was
  never configured is virgin and starts in FRESH claim mode.
- **Re-flashing over USB** without erasing NVS keeps OS2S, so the device is not fresh. A full
  flash erase ("erase all" in the web flasher, `esptool erase_flash`, SWD mass erase) makes it
  virgin.
- **No remote path sets FRESH.** CONFIG_CLEAR does not touch OS2S, and no opcode erases it. Only
  three things can make a device fresh: virgin storage, a factory reset (physical), or replaced
  firmware (§ 1.4 item 1).
- `OS2S.FRESH` is cleared when the FRESH session ends (§ 7.3), whatever the reason. A power loss
  during a FRESH session keeps it, so the next boot starts a new FRESH session.

### 7.2 Detecting two quick power-cycles

**What counts as a power-cycle.** A boot counts only if its reset cause is one of:
- power-on;
- the external reset pin;
- brown-out, but only on hardware that cannot tell brown-out from power-on.

Everything else does not count. In particular, a remote peer cannot cause a counted boot,
except by inducing a brown-out (§ 1.4 item 18).

| Target | Counted | Not counted |
|---|---|---|
| esp32-idf (`esp_reset_reason()`) | `ESP_RST_POWERON` (also the EN/CHIP_PU button on ESP32 parts), `ESP_RST_EXT` | `ESP_RST_BROWNOUT`, `ESP_RST_SW`, `ESP_RST_PANIC`, `ESP_RST_INT_WDT`, `ESP_RST_TASK_WDT`, `ESP_RST_WDT`, `ESP_RST_DEEPSLEEP`, `ESP_RST_SDIO`, `ESP_RST_USB`, `ESP_RST_JTAG`, unknown |
| nordic-zephyr nRF52840 (`hwinfo_get_reset_cause()`) | no flag set (power-on **or** brown-out: RESETREAS cannot separate them), `RESET_PIN` | `RESET_SOFTWARE`, `RESET_WATCHDOG`, `RESET_CPU_LOCKUP`, `RESET_LOW_POWER_WAKE` (System OFF wake, by pin or otherwise), `RESET_DEBUG`, any other flag |
| nordic-zephyr nRF54L15 / LM20A | `RESET_POR` if the driver reports it, `RESET_PIN` | as nRF52840; `RESET_BROWNOUT` if reported separately. **To be verified per SoC (§ 19 H8)** |
| efr32bg22-slc (`RMU_ResetCauseGet()` / EMU reset cause) | POR, PIN | AVDD/DVDD/DEC brown-out, SYSREQ (software), WDOG, LOCKUP, EM4 wake, any other |

On power-latch boards (D-FF or MOSFET latch), switching on with the latch button is a power-on
reset, so the gesture is "off and on twice" with that button. Deep-sleep, System OFF and EM4
wakes **never** count, whether a timer or a pin woke the device. That is what makes review A8
moot: no wake, emulated or real, opens claiming.

Reset-cause registers that accumulate (nRF RESETREAS) MUST be read once and then cleared at
every boot (`hwinfo_clear_reset_cause()`). Otherwise a pin reset from days ago would still be
flagged.

**Algorithm** (one 16-byte OS2S record; at most two small writes per counted boot):

```
constants  T_ARM = 2 000 ms uptime, T_CLEAR = 10 000 ms uptime

at boot, state UNCLAIMED, not entering CONTINUE or FRESH:
   if cause is counted:
       if OS2S.short_boots >= 1:            # the previous counted boot lived 2-10 s
           write short_boots = 0            # consume the gesture
           enter claim mode, kind GESTURE
       else:
           at uptime T_ARM:   write short_boots = 1
           at uptime T_CLEAR: write short_boots = 0
   else:
       if OS2S.short_boots != 0: write short_boots = 0   # any other reset breaks the sequence

before deep sleep / System OFF / EM4 / power-off / planned reboot, while short_boots = 1:
   write short_boots = 0 first                           # the on-period ends here

state CLAIMED or LOCKED: the counter is not maintained (no writes at all);
   a non-zero value left from before the claim is cleared once
```

- A POR boot of an unclaimed device stays awake at least until T_ARM. Today's boot (boot screen,
  advertising) already does.
- **User instruction** (identical in every client and on the website): *"Switch the display off
  and on. Wait about 5 seconds. Switch it off and on again. It now shows SETUP MODE."* Keep it
  off for at least 2 seconds each time, so that supply capacitors drain and the chip really
  resets.
- Battery boards with a power switch or battery pull work the same way.

**Flaky power.** A false trigger needs: power lost, power back for 2-10 s, power lost again, and
power back. Bounces shorter than 2 s never arm the counter, and they cost no writes. If a false
trigger happens anyway:
- on an **unclaimed** device: the device is in claim mode for up to 10 minutes. Its setup screen
  replaces the picture, uploads are refused (FD), and then it reboots into normal mode, which
  redraws the normal boot screen as any reboot does today. No data changes.
- on a **claimed or LOCKED** device: nothing happens.

The worst case is therefore 10 minutes of no uploads and one lost picture on an open device.

**Wear** (writes happen only on counted boots of unclaimed devices; claimed devices write nothing):

| Target | Store | Cost per counted boot | Note |
|---|---|---|---|
| esp32-idf | NVS namespace `od_sec`, key `s` | 2 writes of a 16-byte blob, about 3 NVS entries (96 B) each | NVS spreads wear over its partition; negligible |
| nordic-zephyr | a **separate** NVS/settings area for OS2K/OS2S/OS2R (plan WP2) | 2 x (8 B ATE + 16 B) | **must not** share the 2 x 4 kB config NVS: a 4 kB config leaves no room, and every sector fill there forces a GC that copies the config (§ 19 H13) |
| efr32bg22-slc | NVM3 object `OS2S` | 2 x (16 B + NVM3 header) | counted in the NVM3 page-capacity budget that gates the 3120 B config (plan WP2) |

### 7.3 State machine

```
ENTER(kind)
  FRESH / GESTURE:  new session  renders_used = 0, restarts_used = 0,
                                  config_locked = (kind == GESTURE)
  CONTINUE:         session from OS2C; restarts_used += 1
                    if restarts_used > 3: END(RESTART_CAP) without drawing
  deadline = now + 600 s
  draw the SETUP screen (§ 7.8) if HAS_PANEL, else start the LED/buzzer pattern (§ 7.7)
  start BLE; MSD status bit 2 = 1; do NOT start WiFi; action runner off; no deep sleep;
  advertising: boost interval for the first 60 s, then the normal interval

IN CLAIM MODE (device unclaimed)
  SEC_CLAIM per § 8. Each NONCE processed: renders_used += 1, config_locked = 1, draw the CODE screen
  a pending claim ends without success and renders_used == 3       -> END(ATTEMPTS)
  SEC_CLAIM EXIT                                                     -> END(CANCEL)
  deadline reached, no claim pending                                 -> END(TIMEOUT)
     (a pending claim extends the deadline to its own CONFIRM timeout, § 17)
  CMD_REBOOT, or the reboot that applies a config write               -> write OS2C; software reset (-> CONTINUE)
  CONFIRM ok + persisted                                             -> CLAIMED, still in claim mode

IN CLAIM MODE (device now claimed; the claiming connection holds a session)
  the CM refusals of § 6 still apply (sealed FD); config writes are allowed in the session
  (secrets included: the device is claimed)
  SEC_CLAIM EXIT, sealed CMD_REBOOT, the reboot that applies a config write,
  disconnect of that connection, or the deadline                     -> END(CLAIMED)

END(reason)
  clear OS2C; if OS2S.FRESH = 1: write FRESH = 0
  flush any queued reply (at most 500 ms)
  software reset WITHOUT OS2C -> normal-mode boot (the normal boot screen, if drawable)
```

The restart cap bounds what a peer can do with CMD_REBOOT in claim mode: at most 3 extra SETUP
refreshes and 30 extra minutes. Each restart restarts the timer, as the maintainer specified
("planned reboot, timer restarts").

### 7.4 What claim mode refuses, and why

The allow-list is in § 6:
- allowed: identity and status reads, CONFIG_READ, SEC_CLAIM, REBOOT, and config writes under the
  rule below;
- refused with FD: everything else, which covers everything that can put pixels on the panel:
  - uploads (0x0070-72, 0x0076, 0x0080-82, 0x0085);
  - SPI_RAW (every sub);
  - the image store and actions (0x0047-0x004D);
- also refused, to keep the device awake, its update path closed and its indicators its own:
  - ENTER_DFU and SMP;
  - POWER_OFF and DEEP_SLEEP;
  - LED and BUZZER;
  - NFC.

The device's own content sources are off too (§ 6 rule 4).

**Config writes** (0x0041 / 0x0042 / 0x0045) are allowed only when both hold:
- the session kind is FRESH;
- `config_locked = 0`, i.e. no code has been rendered yet in this session.

Rationale:
- A fresh device has no drive configuration, so it cannot draw anything until one is written.
  The maintainer's flow writes it in claim mode (§ 13.1).
- The code is drawn through the drive configuration, and a drive sequence can write panel RAM.
  The `v3/decisions` WINDOW and FILL escapes are enough to paint digits. So a configuration
  written *during* a claim is a drawing channel. Locking config writes at the first render and
  in every GESTURE session closes it. FRESH sessions remain exposed before the first render
  (§ 1.4 item 5).
- **A configuration written before claim mode cannot show the attacker's code.** The code depends
  on Nc, which the client reveals only after the commitment (§ 8.3). The digits therefore exist
  only during the claim. In claim mode nothing the attacker sends reaches the panel or the
  recipe, and the recipe has no input it could compute digits from. A planted recipe can at most
  blank or garble the screen. That is a DoS: the user sees no code and stops.
- **Secrets** are refused as in normal mode (0x10) while the device is unclaimed. After the
  claim, in the same session, they are accepted (§ 13.1 step 9).

CONFIG_READ stays allowed. An unclaimed device holds no new secrets, and the 2.x secrets of a
flagged device are readable in normal mode anyway. Claim mode adds no exposure.

### 7.5 Attempts and timers

- **3 code renders per claim-mode session**, counted across CONTINUE restarts. A BEGIN that does
  not reach NONCE renders nothing and is not counted; the rate limit bounds it (§ 8.7).
- When the third render's claim ends without success (wrong tag, ABORT, timeout, disconnect, a new
  BEGIN), the device answers that event or the next SEC_CLAIM with ATTEMPTS_EXHAUSTED and ends
  claim mode.
- The deadline is 600 s from each ENTER; a pending claim extends it to that claim's CONFIRM
  timeout.
- At most 3 restarts, so at most 40 minutes per session.
- G5 is therefore at most 3·10⁻⁶ per session (§ 1.3).

### 7.6 The gesture on a claimed or LOCKED device: ignored

Decision: **ignored**. The counter is not maintained, nothing is drawn, nothing is written, and
the LED does nothing.

- A claimed device must not open claiming. That is the point of the claim.
- An info screen ("this display is claimed") would let flaky power wipe the owner's picture on
  exactly the devices that matter most. It would also need the counter writes on every claimed
  device's counted boots.
- The ways to change ownership are SEC_KEY UNCLAIM (owner, remote; then the gesture on the now
  unclaimed device) and the factory reset (no key).
- Clients explain this when a claimed device is selected: *"This display is claimed. Switching
  it off and on does nothing. To hand it over, release it from the app that owns it, or
  factory-reset it."*

### 7.7 Signalling

| Channel | In claim mode | Notes |
|---|---|---|
| MSD status bit 2 | 1, continuously | with bit 3 = 0 while unclaimed (§ 5.3) |
| READ_IDENT tail flags | bit 5 CLAIM_MODE | plaintext |
| SEC_STATUS | flags bit0, bit5 (FRESH), bit6 (config locked); `window_s`; `renders_left` | plaintext, advisory |
| panel | SETUP screen, then CODE screen (§ 7.8) | only if HAS_PANEL |
| LED (first LED of packet 0x21, if configured) | waiting: double blink every 2 s · code shown: double blink every 1 s · claimed: 3 long flashes | the same table as FACTORY_RESET.md § 3.3 |
| buzzer (0x29, if configured) | entering claim mode: two short beeps · claimed: one long beep | |

A virgin device has no config, so it knows no LED, buzzer or panel. Advertising and READ_IDENT
are then its only signals. After the FRESH config write, the CONTINUE boot uses whatever the new
config provides.

### 7.8 The setup screen family

**Rendering.** The firmware draws these screens itself:
- through the boot-screen path in **display mode 0**: `od_display_boot()` → `od_boot_screen_render()`
  on ESP32 (incl. its IT8951/parallel sinks) and nordic-zephyr; BG22 uses its own renderer in
  `targets/efr32bg22-slc/opendisplay_display.cpp`;
- with the renderer's new screen-kind parameter: NORMAL / SETUP / CODE / RESET.

Mode 0 follows the `v3/decisions` convention. If mode 0 cannot be drawn (NEEDS_MIGRATION, a
missing 0xB3 on a native board, or no display), HAS_PANEL = 0 and the LED/buzzer patterns are
used instead.

**Fixed layout, firmware text only.** These screens take **no text from the config**:
- the 0x2C `manufacturer_name` / `model_name` header of the boot screen is **omitted** (review A1:
  it is a drawing vector);
- the battery/temperature footer and the colour swatches are omitted;
- the header is the fixed project logo or the fixed string "OpenDisplay".

The only variable inputs are:
- the hardware display id;
- the firmware version and target (numbers);
- the code digits;
- inside the QR only: the simple-config indices, which are numbers.

```
SETUP (claim mode, no claim yet)
  full layout (>= 400x300 logical)       reduced layout (small panels)
  "OpenDisplay"                           "SETUP"
  "SETUP MODE"                            "OD1A2B3C"
  "ID:  OD1A2B3C"                         QR: setup URL
  "Open the OpenDisplay app,"
  "or scan:"
  QR: setup URL (k=s)
  "Closes after 10 minutes"

CODE (a claim is pending)
  "OpenDisplay"                           "CODE"
  "CLAIM CODE"                            "123 456"
  "123 456"      (largest scale)          "OD1A2B3C"
  "ID:  OD1A2B3C"
  "Type this code in your app."
  "Did not start a claim? Ignore."
  (no QR: the code is the only large element)

RESET (drawn before the erase, FACTORY_RESET.md § 4)
  "OpenDisplay"                           "RESET"
  "FACTORY RESET"                         "OD1A2B3C"
  "Settings and key erased"               QR: setup URL
  "ID:  OD1A2B3C"
  "Scan to set up again"
  QR: setup URL (k=r)
```

When text and QR do not both fit, the QR wins on SETUP and RESET, and the code wins on CODE.
E-paper keeps each screen until the next refresh. After a factory reset there is no config, so
the RESET screen stays visible during the fresh claim mode that follows and does the job of the
SETUP screen. That is why both carry the same URL and differ only in `k`.

**Setup URL (provisional; replaces revision 1's `/r/`, which never shipped).**

```
https://opendisplay.org/s/?k=<kind>&i=<id>&t=<target>&v=<fw>[&b=<driver>][&d=<display>][&p=<power>]

k   s = claim mode (SETUP screen), r = factory reset (RESET screen)
i   6 upper-case hex digits: od_boot_app_device_id24(), the suffix of the advertised name "OD1A2B3C"
t   READ_IDENT fw_target, decimal: 1 ESP32, 2 NRF54, 3 SILABS, 4 NRF52811, 5 NRF52840 (v3/decisions)
v   firmware major.minor.patch, decimal
b   ManufacturerData.simple_config_driver_index   decimal; omitted when 0 or when there is no 0x02 packet
d   ManufacturerData.simple_config_display_index  omitted when 0
p   ManufacturerData.simple_config_power_index    omitted when 0
```

- Example: `https://opendisplay.org/s/?k=s&i=1A2B3C&t=3&v=3.0.0&b=16&d=10&p=2` (65 bytes).
- Worst case: `t=255`, `v=255.255.255` and three `65535` indices give 83 bytes. That is inside
  the byte capacity of QR version 6 ECC-M (106), which the renderer already hard-codes, so the
  renderer needs no size change.
- QR v6 + quiet zone = 49 modules. That is 4 px/module on a 200x200 1.54in (196 px), 3 px on
  152x152 (147 px), and 2 px on a 128-px-wide 2.9in (98 px, about 22 mm). Host render tests
  confirm all three (plan WP5).
- The URL carries **no secret**: the id is advertised, and the indices and version are public.
- Parameter order is fixed, so the string is deterministic (vectors V13).

**Rules for the `/s/` page** (website, plan WP9; review § 5 phishing advice):
1. Parse every parameter strictly: `k` ∈ {s, r}; `i` exactly 6 hex digits; `t`, `b`, `d`, `p`
   decimal integers in range; `v` three decimal integers. Render values as text, never as HTML.
   Ignore unknown parameters.
2. **Never ask for a secret on this page.** WiFi passwords and FindMy keys are entered only in
   the wizard, after a successful claim, inside the session (§ 13.1).
3. Show the board, display and power option that will be preselected, by name, and ask the
   user to confirm them before anything is written. A crafted link on the genuine domain could
   otherwise preselect a wrong drive recipe.
4. Say plainly that only `https://opendisplay.org/s/` is genuine, and that a display in claim
   mode or factory reset can be redrawn by anyone nearby *before* it enters that state. Claim
   mode itself cannot be redrawn.
5. No analytics or third-party requests that would leak the id. The page works from the query
   string alone.
6. For `v` < 3: explain that 2.x firmware has no setup screen and link to the normal toolbox.

### 7.9 Battery

Claim mode keeps the device awake and advertising:
- 10 minutes per ENTER, at most 40;
- at most 1 SETUP + 3 CODE refreshes, plus one SETUP per restart, plus the normal boot screen at
  the end.

Estimates, **not measured** (§ 19 H12):
- ESP32 with BLE advertising at roughly 20-40 mA: about 3-7 mAh per 10 minutes;
- nRF52840 / BG22 advertising at a 100 ms → normal interval: well under 1 mAh.

A false trigger costs one such session. Claim mode never starts WiFi.

---

## 8. Claim protocol — `CMD_SEC_CLAIM` 0x0055

### 8.1 Messages

All plaintext, BLE only, fragmented per § 3.3. Lengths are body lengths.

| Sub | Direction | Body | Len |
|---|---|---|---|
| 0x01 BEGIN | H->D | `[ver=0x02][frag_max:1][Qc:64]` | 66 |
| 0x01 BEGIN reply | D->H | `[dflags:1][Qd:64][Cd:16]` | 81 |
| 0x02 NONCE | H->D | `[Nc:16]` | 16 |
| 0x02 NONCE reply | D->H | `[Nd:16][W:28]` | 44 |
| 0x03 CONFIRM | H->D | `[tag_c:16]` | 16 |
| 0x03 CONFIRM reply | D->H | `[tag_d:16][timeout_s:2 LE]` | 18 |
| 0x04 ABORT | H->D | empty | 0 |
| 0x04 ABORT reply | D->H | empty | 0 |
| 0x05 EXIT (new) | H->D | empty | 0 |
| 0x05 EXIT reply | D->H | empty; then END (§ 7.3) | 0 |

At MTU 23 a NONCE request is exactly one 20-byte write: 4 header bytes + 16.

EXIT is accepted in claim mode from any peer. After a successful claim it is also accepted as a
plaintext frame, because SEC_CLAIM is in the plaintext set. Either way it can only end claim
mode, which the deadline would do anyway (DoS, § 1.4 item 8). Outside claim mode, EXIT gets the
same error as any other sub: NOT_IN_CLAIM_MODE, ALREADY_CLAIMED or LOCKED.

### 8.2 Device and client computations

```
BEGIN (client):     dc, Qc  <- fresh P-256 key pair        (never reused)
BEGIN (device):     check: claim mode, unclaimed, renders_used < 3, rate limit (§ 8.7),
                           ver == 0x02, frag_max in 20..244
                    discard any pending claim (its key candidate is forgotten; counts a
                           used render if its NONCE was processed)
                    validate Qc (§ 2)                        -> BAD_POINT
                    dd, Qd  <- fresh P-256 key pair
                    Nd      <- RAND(16)
                    K_new   <- RAND(16)                      the device key candidate
                    Cd      =  CMAC(Nd, "OD2/commit" || uid || Qd || Qc)
                    reply [dflags][Qd][Cd]                   dflags.KEY_LEGACY = 0
NONCE (client):     Nc      <- RAND(16); send
NONCE (device):     Z       =  ECDH(dd, Qc); destroy dd
                    Tc      =  "OD2/claim" || 0x02 || dflags || uid || Qc || Qd || Nc || Nd   (179 B)
                    PRK     =  CMAC(0^16, Z)
                    k_wrap  =  KDF(PRK, "OD2/wrap", Tc)
                    W       =  CCM(k_wrap, n = 0x00 * 13, a = "OD2/wrap" || uid, p = K_new)   (16 + 12)
                    code    =  Code(Tc)       (§ 8.4)
                    renders_used += 1; config_locked = 1
                    queue and flush the reply [Nd][W]; THEN draw the CODE screen (§ 7.8)
NONCE reply (client): check ct_equal(Cd, CMAC(Nd, "OD2/commit" || uid || Qd || Qc)) else abort
                    Z = ECDH(dc, Qd); destroy dc; same Tc, PRK, k_wrap
                    K_new = CCM-decrypt(W)  else abort
                    code  = Code(Tc); ask the user to type it (§ 8.4)
                    on a correct code: persist {uid, K_new} as a CANDIDATE and verify that it is
                    durable (§ 8.8) BEFORE sending CONFIRM
                    K_conf = KDF(K_new, "OD2/k-conf", uid)
                    tag_c  = KDF(K_conf, "OD2/claim-c", Tc); send CONFIRM
CONFIRM (device):   K_conf = KDF(K_new, "OD2/k-conf", uid)
                    ct_equal(tag_c, KDF(K_conf, "OD2/claim-c", Tc)) else AUTH_FAILED, discard pending
                    persist OS2K {origin CLAIMED, gen = OS2S.key_gen + 1, K_new} (atomic)
                            else STORAGE_ERROR, discard pending, device stays unclaimed
                    write OS2S.key_gen = gen; rewrite config 0x27 at rest (§ 10.2)
                            (a failure here is repaired at the next boot, § 10.1; the claim stands)
                    tag_d  = KDF(K_conf, "OD2/claim-d", Tc)
                    session keys: k_h2d = KDF(PRK, "OD2/h2d", Tc), k_d2h = KDF(PRK, "OD2/d2h", Tc),
                                  sid   = KDF(PRK, "OD2/sid", Tc); counters = 0; session OPEN
                    reply [tag_d][timeout_s]
CONFIRM reply (client): ct_equal(tag_d, KDF(K_conf, "OD2/claim-d", Tc)) else "could not confirm"
                    (keep the candidate, § 8.8)
                    mark K_new CONFIRMED; derive the same session keys
```

The CONFIRM reply is the device's last plaintext word on this connection apart from the
plaintext set. Everything else after it is sealed (§ 9).

The single-use `k_wrap` is why a constant CCM nonce is acceptable for W (§ 19 E10). Review E10
suggested deriving K_new from PRK and dropping W. That is not adopted: K_dev stays purely
device-random, and W costs no extra code, because CCM is needed for records anyway.

### 8.3 Why the commitment

A plain "code = f(transcript)" is broken. An attacker in the middle, playing the device towards
the client, receives Qc. It can then grind its own Qd' offline until the client-side code equals
the device-side code: 10⁶ trials of ECDH + CMAC take seconds on a PC.

Following Bluetooth LE Secure Connections numeric comparison, the device commits to Nd (inside
Cd) before it learns Nc, and the client reveals Nc only after receiving Cd:

- Playing the device towards the client, the attacker must fix Qd' and Nd' (via Cd') before it
  sees Nc. After that, nothing it controls changes the client's code.
- Playing the client towards the device, the attacker must send Nc' before it learns Nd. The
  device's code is then a fresh pseudo-random function of Nd that the attacker could not
  predict.
- The two codes therefore match with probability about 10⁻⁶ per device-side render, at most 3 per
  session (§ 7.5). This assumes the panel shows only the device's own digits, which claim mode
  ensures (§ 7.4).

A known-key CMAC is **not** a binding commitment. CBC-MAC can be inverted when the key is known,
so "CMAC(public key, Qd || r)" can be opened to anything. Here the commitment is binding because
Nd is the CMAC **key** and stays secret until the reveal. Opening Cd to a different (Nd', Qd')
means finding a key collision of AES-CMAC on a chosen message that includes the fresh Qc. This is
the f4 construction (§ 19 E3).

### 8.4 The confirmation code

```
code_raw = CMAC(Nc, "OD2/code" || uid || Qc || Qd || Nd)
code     = (uint32 big-endian of code_raw[12..15]) mod 1 000 000      six decimal digits, zero-padded
shown as "123 456" (a space after the third digit)
```

- **Length and alphabet.** 6 decimal digits. The bias of the mod (2³² mod 10⁶ = 967 296) is a
  relative bias below 2.3·10⁻⁴, negligible against 10⁻⁶.
- **Display.** The device draws the CODE screen (§ 7.8) right after it has queued and flushed the
  NONCE reply. The reply must not wait for the refresh, which takes 1-40 s. On BG22, where the
  refresh blocks the loop, the reply must already be in the stack's TX buffer.
- **How long.** E-paper keeps the code until the next refresh. The device does not redraw on
  success, failure or timeout. The code is worthless after the claim ends: it is a function of
  values that all went over the air. Claim mode ends with a reboot, and the normal boot screen
  replaces it.
- **Client, all three (decided: the website too).** The client says: *"Enter the 6-digit code
  that now appears on the display. It can take up to a minute. If a code was already showing
  before you pressed Claim, stop: someone else is claiming this display."* The user **types** the
  code. The client compares it with its own `code`; constant time is not required, since the code
  is not secret.
  - On a match: CONFIRM.
  - On a mismatch: send ABORT, do not keep the candidate, and warn: *"The code does not match.
    Someone nearby may be interfering. The display may now be claimed by someone else. If so,
    factory-reset it and try again."*
  - Three wrong entries for the same claim count as a mismatch.
- **Never** auto-confirm, never pre-fill the field, and never offer "skip" when HAS_PANEL = 1,
  except through the blank-panel path of § 8.6.

### 8.5 What the claim guarantees (and what not)

- Against P: K_new is wrapped under a key derived from an ECDH secret that P does not have (G4).
- Against A, if the user typed the right code: G5 (§ 1.3).
- **Not prevented**: first-claimer-wins (decision 7, § 1.4 item 4). An attacker in claim-mode
  range can claim the device. So can an attacker in the middle who obtains the device-side K_new
  and sends its own valid CONFIRM. The owner's client sees ALREADY_CLAIMED or a code mismatch,
  and the owner may see a CODE screen they did not start. **Detection, not prevention.**

### 8.6 No panel, and a panel that stays blank

**HAS_PANEL = 0** means one of: no display configured, mode 0 not drawable, or the boot path
reports no display. In that case:
- the device computes everything, accepts CONFIRM, and draws nothing. The LED shows the "code
  shown" pattern (§ 7.7).
- the client MUST NOT ask for a code, and MUST refuse this path if it has **any record of a
  display** for this uid or address (review A15). Such a record exists if it wrote a display
  config to the device, read one in an earlier session, or saw a SETUP screen confirmed by the
  user in this wizard. The client then says: *"This display reported a screen before. Claiming
  without the code is refused, because someone may be interfering."*
- otherwise the client shows: *"This display reports that it has no screen, so its code cannot
  be checked. Claiming is then protected only by claim mode, which you started on the device.
  Continue only if the display really has no screen and nobody you distrust is nearby."* It
  requires an explicit acknowledgement (maintainer Q12).
- HAS_PANEL is unauthenticated before the claim, and an attacker can clear it. The record check
  and the acknowledgement are the defences. Residual: § 1.4 item 6.

**Blank panel** (HAS_PANEL = 1, but nothing appears: a wrong or untested preset). This path is
offered only after the wizard's panel check has failed (§ 13.1 step 6) and the user picked "the
display stays blank" for every preset tried:
- the client shows the same warning with "its screen does not work" in place of "it has no
  screen", requires the acknowledgement, and sends CONFIRM without comparing a code;
- the device rendered (invisibly) and counted the attempt.

### 8.7 State rules, error table, rate limiter

- Each sub is accepted once per pending claim, in the order BEGIN → NONCE → CONFIRM. A repeated
  NONCE or CONFIRM is BAD_STATE and discards the claim, so W is computed exactly once per k_wrap.
- One pending claim at a time, on the one connection. **A new BEGIN discards the pending claim and
  generates a new K_new.** A pending claim is also discarded on:
  - disconnect;
  - ABORT;
  - a failed CONFIRM tag;
  - any MALFORMED or BAD_STATE in its sequence;
  - timeout.
- K_new exists only in RAM until a valid CONFIRM, so a lost NONCE reply or an abandoned claim
  cannot leave a key that nobody holds.
- After a successful claim the device is CLAIMED immediately: every later connection needs
  SEC_AUTH. The session created by CONFIRM lets the client continue without a second handshake,
  for example to write the config with its secrets (§ 13.1).

**Error table (A18.5):**

| Situation | Answer |
|---|---|
| any sub over LAN | WRONG_TRANSPORT |
| any sub, device LOCKED | LOCKED |
| any sub except EXIT, device CLAIMED (including after the claim, in claim mode) | ALREADY_CLAIMED |
| any sub, unclaimed, normal mode | NOT_IN_CLAIM_MODE |
| BEGIN, renders_used = 3 | ATTEMPTS_EXHAUSTED, then END |
| BEGIN refused by the rate limiter | RATE_LIMITED |
| BEGIN with a bad `ver`, `frag_max` or length | MALFORMED |
| BEGIN with an invalid Qc | BAD_POINT |
| NONCE with no BEGIN reply pending, or a second NONCE | BAD_STATE (discards) |
| NONCE more than 30 s after the BEGIN reply | EXPIRED (discards) |
| CONFIRM before NONCE, or after the claim was discarded | BAD_STATE |
| CONFIRM more than 300 s after the NONCE reply | EXPIRED (discards) |
| CONFIRM with a wrong tag | AUTH_FAILED (discards; counts in the rate limiter) |
| CONFIRM, OS2K persist fails | STORAGE_ERROR (discards; unclaimed) |
| ABORT with nothing pending | empty reply |
| ECC/RNG failure at any step | CRYPTO_ERROR (discards) |
| a display pass or flash erase holds the device | BUSY |

**Rate limiter (A18.6), per device, RAM only, reset at boot:**
- The device keeps a ring of the uptimes of the last 10 *counted events*.
- Counted events: every complete, well-formed BEGIN and HELLO (counted before the ECC work),
  every AUTH_FAILED, and every failed CONFIRM tag.
- A new BEGIN or HELLO is refused with RATE_LIMITED if the ring holds 10 entries and the oldest
  is less than 60 000 ms old. Refused starts are not counted.

### 8.8 Client key retention (normative for every client, review A2)

A client never deletes or replaces a stored key on unauthenticated evidence. Per uid, it keeps
one **confirmed** key (or none) and a list of **candidates**, with their origin (claim or
rotation) and time.

1. **Before CONFIRM or ROTATE_CONFIRM**, the candidate is durably stored, and the client has
   checked this:
   - HA: the `entry.data` update returned, and the entry re-read shows the key;
   - py-opendisplay: the key-store write was fsynced, or the caller's `on_key` callback returned
     without error;
   - website: the `localStorage` write was read back. If storage may not survive the tab
     (`navigator.storage.persisted()` is false, or the browser reports a private window), the
     client **first offers a key download** ("save this key file") and continues only after the
     user confirms that they saved it.

   If the store fails, the client sends ABORT (claim) or simply does not confirm (rotation).
2. A candidate becomes **confirmed** only by authenticated evidence: a verified `tag_d` in the
   CONFIRM reply, or a verified FINISH reply of SEC_AUTH with that key.
3. On authenticated success with key X for uid U, X is confirmed and every other key stored for U
   is deleted. The device holds exactly one key, so the others cannot be in force.
4. **Negative results keep every key.** These include: SEC_STATUS (any value), UNCLAIMED,
   NOT_CLAIMED, AUTH_FAILED, LOCKED, a bad `tag_d`, a bad FINISH tag, timeouts, disconnects and
   ATTEMPTS_EXHAUSTED. The client reports "could not confirm the key; keeping it" and tries again
   later. It never removes a key automatically on these.
5. When authenticating, the client tries the confirmed key first, then candidates newest first,
   at most 3 HELLOs per connection (rate limit).
6. A key is deleted otherwise only by explicit user action, after a warning that the display may
   then need a factory reset.
7. A successful **new claim** of the same uid, proven by `tag_d`, is rule 3.

---
## 9. Session establishment and records — `CMD_SEC_AUTH` 0x0056

### 9.1 Decision: device key + fresh ECDH (forward secrecy)

Session keys are derived from K_dev **and** a fresh ephemeral-ephemeral ECDH, not from K_dev and
nonces alone. The reasons:
- **Forward secrecy against X (G3).** A stolen HA backup or a later client compromise must not
  decrypt recordings of every past session, which v1 allowed (review W8).
- **No offline guessing for an unauthenticated peer (G6).** The client proves first (§ 9.3), so
  the device reveals nothing that depends on K_dev until the client has proven it knows K_dev.
- **The code is already there.** The claim needs ECDH on every target, and on BG22 and nRF the
  PSA ECDH, keygen and export functions are already placed for the BLE security manager.

Cost per connection, compared with a key-only design:
- one P-256 key generation and one ECDH on each side;
- +160 bytes over the air (two points);
- one extra round trip: 2 RTT in total instead of 1.

Latency is estimated, **not measured**, at tens of milliseconds to about 200 ms per ECC
operation on BG22 CRYPTOACC, nRF CC310/CRACEN and ESP32 mbedTLS (§ 19 H2). Through an ESPHome
proxy each round trip takes about 50-150 ms, so a handshake takes about 0.3-1 s. A key-only
fallback is not specified.

### 9.2 Messages

| Sub | Direction | Body | Len |
|---|---|---|---|
| 0x01 HELLO | H->D | `[ver=0x02][frag_max:1][Qc:64][Nc:16]` | 82 |
| 0x01 HELLO reply | D->H | `[dflags:1][Qd:64][Nd:16]` | 81 |
| 0x02 FINISH | H->D | `[tag_c:16]` | 16 |
| 0x02 FINISH reply | D->H | `[tag_d:16][timeout_s:2 LE]` | 18 |

### 9.3 Computations

```
HELLO (client):   dc, Qc <- fresh; Nc <- RAND(16)
HELLO (device):   claimed? else NOT_CLAIMED (LOCKED -> LOCKED). rate limit (§ 8.7).
                  tear down any live session (§ 9.6)
                  validate Qc -> BAD_POINT
                  dd, Qd <- fresh; Nd <- RAND(16)
                  Z  = ECDH(dd, Qc); destroy dd
                  Ta = "OD2/auth" || 0x02 || dflags || uid || Qc || Qd || Nc || Nd     (178 B)
                  PRK = CMAC(K_ext, Z)
                  precompute exp_c = KDF(PRK, "OD2/auth-c", Ta), tag_d = KDF(PRK, "OD2/auth-d", Ta),
                  k_h2d, k_d2h, sid; keep only these (not Qc/Qd/Z/PRK)
                  reply [dflags][Qd][Nd]
HELLO reply (client): Z = ECDH(dc, Qd); destroy dc; uid = the STORED uid for this key;
                  dflags = the byte just received
                  same Ta, PRK (K_ext from the stored K_dev), tag_c = KDF(PRK, "OD2/auth-c", Ta)
                  send FINISH
FINISH (device):  ct_equal(tag_c, exp_c) else AUTH_FAILED (counts in the rate limiter; discard)
                  session OPEN: install k_h2d, k_d2h in HAL slots; tx_ctr = rx_last = 0
                  reply [tag_d][timeout_s]
FINISH reply (client): ct_equal(tag_d, KDF(PRK, "OD2/auth-d", Ta)) else "device failed mutual
                  authentication" (wrong device or attacker): disconnect, keep every key (§ 8.8)
```

The device proves second. An attacker who connects and sends HELLO learns only Qd and Nd, which
do not depend on K_dev.

An attacker who **impersonates a device** to a real client learns tag_c. That is a function of
K_ext and of an ECDH secret the attacker shares, so it allows an offline guess of a weak key at a
few AES operations per guess. Claimed keys are 128-bit random and cannot be guessed. Migrated
legacy keys may be weak (§ 10.5, residual R4), which is why clients rotate them by default.

Error table: as § 8.7, with:
- FINISH without a pending HELLO → BAD_STATE;
- FINISH more than 30 s after the HELLO reply → EXPIRED.

### 9.4 Sealed record format

Keys: `k_h2d` and `k_d2h`, different for every session and direction. Counters: 32-bit, one per
direction. The first sealed frame of a session in each direction uses counter **1**.

```
nonce13(dir, ctr) = dir:1 || ctr:4 LE || 0x00 * 8           dir = 0x48 'H' (host->device), 0x44 'D'

H->D sealed frame:
   [cmd_hi][cmd_lo][ctr:4 LE][C][T:12]
   C||T = CCM(k_h2d, nonce13('H', ctr), a = 0x02 || cmd_hi || cmd_lo, p = command payload)

D->H sealed frame:
   [0xE5][ctr:4 LE][C][T:12]
   C||T = CCM(k_d2h, nonce13('D', ctr), a = 0x02 || 0xE5, p = the complete plaintext response
          frame [status][cmd_echo][data...] as it would have been sent without a session)
```

- **What each frame authenticates:**
  - the opcode (AAD, H->D), or the whole response including its status and echo (plaintext,
    D->H);
  - the exact payload length (CCM encodes it in its first block B0, so v1's inner `[len:1]` byte
    is dropped);
  - the direction (a separate key, plus the `dir` nonce byte);
  - the counter (nonce);
  - the session (keys are unique per handshake);
  - the protocol version (AAD byte 0x02).
- `0xE5` is a status value no response uses: 0x00 ACK, 0xFF NACK, 0xFE auth required, and BG22's
  defective 0x01. On a connection with a live session, every device->host frame is either sealed
  (first byte 0xE5) or one of the plaintext frames of § 9.5.
- **Empty payloads are legal** (CCM with zero-length plaintext). For example, a sealed
  CONFIG_READ request is `[00][40][ctr:4][T:12]`, 18 bytes. The current HAL contract ("at least
  one ciphertext byte") must be widened (plan WP2).
- **Tag length 12 bytes (96 bits).** The forgery probability is 2⁻⁹⁶ per attempt. A session is
  torn down after 3 bad tags, so an online forger's total success probability is about 2⁻⁹⁴·⁴.
  96 bits keeps the existing HAL slot policy (`OD_HAL_CRYPTO_TAG_LEN 12`).
- **Overhead**: H->D 2 + 4 + 12 = 18 bytes; D->H 1 + 4 + 12 = 17 bytes (v1: 29). The largest
  sealed payload in one 253-byte frame is 235 bytes H->D, and a 236-byte plaintext response D->H.
  The header's encrypted chunk budget of 154 bytes may be raised to 230 under SEC2; until the
  header says so, clients keep 154.
- **MTU**: sealed frames are not fragmented. A sealed frame needs `len + 3 <= ATT MTU`, exactly
  like the plaintext protocol today. At MTU 23 almost nothing sealed fits (residual R9; whether
  every proxy path negotiates ≥ 185 is § 19 H6).
- **Response binding (review A17) is not adopted.** Reasons, in Appendix A:
  - PIPE keeps many frames outstanding, and SACKs answer sets of them, so "the request this
    answers" is ill-defined;
  - a trial decryption against two candidate bindings would complicate strike counting;
  - G7 is already met: per-handshake keys separate connections, and inside a session one ATT
    bearer, a strictly increasing device counter and the authenticated cmd echo order the
    responses.

### 9.5 Receive rules, counters, retries

**Counter range (A14).** Usable values are 1 .. 0xFFFFFFFE. A receiver rejects 0 and 0xFFFFFFFF.
A sender that has used 0xFFFFFFFE seals nothing more: the device tears the session down, and the
client runs a new HELLO.

**Device: counter consumption.** A device counter value is consumed when a frame is sealed. A
sealed frame that is then dropped (TXQ full, a deferred outcome that never runs) is lost. The
device never seals different content under a counter it has used. Clients tolerate gaps.

**Device (host->device):**

1. If the opcode is in the plaintext set (§ 6 rule 1), process the frame as plaintext. A frame
   for such an opcode that is long enough to be sealed is still parsed as plaintext.
2. Otherwise, with no live session: FE (or FD in claim mode before the claim, § 6).
3. With a live session, a gated frame shorter than 18 bytes cannot be a sealed record. It gets
   the sealed NACK `[FF][cmd_lo][0xF1]` (`OD_SEC_ERR_RECORD`, provisional), not FE, because the
   session exists, and it counts nothing.
4. Counter check **before** decrypt, with `od_nonce_window` semantics unchanged: accept a counter
   greater than rx_last, or one within the 256-entry backward window that has not been seen yet.
   Reject 0 and 0xFFFFFFFF. A rejected counter gets the sealed NACK `[FF][cmd_lo][0xF2]` (REPLAY)
   and does not count as a strike, per the lossy-link rule of `od_session.h`. The backward window
   exists because write-without-response frames can arrive out of order across retries.
5. CCM decrypt-and-verify. On failure: the sealed NACK `[FF][cmd_lo][0xF3]` (INTEGRITY), and
   strike + 1. At 3 strikes the session is torn down (§ 9.6). The plaintext of a failed frame is
   never read.
6. Only after the tag has verified: commit the counter to the window, then dispatch (and apply
   the claim-mode refusals, § 6).

**No-ack frames (A6).** Write-without-response exists on every target:
- `targets/esp32-idf/src/od_ble_nimble.cpp:363,380`;
- `targets/nordic-zephyr/src/opendisplay_ble.c:572`;
- `targets/efr32bg22-slc/opendisplay_ble.c:1696`.

Some frames get no response even in plaintext:
- SPI_RAW DATA_NOACK;
- PIPE DATA 0x0081 between SACKs;
- the fire-and-forget DEEP_SLEEP;
- direct-write data frames on targets that do not ack each one.

For such frames the device sends **no** sealed NACK of any kind:
- REPLAY is silent (as today's PIPE rule in `od_gate.c`);
- INTEGRITY is silent but still counts a strike;
- RECORD is silent.

The third strike tears the session down, and the next frame gets FE. The transfer's own
completion check (SACK, END) tells the client what arrived.

**Client (device->host)**, while a session is live:

1. First byte 0xE5 and length ≥ 17: the counter must be **strictly greater** than the last
   accepted device counter. The device emits in order on one ATT bearer, so a gap is allowed but
   a repeat or decrease is not. Decrypt and verify. On failure, discard the frame and count it;
   after 3 failures, disconnect. Then parse the decrypted plaintext exactly like an unencrypted
   response.
2. Exactly `[0x00][cmd_lo][0xFE]` (3 bytes): **session gone** (expired, torn down, device
   rebooted). Drop the session state (§ 9.7).
3. A plaintext response to an opcode of the plaintext set (§ 6 rule 1) that the client sent:
   accept it as plaintext. It is unauthenticated by design and must not be trusted for security.
4. A 4-byte `[FF][cmd_lo][sub][err]` to a SEC opcode the client sent: accept it, as advisory
   (§ 8.8).
5. **Anything else is discarded.** This replaces py-opendisplay's "any notification < 31 bytes is
   plaintext" rule (review W7).

The sealed NACK codes 0xF1..0xF3 travel inside a sealed D->H frame, so a client can trust them.
0xF4 (revision 1's ROTATION_REQUIRED) is **reserved and never sent** (§ 10.5).

**Client retries (A14).**
- A retried command is either the identical sealed bytes or a re-seal under a new counter. A new
  counter is recommended. A client never re-seals different content under a counter it has used.
- **REPLAY (0xF2)** means: a frame with that counter was already accepted, the command may have
  executed, and its response may have been lost.
  - For idempotent commands (reads, a whole config write, an image upload from its START), the
    client re-sends under a new counter.
  - Otherwise it re-reads the state before deciding.

### 9.6 Session lifetime and teardown

- **Lifetime** is absolute, counted from FINISH or from the claim CONFIRM.
  - `timeout_s` = `SecurityConfig.session_timeout_seconds`. On 3.0, **0 means the default
    3600 s** (on 2.x it meant "never"). Values 1..59 are raised to 60.
  - It is snapshotted at session open, and reported in the FINISH/CONFIRM reply and in
    SEC_STATUS.
- **Teardown** (device). Teardown releases both HAL slots and zeroes the state. It happens on:
  - a BLE disconnect of the session's connection;
  - reaching the lifetime (checked on every inbound frame, as `od_session_alive` does now);
  - 3 bad tags;
  - a counter that would pass 0xFFFFFFFE in either direction;
  - a HELLO (a new handshake replaces the session immediately; the client asked for it);
  - a successful ROTATE_CONFIRM or UNCLAIM (after the sealed reply is queued, § 11);
  - the end of claim mode (it reboots);
  - a factory reset.

  A config save does **not** tear the session down: the key is not in the config record.
- **One connection, one session** (§ 5.4). A session never survives the connection it was made
  on.
- On disconnect, the device MUST also:
  - discard any pending claim, handshake and reassembly;
  - **drop notifications still queued for that connection** (od_txq), so nothing of an old
    connection can be delivered on a new one.
- **Deep sleep, power-off, reboot**: all RAM state is gone. The client's next connection starts
  with a HELLO. There is no session resumption.

### 9.7 Client rules for reconnects (py-opendisplay#141, opendisplay.org#95)

1. Session state lives in the connection object. On **any** disconnect, the client deletes it
   (keys, counters, sid). That includes the client's own disconnect, a proxy drop, and the device
   going to sleep.
2. On every new connection to a claimed device, the client sends SEC_AUTH HELLO before its first
   gated command. It does not "try the old session first".
3. Before HELLO, the client drains pending notifications. While a handshake is running, it
   accepts only fragments with the expected opcode and sub and discards anything else. A
   handshake that does not complete within the § 17 limits is restarted once from HELLO, then
   reported.
4. On SESSION_REQUIRED during a session: delete the session, run one HELLO, then retry the failed
   command **once** if it is idempotent. A mid-stream PIPE or direct-write data frame is not
   retried; the transfer restarts.
5. Proactive renewal: at 90 % of `timeout_s`, between transfers, run HELLO (which replaces the
   session).
6. A client never sends v1 (0x0050 step 1/2) to a device it has ever seen report firmware major
   ≥ 3 or READ_IDENT SEC_V2. It never uses a SEC2 K_dev as a v1 key, except K_v1 after a
   deliberate downgrade (plan § 5.2).

Every handshake binds both fresh ephemeral keys, both nonces, the uid and dflags. So a reply
from an earlier handshake or connection cannot complete a new one: it fails the tag check with
AUTH_FAILED, instead of producing a confusing "mutual authentication" error on a session that
half-exists.

---

## 10. Key storage, migration and config interaction

### 10.1 The boot decision (normative; fail-closed by construction)

K_dev lives in the OS2K record, never in the config record (§ 4.1). The backends cannot tell a
missing record from a corrupt one:
- ESP-IDF NVS drops a CRC-failed entry and reports NOT_FOUND;
- Zephyr NVS may return an *older* entry when the newest one is corrupt (§ 19 H3);
- NVM3 behaviour on a bad object is unverified.

So the spec never distinguishes them: every record is either VALID or NOT VALID. Instead, the
claimed state has **two independent pieces of evidence**:
- the **SEC2 marker** in config 0x27 (`reserved[0] = 0x02`, § 10.2), stored in the config record;
- the **key generation** `OS2S.key_gen`, which detects an older OS2K coming back.

At every boot, before claim-mode entry and before the radio starts, the device evaluates these
rows **top to bottom**, and the first match wins:

| # | OS2K | Config 0x27 | Other secrets in config (§ 10.4) | OS2S | Result |
|---|---|---|---|---|---|
| 1 | VALID, `gen < OS2S.key_gen` | any | any | VALID | **LOCKED**: an older key record came back |
| 2 | VALID | any | any | any | **CLAIMED.** Repairs, each idempotent: set `OS2S.key_gen = gen` if OS2S is not valid or differs (`gen = key_gen + 1` is an interrupted claim or rotation); rewrite config 0x27 at rest (§ 10.2) if it is not the expected form, or write a config holding only 0x27 if the config record is gone |
| 3 | NOT VALID | SEC2 marker | any | any | **LOCKED**: evidence of a claim, key unreadable (maintainer Q9) |
| 4 | NOT VALID | no marker, `encryption_enabled != 0` **and** key non-zero (an unmigrated 2.x key) | any | any | **MIGRATE (B2, § 10.5)** → CLAIMED |
| 5 | NOT VALID | no marker, no key in force | present | NOT VALID (first 3.0 boot) | **UNCLAIMED + LEGACY_SECRETS**; write `OS2S {MIGRATED, LEGACY_SECRETS}` |
| 6 | NOT VALID | no marker, no key in force | present | VALID with LEGACY_SECRETS | **UNCLAIMED + LEGACY_SECRETS** |
| 7 | NOT VALID | no marker, no key in force | present | VALID without LEGACY_SECRETS | **LOCKED**: 3.0 never stores secrets on an unclaimed device, so these come from a claim whose key is gone |
| 8 | NOT VALID | no marker, no key in force, or no config | none | any | **UNCLAIMED** (fresh per § 7.1) |

Notes:

- **LOCKED** behaves as follows:
  - every gated command gets FE;
  - SEC_AUTH and SEC_CLAIM answer LOCKED;
  - SEC_STATUS reports state 3, MSD bit 3 = 1 and READ_IDENT CLAIMED;
  - the gesture is ignored, and WiFi is not started.

  Recovery is a factory reset (reset pin) or a full USB/SWD erase. Nothing is exposed: secrets are
  readable only in a session, and no session can be opened.
- A 2.x 0x27 with a non-zero key but `encryption_enabled = 0` had no key in force on 2.x. At the
  first 3.0 boot that stale key is zeroed in place (one config rewrite), and the device is then
  handled by rows 5-8.
- Residual (§ 19 H3): two coincident corruptions, such as an older OS2K plus an older OS2S, or
  an old 2.x config plus a missing OS2K, can bring back an older key. That key's holder then
  regains access. Each needs a backend that returns old entries; this is tested by injection.
- The reset marker OS2R is evaluated before this table (FACTORY_RESET.md § 5.3). A factory reset
  in progress always wins.

### 10.2 Config packet 0x27 on 3.0: what is stored and what is read

**At rest (the config record):**

| Device | `encryption_enabled` | `encryption_key` | `reserved[0]` | other fields |
|---|---|---|---|---|
| CLAIMED | 1 | **K_v1**: = K_dev while the origin is LEGACY and the key was never rotated; otherwise `KDF(K_dev, "OD2/v1-compat", uid)` | **0x02** (`OD_SEC27_AT_REST_SEC2`, provisional) | `session_timeout_seconds`, `flags`, `reset_pin` as stored (§ 10.6) |
| UNCLAIMED | 0 | 16 x 0x00 | 0x00 | as written |

Why, review A5: if 2.x firmware boots on this config (an owner rollback, or an attacker on the
DFU path), it sees encryption on with K_v1 and fails closed. It does not hand out the WiFi password
and FindMy keys to anyone. A client holding K_dev computes K_v1 and can still talk v1 to a
deliberately downgraded device, after the user clears its pin (plan § 5.2). A migrated, never
rotated device downgrades to exactly its 2.x state. Whether 2.x parses a 3.0 config at all is
§ 19 H4.

**3.0 code never takes a security decision from these bytes**, except the boot decision of
§ 10.1. The gate, MSD bit 3, the SMP predicate and READ_IDENT all come from the security state.

**Read view (CONFIG_READ, "truthful"):**

| Device | `encryption_enabled` | `encryption_key` | `reserved[0]` |
|---|---|---|---|
| CLAIMED (sealed read; a plaintext read gets FE) | 1 | **K_dev** | 0x00 (the read image is a 2.x-format backup) |
| UNCLAIMED (open read) | 0 | 16 x 0x00 | 0x00 |

On a claimed device the stored config always contains 0x27 (§ 10.1 row 2, § 10.3), so the read
only substitutes values in place. No CRC is computed over the read image.

### 10.3 Config writes: the key field

The key is set **only** through the claim (§ 8) and rotation (§ 11). The 0x27 key field of a
written config must **match**:

| Device | 0x27 in the written config | Result |
|---|---|---|
| CLAIMED | absent | accepted; the stored 0x27 at rest is inserted **immediately before the first packet whose type byte is greater than 0x27, or at the end if there is none** (A18.8) |
| CLAIMED | key all-zero, or key == K_dev | accepted. The key is unchanged. `encryption_enabled` and `reserved[0]` in the blob are **ignored**. Stored at rest per § 10.2; reset pin per § 10.6 |
| CLAIMED | any other non-zero key | **NACK `[FF][41\|42][0x11]` `OD_ERR_CONFIG_KEY_MISMATCH`**, nothing stored |
| UNCLAIMED | `encryption_enabled == 0` and key all-zero | accepted |
| UNCLAIMED | `encryption_enabled != 0` or a non-zero key | **NACK `[FF][41\|42][0x10]` `OD_ERR_CONFIG_SECRET_NEEDS_CLAIM`**, nothing stored (§ 10.4) |

0x10 and 0x11 are CONFIG_WRITE error bytes in namespace 4g, next to `v3/decisions`'
`OD_ERR_CONFIG_WRITE_FAILED 0x00` and `OD_ERR_CONFIG_WRITE_BUSY 0x01`. The NACK frame is
`[0xFF][0x41|0x42][err][0x00]`, the 4-byte 4g shape.

The rule is "must match" rather than "ignored". A silently ignored key leaves a device the user
believes carries the key they wrote, whereas a mismatch is loud. Accepting the all-zero key lets
clients that blank the field write without knowing the key. Accepting an equal key makes
read → edit → write round-trip exactly.

To restore a backup onto a *different* (or re-claimed) device, the client must zero the 0x27 key
and `encryption_enabled` before writing; otherwise it gets 0x10 or 0x11.

`flags` bit 0 (REWRITE_ALLOWED) and bit 1 (SHOW_KEY_ON_SCREEN) are **reserved**. The device stores
them as sent, returns them as sent, and gives them no effect. Clients write 0. The Nordic
"erase and replace without a session" path (`targets/nordic-zephyr/src/od_cmd_config.c`) is
deleted.

Factory-embedded configs (ESP32 `tryProvisionFactoryEmbed`, Nordic `factory_config.c`) must carry
neither a key nor any § 10.4 secret. A factory config that does is rejected like a write: it is
logged and not loaded.

### 10.4 Secrets need a key

An **unclaimed** device refuses to **store** secrets (decision 9).

**The closed list.** There are no FindMy/FMDN provisioning opcodes (header on `v3/decisions`;
BG22 syncs `od_net_a` / `od_net_g` only from config 0x2D), so every secret enters through a
config write or a factory config. **This list is closed.** Any future packet that carries a
secret must be added here, and to the protocol header's packet table, before it ships (A13).

| Packet | Field(s) | "Carries a secret" when |
|---|---|---|
| 0x26 WifiConfig | `password[32]` | any of the 32 bytes is non-zero |
| 0x27 SecurityConfig | `encryption_enabled`, `encryption_key[16]` | `encryption_enabled != 0` or any key byte is non-zero (§ 10.3) |
| 0x2D FindMyConfig | `eik[32]`, `net_a_adv_key[28]`, `advertisement_key[20]` | any byte of any of the three is non-zero |

The check covers **every** instance of the packet in the blob; singletons are not trusted to be
single.

Not secrets:
- the SSID, `encryption_type`, `server_host` / `server_port` (an open network with an SSID and no
  password is storable);
- FindMy flags, `fmdn_k`, the curve and the time bases, when the key material is zero;
- everything else.

`net_a_adv_key` and `advertisement_key` are broadcast, so they are not confidential. They are
gated anyway: an open device must not be turnable by anyone in range into a beacon registered to
*their* tracker account.

**One walk (A13).** The scan is a callback inside the **same TLV walk the loader uses to store
and apply the config**, over the same assembled blob. It is not a second parser. If the walk does
not end exactly at the end of the blob (a truncated packet, or a length that runs past the end),
the write is refused with `OD_ERR_CONFIG_WRITE_FAILED` (0x00) on every device. A packet type the
loader skips by its length is skipped identically by the scan. The host wire corpus must contain
only clean blobs, which is checked when WP4 lands.

**What the device answers.** The whole write is refused with `[FF][41][10][00]` (single frame),
or `[FF][42][10][00]` on the last chunk of a chunked write, where the complete blob is first
known. Nothing is stored, and the previous config stays.
- Clients turn 0x10 into: *"Claim this display first, then enter the WiFi / FindMy settings."*
- Old clients see a NACK and report a failed write: loud, not silent.

Rejecting the whole write is deliberate, rather than dropping the secret fields. A dropped field
would be a device that ACKed a WiFi password and then never joins the network, with nothing
telling the user why.

**Exception for flagged 2.x devices.** On an unclaimed device with LEGACY_SECRETS (§ 10.5), a write
whose secret fields are **byte-identical** to the stored ones is accepted, because it adds no
secret. This is what lets a client read the config, add drive packets and write it back without
the gesture. Different or additional secret bytes still get 0x10. A write without the secrets
removes them, and the LEGACY_SECRETS flag is cleared at the next boot (§ 10.1 row 8).

**On a claimed device** secrets are stored normally (the write is sealed).

**Scrub procedure** (used by SEC_KEY UNCLAIM and factory-reset step R1b):
1. One **atomic** config rewrite that:
   - zeroes every 0x26 `password`;
   - zeroes every 0x2D `eik`, `net_a_adv_key` and `advertisement_key`;
   - sets 0x27 to the UNCLAIMED form (`encryption_enabled` 0, key 0, `reserved[0]` 0), keeping
     `session_timeout_seconds`, `flags` and `reset_pin`;
   - keeps everything else.
2. esp32-idf: erase the ESP-IDF WiFi namespace `nvs.net80211` (A16).
3. efr32bg22-slc: delete any FMDN runtime NVM3 objects kept outside the config, if any exist.

**ESP32 WiFi copy (A16).** 3.0 calls `esp_wifi_set_storage(WIFI_STORAGE_RAM)` before any
`esp_wifi_set_config` (`src/wifi_service.cpp`), so ESP-IDF keeps no second copy of the password.
The first 3.0 boot erases `nvs.net80211` once, because 2.x left a copy there with
`CONFIG_ESP_WIFI_NVS_ENABLED=y`. Every scrub and factory reset erases it again.

### 10.5 Devices upgraded from 2.x (decided)

A 2.x config is stored verbatim and may hold a v1 key (0x27), a WiFi password and FindMy key
material. Nothing is written away silently at upgrade, except a stale, never-enabled key (§ 10.1
note).

**(a) No v1 key, with stored WiFi/FindMy secrets: keep working, flagged** (option A1). The
secrets keep working: WiFi joins, FindMy advertises.
- SEC_STATUS flags bit 3, READ_IDENT bit 6 and the boot row 5/6 flag mark the device.
- Every new client shows: *"This display stores a WiFi password or FindMy keys but is not
  claimed. Anyone nearby can read them. Claim it now: switch the display off and on, wait about
  5 seconds, switch it off and on again."*
- After a successful claim, the client adds: *"Change your WiFi password: it was readable while
  the display was unclaimed."* (residual R8)
- A new write that carries *new* secret bytes gets 0x10. An identical write is accepted (§ 10.4).
  So drive packets can be added without the gesture.

**(b) A v1 key: migrated as the device key (B2).** The first 3.0 boot (§ 10.1 row 4):
1. writes OS2K `{origin LEGACY, gen 1, K_dev = the 0x27 key}`;
2. writes `OS2S {key_gen 1, MIGRATED}`;
3. rewrites 0x27 at rest with the SEC2 marker and K_v1 = K_dev.

Each step is idempotent, and a power cut repeats at most one of them (rows 2 and 4). The device
is then CLAIMED:
- `dflags.KEY_LEGACY = 1`, and SEC_STATUS flags bit 4 is ROTATION_SUGGESTED;
- sessions are fully functional; nothing is restricted;
- revision 1's forced rotation, sealed NACK 0xF4 and allow-list are **deleted** (0xF4 is
  reserved).

**Client default rotation** (maintainer decision; addresses residual R4). On the first SEC2
session with a device whose `KEY_LEGACY = 1`, the client rotates (§ 11) by default, following the
§ 8.8 persistence rules:

| Client | Default | Opt-out |
|---|---|---|
| Home Assistant | rotates automatically after the first successful SEC_AUTH. The new key is written to `entry.data` (checked) **before** ROTATE_CONFIRM | options flow: "Keep the migrated key" |
| website | rotates on connect and stores the new key under the uid; offers the key file download first if storage is not persistent | toolbox setting "Do not rotate migrated keys" |
| py-opendisplay | rotates on the first SEC2 session, but **only into a durable store**: the configured `key_store` (default: a 0600 JSON file in the user config directory) or the caller's `on_key_rotated(uid, new_key)` callback. The new key's location is logged | `rotate_legacy_key=False`; CLI `--no-rotate` |

py-opendisplay never rotates into memory only: a library caller with a hard-coded key and no
store would be locked out (A2). It uses the default file store and says where it is.

Rotation needs no gesture and no claim. It happens inside a session. Old clients cannot use a
claimed 3.0 device in any case (decision 1).

### 10.6 CONFIG_CLEAR 0x0045 and the reset pin (review A9)

The reset-pin fields are 0x27 `flags` bits 2-5 and `reset_pin` (FACTORY_RESET.md § 2).

- **Claimed device: sticky-enabled.**
  - CONFIG_CLEAR is one atomic replace of the config record with a config that holds **only** the
    stored 0x27 at rest: marker, K_v1, timeout, flags, reset pin. So the claim evidence and the
    reset pin survive.
  - A config write that omits 0x27, or that carries `RESET_PIN_ENABLED = 0` while the stored 0x27
    has it set, is accepted with the **stored** reset-pin fields kept.
  - A write may enable the pin, move it to another pin, or change polarity and pulls, as long as
    it stays enabled. To disable it, the owner unclaims first.
  - The read shows the effective values.
- **Unclaimed device**: CONFIG_CLEAR erases the config record, as today. OS2S is not touched, so
  the device does not become fresh.
- SEC_KEY UNCLAIM keeps the config (minus secrets), and therefore the reset pin. A factory reset
  erases everything.
- This stops CONFIG_CLEAR or a careless write from removing the owner's recovery path. It does
  **not** stop someone holding the key from installing firmware that ignores the pin (§ 1.4
  item 2). Recovery from a takeover is guaranteed only by a USB/SWD reflash.

CONFIG_CLEAR never touches OS2K, OS2S or the image store.

### 10.7 Where the key can be read

- Over the air, the key appears only:
  1. wrapped in the claim reply (§ 8);
  2. sealed in the ROTATE reply (§ 11);
  3. sealed in a CONFIG_READ on a claimed device (§ 10.2), or over TLS LAN (§ 12).

  That is, only to a peer that already holds it or is becoming its holder.
- The panel **never** renders the key, and no QR carries it. 3.0 deletes the `SHOW_KEY_ON_SCREEN`
  rendering and the key bytes of the boot payload: `od_boot_payload.c` (`od_boot_payload_build` /
  `od_boot_url_build`) and BG22 `opendisplay_display.cpp`. The payload's key field is sent as
  zeros.
- Clients MAY show a key to their own user so it can be moved to a second client (HA options flow
  "Show key", `opendisplay key export`). That is client UX and the user's responsibility.

### 10.8 Keys in website URLs (`?key=`)

`?key=` is kept, by maintainer decision: restoring a device from a link that carries its key is
wanted. The exposure is real. A key in a query string lands in:
- browser history and sync;
- the web server's and any proxy's access logs;
- the `Referer` of requests the page makes;
- screenshots, and every place the link is pasted.

Anyone with the link holds the device. Recommended mitigations that keep the feature (review R6:
"adopt at least the first, third and fourth"):
1. After reading the parameter, remove it from the address bar (`history.replaceState`).
2. Never put a key into a link the site generates unless the user explicitly asks for a "link
   with key", and label such a link as containing the key.
3. Also accept the key in the fragment (`#key=`), which browsers do not send to the server, and
   prefer it in generated links. Keep accepting `?key=` for existing links.
4. Send `Referrer-Policy: no-referrer` (or `same-origin`) on the toolbox pages.
5. Offer "forget this key" for the browser's stored keys.

---

## 11. Key rotation and unclaim — `CMD_SEC_KEY` 0x0057 (sealed only, BLE only)

Sealed requests are `[00][57][ctr][CCM(sub || body)][T]`. The decrypted plaintext response frames
are `[00][57][sub][...]` and `[FF][57][sub][err]`, inside a sealed D->H frame. **The device does
the work and persists first, and only then replies** (review A10).

| Sub | Request body | Device, in this order | Sealed response |
|---|---|---|---|
| 0x01 ROTATE | empty | K_new = RAND(16), held pending in RAM (it replaces any earlier pending K_new) | `[K_new:16]` |
| 0x02 ROTATE_CONFIRM | `[tag:16]`, tag = KDF(K_conf_new, "OD2/rotate-c", uid \|\| sid), with K_conf_new = KDF(K_new, "OD2/k-conf", uid) | (1) verify the tag, else AUTH_FAILED (the pending key is dropped); (2) persist OS2K `{origin CLAIMED, gen + 1, K_new}` atomically, else STORAGE_ERROR (old key in force); (3) `OS2S.key_gen = gen + 1`; (4) rewrite 0x27 at rest with the new K_v1. Failures of (3) and (4) are repaired at boot (§ 10.1 row 2); the new key is in force once (2) succeeded | empty success, sealed under the **current** session; then teardown |
| 0x03 UNCLAIM | `[0xA5][0x5A]` (a guard against a stray frame) | (1) scrub (§ 10.4), one atomic config rewrite, else STORAGE_ERROR (still claimed); (2) erase OS2K, else STORAGE_ERROR (still claimed, no secrets; row 2 repairs the marker at boot); (3) `OS2S.key_gen = gen + 1`, a tombstone, so that an erased OS2K which a backend brings back is caught by § 10.1 row 1 (a failure here is harmless) | empty success; then teardown. The device is UNCLAIMED in **normal mode**, open. Claiming again needs the gesture |

Rules:

- Rotation follows the claim's pending/confirm pattern:
  - a lost ROTATE reply, or an abandoned rotation, leaves the old key in force;
  - a new ROTATE replaces the pending K_new;
  - the pending key dies with the session.
- The client stores K_new as a **candidate** (durably, § 8.8) before ROTATE_CONFIRM. It then
  re-runs SEC_AUTH with K_new. If that fails or the reply was lost, it keeps both keys and tries
  them in order next time (§ 8.8 rules 4-5). It deletes the old key only after an authenticated
  success with K_new.
- `sid` binds the confirmation to this session. A confirmation captured from one session is
  useless in another.
- After rotation, a client that held the old key (M) can no longer authenticate.
- **Re-claim by a new owner:**
  - with the current owner's cooperation: UNCLAIM, then the gesture, then a claim;
  - without it: factory reset, which ends in fresh claim mode.

  There is no remote re-claim of a claimed device.

---

## 12. LAN transport (review A12)

| Device state | Plaintext TCP (port 2446) | TLS-PSK |
|---|---|---|
| UNCLAIMED, normal mode | open, as today (the § 10.4 rules apply) | not offered: there is no K_lan |
| claim mode | **WiFi is not started** | — |
| CLAIMED | gated exactly like a BLE connection without a session: the plaintext set (minus SEC_*) is answered, and every gated command gets FE `[00][cmd][FE]` | PSK = K_lan (§ 2.2) instead of v1's `CMAC(K, "opendisplay-tls-psk")`. A TLS connection is "C/s" for gating (as today, `od_dispatch.c` origin check). CONFIG_READ over TLS is truthful (it returns K_dev) |
| LOCKED | WiFi is not started | — |

- SEC_CLAIM, SEC_AUTH and SEC_KEY from any LAN origin get WRONG_TRANSPORT. Sessions, claims and
  rotation are BLE-only, which also keeps claiming physical (claim mode).
- The TLS ciphersuite should be an (EC)DHE-PSK suite for forward secrecy. That is not verified on
  the ESP32 build (§ 19 E14, residual R12).
- The LAN transport is unverified on hardware.

---
## 13. Flows (the reference for every client)

### 13.1 Fresh install, end to end

| # | Device | Client | User |
|---|---|---|---|
| 1 | Flashed with a full erase, or new from the factory. The first boot is virgin: it writes `OS2S {FRESH}` and enters **FRESH claim mode** (§ 7.1). There is no config, so nothing is drawn. MSD bit 2 = 1, bit 3 = 0 | | flashes or unboxes the display and powers it |
| 2 | | lists "displays waiting to be set up" from the MSD (§ 5.3), connects, reads READ_IDENT (`display_count` 0, CLAIM_MODE) and SEC_STATUS (FRESH, `window_s`) | picks the display by its name `OD1A2B3C` |
| 3 | accepts the config write (FRESH, `config_locked = 0`) | writes the display config: drive packets (0xAE-0xB1, or 0xB3 on native boards), 0x02 simple-config ids, 0x27 with the preset's default reset pin, **no secrets** | picks board, display and power option (preselected when the wizard was opened from a setup QR) |
| 4 | the reboot that applies the config writes OS2C and resets → **CONTINUE**, restart 1, the timer restarts | sends CMD_REBOOT if the target does not reboot by itself, then reconnects | |
| 5 | draws the **SETUP** screen through the new config, in mode 0. This proves the preset drives the panel | | |
| 6 | | asks *"Does the display show SETUP MODE and the ID OD1A2B3C?"* | **Yes** → step 7. **No / garbled / blank** → another preset (back to step 3, at most 3 restarts), or the blank-panel path (§ 8.6) |
| 7 | BEGIN, NONCE: renders_used = 1, `config_locked = 1`, draws the **CODE** screen | runs BEGIN and NONCE; asks for the code | types the 6 digits from the display |
| 8 | CONFIRM: persists OS2K, session OPEN, device **CLAIMED** (still in claim mode) | stores the candidate durably (§ 8.8), sends CONFIRM, verifies `tag_d` → confirmed | |
| 9 | stores the sealed config (claimed) | writes the full config **with secrets** (WiFi password, FindMy) in the session | enters the WiFi / FindMy settings |
| 10 | END(CLAIMED): FRESH cleared, reboot into **normal mode**, normal boot screen | sends SEC_CLAIM EXIT, or a sealed CMD_REBOOT | |
| 11 | normal operation | reconnects: HELLO with the confirmed key, then uploads | done |

The display is claimed **before** any secret reaches it, and the code was checked on a screen the
user has just seen the device draw.

### 13.2 Unhappy paths

- **U1, the panel never draws** (wrong or untested preset, or no panel connected).
  - The user tries other presets. Each attempt is a restart, at most 3.
  - If none works, there are two ways out:
    - **"My display stays blank"** → the blank-panel claim with its warning (§ 8.6);
    - **"Cancel setup"** → SEC_CLAIM EXIT → normal mode, unclaimed and open.
  - To claim later: fix the panel config in normal mode, then do the gesture. Config writes are
    refused in gesture claim mode, so the working preset must be written first.
- **U2, timeout mid-setup** (600 s since the last ENTER or restart).
  - END(TIMEOUT): FRESH is cleared and the device reboots into normal mode.
  - The config written so far stays, the device is unclaimed and open, and its boot screen is
    drawn if it can be.
  - Client text: *"Setup mode has ended. The display works without a key. To claim it, switch it
    off and on, wait about 5 seconds, and switch it off and on again."*
- **U3, an attacker in range during fresh claim mode.**
  - *(a) First claimer.* Someone else claims first. The owner sees a CODE screen they did not
    start, and/or their BEGIN gets ALREADY_CLAIMED. The client says: *"This display was just
    claimed by someone else. Factory-reset it: hold its reset button for 5 seconds while
    switching it on."*
    - If the attacker's config has no reset pin (it may have written one without), the only
      recovery is a full USB/SWD erase and reflash (§ 1.4 item 4). The client links the
      flasher.
  - *(b) Config interference.* The attacker writes its own config before or after the owner's.
    The SETUP screen then does not look as expected, or the panel check fails. The wizard says:
    *"The display does not show what we expected. Someone nearby may be interfering. Cancel
    and try again later, or somewhere else."*
  - *(c) A two-radio MITM.* The attacker impersonates the device to the owner and writes the real
    device's drive config. This can defeat the code (§ 1.4 item 5). Mitigation: § 15 P4.
- **U4, power lost during fresh claim mode.** FRESH survives, so the next boot is a new FRESH
  session: 10 minutes and 3 codes. The wizard reconnects and resumes at the step it was on.
- **U5, the CONFIRM reply is lost.** The client keeps the candidate (§ 8.8). The device is
  claimed and in claim mode until the connection drops; then it reboots into normal mode. The
  client reconnects and runs HELLO with the candidate. On success the candidate is confirmed, and
  the client continues at step 9: the device is claimed, so secrets go into the new session.
- **U6, the user abandons before CONFIRM.** Nothing is persisted. Claim mode times out (U2).
- **U7, three codes used without success.** ATTEMPTS_EXHAUSTED → normal mode, unclaimed, FRESH
  cleared. Claiming now needs the gesture.

### 13.3 Claiming a configured, unclaimed device (the gesture flow)

1. The client finds the device in normal mode: BEGIN would get NOT_IN_CLAIM_MODE. It does not
   send BEGIN to find out; it reads SEC_STATUS.
2. If the device cannot draw yet (NEEDS_MIGRATION), the client first writes the drive packets
   in normal mode. That needs no gesture. It confirms the boot screen is drawn.
3. The client shows the gesture instruction and waits: it scans for MSD bit 2, or polls by
   reconnecting.
4. The device enters GESTURE claim mode, with config writes refused, and draws SETUP.
5. Steps 7-11 of § 13.1.

### 13.4 Upgrading from 2.x to 3.0: when is the gesture needed?

| 2.x device | After the 3.0 OTA | Drive packets | Gesture needed? |
|---|---|---|---|
| no key, no secrets | UNCLAIMED, normal mode (NEEDS_MIGRATION until drive packets exist) | written openly | **no**. Only to claim, which is optional and suggested |
| no key, WiFi/FindMy secrets | UNCLAIMED + LEGACY_SECRETS; WiFi/FindMy keep working | written openly; the identical-secret rule (§ 10.4) accepts the read-modify-write | **no** for drive packets. **Yes** to claim, and the client prompts for it |
| v1 key | CLAIMED, origin LEGACY (B2) | written in a session | **never**; rotation happens inside the session, by default |
| never configured | virgin → FRESH claim mode | written in claim mode | no (it is fresh) |

The ordering hazard remains: HA pushes BG22 OTAs itself, so the HA release that offers 3.0 MUST
carry the SEC2-capable py-opendisplay (plan WP10).

### 13.5 Ownership changes and failures

| Situation | Path |
|---|---|
| hand over, current owner cooperates | owner: SEC_KEY UNCLAIM (device open, normal mode) → new owner: gesture → claim |
| lost key | factory reset (reset pin) → FRESH claim mode → § 13.1 from step 3 (the reset erased the config; the RESET screen's QR preselects the old preset) |
| LOCKED (corrupt key record) | the gesture is ignored; factory reset, or a full USB/SWD erase |
| rotation | in any session, sealed SEC_KEY; no claim mode |
| a stranger claimed my device (U3a) | factory reset; if impossible, USB/SWD erase |

### 13.6 Old (v1-only) clients and claim mode

- An unclaimed device in claim mode looks like any unclaimed device: MSD bit 3 = 0, and reads
  work.
- In a FRESH session, before the first code, config writes work. Secrets get 0x10.
- Uploads, LED, buzzer, DFU and sleep get `[FF][cmd][FD]`. Old clients report a failed command.
- After at most 10 minutes (more with restarts), the device reboots into normal mode, and old
  clients work as on any unclaimed device.
- Release note: *a freshly flashed or factory-reset 3.0 display refuses picture uploads for up to
  10 minutes after each power-on, until it is claimed or its setup mode ends. New apps can end
  setup mode at once ("Skip").* (A FRESH session that ends clears FRESH, so this happens at most
  once per fresh state, plus once per power loss during it.)
- A claimed 3.0 device refuses v1 (`[00][50][05]`) and answers FE to gated commands.

### 13.7 Third-party "Core" devices (SEC2 is an optional module)

| Build | Behaviour |
|---|---|
| `OD_SEC2=1` (every OpenDisplay 3.0 build) | this specification |
| `OD_SEC2=0` (a third-party product without the module) | READ_IDENT SEC_V2 = 0. 0x0054-0x0057 are unknown opcodes. 0x0050 answers NOT_CONFIG. No claim mode, no gesture counter, no OS2* records. MSD bit 3 = 0. Panel writes are always allowed, and FD is never sent. **Secrets: refused with 0x10** (the rule stays universal, so "secrets accepted" always means "protected"), **pending Q14** |

Clients treat `SEC_V2 = 0` on firmware ≥ 3 as "this display cannot be claimed or hold secrets".

---

## 14. The decisions the gesture touches, revisited

| Topic | Outcome |
|---|---|
| "First 10 minutes after boot" window, REBOOT_REQUIRED | **deleted**; NOT_IN_CLAIM_MODE (0x04) replaces it |
| Panel writable by an attacker during the claim (A1) | **closed**: refused in claim mode (§ 7.4). The remaining FRESH residual is named (§ 1.4 item 5) |
| Window reopened remotely (A8) | **moot**: no wake counts. The only residual is an induced brown-out on nRF52 (§ 1.4 item 18) |
| Claim attempts (A7) | 3 renders per session (§ 7.5) |
| Secrets need a key | normal mode refuses (0x10). The client says "claim first" and gives the gesture text. In claim mode the secrets follow in the claiming session (§ 13.1 step 9) |
| 2.x devices with secrets, flagged | keep working. The gesture is needed to claim, but not for drive packets (identical-secret rule) |
| 2.x v1 key | migrated (B2). No claim, no gesture; default client rotation inside a session |
| Rotation | unchanged; no claim mode |
| Re-claim | UNCLAIM + gesture, or factory reset (§ 13.5) |
| Lost key | factory reset → FRESH claim mode |
| Corrupt key record | LOCKED; gesture ignored; factory reset |
| Factory reset on boards without a reset pin, by a gesture family (e.g. five power-cycles) | **option only, not adopted** (Appendix B.1) |
| Factory reset's end | FRESH claim mode. Its RESET screen doubles as the SETUP screen (§ 7.8) |
| DFU | as today on claimed and open devices; refused in claim mode |
| Old clients | § 13.6 |
| Core devices | § 13.7 |
| Advertising, privacy | MSD bit 2 during claim mode only (§ 5.3) |
| Battery | § 7.9 |
| Devices without a panel | warning path, refused when the client knows of a display (§ 8.6) |
| Website code check | typed, like HA and py |

---

## 15. A unified setup experience (proposal; separable)

The **decided** parts are above:
- claim mode and the gesture;
- the typed code;
- default rotation with opt-out;
- the no-panel warning;
- the setup screen showing claim mode.

Everything in this section is **my proposal**. Each item stands alone, and the maintainer can take
or drop it without touching the protocol.

| # | Proposal | What it gives |
|---|---|---|
| P1 | **One wizard, same steps and words, in all three clients**: Identify → Configure display → Verify panel → Claim → Secrets → Done (§ 13.1). The user-facing strings of § 7-§ 13 live in one shared text file (py-opendisplay ships it; the website and HA copy it), so the gesture text, warnings and code prompt are identical everywhere | a user who switches app sees the same thing; translations are done once |
| P2 | **The setup QR opens the website wizard** with the device identity and the preset preselected: `/s/?k=s&i=…&b=…&d=…&p=…`. The page filters Web Bluetooth by the name `OD<i>` and shows the preset for confirmation (§ 7.8 rule 3) | no searching for the board or panel after a factory reset; fewer wrong presets |
| P3 | **HA discovers devices in claim mode.** A MSD candidate (bit 2 = 1, bit 3 = 0) creates a discovery card: *"OpenDisplay OD1A2B3C is waiting to be set up"*. The config flow then runs the wizard with py-opendisplay's vendored presets. Unclaimed devices in normal mode keep today's card, with two choices: "Claim (needs the power-cycle gesture)" and "Use without a key" | setup starts from where most users already are |
| P4 | **"Extra-secure setup"**: configure the display (FRESH or normal mode), end setup mode, then claim it in **gesture** claim mode, where config writes are locked | closes the fresh-setup MITM residual (§ 1.4 item 5) at the cost of one gesture. Offer it as an option, not the default |
| P5 | `opendisplay setup [--url 'https://opendisplay.org/s/?…']` in py-opendisplay runs the same wizard on the command line; the URL preselects the preset | scripted lab setups; the QR works on a PC too |
| P6 | **Recovery is setup**: the RESET QR lands in the same wizard with the old preset known from `b/d/p`, and nothing else differs | one flow to maintain |
| P7 | **Migration assistant** for 2.x devices, one screen per client: "Update drive data" (no gesture) → "Rotate key" (automatic for v1 keys) or "Claim (gesture)" for flagged devices → "Change your WiFi password" reminder | one place that walks the 2.x → 3.0 cases of § 13.4 |
| P8 | **One key export format** for moving a key between clients: `OD2K:<uid 16 hex>:<key 32 hex>`, as text or as a QR shown on the *phone/PC screen* (never the e-paper). It is imported as a **candidate** until an authenticated success (§ 8.8) | website → HA handover without retyping; mistakes cannot overwrite a working key |

---

## 16. Compatibility

### 16.1 Clients x firmware

| Client \ device | 3.0 unclaimed, normal | 3.0 claim mode | 3.0 claimed | 2.x (any) | legacy Firmware_NRF |
|---|---|---|---|---|---|
| new (SEC2 + v1) | open; claim via the gesture | claim | SEC2 | v1 + mitigations | v1 + mitigations |
| old (v1 only) | open; a config with a key, WiFi password or FindMy keys gets 0x10 | reads and (FRESH) config writes; everything else FD (§ 13.6) | refused: v1 step 1 → `[00][50][05]`; gated → FE | v1 | v1 |

### 16.2 What an old client sees on a claimed device

- `CMD_AUTHENTICATE [00 50 00]` gets `[00][50][05]` AUTH_STATUS_V2_REQUIRED. py-opendisplay 7.x
  raises on the unknown status, and HA reports a failed authentication. On an unclaimed 3.0
  device it gets `[00][50][03]` NOT_CONFIG, which is what 2.x devices without a key answer.
- MSD bit 3 = 1 makes old clients ask for a key. The key they have (if the device was migrated)
  does not help them.

---

## 17. Timing limits and rate limits

| Limit | Value |
|---|---|
| claim mode | 600 s from each ENTER (§ 7.3); at most 3 restarts, so at most 40 min; a pending claim extends it to its CONFIRM timeout |
| code renders | 3 per claim-mode session (§ 7.5) |
| gesture on-period | the previous counted boot lived ≥ 2 000 ms and < 10 000 ms (§ 7.2) |
| BEGIN reply → NONCE | 30 s |
| NONCE reply → CONFIRM | 300 s (the user reads a code that a slow panel may take 40 s to draw) |
| HELLO reply → FINISH | 30 s |
| fragment → next fragment of the same message | 5 s |
| device reply latency after a LAST fragment (ECC included) | SHOULD be ≤ 2 s; client timeout 10 s |
| handshake starts | the ring of 10 counted events per rolling 60 000 ms (§ 8.7) |
| session lifetime | `timeout_s`, default 3600 s, absolute; snapshotted at open |
| bad tags per session | 3 → teardown |
| counters | 1 .. 0xFFFFFFFE per direction; then teardown and a new HELLO |
| END reply flush before the reboot | ≤ 500 ms |

The rate limiter is per device, not per peer, since there is one connection. It is a DoS lever
(§ 1.4 item 8).

---

## 18. Test vectors (produced by the reference implementation)

Real vectors cannot be computed by hand. **py-opendisplay's SEC2 module is the reference
implementation.** It generates `sec2_vectors.json` from fixed inputs. The file is committed to
`Firmware_Unified/tests/vectors/` for the firmware host tests, and to the website tests. The
website's JS implementation must reproduce every vector independently (WebCrypto ECDH + its own
CMAC/CCM). Two independent implementations must agree before firmware is tested against them.

**Blocking before WP1 generates them:**
- the cryptographer's K0 decision (§ 2.1);
- the uid byte order on Nordic (§ 4.3, § 19 H11).

| Group | Inputs | Outputs to check |
|---|---|---|
| V0 primitives | RFC 4493 CMAC; SP 800-38C / CAVP CCM with a 12-byte tag and 13-byte nonce, incl. zero-length plaintext; CAVP P-256 ECDH (KAS ECC CDH) | as published |
| V1 KDF | K_in, Label, Context for every label of § 2.3, with both context lengths where two exist | the KDF output and the full CMAC input bytes |
| V2 key hierarchy | K_dev, uid; a LEGACY and a CLAIMED origin | K_ext, K_conf, K_lan, K_v1 |
| V3 claim, happy path | uid, dflags (with CLAIM_MODE_FRESH 0 and 1), dc, dd (private scalars), Nc, Nd, K_new | Qc, Qd, Cd, Tc (hex), Z, PRK, k_wrap, W, code_raw, code, K_conf, tag_c, tag_d, k_h2d, k_d2h, sid; **every fragment of every message as wire bytes** at frag_max 20, 21 and 244 (deterministic D->H split) |
| V4 auth, happy path | uid, dflags, K_dev, dc, dd, Nc, Nd | Qc, Qd, Ta, Z, PRK, tag_c, tag_d, k_h2d, k_d2h, sid; wire fragments |
| V5 records | k_h2d, k_d2h; H->D frames for cmd 0x0040 (empty) and 0x0041 (200 B) at counters 1, 2, 255, 256, 0xFFFFFFFE; D->H frames for an ACK, a NACK, a 0x73 notification, a sealed `[FF][cmd][FD]` | wire bytes; counters 0 and 0xFFFFFFFF rejected |
| V6 rotation | K_dev, sid, K_new | K_conf_new, the rotate tag, the new K_v1 |
| V7 negative | each of: Qc not on the curve; Qc = (0, 0); X ≥ p; Y ≥ p; Cd mismatch; W with one flipped bit; tag_c from a wrong Tc (one byte of Nd changed); wrong uid; an H->D frame sealed with k_d2h (reflection); a replayed counter; counter 0; counter 0xFFFFFFFF; AAD byte 0x01 instead of 0x02; a truncated tag; a fragment index gap; an empty non-LAST fragment; index 127 without LAST; a body one byte long or short; a no-ack frame with a bad tag | the exact error (`OD_SEC_ERR_*`, sealed NACK code, silence, or "client discards") |
| V8 code edge cases | code_raw values giving 000 000, 999 999, and the bias boundary | the formatted code strings |
| V9 boot decision | one case per row of § 10.1 (records and config as bytes) | the resulting state, and every repair write as bytes |
| V10 secret detection | config blobs: an empty WiFi password with an SSID; one non-zero password byte at offset 31; two 0x26 packets, only the second with a password; 0x2D with only `advertisement_key[19]` set; 0x27 enabled with a zero key; 0x27 key == K_dev on a claimed device; 0x27 with another key; a chunked write whose secret is in the last chunk; a truncated last packet; a LEGACY_SECRETS device with identical and with changed secrets | accept / NACK 0x10 / 0x11 / 0x00, with the exact NACK frame |
| V11 config at rest and read | claimed device: a write with and without 0x27 (insertion position), with the reset pin disabled (sticky), CONFIG_CLEAR; unclaimed device | the stored config bytes (marker, K_v1) and the exact read image |
| V12 claim mode (host traces) | reset-cause and uptime traces: counted/uncounted causes, on-periods of 1.9 s, 2.0 s, 9.9 s and 10.0 s, deep sleep after arming, a claimed device; claim-mode sessions with restarts, attempts and timeouts | the gesture verdict, OS2S writes (count and bytes), the END reason, FRESH clearing |
| V13 setup URL | all fields, omitted fields, maxima (83 B), `k=s` and `k=r` | the exact strings and the QR version / ECC |

Firmware host tests (`tests/host/`) drive `od_sec2` and `od_claim_mode` with the HAL crypto bound
to a host implementation, and reproduce V1-V13 byte for byte. The device's random choices are
injected through a test-only RNG/keygen seam.

---

## 19. Needs a cryptographer, or a measurement on a device

Nothing below is settled. **E** items need someone with protocol-cryptography expertise before
the wire format is frozen. **H** items need hardware and are open rows in
`docs/HARDWARE_VERIFICATION_CHECKLIST.md`.

| # | Item | Status after the review |
|---|---|---|
| E1 | AES-CMAC as the 800-56C extractor of a P-256 x-coordinate, with salt 0^16 (claim) and the secret salt K_ext (auth) | The reviewer considers it acceptable (same as BT f5), justified in the ideal-cipher model; the forward secrecy of SEC_AUTH after a K_ext leak rests on CMAC with a known key being a good extractor (a "dual-PRF"-like assumption without a published CMAC analysis). **Needs a cryptographer's sign-off on the assumption** |
| E2 | Single-block CMAC counter-mode KDF; the SP 800-108r1 K0 variant | Revision 1's argument was wrong (key control applies to one block). Conformance is no longer claimed. **Decide before vectors: adopt K0 or keep the current form** (§ 2.1) |
| E3 | The f4/g2 adaptation (commitment and code inputs, order, truncation) | The reviewer checked the order and found it sound, given claim mode (A1) and the cap (A7). **A formal model (e.g. Tamarin) of claim + CONFIRM is recommended as cheap insurance** |
| E4 | Nd used as a key once and as data in Tc | reviewer: harmless. Closed unless the cryptographer objects |
| E5 | CCM with 96-bit tags, the dir+counter nonce, zero-length plaintexts | reviewer: fine, with the A14 consumption rule (now § 9.5). Closed unless the cryptographer objects |
| E6 | Label separation in the key hierarchy (K_conf for claim and rotation; K_ext only as salt; K_v1 new) | reviewer: fine. Confirm that K_v1 adds no interaction: it is used only by v1 firmware after a downgrade |
| E7 | Peer-point validation | now done explicitly in shared code (§ 2). See H1 for the backends |
| E9 | Client-proves-first; offline guessing by a device impersonator against weak legacy keys | reviewer: correct. The impersonator oracle is unavoidable without a PAKE; mitigated by default rotation (§ 10.5) |
| E10 | W under a constant nonce with a single-use k_wrap | reviewer: correct. The derive-instead option is not adopted (§ 8.2) |
| E11 | 6-digit code, 3 renders per session | the reviewer's bound: 3·10⁻⁶ per session, given claim mode |
| E12 | A corrupt key record | **fail-closed, LOCKED** (maintainer); the fail-open wording of revision 1 is corrected; implemented by § 10.1 |
| E13 | Constant-time compares; ECC side channels | ct_equal specified; side channels out of scope (§ 1.4 item 16) |
| E14 | LAN TLS-PSK with a static PSK | no forward secrecy unless (EC)DHE-PSK is negotiated (R12) |
| H1 | V0 and V7 (invalid points) through the real HAL on each backend: Silabs CRYPTOACC, nrf_cc3xx (CC310), CRACEN (nRF54), Oberon, ESP-IDF HW ECC on C6/S3 | open |
| H2 | ECC keygen + ECDH latency per target; handshake time via an ESPHome proxy | open (§ 9.1 is an estimate) |
| H3 | Corruption semantics with injected faults: does Zephyr NVS return an older entry for a corrupt newest ATE? What does NVM3 do on a bad object? ESP-IDF NVS on a torn write | open; it drives § 10.1 and its residual |
| H4 | What 2.x firmware does with a 3.0 config (downgrade): does it parse it, and what does CONFIG_READ return? | open; it decides how much § 10.2 buys |
| H5 | ESP32 RNG quality during a claim (BT on) per board | open |
| H6 | ATT MTU through ESPHome proxies (≥ 185?) | open (R9) |
| H7 | BG22 effective connection count (`sl_bluetooth_config.h` (0) vs `sl_bluetooth_connection_config.h` (4)) | open; WP7 sets 1 |
| H8 | Reset causes per board: unplug/replug, battery pull and latch button give POR (not brown-out); nRF54 POR/BOR reporting; RESETREAS clearing | **open, needed before the gesture ships** |
| H9 | Gesture usability: the 2-10 s window on real boards with large supply capacitors | open |
| H10 | Setup QR scanned from a 1.54in panel at 4 px/module, and from a 2.9in at 2 px/module | open |
| H11 | nRF52840 / nRF54 `hwinfo_get_device_id` bytes vs FICR, recorded per board | **open, blocks vectors** |
| H12 | Claim-mode battery cost per target | open |
| H13 | Nordic storage budget: OS2K/OS2S/OS2R in a separate area, with a 4 kB config still fitting the 2 x 4 kB config NVS | open |

---

## Appendix A. Review response (review_security_v2.md)

"Agree" means applied as the reviewer asked. "Changed" means applied in a different form, for the
reason given. "Disagree" means not applied, with reasons.

| Finding | Response | Where |
|---|---|---|
| **A1** the code's display is writable | **Agree; changed.** The reviewer's option 1 (freeze the panel during the window) is superseded by the maintainer's claim mode, which is stricter: panel writes are refused, the device draws a firmware-only screen, and claiming starts physically. Option 2 (the user power-cycles right before the claim) is the gesture itself. Option 3 is in the client text. Option 4: G5 is rewritten. Additionally, the drive-recipe vector the reviewer did not list (a config written during the claim can paint digits) is closed by `config_locked`. Its FRESH-mode residual is named | § 1.3, § 1.4.5, § 7.4, § 7.8 |
| **A2** clients delete keys on unauthenticated results | **Agree.** Normative retention rules, durable persistence checks per client, and the key download in non-persistent browsers | § 8.8, § 11 |
| **A3** absent vs corrupt | **Agree.** Independent evidence (0x27 marker + OS2S key_gen); no magic/CRC distinction; an 8-row boot table. Of the reviewer's two options ("LOCKED" or "scrub then UNCLAIMED"), the maintainer chose LOCKED | § 10.1 |
| **A4** factory reset erases the key before the secrets | **Agree.** R1b scrub before R2; a persistent R2 failure → LOCKED | FACTORY_RESET § 5 |
| **A5** downgrade opens the device | **Agree**, with the derived key rather than the random-key alternative, so that a client holding K_dev can still reach a downgraded device. Plus a refinement: unrotated LEGACY keys are stored as themselves, so a downgrade restores exactly 2.x | § 10.2 |
| **A6** contradictions | **Agree** with all four: B2, DFU as today, the E12 wording, write-without-response (with the sealed no-ack rules) | § 0, § 6.1, § 9.5, § 10.5, § 19 |
| **A7** attempts uncapped | **Agree**, per claim-mode session instead of per physical boot (claim mode is the unit now) | § 7.5 |
| **A8** window reopened via wake | **Agree in substance; now moot.** No wake counts, and claim mode needs FRESH or the gesture. The reviewer's "POR + nRESET only" is adopted, except that on nRF52 brown-out cannot be separated; that residual is named | § 7.2, § 1.4.18 |
| **A9** recovery path erasable by the key holder | **Agree** with "preserve the reset pin" (sticky-enabled), rather than latching it into the secret record: the pin stays in one place, which clients already edit. § 1.4 item 4 is corrected: recovery from a takeover is guaranteed only by USB/SWD | § 10.6 |
| **A10** SEC_KEY ordering | **Agree** | § 11 |
| **A11** connection count | **Agree:** one connection is normative; BG22 to be fixed and verified | § 5.4 |
| **A12** LAN underspecified | **Agree;** FE for plaintext on claimed devices (the closed-port alternative is not chosen: one refusal shape for clients); SEC_* BLE-only; no TLS when unclaimed; no WiFi in claim mode | § 12 |
| **A13** scan vs loader | **Agree** | § 10.4 |
| **A14** counters and retries | **Agree.** 0xFFFFFFFF is unusable (V5 fixed accordingly) | § 9.5 |
| **A15** HAS_PANEL stripping | **Agree,** plus the separate blank-panel path for a configured panel that does not draw | § 8.6 |
| **A16** ESP32 WiFi NVS copy | **Agree:** `WIFI_STORAGE_RAM`, plus erasing in every scrub and at the first boot | § 10.4 |
| **A17** response binding | **Disagree for 3.0.** PIPE's many outstanding frames and set-wise SACKs make "the request a response answers" ill-defined. Trial decryption against two bindings complicates strike counting. G7 is met by per-handshake keys plus in-order delivery and the authenticated echo. Worth revisiting if pipelining is ever removed | § 9.4 |
| **A18.1-12** interop | **Agree with all**, resolved: § 2.3 (1, 2), § 4.3 (3), § 3.3 (4), § 3.2 / § 8.7 (5, 6), § 5.1 (7), § 10.3 (8), § 2 (9), § 9.5 (10), § 4.4 (11), § 18 (12) | as listed |
| R1 first-claimer-wins | accepted by the maintainer (no button press); now limited to claim mode, which only physical action starts | § 1.4.4, § 1.5 |
| R2 DFU ceiling | accepted by the maintainer (as today); stated plainly | § 1.4.2, § 1.5 |
| R3 reflashed while open | accepted; "claim promptly after flashing" is in the wizard; the FRESH state makes the first boot the claim | § 1.3 G5 |
| R4 migrated weak keys | mitigated by default client rotation (the reviewer's recommendation, now a maintainer decision) | § 10.5 |
| R5 any session reads K_dev | accepted by the maintainer; stated plainly | § 1.5 |
| R6 `?key=` | recommendations kept and marked as the reviewer's minimum | § 10.8 |
| R7 best-effort erase | **Agree:** the wording changed from "secure-erase where supported" to "best effort" | § 1.4.3 |
| R8 change the WiFi password | **Agree:** in the client text after claiming a flagged device | § 10.5 |
| R9, R11, R12 | accepted residuals; R11 additionally: SEC_CLAIM EXIT can cancel someone's claim mode | § 1.4 |
| R10 no panel | narrowed by A15 | § 8.6 |
| C1-C10 | carried into § 19 as E1-E14 and H1-H13 (C2 → E1, C3 → E2, C4 → E3, C5 → H3, C6 → H4, C7 → H5, C8 → H6, C9 → H7, C10 → E13) | § 19 |
| E10 option (derive K_dev) | **Disagree:** it keeps K_dev non-random, and W costs nothing extra (CCM is needed anyway) | § 8.2 |
| Recovery-page phishing advice | **Agree,** as rules for the `/s/` page | § 7.8 |

---

## Appendix B. Options considered and not adopted

### B.1 Factory reset by a gesture family (e.g. five quick power-cycles) on boards without a reset pin

The maintainer asked for this as an option with a risk analysis only. **It is not adopted.**

**Mechanism.** The same OS2S counter as § 7.2. N = 4 consecutive short on-periods (the fifth
power-on) would trigger the factory reset of FACTORY_RESET.md.

**Risks:**
1. **The false positive is catastrophic.** The key, the WiFi and FindMy settings (including a
   FindMy registration that may be impossible to redo) and the images are erased. The device then
   returns *fresh*, in claim mode, so **a power glitch would hand the device to anyone nearby**
   (first-claimer-wins). For the claim gesture a false positive costs 10 minutes; here it costs
   the device.
2. **Flaky power produces exactly this pattern.**
   - A supercapacitor or solar board near its threshold boots, starts a refresh, collapses and
     reboots every few seconds.
   - So does a weak battery whose voltage sags under the refresh current, or a USB hub that
     power-cycles a port it considers faulty.
   - On nRF52, brown-out and power-on look the same (§ 7.2), so such loops count, and five in a
     minute would wipe the device. Excluding brown-out is impossible there.
3. **Wear and cost on claimed devices.** The claim gesture writes nothing on claimed devices. A
   reset gesture must count on claimed devices too: about 2 writes per counted boot on every
   device in the fleet.
4. **It is remote-inducible where brown-outs are.** It needs a load the attacker can force. On a
   claimed device the attacker cannot force refreshes without a session, but the device's own
   scheduled refreshes can loop on their own.

**If it is ever reconsidered, the minimum mitigations:**
- never on targets that cannot separate brown-out (so not on nRF52);
- a longer pattern (≥ 7);
- skipped when the measured battery voltage is low;
- a final confirmation step: after the pattern the device draws "RESET? Switch off and on once
  more", and a sixth power-on within 30 s, with a stable on-period of ≥ 30 s before the erase,
  confirms it;
- the RESET screen is drawn before anything is erased (as FACTORY_RESET.md already requires).

**Recommended instead for button-less boards:**
- presets may define a solder pad or jumper as the reset pin (FACTORY_RESET.md § 2.1);
- otherwise, a full USB/SWD erase.

### B.2 Button press to accept a claim

Excluded by the maintainer (decision 7). It would turn first-claimer-wins from "detected" into
"prevented" on reset-pin boards (review R1). Claim mode narrows the exposure to a physically
started window instead.

### B.3 Binding the DFU unlock to the unlocking connection

Excluded by the maintainer (decision 11). It would have closed "the next connector wins" after a
sealed ENTER_DFU on ESP32 and Nordic (not on BG22, whose AppLoader is outside the application).

### B.4 Response binding (A17), derive-instead-of-wrap (E10)

See Appendix A.

### B.5 Signalling claim mode by the local name

Rejected: a longer name overflows the 31-byte advertising PDU, and HA matches on the `OD` prefix
(§ 5.3).

### B.6 An info screen when the gesture is used on a claimed device

Rejected (§ 7.6): it gives flaky power a way to wipe pictures on claimed devices, and it needs
counter writes on every claimed device.
