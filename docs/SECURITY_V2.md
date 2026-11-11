# OpenDisplay Secure Session v2 ("SEC2") — normative specification

Status: **DRAFT revision 5 for maintainer and expert review, 2026-11-11.** Implemented on
`v3/head` for all three targets and host-tested. Parts were run on two devices (one ESP32-S3
reTerminal Sticky, one EFR32BG22 M3 Pro); § 19.1 says exactly which, and lists what has never run
on hardware. Nordic has not run on hardware at all. Every opcode, status byte, error code, flag
bit, record layout and label is **provisional** until it is written into the canonical
`opendisplay-protocol` header. § 19 lists what still needs a cryptographer or a measurement on
a device; those points are not settled claims.

Companion documents: [FACTORY_RESET.md](FACTORY_RESET.md) (reset pin, erase ordering, reset
screen) and [SECURITY_V2_IMPLEMENTATION_PLAN.md](SECURITY_V2_IMPLEMENTATION_PLAN.md) (work
breakdown, cost, remaining questions).

The words MUST, MUST NOT, SHOULD and MAY are used in the RFC 2119 sense.

---

## Changelog

### Revision 5 (2026-11-11)

Inputs: the advertising investigation (`advert_claim_mode`), the accepted setup-flow proposals
P1, P2, P6 and P7, and eight maintainer decisions of 2026-11-11 (A-G below). Implemented on
`v3/round5` and host-tested; nothing of revision 5 has run on hardware.

| What changed | Why |
|---|---|
| **A. MSD status bits stay the claim-mode signal**, renamed: bit 2 `CLAIM_MODE`, bit 3 `SESSION_REQUIRED`. "Waiting to be claimed" = bit 2 set and bit 3 clear; confirmed with a plaintext READ_IDENT / SEC_STATUS. `dynamic[11]` is **not** a claim sentinel. All three targets now take both bits, and the unconfigured rule, from one shared function (§ 5.3); BG22 never set bit 3 before | maintainer decision A; BG22 defect |
| **B. "Unconfigured" = `dynamic[11]` all 0xFF** (protocol 2.2) on every target, from shared code, with "no display configured" defined for 3.0 as **no drivable display** (§ 5.3.2). It combines with claim mode on a virgin device | maintainer decision B; only ESP32 did it |
| **C. Zephyr targets advertise the name in the advertising PDU** (flags 3 + MSD 18 + name 10 = 31 B); the scan response keeps the 16-bit service UUID and Channel Sounding's Ranging UUID (§ 5.3.3). The § 5.3 PDU facts are corrected per target | maintainer decision C; HA name-based discovery and passive scanners |
| **D. Claim-mode advertising boost** implemented: the shared 20-30 ms boost window, re-armed for the first 60 s of every claim-mode ENTER, never while connected; at most 4 × 60 s per claim-mode session (§ 7.3, § 7.9) | maintainer decision D; § 7.3 was unimplemented |
| **E. SHOW_KEY_ON_SCREEN (0x27 `flags` bit 1) is honoured again.** On a CLAIMED device with the bit set, the NORMAL boot screen shows K_dev as 2.x did (the key lines and the key bytes of the `/l/` QR payload), permanently while the bit is set. The bit is stored only by a config write on a claimed device; setup/CODE/RESET screens never show a key; rotation redraws the boot screen; default off (§ 10.3, § 10.7, § 1.4 item 20) | maintainer decision E; reverses revision 3/4 |
| **F. One setup/recovery URL grammar on the existing `/l/` entry point**: `https://opendisplay.org/l/?f=1&k=<1\|2>&i=…&t=…&v=…[&b=][&d=][&p=]`, format version first, numbers only, append-only, ≤ 83 B worst case, QR v6-M. `/s/` (never shipped) is withdrawn (§ 7.8.1) | maintainer decision F (P2, P6) |
| **G. The setup flow is normative where it concerns the device** (new § 15): device-side triggers per state for the one wizard (P1), the QR (P2), recovery is setup (P6), migration-assistant inputs (P7). P4, P5 and P8 are **dropped by decision** | maintainer decision G |

### Revision 4 (2026-11-11)

Inputs: the first hardware runs (ESP32-S3 Sticky: `impl_sec2-esp32-hw`; BG22 M3 Pro:
`impl_sec2-hw`, `impl_reconnect-fix`), the py-opendisplay cross-harness, and two maintainer
decisions of 2026-11-11 (advertised data is public; the five-cycle reset can be switched off per
device).

| What changed | Why |
|---|---|
| **Advertised data is public by design.** READ_MSD (0x0044), and any opcode that returns only what the device already advertises, is in the plaintext set on a claimed device. READ_MSD returns exactly the 16 advertised MSD bytes and nothing else, on every target (§ 6 rule 6) | maintainer decision |
| **The five-cycle factory reset can be switched off per device**: 0x27 `flags` bit 6 `NO_RESET_PATTERN` (provisional). Clear = on, the default. A LOCKED device ignores it. The security consequence both ways, and the client warning for "claimed, pattern off, no reset pin" (§ 7.10.1, FACTORY_RESET.md § 3.4) | maintainer decision |
| **The oversize refusal `[FF][cmd][FE]` is sealed inside a live session** for a gated opcode, and plaintext otherwise. A client accepts both forms (§ 3.4) | a sealed session must not carry a plaintext answer to a sealed request; the cross-harness saw `ff71fe` in clear |
| **Repeated FE closes the link** on BG22 (3 in a row) and ESP32 (10 in a row); Nordic does not. What a client concludes from that disconnect (§ 6 rule 5) | measured on both devices; the spec was silent |
| 0x0057 SEC_KEY in claim mode is **FD**, as the firmware answers; the table said NOT_CLAIMED. The claim-mode allow-list now names the 0x0056 refusal (§ 6) | ESP32 run D-7: the gate runs before routing |
| **BUSY now counts in the handshake rate limiter**; RATE_LIMITED refusals still do not (§ 8.7, § 17) | otherwise BEGIN/HELLO during a refresh is a free ECC-less probe loop; BG22 run finding 6 |
| **Superseded secrets are purged from the raw storage** after UNCLAIM and factory reset, on every target (new § 10.4a, OS2S flag bit 3 PURGE). Key rotation does not purge; the residual is stated (§ 11) | ESP32 run D-2 / D-4: NVS kept 6 old WiFi-password copies and the old K_dev |
| The crypto HAL contract: buffers are **never NULL**, also for an empty plaintext (§ 9.5 rule 5b) | BG22 PSA refused a NULL plaintext on hardware |
| The power-cycle count is written on time while the panel draws, on every target (§ 7.2) | ESP32 run D-5: a blocking refresh moved T_ARM to 5.66 s and the "wait 5 s" instruction failed |
| Measured: handshake about 90 ms per step on BG22; ESP32-S3 reset causes; the gesture window after the timing fix; the BG22 advertising pause of about 24 s and that BUSY is visible only on an open link (§ 7.2, § 17, § 19.1) | hardware runs |
| § 19 split: what the hardware runs answered (§ 19.1), what is still unverified (§ 19.2), and the open client-option questions (§ 19.3) | maintainer request |

### Revision 3 (2026-11-11)

Inputs: the maintainer's decision to adopt the five-power-cycle factory reset, the firmware
implementation (`v3/sec2`), and py-opendisplay's independent implementation and its twelve
reported ambiguities (each decided below, § 3.3, § 6, § 9.5, § 10.4).

| What changed | Why |
|---|---|
| **Factory reset by five quick power-cycles** is normative (§ 7.10), on every device: unclaimed, claimed and LOCKED. It runs the same erase as the reset pin, which stays. Appendix B.1 is reduced to its history | maintainer decision |
| The power-cycle count is one counter for both patterns. Claim mode no longer consumes it, so the reset passes through claim mode; claimed and LOCKED devices now maintain it (writes on their counted boots, § 7.2 wear) | needed by § 7.10; replaces "claimed devices write nothing" |
| What an attacker can do: physical access to the power supply is a factory reset, a DoS, never a data leak (§ 1.4 item 19, § 1.5) | follows from § 7.10 |
| After the claim, still in claim mode, the claiming session may write the config (0x0041/0x0042/0x0045), secrets included, whatever the session kind and `config_locked`. The § 6 CM column said FD; § 7.3 and § 13.1 step 9 said allow. Allow wins (§ 6 C/s note) | interop finding (py-opendisplay cross-harness) |
| Three receive details are pinned: a sealed-looking frame of a plaintext-set opcode is parsed as plaintext (§ 6 rule 1 no longer says FE); the third bad tag gets its sealed F3, then the teardown; a crypto-engine fault on open answers sealed `[FF][cmd][00]`, counts no strike and commits no counter (§ 9.5) | the specs disagreed or were silent |
| SEC_CLAIM BEGIN and SEC_AUTH HELLO answer **BUSY (0x0C)** at once while a display pass runs or is queued; the device's own blocking does not count against a peer's handshake timers (§ 17) | bounded handshake latency |
| A config write that does not walk exactly to its CRC (a dangling byte, an unknown legacy id that abandons the walk) is refused 0x00; on an unclaimed device a written 0x27 `reserved[0]` is stored as 0 (§ 10.2, § 10.4) | an open writer must not plant claim evidence (§ 10.1 row 3) |
| D->H fragment split: the LAST fragment always carries 1..`frag_max − 4` bytes (never an empty LAST after an exact multiple); `frag_max` counts the whole frame including its 4 header bytes (§ 3.3) | py-opendisplay ambiguities 1 and 3 |

**The twelve py-opendisplay ambiguities, decided** (numbering as reported by py-opendisplay):

| # | Question | Decision |
|---|---|---|
| 1 | LAST fragment after an exact multiple | LAST carries 1..`frag_max − 4` bytes, never empty (§ 3.3) |
| 2 | H->D split | the device accepts any split within § 3.3 |
| 3 | does `frag_max` include the header | yes, the whole frame (§ 3.3) |
| 4 | config write after the claim, still in claim mode | allowed (§ 6); the firmware was wrong and is fixed |
| 5 | SEC_KEY after the claim, still in claim mode | FD until the reboot (§ 6) |
| 6 | the third bad tag | sealed F3, then the teardown (§ 9.5 rule 5) |
| 7 | a sealed frame of a plaintext-set opcode | parsed as plaintext, never FE (§ 6 rule 1) |
| 8 | LEGACY_SECRETS write with fewer secrets than stored | accepted, per-instance identity (§ 10.4) |
| 9, 11 | (no spec question; client-internal) | no change |
| 10 | client strictness on unexpected replies | strict is conforming |
| 12 | 0x27 key field mismatch | the device answers 0x11; a client MAY raise before sending |

The read view (§ 10.2) carries the stored CRC unchanged; no CRC is computed over the read image.
py-opendisplay's vector V11 recomputes it and must change.

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
   writes and is not affected. An "extra-secure setup" client option (former proposal P4) was
   **dropped by maintainer decision** (revision 5); an owner who wants that property configures
   the display in normal mode and claims it by the gesture (§ 13.3).
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
19. **Physical access to the power supply is a factory reset** (§ 7.10). Whoever can switch a
    display's power five times in a row, or a supply that fails in exactly that rhythm, erases
    its key, WiFi password, FindMy keys, images and config, and leaves it FRESH and claimable by
    the first claimer nearby. This is a **denial of service, never a data leak**: the secrets are
    scrubbed first (FACTORY_RESET.md R1b), and nothing is read out. The owner notices (the RESET
    screen, a device that no longer answers to its key) and re-claims. An attacker who can only
    reach the radio cannot trigger it, except on nRF52 by inducing the brown-outs of item 18,
    five times with the right timing.
20. **The key on the screen is an owner-chosen exposure** (revision 5, maintainer decision E).
    With 0x27 `flags` bit 1 SHOW_KEY_ON_SCREEN set, a claimed device prints K_dev on its normal
    boot screen and puts it in the boot screen's QR, permanently, until the bit is cleared or the
    key is rotated. Anyone who can see the display, or a photo of it (including the QR), holds
    the key and with it everything of item 12: gated commands, DFU, the WiFi password and the
    FindMy keys. Only the owner can set the bit (a sealed config write on a claimed device,
    § 10.3); it is off by default and cleared by a factory reset and by UNCLAIM's scrub. It
    never appears on the SETUP, CODE or RESET screens (§ 7.8), so the claim's code check is
    unaffected. What this does not change: a radio-only attacker without sight of the panel gains
    nothing.

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
  setup, configure the display first, then switch it off and on twice and claim it (§ 13.3).
- **Physical access wins.** Someone holding the display can reset it, re-flash it or read its
  memory. **Switching the power off and on five times in a row resets the display to factory
  state** (§ 7.10). Anyone who can reach the power switch, the battery or the USB plug can do
  that: your settings and keys are erased (not revealed), and the display must be set up again.
  A badly failing power supply can do it too.
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
  and the LAST fragment carries the rest, which is **1 .. `frag_max − 4` bytes**: a body that is
  an exact multiple ends with a full LAST fragment, never an empty one. A message with an empty
  body (ABORT reply, EXIT reply) is one LAST fragment with no chunk. Vectors depend on this, and
  a client MAY reject any other split.
- `frag_max` is declared by the client in byte 1 of BEGIN or HELLO, in the range 20..244. It is
  the size of the **whole** fragment frame, the 4 header bytes included. Clients that do not know
  their MTU MUST send 20. A client sends its own fragments with the same deterministic split; the
  device accepts any split within the rules above.
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
| `[FF][cmd_lo][FE]` | frame too long (existing, `od_dispatch.c` `refuse_oversize`). **Sealed** when a session is live on this connection and the opcode is gated; plaintext when no session is live, and always for a plaintext-set opcode (§ 6 rule 1) | dispatcher |
| **`[FF][cmd_lo][FD]`** | **CLAIM_MODE: refused because the device is in claim mode** (new, provisional, `OD_GATE_CLAIM_MODE`) | dispatcher gate, plaintext on an unclaimed device and sealed after the claim |
| `[FF][cmd_lo][F1..F3]` inside a sealed D->H frame | RECORD / REPLAY / INTEGRITY (§ 9.5) | record layer |
| `[FF][cmd_lo][sub][err]` | SEC opcode error (§ 3.2) | SEC handlers |

**Oversize inside a session (revision 4).** A frame longer than the receive buffer is refused
before the record layer can open it. Inside a live session the refusal for a gated opcode is
sealed like any other answer, so a session never carries a plaintext reply to a sealed request.
A client MUST accept the oversize refusal in **both** forms: sealed (inside a live session) and
plaintext (no session, a plaintext-set opcode, or a firmware before revision 4). It reports
"frame too long" and does not count it as an integrity failure. Over BLE, GATT usually refuses
an over-MTU write first, so the refusal is rare in practice.

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
| **OS2S** state | `"OS2S"(4) \| ver 0x01 \| flags (1) \| short_boots (1) \| reserved (1) \| key_gen (2 LE) \| reserved (2) \| crc32 (4)` = 16 B. flags: bit0 FRESH, bit1 LEGACY_SECRETS (accepted at the first 3.0 boot), bit2 MIGRATED (first 3.0 boot done), bit3 PURGE (superseded secrets still in the raw medium, § 10.4a; revision 4, internal), others 0 | first 3.0 boot; the gesture counter (§ 7.2); claim-mode end (clears FRESH); key_gen on claim/rotation; set FRESH by factory reset |
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

Revision 5 (maintainer decision A, B, C). The 16-byte MSD has no free status bit:
- bit0 is battery bit 8;
- bit1 is REBOOT;
- bits 4-7 are the loop counter;
- `dynamic[11]` is config-owned.

So 3.0 **reuses** bits 2 and 3, whose old meanings are dead or agree with the new ones
(`advert_claim_mode` § B):

```
MSD status (byte 15 of the 16-byte MSD, i.e. after the company id 46 24 and dynamic[11]
and the temperature and battery bytes):
  bit2 CLAIM_MODE        1 continuously while in claim mode (any kind: FRESH, GESTURE,
                         CONTINUE, and after a factory reset), else 0.
                         (2.x name CONNECTION_REQUESTED; no shipped firmware ever set it.)
  bit3 SESSION_REQUIRED  1 iff the security state is CLAIMED or LOCKED, from the security
                         state, never from the 0x27 bytes at rest (§ 10.2).
                         (2.x name ENCRYPTION_ENABLED; same client meaning: "needs a key".)
  "waiting to be claimed" = bit2 == 1 && bit3 == 0.
```

- The signal is **advisory and unauthenticated**. A client lists candidates from it, then
  confirms after connecting with a plaintext READ_IDENT (tail flag bit 5 CLAIM_MODE, bit 4
  CLAIMED) or SEC_STATUS. Every security decision is taken inside the authenticated transcript.
- `dynamic[11]` is **not** a claim-mode sentinel. Claim mode does not change it (except through
  the unconfigured rule below). A sentinel was rejected: it would make an old Home Assistant
  decode phantom button presses on a configured device put into gesture claim mode.
- bit 2 and bit 3 = 1 together happen only after the claim, while still in claim mode (§ 7.3,
  "IN CLAIM MODE (device now claimed)"); clients treat it as "claimed".
- An OD_SEC2=0 build (§ 13.7) sends bit 2 = bit 3 = 0.

**Every target builds the status bits and the unconfigured rule through one shared function**
(`od_advert_apply_device_state()`, `shared/core/od_advert.c`), fed from `od_claim_mode_active()`,
`od_sec2_needs_session()` and the drivable-display predicate below. No target computes them.

#### 5.3.1 Bytes in every state (normative; host-tested, `tests/host/advert_state_test.c`)

`DD` = the config's dynamic bytes, `c` the loop-counter nibble (the status byte's high nibble), so
`c4` means `(c << 4) | 0x04`; battery bit 8 and REBOOT = 0 in these examples.

| State | bit3 | bit2 | `dynamic[11]` | status byte |
|---|---|---|---|---|
| unclaimed, normal mode, display drivable | 0 | 0 | DD | `c0` |
| unclaimed, normal mode, no drivable display (§ 5.3.2) | 0 | 0 | FF × 11 | `c0` |
| virgin device (FRESH claim mode, no config) | 0 | 1 | FF × 11 | `c4` |
| FRESH claim mode after the drive config was written (CONTINUE boot) | 0 | 1 | DD | `c4` |
| GESTURE claim mode | 0 | 1 | DD | `c4` |
| claim mode after a factory reset (FRESH, config erased) | 0 | 1 | FF × 11 | `c4` |
| claimed inside claim mode (between CONFIRM and the END reboot) | 1 | 1 | DD | `cC` |
| CLAIMED, normal mode | 1 | 0 | DD | `c8` |
| LOCKED | 1 | 0 | DD (or FF × 11 when no drivable display) | `c8` |
| CLAIMED by migration of a 2.x v1 key (origin LEGACY) | 1 | 0 | DD (FF × 11 until drive packets are written) | `c8` |

The REBOOT bit (bit 1) is independent of all rows (set after a reboot until the first connect).

#### 5.3.2 "No display configured" in 3.0 (the 0xFF rule)

Protocol 2.2: *`dynamic[11]` is all 0xFF when no display is configured.* 3.0 applies it on every
target and defines the condition as **no drivable display**:

> The config record is absent or not loaded, **or** it has no display packet (0x20), **or** no
> display packet of the config is drivable. A display is drivable when its instance has drive
> data (a 0xB1 panel packet and a mode-0 0xAE packet, `od_cfg_has_drive_data()`), or when the
> firmware drives it with a built-in controller driver selected by 0xB3 (ESP32 only, READ_IDENT
> BUILTIN_DRIVER).

Equivalently: READ_IDENT reports `display_count = 0`, or every display record would report
NEEDS_MIGRATION. The same predicate sets `dflags.HAS_PANEL` (§ 4.4) and decides whether the
SETUP screen can be drawn (§ 7.8), so a client never sees "drivable" in one place and not in
another.

Consequences:
- A **virgin** device (no config) is in FRESH claim mode and has no drivable display, so it
  advertises bit 2 = 1, bit 3 = 0 **and** FF × 11. Both signals apply and do not conflict: a
  client reads "waiting to be claimed, and needs a display configuration".
- A 2.x device upgraded to 3.0 that has no drive packets yet advertises FF × 11 until its config
  is migrated. Its button, touch and sensor slots are **not reported** in that state. Clients MUST
  treat FF × 11 as "unconfigured" and MUST NOT decode the slots (a 0xFF button byte would decode
  as button 7, 15 presses, pressed; SHT40 `FF FF FF` as a read failure).
- 0xFF is the whole-area rule only. A slot that holds 0xFF on a configured device keeps its
  slot meaning; a configured device whose eleven bytes all happen to be 0xFF is not possible with
  today's slot encodings (it would need every slot in use with identical 0xFF values).

#### 5.3.3 Advertising PDU and scan response, per target (31-byte legacy PDUs)

AD structure size = 1 (length) + 1 (type) + payload: flags 3 B, name `OD`+6 hex 10 B, MSD
2 + 16 = 18 B, UUID16 4 B, UUID128 18 B.

| Target | ADV PDU | used | Scan response |
|---|---|---|---|
| esp32-idf (NimBLE) | flags + name (complete) + MSD | 31 / 31 | UUID128 OpenDisplay service (18 B) |
| efr32bg22-slc | flags + MSD + name (complete) | 31 / 31 | UUID16 0x2446 (4 B) |
| nordic-zephyr: nRF54L15, nRF54LM20A, XIAO nRF52840 (**revision 5**) | flags + MSD + name (complete) | 3 + 18 + 10 = **31 / 31** | UUID16 0x2446 (4 B) [+ UUID16 Ranging 0x185B with Channel Sounding (4 B)] = 4-8 / 31 |
| Firmware 2.x ESP32 / nRF52840 (Bluefruit) | flags + name + MSD | 31 / 31 | none |
| legacy Firmware_NRF | flags + full name + MSD | 31 / 31 | UUID128 |

Revision 5 moved the Zephyr name from the scan response into the ADV PDU, so passive scanners,
passive ESPHome proxies and HA's `name.startswith("OD")` discovery check see it on every target.
The name is always exactly 8 characters (`"OD"` + 6 upper-case hex), so the PDU is exactly full;
a build-time assert in `targets/nordic-zephyr/src/opendisplay_ble.c` and the shared constants in
`od_advert.h` (`OD_ADVERT_AD_*`, host-tested) hold the arithmetic. Other advertising sets are
unaffected: Zephyr runs one legacy advertiser and no FindMy/FMDN set; BG22's FindMy "A" and FMDN
"G" sets (`od_hal_adv2.c`) are separate sets with their own payloads, and the ESP32 has no second
set. A second MSD element with the same company id would replace the first in bleak/HA's
per-company dictionary, so none is added anywhere.

Changing the local name was rejected as a signal: on ESP32, BG22 and the 2.x fleet any longer
name overflows the full PDU (ESP32 then drops the name, BG22 truncates it), and HA matches on the
`OD` prefix (`config_flow.py:82-87`).

Privacy: the signal reveals "being set up now" for at most the claim-mode duration (§ 1.4
item 9). It is spoofable; it only lists candidates, and the code protects the claim.

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
state the CM refusals still apply, and come back sealed, **except the config writes 0x0041,
0x0042 and 0x0045: the claiming session may write the config, secrets included, whatever the
session kind and `config_locked`** (§ 7.3, § 13.1 step 9). SEC_KEY stays FD until the reboot.

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
| 0x0044 | READ_MSD | open | open | open | **plaintext** | open | public by design: exactly the 16 advertised MSD bytes, nothing more (rule 6) |
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
| 0x0057 | SEC_KEY | NOT_CLAIMED (4-byte error) | **FD** (the gate runs before routing; revision 4 aligns the table with the firmware) | FE | sealed | FE | rotation / unclaim, BLE only |
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
  0x0044, 0x0046, 0x0050 refusal, 0x0054, 0x0055, 0x0056 refusal (NOT_CLAIMED)};
- every other opcode, including any added later, is FD.

Rules:

1. The **plaintext set** on a claimed device is exactly {0x0043, 0x0044, 0x0046, the 0x0050
   refusal, 0x0054, 0x0055 (refusal or EXIT), 0x0056}. These are always sent and answered in
   plaintext, even inside a session. A client MUST NOT seal them. The device parses a frame for
   one of these opcodes as plaintext whatever it looks like (§ 9.5 rule 1): a sealed-looking
   one is malformed plaintext and gets that opcode's own malformed answer, never FE and never an
   open. So there is no second encoding of the same command.
2. A device MUST NOT execute any part of a gated handler before the frame's tag has verified.
   That includes partial parsing with side effects, such as arming a flash PRELOAD.
3. A claimed device answers FE, never a NACK or silence, to a plaintext gated command, so that a
   client always learns "session required" in one shape.
4. In claim mode, the device's **own** content sources are off as well:
   - the action runner (0xB2 triggers: buttons, timers, BOOT);
   - scheduled image-store rotation;
   - the post-config-write boot-screen redraw (replaced by the setup screen, § 7.8).

   The only pixels drawn in claim mode are the firmware's setup screens.
5. **Repeated FE closes the link** (measured, revision 4). A run of consecutive FE answers on one
   BLE connection ends that connection:

   | Target | Run that closes | How |
   |---|---|---|
   | efr32bg22-slc | 3 | the third gated command is refused and the link closed at once (`OD_AUTH_ABUSE_LIMIT`); the third FE may not arrive |
   | esp32-idf | 10 | the tenth FE is sent, then the link drops within 500 ms (`OD_AUTH_ABUSE_THRESHOLD`, `OD_AUTH_ABUSE_FLUSH_MS`); on the bench the link went at the eleventh command |
   | nordic-zephyr | none | no auth-abuse drop |

   Any answered command (ACK or handler NACK) and a completed SEC_AUTH reset the run. Plaintext-set
   commands neither reset nor advance it; LAN traffic never counts. **What a client concludes:** a
   disconnect right after one or more FE answers means "this peer kept sending gated commands
   without a session". It is not a crash, not a lost key and not evidence about the device's
   state, so a client deletes nothing (§ 8.8). It reconnects and runs SEC_AUTH **before** any
   gated command. A client MUST NOT send more than two gated commands without a session.
6. **Advertised data is public** (maintainer decision, revision 4). Everything the device
   broadcasts in its advertisement can be read by anyone in range without connecting. Answering
   the same bytes over a connection without a session therefore discloses nothing, and lets a
   client or HA read a claimed device's sensor values without its key. So READ_MSD (0x0044), and
   any future opcode that returns **only** advertised data, is in the plaintext set. Such an
   opcode MUST NOT return anything beyond the advertisement. READ_MSD returns exactly the 16-byte
   MSD that is (or is about to be) on air, on every target:
   - company id (2 B);
   - `dynamic[11]`: config-owned sensor bytes (buttons, touch, SHT40, BQ27220 fuel gauge);
   - temperature (1 B);
   - battery (1 B, with bit 8 in the status byte);
   - status bits: REBOOT, CONNECTION_REQUESTED, ENCRYPTION_ENABLED (claimed or LOCKED), claim mode
     through CONNECTION_REQUESTED (§ 5.3), and the loop counter.

   On Nordic the reply is the staging copy, which may be one update newer than the copy on air.
   What this costs: a claimed device's sensor values and battery are public, which they already
   were over the air (§ 1.4).

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
- `OS2S.FRESH` is cleared when the FRESH session ends (§ 7.3), whatever the reason (Q15, decided). A power loss
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

k = OS2S.short_boots: the consecutive previous counted boots that lived 2-10 s

at boot, every state (UNCLAIMED, CLAIMED, LOCKED):
   if cause is counted:
       if k >= 4 and the pattern is on:      # five quick power-cycles (§ 7.10; off: § 7.10.1)
           factory reset
       if k >= 1 and UNCLAIMED and not CONTINUE / FRESH:
           enter claim mode, kind GESTURE    # the count is NOT consumed
       at uptime T_ARM:   write short_boots = k + 1
       at uptime T_CLEAR: write short_boots = 0
   else:
       if k != 0: write short_boots = 0      # any other reset breaks the sequence

before deep sleep / System OFF / EM4 / power-off / planned reboot, while armed:
   write short_boots = 0 first               # the on-period ends here
```

The gesture is unchanged for the user (k ≥ 1: two quick power-cycles). Not consuming it is what
lets the five-cycle reset pass through claim mode; a claim-mode boot with k ≥ 2 is therefore a
pattern in progress and draws its SETUP screen only after T_CLEAR (§ 7.10). The thresholds and
windows are compile-time constants (`OD_CM_T_ARM_MS`, `OD_CM_T_CLEAR_MS`,
`OD_CM_GESTURE_SHORT_BOOTS`, `OD_FR_PATTERN_SHORT_BOOTS` in `shared/core/od_claim_mode.h`).

- A POR boot of an unclaimed device stays awake at least until T_ARM. Today's boot (boot screen,
  advertising) already does.
- **The count is written on time, whatever the panel does** (revision 4). A blocking refresh at
  boot MUST NOT delay the T_ARM write: every blocking panel wait calls
  `od_claim_mode_tick_count()` at least every 100 ms (ESP32 the EPD busy-wait; BG22 `io_delay_ms`;
  Nordic the boot-display wait). On the ESP32-S3 Sticky the boot-screen refresh had moved the
  T_ARM write to 5.66 s, so a 4.25 s on-period failed. With the tick, T_ARM lands at 2.9 s
  (about 0.9 s of display init does not pass through the busy-wait) and a 4.25 s on-period opens
  claim mode. **Measured effective window there: an on-period of about 2.9 s to 10 s.** BG22 and
  Nordic have the tick and a host test (a 5.6 s refresh still arms at 2.0 s), not a hardware
  measurement.
- **Measured reset causes, ESP32-S3 (reTerminal Sticky, CH343 USB-serial):** the EN pin (which
  the USB-serial RTS line drives, and the reset button) reports `ESP_RST_POWERON` and counts; so
  does esptool's default reset (bench only). REBOOT, claim-mode END and esptool's hard reset
  report `ESP_RST_SW` and do not count, and clear k. Watchdog, panic and deep-sleep wake causes
  were not exercised (§ 19.2).
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

The worst case of the two-cycle pattern is therefore 10 minutes of no uploads and one lost
picture on an open device. The five-cycle pattern's false trigger is far worse (§ 7.10).

**Wear** (every counted boot, on every device, since § 7.10; a boot that lives past T_CLEAR
writes twice, one cut between T_ARM and T_CLEAR once, one cut before T_ARM not at all; uncounted
boots write only to clear a non-zero count):

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
  advertising: boost interval for the first 60 s of this ENTER, then the normal interval

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

**The advertising boost (revision 5, decision D).** It uses the shared boost seam
`od_adv_app_boost()` (`shared/core/od_adv_app.h`), the same 20-30 ms window a button press opens
for 3 s on every target. `od_claim_mode_tick()` re-arms it every `OD_CM_BOOST_REARM_MS` (1 s) while
all of these hold:
- claim mode is active;
- less than `OD_CM_BOOST_MS` (60 s) has passed since this boot's ENTER (FRESH, GESTURE or CONTINUE);
- no application connection is open (`od_claim_app_connected()`; a connection stops the
  advertiser anyway, and the boost must not outlive it).

After the last re-arm the window closes within 3 s and the target's own reconcile restores its
idle interval (ESP32 160-1000 ms, Nordic and BG22 1000 ms). Bound: one 60 s window per ENTER,
and ENTERs are capped by the restart cap below, so at most 4 × 60 s = 240 s of fast advertising per
claim-mode session, plus a 3 s tail each. A disconnect inside the 60 s re-arms it for the rest of
the 60 s; nothing extends the window past 60 s from ENTER. Host-tested in `claim_mode_test.c`.

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

Decision: **ignored** for claiming. Nothing is drawn and the LED does nothing. The counter is
maintained (two small writes per counted boot), because the same count is the five-cycle factory
reset (§ 7.10), which works on claimed and LOCKED devices.

- A claimed device must not open claiming. That is the point of the claim.
- An info screen ("this display is claimed") would let flaky power wipe the owner's picture on
  exactly the devices that matter most. It would also need the counter writes on every claimed
  device's counted boots.
- The ways to change ownership are SEC_KEY UNCLAIM (owner, remote; then the gesture on the now
  unclaimed device) and the factory reset (no key).
- Clients explain this when a claimed device is selected: *"This display is claimed. Switching
  it off and on twice does nothing. To hand it over, release it from the app that owns it, or
  factory-reset it (switch it off and on five times, waiting about 5 seconds each time)."*

### 7.7 Signalling

| Channel | In claim mode | Notes |
|---|---|---|
| MSD status bit 2 CLAIM_MODE | 1, continuously | with bit 3 SESSION_REQUIRED = 0 while unclaimed (§ 5.3) |
| advertising interval | 20-30 ms for the first 60 s of each ENTER while not connected, then the normal interval | § 7.3 |
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
  QR: setup URL (k=1)
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
  QR: setup URL (k=2)
```

When text and QR do not both fit, the QR wins on SETUP and RESET, and the code wins on CODE.
E-paper keeps each screen until the next refresh. After a factory reset there is no config, so
the RESET screen stays visible during the fresh claim mode that follows and does the job of the
SETUP screen. That is why both carry the same URL and differ only in `k` (§ 7.8.1).

**The key is never on these screens.** SETUP, CODE and RESET never show K_dev, whatever 0x27
`flags` bit 1 says (§ 10.7): they are drawn while unclaimed or during a reset, and their only
variable inputs are the ones listed above.

#### 7.8.1 The setup/recovery URL (revision 5, normative; decision F)

The QR on the SETUP and RESET screens is **frozen forever** in every device that is never
updated. So its grammar is designed for longevity:

- **One entry point** for every firmware-generated QR code: `https://opendisplay.org/l/`. The
  path names no behaviour; the kind is a field.
- An explicit **format version** `f` is the first field. It is the URL format's version, not the
  firmware's.
- **Append-only, named fields**, and a page ignores fields it does not know.
- **Numbers only**: no config-supplied text ever enters the URL (review A1).
- Short: worst case 83 bytes, QR version 6, ECC level M.

**Why `/l/` and not a new path.** `/l/` is already a frozen contract: every 2.x and 3.0 normal boot
screen links to `https://opendisplay.org/l/?<payload>`, where `<payload>` is the base64url (no
padding) of a 23-byte record (`od_boot_payload_build()`: tag type 2 B BE, device id 3 B, key
16 B, manufacturer id 2 B BE; 31 characters). The website must serve `/l/` for those devices for
as long as they exist, so a second path would be a second contract to keep alive forever for the
same job ("a firmware QR was scanned; identify the device and start the right flow"). The two
grammars cannot be confused: a legacy query is one 31-character base64url token with no `=`;
the setup grammar's first field is `f=<digits>`. `/s/` (revision 2) never shipped on any device
and is withdrawn. The normal boot screen keeps the legacy payload byte for byte (it is what 2.x
showed, and it carries the key when SHOW_KEY_ON_SCREEN is set, § 10.7, which the setup grammar
never does).

**Grammar (format 1)**

```
url      = "https://opendisplay.org/l/?" "f=1" "&k=" kind "&i=" id "&t=" target "&v=" fw
           [ "&b=" index ] [ "&d=" index ] [ "&p=" index ]
kind     = "1"        ; SETUP screen (claim mode)
         / "2"        ; RESET screen (factory reset; the fresh claim mode follows)
id       = 6 HEXUP    ; od_boot_app_device_id24(): the suffix of the advertised name "OD1A2B3C"
target   = 1*2DIGIT   ; READ_IDENT fw_target, 1..99: 1 ESP32, 2 NRF54, 3 SILABS, 4 NRF52811,
                      ;   5 NRF52840; the field is omitted for a value outside 1..99
fw       = dec "." dec "." dec        ; firmware major.minor.patch, each 0..255
index    = 1*4DIGIT   ; 1..9999, decimal, no leading zero
b        = ManufacturerData.simple_config_driver_index   (no 0x02 packet, 0, or > 9999: omitted)
d        = ManufacturerData.simple_config_display_index  (0 or > 9999: omitted)
p        = ManufacturerData.simple_config_power_index    (0 or > 9999: omitted)
dec      = "0" / %x31-39 *2DIGIT ; no leading zeros
HEXUP    = DIGIT / %x41-46
```

The firmware emits the fields in exactly this order, so the string is deterministic. Builder:
`od_setup_url_build()` (`shared/core/od_boot_payload.c`), host vectors in
`tests/host/boot_payload_test.c`.

**Byte budget.** Prefix 27 + `f=1` 3 + `&k=2` 4 + `&i=FFFFFF` 9 + `&t=99` 5 + `&v=255.255.255` 14
+ 3 × `&b=9999` 21 = **83 bytes**, the revision 4 bound. QR version 6 at ECC M holds 106 bytes in
byte mode; the renderer hard-codes version 6 / ECC M. Version 6 + a 4-module quiet zone is 49
modules: 4 px/module on a 200 × 200 1.54in panel (196 px), 3 px on 152 × 152 (147 px), 2 px on a
128-px-wide 2.9in (98 px). The smallest supported panel (1.54in, 152 × 152 or 200 × 200) is
covered with 23 bytes of capacity to spare for appended fields.

**Test vectors** (also in `tests/host/boot_payload_test.c`):

| # | Inputs | URL | Bytes |
|---|---|---|---|
| U1 | SETUP, id 0x1A2B3C, t 3, 3.0.0, b 16, d 10, p 2 | `https://opendisplay.org/l/?f=1&k=1&i=1A2B3C&t=3&v=3.0.0&b=16&d=10&p=2` | 69 |
| U2 | RESET, id 0x00ABCD, t 1, 3.0.0, no 0x02 packet | `https://opendisplay.org/l/?f=1&k=2&i=00ABCD&t=1&v=3.0.0` | 55 |
| U3 | RESET, id 0xFFFFFF, t 99, 255.255.255, b = d = p = 9999 (worst case) | `https://opendisplay.org/l/?f=1&k=2&i=FFFFFF&t=99&v=255.255.255&b=9999&d=9999&p=9999` | 83 |
| U4 | SETUP, id 0x000001, t 5, 3.1.12, b 10000, d 0, p 7 | `https://opendisplay.org/l/?f=1&k=1&i=000001&t=5&v=3.1.12&p=7` | 60 |

**Evolution rules** (normative for firmware and website):
1. `f`, `k` and `i` are the **core**: their names, position (`f` first) and meaning are the same
   in every format version. A new kind gets the next number; a number is never reused.
2. A new optional field is **appended** after the existing ones with a new letter. A letter is
   never reused or redefined. Appending does not change `f`.
3. `f` is incremented only for a change an old page would misread (a changed meaning of a
   non-core field). Firmware never emits a URL above 83 bytes worst case without a QR version
   change that every supported panel can draw.

**Rules for the `/l/` page** (website; review § 5 phishing advice):
1. **Dispatch.** The query's first field is `f=<1-3 digits>` → setup grammar. Otherwise the
   legacy device link: today's 23-byte payload decoding, unchanged.
2. **Known format (`f=1`).** Parse every field strictly: `k` ∈ {1, 2}; `i` exactly 6 hex digits;
   `t`, `b`, `d`, `p` decimal in range; `v` three decimal integers 0..255. An optional field that
   fails its check is ignored (as if absent); a bad `i` or `k` → rule 3's generic flow. Ignore
   unknown fields; when a field repeats, the first occurrence wins. Render values as text, never
   as HTML.
3. **Unknown format (`f` ≥ 2, or not 1-3 digits).** Use only the core: `k` (an unknown kind is
   treated as 1) and `i` (if it is 6 hex digits). Preselect nothing. Say: *"This display shows a
   newer setup code than this page knows. You can still set it up."* and start the generic setup
   wizard, filtered to the name `OD<i>` when `i` is valid.
4. **Never ask for a secret on this page.** WiFi passwords and FindMy keys are entered only in
   the wizard, after a successful claim, inside the session (§ 13.1).
5. Show the board, display and power option that will be preselected (`b`, `d`, `p`), by name,
   and ask the user to confirm them before anything is written. A crafted link on the genuine
   domain could otherwise preselect a wrong drive recipe.
6. Say plainly that only `https://opendisplay.org/l/` is genuine, and that a display in claim
   mode or factory reset can be redrawn by anyone nearby *before* it enters that state. Claim
   mode itself cannot be redrawn.
7. No analytics or third-party requests that would leak the id. The page works from the query
   string alone.
8. `k=2` (RESET): say *"This display was reset to factory settings. Its settings and key were
   erased."* and run the same wizard as `k=1` (§ 15, recovery is setup).
9. For `v` < 3: explain that 2.x firmware has no setup screen and link to the normal toolbox.

### 7.9 Battery

Claim mode keeps the device awake and advertising:
- 10 minutes per ENTER, at most 40;
- at most 1 SETUP + 3 CODE refreshes, plus one SETUP per restart, plus the normal boot screen at
  the end.

Estimates, **not measured** (§ 19 H12):
- ESP32 with BLE advertising at roughly 20-40 mA: about 3-7 mAh per 10 minutes. The CPU stays
  awake in claim mode anyway, so the boost adds little;
- nRF52840 / BG22: the boost advertises every 20-30 ms instead of every 1000 ms for 60 s. One
  legacy advertising event on three channels costs roughly 10-20 µC, so the boost averages about
  0.4-1 mA over its 60 s, i.e. **at most about 0.02 mAh per ENTER and 0.07 mAh per claim-mode
  session** (4 ENTERs). The rest of the 10 minutes at 1000 ms is well under 0.1 mAh. A
  CR2032 (≈ 220 mAh) loses well under 0.1 % per claim-mode session.

A false trigger costs one such session. Claim mode never starts WiFi.

---

### 7.10 Factory reset by five quick power-cycles (normative since revision 3)

**Trigger.** A counted boot (§ 7.2) with `short_boots ≥ 4`: the fifth power-on after four
on-periods of 2-10 s each, with no other reset in between. It runs at boot step 3 of
FACTORY_RESET.md § 3.1, before the boot decision, on **every** device: unclaimed, claimed and
LOCKED. LOCKED is the case it exists for on boards without a reset pin. The reset pin (0x27)
stays; both triggers run the **same** erase (FACTORY_RESET.md § 5, R1-R8), ending in FRESH claim
mode.

**Interaction with claim mode.** The gesture's count is not consumed (§ 7.2), so cycles 2-4 of
the pattern may boot an unclaimed device into claim mode; they still count. A claim-mode boot
with k ≥ 2 defers its SETUP screen until T_CLEAR, and every counted boot with k ≥ 1 defers the
normal boot-screen refresh until T_CLEAR (`od_claim_mode_defer_boot_screen()`), so the pattern
costs no e-paper refresh per quick boot. The feedback while counting is the first LED: k + 1
short blinks at T_ARM (`OD_CLAIM_SIG_PATTERN_COUNT`); the RESET screen is drawn once, before the
erase (R3).

**Instruction** (every client and the website): *"Switch the display off and on five times. Each
time, wait about 5 seconds before switching it off, and keep it off for at least 2 seconds. It
then shows FACTORY RESET and starts fresh."*

**False triggers from flaky power (honest analysis).** A false trigger needs five power-ons in
a row whose first four on-periods each last 2-10 s, with no brown-out, watchdog, software or
wake reset in between, and no on-period longer than 10 s.
- A supply that bounces in under 2 s never arms the count and costs no writes.
- A supply that browns out without losing power entirely is a BROWNOUT reset on esp32-idf and
  efr32bg22-slc, which clears the count.
- **The residual risk is a supply that collapses below power-on reset and recovers, in a 2-10 s
  rhythm, five times**: a supercapacitor or solar board at its threshold that starts a refresh
  and dies, a battery that sags below POR under the refresh current, a hub that power-cycles a
  port. On nRF52 every brown-out also looks like a power-on (§ 7.2), so a brown-out loop counts
  there. Such a device is erased and left FRESH: a DoS and, until its owner re-claims it, a
  device anyone nearby can claim (first-claimer-wins, § 8.5). Its secrets are scrubbed before
  the key is erased, so nothing leaks.
- **Chosen mitigation:** the 2-10 s window, consecutive counted boots only, every uncounted reset
  and every long boot clearing the count, and a per-build switch: `OD_FR_PATTERN_SHORT_BOOTS 0`
  disables the pattern for a board profile (energy-harvesting boards SHOULD set it and rely on a
  reset pin). Not adopted, as the maintainer asked for five cycles: a longer pattern, a battery
  voltage gate, a confirmation power-on. See § 19 for the nRF52 brown-out canary that would
  narrow the nRF52 case.

**Wear.** At most two 16-byte OS2S writes per counted boot on every device (§ 7.2 table). A
display that is switched on twice a day writes about 1 500 small records a year.

#### 7.10.1 Switching the pattern off per device (revision 4, maintainer decision)

The pattern is **on by default on every target.** A config switches it off with 0x27
SecurityConfig `flags` **bit 6 `NO_RESET_PATTERN`** (provisional: bits 0-5 are taken; bit 0
REWRITE_ALLOWED is not reused; `OD_SECURITY_FLAG_NO_RESET_PATTERN` in
`shared/core/od_sec2_wire.h`, PROTOCOL_CHANGES_3.0 row 20).

| Bit | Device state | Effect |
|---|---|---|
| clear (default; every existing config) | any | the pattern resets, as above |
| set | CLAIMED | no reset by the pattern; the count is not kept (no OS2S writes per boot). The reset pin still works |
| set | UNCLAIMED | no reset by the pattern; the two-cycle claim gesture still counts, and k ≥ 4 stays a claim-mode boot |
| set | LOCKED | **ignored**: the pattern resets. The stored config may be what locked the device, and the pattern is its only way out without a reset pin |

- The bit is read from the 0x27 that was loaded at boot, like `reset_pin`. It is written like
  any other 0x27 flag: on a claimed device only through a sealed config write, so only the owner
  can change it; on an unclaimed device by anyone in range.
- `OD_FR_PATTERN_SHORT_BOOTS 0` stays the build-level override: it removes the pattern from a
  whole build whatever the bit says.
- The two-cycle claim gesture is unaffected.

**Security consequences, both ways.**
- **Off, for flaky power.** An owner whose supply produces the § 7.10 rhythm (energy harvesting,
  a sagging battery, a port that power-cycles) switches the pattern off and no longer risks an
  erase.
- **Off, on an unclaimed device.** Anyone in range can set the bit, as they can write any
  config there. That is acceptable: they could equally claim the device. The bit gives them
  nothing beyond that, and a later owner sees and can change it.
- **Off, claimed, and no reset pin: no lost-key recovery.** Such a device has no firmware path
  back if its key is lost; the only recovery is a wired reflash with a full erase (FACTORY_RESET.md
  § 7.3). **Clients MUST warn before writing a config that leaves a claimed device with the bit
  set and no enabled reset pin**, and require an explicit confirmation. The stored reset pin
  counts, because it is sticky (§ 10.6). Follow-up for the website, py-opendisplay and HA;
  py-opendisplay `v3/py` 801047e already refuses it without `allow_no_recovery`.
- **On (the default).** Physical access to the power supply is a factory reset: a DoS, never a
  data leak (§ 1.4 item 19).

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
| a display pass or flash erase holds the device | BUSY (counts in the rate limiter, revision 4) |

**Rate limiter (A18.6), per device, RAM only, reset at boot:**
- The device keeps a ring of the uptimes of the last 10 *counted events*.
- Counted events: every complete, well-formed BEGIN and HELLO (counted before the ECC work),
  **every BEGIN and HELLO answered BUSY** (revision 4), every AUTH_FAILED, and every failed
  CONFIRM tag.
- A new BEGIN or HELLO is refused with RATE_LIMITED if the ring holds 10 entries and the oldest
  is less than 60 000 ms old. RATE_LIMITED refusals are not counted. A full ring answers
  RATE_LIMITED before BUSY.
- Why BUSY counts: otherwise a peer can repeat BEGIN/HELLO during every refresh without limit.
  A well-behaved client retries a BUSY after the pass (a few seconds), so the ten-per-minute
  budget is not reached in normal use.

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
   strike + 1. At 3 strikes the session is torn down (§ 9.6): **the third failure's F3 is still
   sealed and sent under the session, and the teardown follows it**; the next frame gets FE. The
   plaintext of a failed frame is never read.
5b. **A crypto-engine fault** (the HAL returns an error that is not a tag mismatch, or the
   plaintext does not fit): the sealed `[FF][cmd_lo][0x00]`. It counts no strike, and the counter
   is not committed, so the client may retry the identical frame. A no-ack frame stays silent.
   The crypto HAL contract (`shared/hal/od_hal_crypto.h`): the plaintext and ciphertext buffers
   are **never NULL**, also for an empty plaintext. A NULL buffer is refused before the backend
   on every target (BG22 PSA returned an error for a tag-only CCM decrypt with NULL on hardware).
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
| UNCLAIMED | 0 | 16 x 0x00 | 0x00 (a written non-zero value is stored as 0x00) | as written |

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

`flags` bit 0 (REWRITE_ALLOWED) is **reserved**: the device stores it as sent, returns it as sent,
and gives it no effect. Clients write 0.

`flags` bit 1 **SHOW_KEY_ON_SCREEN** is honoured again (revision 5, decision E; § 10.7):

| Device | A written bit 1 | Stored | Effect |
|---|---|---|---|
| CLAIMED (the write is sealed, so it comes from a session) | 0 or 1 | as sent | 1: the normal boot screen shows K_dev (§ 10.7) |
| CLAIMED, inside claim mode after the claim (the claiming session) | 0 or 1 | as sent | takes effect at the END reboot's normal boot screen |
| UNCLAIMED (open write; also FRESH claim mode before the claim) | 1 | **0** | none: there is no key to show, and an open writer must not pre-arm a key display for the device's future owner |
| a 0x27 that a claimed write omits (inserted at rest, above) | — | the stored value | unchanged |

- The bit is part of 0x27 and is changed only by a config write. It is **not** sticky like the
  reset pin (§ 10.6): CONFIG_CLEAR on a claimed device keeps the stored 0x27, and with it the bit.
- The default is 0. Factory-embedded configs must not set it (a set bit is stored as 0 on the
  unclaimed device they configure).
- A 2.x device migrated with its v1 key (§ 10.5 b) keeps a stored bit 1 = 1 and goes on showing
  its key, now as K_dev (which equals the v1 key until the first rotation) — as on 2.x.
- The read view returns the stored bit. The Nordic
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
the write is refused with `OD_ERR_CONFIG_WRITE_FAILED` (0x00) on every device. That includes a
dangling byte between the last packet and the CRC, and an unknown legacy id, which abandons the
rest of the walk. A packet type the loader skips by its length is skipped identically by the
scan. The host wire corpus must contain
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
secret. Per packet instance: every 0x26 / 0x2D instance in the write that carries a secret must
equal (in its secret fields) one stored instance; a write with fewer of them is accepted. This is what lets a client read the config, add drive packets and write it back without
the gesture. Different or additional secret bytes still get 0x10. A write without the secrets
removes them, and the LEGACY_SECRETS flag is cleared at the next boot (§ 10.1 row 8).

**On a claimed device** secrets are stored normally (the write is sealed).

**Scrub procedure** (used by SEC_KEY UNCLAIM and factory-reset step R1b):
1. One **atomic** config rewrite that:
   - zeroes every 0x26 `password`;
   - zeroes every 0x2D `eik`, `net_a_adv_key` and `advertisement_key`;
   - sets 0x27 to the UNCLAIMED form (`encryption_enabled` 0, key 0, `reserved[0]` 0, `flags`
     bit 1 SHOW_KEY_ON_SCREEN 0), keeping `session_timeout_seconds`, the other `flags` and
     `reset_pin`;
   - keeps everything else.
2. esp32-idf: erase the ESP-IDF WiFi namespace `nvs.net80211` (A16).
3. efr32bg22-slc: delete any FMDN runtime NVM3 objects kept outside the config, if any exist.

**ESP32 WiFi copy (A16).** 3.0 calls `esp_wifi_set_storage(WIFI_STORAGE_RAM)` before any
`esp_wifi_set_config` (`src/wifi_service.cpp`), so ESP-IDF keeps no second copy of the password.
The first 3.0 boot erases `nvs.net80211` once, because 2.x left a copy there with
`CONFIG_ESP_WIFI_NVS_ENABLED=y`. Every scrub and factory reset erases it again.

### 10.4a Superseded secrets in the raw storage (revision 4)

Every target's storage is log-structured: ESP-IDF NVS, Silabs NVM3 and Zephyr NVS append a new
copy and only mark the old one deleted. An old copy stays readable in the raw flash until its
page is reclaimed. On the ESP32-S3 run, after UNCLAIM the raw NVS still held 6 old WiFi-password
copies and the last K_dev; before the R5 fix, a factory reset left 3 password copies and the key.
The scrub (§ 10.4) and the key erase are logical only. So they are followed by a **purge**:

1. SEC_KEY UNCLAIM and factory-reset step R6 set OS2S flag bit 3 **PURGE** in the same write that
   ends them.
2. The next boot, in `od_security_boot()` before the radio starts, sees PURGE and calls
   `od_claim_app_storage_purge()`. Only when that succeeds does it clear PURGE. A purge that fails
   or is cut by a power loss is retried at the following boot.

| Target | Purge | Power cut in between |
|---|---|---|
| esp32-idf | read every live entry of the NVS partition into RAM (dropping `nvs.net80211`), erase the partition, write them back. The factory reset already erases the whole partition at R5 | the device boots with whatever was written back. At worst it is blank and unclaimed, which UNCLAIM and the reset already made it |
| efr32bg22-slc | append a throw-away 64 B NVM3 object until NVM3 has repacked and erased every page once (`fifoFirstIdx` advanced by the page count), then delete it. Live objects (config, OS2S, bonds) survive | NVM3's own power-safe repack; PURGE is still set, so the next boot continues |
| nordic-zephyr | rewrite a throw-away settings key with changing bytes until the NVS write sector has moved once per sector; each move garbage-collects a sector, copying only live entries | NVS's own power-safe GC; retried at the next boot |

UNCLAIM keeps the config, so erasing the whole store there (the factory reset's choice on ESP32)
is not an option on BG22 and Nordic. Residuals:
- **Not verified on hardware.** No raw-flash census has been taken after a purge on any target
  (§ 19.2).
- On Nordic, a store whose live entries nearly fill a sector cannot complete the purge. It then
  retries at every boot, costing a few writes and a GC each time, until the config shrinks.
- Between UNCLAIM and the next boot, the old secrets are still in the raw medium.
- Flash encryption is not used on any target. A device with a flash read-out port open is read
  by anyone who holds it; the purge only removes what is no longer current.

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
- **The panel shows the key only when the owner asked for it** (revision 5, decision E,
  reversing revisions 3-4): 0x27 `flags` bit 1 SHOW_KEY_ON_SCREEN set **and** the security state
  CLAIMED. Then, and only on the NORMAL boot screen, permanently while the bit is set:
  - the key lines show K_dev exactly as 2.x did: on the zone layout `KEY1: <hex of bytes 0-7>`
    and `KEY2: <hex of bytes 8-15>` (upper-case hex); on the small layout two lines of 16 hex
    digits;
  - the `/l/` QR payload bytes 5..20 carry K_dev (2.x byte layout, § 7.8.1), so the website's
    device link opens with the key.

  The key shown is **K_dev**, from the OS2K key record (`od_sec2_k_dev()`), never the 0x27 key
  field at rest (which holds K_v1, § 10.2).

  | State | bit 1 | Key lines (zone / small) | QR key bytes |
  |---|---|---|---|
  | UNCLAIMED (incl. LEGACY_SECRETS) | any | `KEY: not set` / `----…` | zero |
  | CLAIMED | 0 (default) | `KEY: hidden` / `XXXX…` | zero |
  | CLAIMED | 1 | `KEY1: …` `KEY2: …` / two hex lines | K_dev |
  | LOCKED | any | `KEY: hidden` / `XXXX…` (the key is unreadable) | zero |

- **Never on the setup screen family.** SETUP, CODE and RESET never show a key (§ 7.8): they
  belong to unclaimed or resetting devices, and their "no attacker-writable text next to the
  code" property is unchanged.
- **Rotation redraws the boot screen** when bit 1 is set: after ROTATE_CONFIRM persisted the new
  key (§ 11), the device redraws the NORMAL boot screen with the new K_dev, replacing whatever the
  panel shows (an uploaded image included), so the panel never shows a dead key. The redraw runs
  from the loop after the reply has left and the session is torn down (`od_sec2_app_key_screen_changed()`);
  a client re-authenticating meanwhile may get BUSY (§ 8.7) and retries.
- **Changing bit 1 by a config write** on a claimed device redraws the boot screen the same way,
  once the write is stored: setting it shows the key, clearing it removes the key from the panel
  at once (not only at the next boot). A write that leaves the bit as it was redraws nothing
  beyond what the target already does after a config write.
- **Cleared** by a factory reset (the config is erased) and by UNCLAIM's scrub (the device is
  unclaimed, so a later read shows the bit cleared, § 10.4 scrub step 1).
- **Client follow-up (normative for every client that offers the option):** before enabling it,
  warn *"Anyone who can see the display, or a photo of it, has the key: they can change the
  display, update its firmware, and read your WiFi password and FindMy keys."* and require an
  explicit confirmation; show the option as off by default; offer "rotate the key" after turning it
  off (a photo taken meanwhile stays valid until a rotation). Threat model: § 1.4 item 20.
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
| 0x03 UNCLAIM | `[0xA5][0x5A]` (a guard against a stray frame) | (1) scrub (§ 10.4), one atomic config rewrite, else STORAGE_ERROR (still claimed); (2) erase OS2K, else STORAGE_ERROR (still claimed, no secrets; row 2 repairs the marker at boot); (3) `OS2S.key_gen = gen + 1`, a tombstone, so that an erased OS2K which a backend brings back is caught by § 10.1 row 1 (a failure here is harmless), with OS2S PURGE set, so the next boot purges the raw medium (§ 10.4a) | empty success; then teardown. The device is UNCLAIMED in **normal mode**, open. Claiming again needs the gesture |

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
- **Rotation leaves the old key in the raw medium** (revision 4, analysed, not fixed). After
  ROTATE_CONFIRM, the superseded OS2K copy and the old K_v1 in the previous 0x27 stay readable in
  raw flash until the storage reclaims their page, on all three targets. Rotation does not purge:
  - Whoever can read the flash already reads the **current** key, so the old one gives them no
    access they do not have.
  - The only extra exposure is retrospective: with the old key, recorded traffic from before the
    rotation can be decrypted, but only v1 (2.x) traffic, which had no forward secrecy. SEC2
    sessions use fresh ECDH, so the old K_dev does not open recorded SEC2 sessions.
  - On ESP32 a purge means erasing and rewriting the whole NVS partition with the claimed config.
    A power cut in between would leave a claimed device without its config. That costs more than
    the residual above.
  A hand-over to a new owner should therefore use UNCLAIM (purged) or a factory reset, not a
  rotation.

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
    device's drive config. This can defeat the code (§ 1.4 item 5). There is no client option
    against it (P4 was dropped, § 15.4); configuring in normal mode and claiming by the gesture
    avoids it (§ 13.3).
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
| `OD_SEC2=0` (a third-party product without the module) | READ_IDENT SEC_V2 = 0. 0x0054-0x0057 are unknown opcodes. 0x0050 answers NOT_CONFIG. No claim mode, no gesture counter, no OS2* records. MSD bit 3 = 0. Panel writes are always allowed, and FD is never sent. **Secrets: refused with 0x10** (the rule stays universal, so "secrets accepted" always means "protected"). No build flag allows storing them (Q14, decided) |

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
| Factory reset by five quick power-cycles | **normative since revision 3** (§ 7.10), on every device; the reset pin stays |
| Factory reset's end | FRESH claim mode. Its RESET screen doubles as the SETUP screen (§ 7.8) |
| DFU | as today on claimed and open devices; refused in claim mode |
| Old clients | § 13.6 |
| Core devices | § 13.7 |
| Advertising, privacy | MSD bit 2 during claim mode only (§ 5.3) |
| Battery | § 7.9 |
| Devices without a panel | warning path, refused when the client knows of a display (§ 8.6) |
| Website code check | typed, like HA and py |

---

## 15. The setup flow (normative where it concerns the device; revision 5, decision G)

The maintainer accepted proposals P1, P2, P6 and P7 and dropped P4, P5 and P8 (§ 15.4). This section states what the **device** guarantees so that every client can start
and drive one flow. The client screens themselves are client design; the shared wording of P1
lives with the clients.

### 15.1 One wizard (P1): the device-side triggers

All clients (website, Home Assistant, py-opendisplay) run the same steps — Identify → Configure
display → Verify panel → Claim → Secrets → Done (§ 13.1) — and enter them from whatever the
device signals. The device MUST present exactly these signals in each state:

| Entry | What the device shows / sends | Where a client learns it | Wizard entry point |
|---|---|---|---|
| **Just flashed or new** (virgin) | FRESH claim mode; MSD bit 2 = 1, bit 3 = 0, `dynamic` = FF × 11 (§ 5.3.1); nothing on the panel (no config); READ_IDENT `display_count` 0, flags CLAIM_MODE; SEC_STATUS FRESH, `window_s` | advertisement, without connecting | Configure display (step 3 of § 13.1) |
| **Claim mode advertised** (FRESH after config, or GESTURE) | MSD bit 2 = 1, bit 3 = 0, `dynamic` = DD; SETUP screen with the QR (k=1) when a panel is drivable; LED/buzzer pattern otherwise (§ 7.7); 60 s fast advertising per ENTER | advertisement; SEC_STATUS `config_locked` tells FRESH-before-code from GESTURE | FRESH: Verify panel; GESTURE: Claim |
| **QR scanned** (SETUP or RESET screen) | the § 7.8.1 URL: `f`, `k`, `i`, `t`, `v`, preset `b`/`d`/`p` | the `/l/` page | website wizard, filtered to `OD<i>`, preset preselected for confirmation |
| **Open device in normal mode** (unclaimed) | MSD bit 2 = 0, bit 3 = 0; READ_IDENT CLAIMED = 0, NEEDS_MIGRATION as applicable; SEC_STATUS state UNCLAIMED (+ LEGACY_SECRETS) | connect + plaintext READ_IDENT / SEC_STATUS | "Use without a key" or "Claim" (gesture instruction, § 13.3); NEEDS_MIGRATION → migration assistant first (§ 15.3) |
| **Claimed device** | MSD bit 3 = 1; READ_IDENT CLAIMED; SEC_STATUS CLAIMED (+ ROTATION_SUGGESTED when KEY_LEGACY) | advertisement; READ_IDENT | no wizard; normal session; legacy keys rotate by default (§ 10.5) |
| **LOCKED** | MSD bit 3 = 1; SEC_STATUS state LOCKED | SEC_STATUS | recovery: factory reset (§ 15.2) |
| **After a factory reset** | RESET screen with the QR (k=2) drawn before the erase, then FRESH claim mode: MSD bit 2 = 1, bit 3 = 0, FF × 11 | panel; advertisement | the same wizard as "just flashed" (§ 15.2) |

Devices never send a state the table does not list. A client that sees an unlisted combination
treats the advertisement as unknown and connects to read SEC_STATUS.

### 15.2 The setup QR and recovery (P2, P6)

- The SETUP and RESET screens carry the § 7.8.1 URL. The `/l/` page opens the wizard with the
  device identity and the preset (`b`, `d`, `p`) preselected; the user confirms the preset before
  anything is written (§ 7.8.1 rule 5).
- **Recovery is setup.** A factory reset ends in FRESH claim mode, and its RESET QR (k=2) differs
  from the SETUP QR only in `k`. The page and every client run the identical wizard for both; the
  only difference is the text that says the display was reset. The preset in the RESET QR is the
  one the erased config had (the firmware reads `b`/`d`/`p` before the erase, FACTORY_RESET.md
  § 4), so the user does not have to find the board or panel again.

### 15.3 Migration assistant inputs (P7)

The assistant walks the 2.x → 3.0 cases of § 13.4, one step per screen: "Update drive data"
(no gesture) → "Rotate key" (automatic for v1 keys) or "Claim (gesture)" for flagged devices →
"Change your WiFi password" reminder. Its inputs are exactly these device facts, all readable in
plaintext without a session:

| Input | Source | Meaning for the assistant |
|---|---|---|
| firmware ≥ 3 | READ_IDENT present / version | assistant applies |
| NEEDS_MIGRATION | READ_IDENT tail bit 1 (or MSD FF × 11 with display packets present) | step "Update drive data" |
| BUILTIN_DRIVER | READ_IDENT tail bit 2 | no drive packets needed; 0xB3 present |
| CLAIMED / KEY_LEGACY | READ_IDENT bit 4; SEC_STATUS state, `dflags` bit 2, flags bit 4 ROTATION_SUGGESTED | step "Rotate key" (inside the session) |
| LEGACY_SECRETS_OPEN | READ_IDENT bit 6; SEC_STATUS flags bit 3 | step "Claim (gesture)" + the WiFi-password reminder |
| SHOW_KEY_ON_SCREEN | 0x27 `flags` bit 1 in a sealed CONFIG_READ | after a rotation: tell the user the boot screen now shows the new key |

### 15.4 Dropped by decision (revision 5)

- **P4 "extra-secure setup"** — dropped. The residual it addressed is stated in § 1.4 item 5; an
  owner can still get the same property by configuring in normal mode and claiming by the gesture
  (§ 13.3).
- **P5 a CLI wizard** (`opendisplay setup --url …`) — dropped.
- **P8 a key export format** (`OD2K:…`) — dropped. Moving a key between clients stays client UX
  (§ 10.7, last bullet); the boot-screen key display (§ 10.7) is the on-device option.
- **P3** (HA discovers devices in claim mode) is covered by § 15.1: HA reads the MSD bits like
  every client.

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
| factory-reset pattern | the fifth counted power-on after four on-periods of 2 000-9 999 ms (§ 7.10) |
| BEGIN / HELLO during a display pass | answered at once with BUSY (0x0C); nothing pending changes; **counts one rate-limit event** (revision 4). The client retries after the pass (a few seconds). **Only observable on a link that is already open**: on BG22 a new client sees no adverts for the length of the refresh instead (next row) |
| BG22: advertising during a refresh with no link open | connectable advertising is paused for the refresh, **about 24 s** on the M3 Pro 2.9in BWRY after a config write or at boot (measured). A client allows at least 30 s to find the device again after a config write, or retries |
| measured handshake latency | BG22 M3 Pro: HELLO 86-96 ms, FINISH 85-91 ms (frag 244), BUSY 89 ms, F3 about 90 ms, so about 90 ms per step. ESP32-S3: BEGIN 142 ms, NONCE 134 ms, CONFIRM 26 ms, HELLO 290 ms, FINISH 88 ms. Both well inside the 2 s target |
| the device's own blocking (a refresh, a flash write) | a loop stall over 2 000 ms moves the pending handshake step's and the reassembly's clocks forward by the stall, so it never expires a peer's step |

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
| V13 setup URL | § 7.8.1 vectors U1-U4: all fields, omitted fields, the 83 B worst case, `k=1` and `k=2` (revision 5 grammar) | the exact strings and the QR version / ECC |

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
| H2 | ECC keygen + ECDH latency per target; handshake time via an ESPHome proxy | **answered for BG22 and ESP32-S3 over direct BLE** (§ 17: about 90 ms per step on BG22; BG22 keygen 12.8 ms, agree 8.8 ms HAL time). Open: Nordic, and via an ESPHome proxy |
| H3 | Corruption semantics with injected faults: does Zephyr NVS return an older entry for a corrupt newest ATE? What does NVM3 do on a bad object? ESP-IDF NVS on a torn write | open; it drives § 10.1 and its residual |
| H4 | What 2.x firmware does with a 3.0 config (downgrade): does it parse it, and what does CONFIG_READ return? | open; it decides how much § 10.2 buys |
| H5 | ESP32 RNG quality during a claim (BT on) per board | open |
| H6 | ATT MTU through ESPHome proxies (≥ 185?) | open (R9) |
| H7 | BG22 effective connection count (`sl_bluetooth_config.h` (0) vs `sl_bluetooth_connection_config.h` (4)) | open; WP7 sets 1 |
| H8 | Reset causes per board: unplug/replug, battery pull and latch button give POR (not brown-out); nRF54 POR/BOR reporting; RESETREAS clearing | **partly answered**: ESP32-S3 EN / USB-serial reset = POWERON, counted; software reset = SW, not counted (§ 7.2). Open: WDT, panic and deep-sleep causes; every Nordic SoC; BG22 entirely; latch boards; battery pull |
| H9 | Gesture usability: the 2-10 s window on real boards with large supply capacitors | open |
| H10 | Setup QR scanned from a 1.54in panel at 4 px/module, and from a 2.9in at 2 px/module | open |
| H11 | nRF52840 / nRF54 `hwinfo_get_device_id` bytes vs FICR, recorded per board | **open, blocks vectors** |
| H12 | Claim-mode battery cost per target | open |
| H13 | Nordic storage budget: OS2K/OS2S/OS2R in a separate area, with a 4 kB config still fitting the 2 x 4 kB config NVS | open. The records currently share the config's settings NVS (`od_hal_secret.c`) |

### 19.1 Answered by the hardware runs (revision 4)

Two devices: one ESP32-S3 reTerminal Sticky (`s3-n32r8-extuart`, USB), one EFR32BG22 M3 Pro
2.9in BWRY (BLE OTA only). Nothing on Nordic.

| Item | Result | Where |
|---|---|---|
| claim end to end (FRESH claim mode, code, CONFIRM, sealed config with secrets, EXIT) | PASS on ESP32-S3 | § 7, § 8 |
| auth, sealed records, replay / truncation / counter 0, three bad tags | PASS on both | § 9.5 |
| rotation with an EN reset at every storage step; UNCLAIM guard | PASS on ESP32-S3 | § 11 |
| corrupt OS2K + marker → LOCKED, recovered only by the five-cycle reset | PASS on ESP32-S3 (injected) | § 10.1 row 3 |
| two-cycle gesture (after the timing fix), five-cycle reset unclaimed / claimed / LOCKED, interrupted reset resuming at R1b, R2, R5c, R6 | PASS on ESP32-S3 | § 7.2, § 7.10, FACTORY_RESET.md § 5.3 |
| nothing of the owner in raw NVS after a factory reset (after the R5 fix) | PASS on ESP32-S3 | § 10.4a |
| claim-mode gating (FD for every gated opcode and every SPI_RAW sub), claimed gating (FE), the plaintext set | PASS on ESP32-S3; claimed gating also on BG22 | § 6 |
| the 600 s claim-mode timeout, FRESH cleared | PASS on ESP32-S3 | § 7.3 |
| HELLO rate limit; BUSY on an open link during a refresh | PASS on BG22 (BUSY was then not counted; it is now, § 8.7) | § 8.7, § 17 |
| handshake latency, reset causes, gesture window, BG22 advertising pause | measured | § 7.2, § 17 |
| SMP refused on a claimed device without a sealed ENTER_DFU; SEC2 → B+FIX OTA after a sealed ENTER_DFU | PASS on ESP32-S3 / BG22 | § 6.1 |

### 19.2 Still unverified on hardware

Each of these has host tests only, or nothing:
- **the claim-mode caps**: the 3-render attempt cap, the restart cap, and a second claimant
  (§ 7.5, § 8.7);
- **reset causes other than EN and software**: watchdog, panic and deep-sleep wake on ESP32; every
  cause on BG22 and on every Nordic SoC (H8);
- **fail-open storage paths**: injected storage write failures (STORAGE_ERROR paths, § 10.1
  repairs); host power-cut tests only;
- **LAN on air**: plaintext LAN refusals and TLS-PSK on a claimed device, the ciphersuite (§ 12);
- **Nordic entirely**: nothing in this document has run on an nRF52840 or nRF54;
- **the reset pin**: hold-to-reset on any board (FACTORY_RESET.md § 3);
- **BG22 on-panel code**: the CODE screen drawn on the BG22 panel, and whether its blocking draw
  starves the claimant's open link;
- **BG22 claim mode and both gestures**: the two-cycle gesture and the five-cycle reset on BG22
  (the test unit was CLAIMED);
- the § 10.4a purge on any target (raw-flash census after UNCLAIM; BG22 and Nordic repack);
- the NO_RESET_PATTERN flag (§ 7.10.1) on any target;
- the sealed oversize refusal on air (GATT refuses an over-MTU write first);
- the BG22 tag-only CCM decrypt through the fixed HAL (§ 9.5 rule 5b).
- **everything of revision 5**: the MSD bytes on air in every § 5.3.1 state (BG22 bit 3 in
  particular), FF × 11 on Nordic and BG22, the Zephyr name in the ADV PDU seen by a passive
  scanner and by HA, the claim-mode boost interval and its 60 s end, the restored key lines and
  `/l/` key bytes on a panel, the redraw after a rotation and after toggling the bit, and the
  `/l/?f=1` QR scanned from a 1.54in panel.

### 19.3 Open questions for the maintainer (not decided here)

1. **Default key file.** Should a client create a durable key store on its own (for example
   py-opendisplay's `~/.config/opendisplay/sec2_keys.json`) when the caller passed none, so that
   claim and rotation work out of the box? Or must every caller pass one explicitly, so a library
   never writes a key file nobody asked for? py-opendisplay `v3/py` 801047e currently does the
   latter in the library and the former only in its CLI.
2. **SEC_STATUS probe on connect.** Should a client probe SEC_STATUS on every connect (costs up
   to 1.5 s against a silent 2.x device), or only when FIRMWARE_VERSION reports 3.x or the device
   is pinned as SEC2? py-opendisplay 801047e currently does the latter, with a per-address memo,
   and keeps downgrade protection for pinned devices.

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
| Recovery-page phishing advice | **Agree,** as rules for the `/l/` setup page (revision 5) | § 7.8.1 |

---

## Appendix B. Options considered and not adopted

### B.1 Factory reset by a gesture family (five quick power-cycles)

Analysed in revision 2 as an option and not adopted. **Adopted in revision 3 by maintainer
decision** and now normative in § 7.10, which carries the risk analysis and the chosen
mitigations. Of revision 2's minimum mitigations, the 2-10 s window and "every other reset breaks
the pattern" are adopted; "not on nRF52", a pattern of ≥ 7, a battery-voltage gate and a
confirmation power-on are not (the per-build switch `OD_FR_PATTERN_SHORT_BOOTS 0` remains for
boards that need it).

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
