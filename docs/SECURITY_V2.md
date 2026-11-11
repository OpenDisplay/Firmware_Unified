# OpenDisplay Secure Session v2 ("SEC2") — normative specification

Status: **DRAFT for maintainer and expert review, 2026-11-11.** Nothing here is implemented.
Every opcode, status byte, error code, flag bit and label in this document is **provisional**
until it is written into the canonical `opendisplay-protocol` header. Section 15 lists every
point where the cryptographic reasoning needs an expert's review; those points are not settled
claims.

Companion documents: [FACTORY_RESET.md](FACTORY_RESET.md) (reset pin, recovery screen, erase
ordering) and [SECURITY_V2_IMPLEMENTATION_PLAN.md](SECURITY_V2_IMPLEMENTATION_PLAN.md) (work
breakdown, cost, open questions).

The words MUST, MUST NOT, SHOULD and MAY are used in the RFC 2119 sense.

---

## 0. Decisions this design is bound by

Made by the maintainer on 2026-11-10/11, not re-argued here:

1. **Clean break.** 3.0 firmware speaks only SEC2. The v1 handshake (`CMD_AUTHENTICATE 0x0050`
   steps 1/2, `od_session.c`) and the v1 envelope are deleted from 3.0 firmware. Old clients
   cannot use a *claimed* 3.0 device. New clients keep v1 for 2.x firmware and for the
   un-updatable `Firmware_NRF` fleet.
2. **A device without a key stays fully open**, as today. No physical-presence requirement for
   ordinary commands.
3. **Works through ESPHome Bluetooth proxies**: application layer only, no BLE pairing or
   bonding, small MTU, write-with-response and notify only. **One key per device.**
4. **Session timeout 1 hour by default.** No advertising privacy changes.
5. **The device generates its key.** Provisioning is a one-time *claim* over an ephemeral ECDH
   exchange; the client must confirm with one authenticated message before the device marks
   itself claimed; until then a new claim restarts with a new key. Afterwards the key is never
   shown on the panel and never sent in clear; it is readable only inside an authenticated
   session (decision 8).
6. **Both claim-time mitigations**: (a) a claim is accepted only during the first 10 minutes
   after boot, otherwise a distinct REBOOT_REQUIRED result; (b) a short confirmation code
   derived from the key-exchange transcript is shown on the e-paper and entered/compared in
   the client.
7. **Factory reset** by reset pin on every target; recovery screen with QR before the erase
   ([FACTORY_RESET.md](FACTORY_RESET.md)).
8. **No write-only secrets** on a claimed device: a sealed CONFIG_READ returns the full config,
   including the 0x27 key, WiFi password and FindMy keys (§ 9.2). A claimed device's config is
   readable only inside a session.
9. **Secrets need a key**: an unclaimed device refuses to store a WiFi password or FindMy key
   material (§ 9.3); it is otherwise fully open.
10. The device key is set only by the claim and by rotation; the 0x27 key field of a config write
    must be zero or match (§ 9.2), and `encryption_enabled` as read is always truthful.

Things the maintainer has **not** decided are presented as options with a recommendation in
[SECURITY_V2_IMPLEMENTATION_PLAN.md § 9](SECURITY_V2_IMPLEMENTATION_PLAN.md#9-questions-only-the-maintainer-can-answer);
where this spec must pick something to be complete, it picks the conservative option and says
"pending Q*n*".

---

## 1. Threat model

### 1.1 Assets

- **K_dev**, the 16-byte device key, and everything it protects: the ability to send gated
  commands (config write, upload, DFU, SPI_RAW, ...).
- Confidentiality of config contents in transit on a claimed device (WiFi password, FindMy keys,
  images).
- Integrity of commands and responses on a claimed device.

### 1.2 Attackers in scope

| Id | Attacker | Capability |
|---|---|---|
| P | Passive sniffer in radio range | records every BLE PDU of any number of sessions, including the claim |
| A | Active peer in radio range without K_dev | connects to the device, impersonates the device to a client (address spoofing, relaying), injects/drops/reorders/replays frames on a link it can reach, runs many handshakes |
| M | Former client | once held K_dev, key was rotated since |
| X | Attacker who learns K_dev *later* (stolen HA backup, etc.) | plus P's recordings |

### 1.3 Security goals

| # | Goal | Holds against |
|---|---|---|
| G1 | A claimed device executes a gated command only if it was sealed under a live session established with K_dev | P, A, M (after rotation) |
| G2 | Confidentiality and integrity of every sealed frame, in both directions; no replay, no reflection, no reordering beyond the receive window, no cross-session splicing | P, A |
| G3 | Forward secrecy: recordings of past sessions stay confidential if K_dev leaks later | X (session establishment uses fresh ECDH) |
| G4 | A passive attacker learns nothing about K_dev from a claim | P |
| G5 | A client that completed a claim with a correctly entered code holds a K_dev no attacker knows (probability of an undetected MITM <= ~1e-6 per attempt) | A at claim time |
| G6 | Offline guessing of K_dev from any transcript is as hard as guessing a 128-bit random key (claimed keys); for migrated legacy keys see § 9.4 | P, A |
| G7 | A stale or misrouted reply from an earlier connection or handshake cannot be accepted as part of a new one (py-opendisplay#141, opendisplay.org#95) | A, and plain bugs |

### 1.4 Explicitly out of scope / NOT protected

This design does **not** protect against the following. Clients and docs must not imply it does.

1. **Unclaimed devices.** Fully open by decision 2: anyone in range can read and write config,
   run SPI_RAW, enter DFU, upload firmware. They cannot be given *new* secrets (decision 9), but a
   device upgraded from 2.x may still hold a WiFi password / FindMy keys readable by anyone until it
   is claimed (§ 9.4 option A1).
2. **Firmware authenticity.** BG22 GBL and ESP32 OTA images are unsigned; nRF images use the
   public MCUboot debug key (accepted for 3.0). Whoever can reach the DFU path owns the device and
   K_dev. On a claimed device the DFU path is reachable only after a sealed ENTER_DFU, but
   *after* that, the AppLoader (BG22) and, unless bound (pending Q6), SMP accept any peer.
3. **Physical access.** Flash readout, SWD/JTAG, USB serial reflash, desoldering, pressing the
   reset button (that is the intended recovery path). K_dev is stored in plaintext flash.
4. **Device-side takeover during the claim window.** An attacker who claims the device before the
   owner (or who sits in the middle of the owner's claim and completes the claim towards the
   device with its own confirmation) ends up owning it. The owner *detects* this (their claim
   fails or reports ALREADY_CLAIMED, or the code does not match) and recovers by factory reset;
   it is not prevented, because the device has no trusted input to confirm the code on its
   side (§ 7.6, expert-review item E8).
5. **Claims of devices without a working panel** have no code check; they rely on the 10-minute
   window only, and a MITM present during that claim wins silently (§ 7.7).
6. **Users who confirm a code without comparing it.** Mitigated by making the user *type* the code.
7. **Denial of service**: jamming, holding the single connection, flooding handshakes (rate
   limit locks the owner out for up to 60 s), forging SESSION_REQUIRED frames to force
   re-handshakes, forcing bad-tag teardowns.
8. **Traffic analysis**: opcodes of host->device frames, frame lengths, timing and the MSD
   advertisement are visible.
9. **Tracking / privacy**: static address and name, plaintext MSD, static FindMy keys (decision 4).
10. **Relay attacks** (distance bounding): an attacker relaying between a far-away device and the
    client is not detected; the session stays end-to-end protected.
11. **A compromised client** (HA host, browser, PC) or a malicious client the owner gave the key to,
    until the key is rotated (§ 10).
12. **2.x firmware and the legacy Firmware_NRF fleet** — they keep v1 and its known weaknesses
    (encryption review W4/W4b/W5/W6). New clients apply the v1 mitigations listed in
    [the plan § 5.3](SECURITY_V2_IMPLEMENTATION_PLAN.md#53-v1-client-mitigations-for-2x-and-legacy).
13. **LAN (TLS-PSK) transport** beyond deriving its PSK from K_dev (§ 11).
14. **Side channels and fault injection** on the device's ECC/AES implementations.
15. **RNG failure** beyond fail-closed behaviour: every target's HAL refuses to produce randomness
    when its backend fails, but entropy quality (e.g. ESP32 with RF off) is not re-verified here.

---

## 2. Primitives

Restricted to what **every** target already links (encryption review § 2.9, BG22 map file of
`v3/seq-streaming`: `psa_raw_key_agreement`, `psa_generate_key`, `psa_export_public_key`,
`psa_mac_compute`, `psa_aead_encrypt/decrypt`, CRYPTOACC TRNG are placed; nRF52840
`CONFIG_PSA_WANT_ALG_ECDH=y`, `..._ECC_KEY_PAIR_GENERATE=y`, `..._ECC_SECP_R1_256=y`,
`..._CMAC=y`, `..._CCM=y`; ESP-IDF mbedTLS ECDH/CMAC/CCM). **No SHA-2, no HMAC, no HKDF** — not
placed on BG22.

| Name | Definition | Reference |
|---|---|---|
| `AES` | AES-128 block cipher | FIPS 197 |
| `CMAC(k, m)` | AES-128-CMAC, full 16-byte output | NIST SP 800-38B, RFC 4493 |
| `CCM(k, n, a, p)` | AES-128-CCM, 13-byte nonce (L = 2), **12-byte tag** (M = 12); output `c || t` | NIST SP 800-38C, RFC 3610 |
| `ECDH(d, Q)` | P-256 (secp256r1) ECC CDH primitive; output Z = the 32-byte big-endian x-coordinate of d·Q. The peer point MUST be validated (on curve, not the identity, coordinates < p) before use — full public-key validation per SP 800-56A § 5.6.2.3.3; P-256 has cofactor 1 | NIST SP 800-56A Rev 3 § 5.7.1.2 |
| `Q` on the wire | 64 bytes `X || Y`, each 32 bytes big-endian (the SEC1 uncompressed form without its `04` prefix). Implementations prepend `04` for PSA / WebCrypto import | SEC1 § 2.3.3 |
| `RAND(n)` | n bytes from the target's CSPRNG/TRNG (`od_hal_crypto_random`, fail-closed) | — |
| `ct_equal` | constant-time comparison of equal-length byte strings | — |

### 2.1 Key derivation function

**Expansion** — NIST SP 800-108 Rev 1, KDF in counter mode, PRF = AES-128-CMAC, counter and
length fields of 8 and 16 bits, **always exactly one output block** (L = 128):

```
KDF(K_in, Label, Context) = CMAC(K_in, 0x01 || Label || 0x00 || Context || 0x00 0x80)
```

- `0x01` is [i]_8, the block counter, constant because L = 128 = one block.
- `Label` is an ASCII string from the table in § 2.3, never containing 0x00. The 0x00 separator
  plus fixed-length Contexts per Label make the input encoding injective.
- `0x00 0x80` is [L]_16 = 128 bits, big-endian.

**Extraction** — NIST SP 800-56C Rev 2 § 5.1 two-step key derivation, randomness extraction
with MAC = AES-128-CMAC (the 128-bit salt is the CMAC key):

```
PRK = CMAC(salt, Z)
```

- Claim: `salt = 0^16` (the all-zero salt 800-56C prescribes when none is agreed).
- Session establishment: `salt = K_ext`, a secret derived from K_dev (§ 2.2). 800-56C allows a
  secret salt; it is how the pre-shared key enters the derivation while Z provides forward
  secrecy.

Expert review E1/E2 (§ 15) covers both choices: CMAC as an extractor of a P-256 x-coordinate,
and the single-block CMAC counter-mode expansion (including whether SP 800-108r1's CMAC
caution — its "K0"-style modification for CMAC-based KDFs — applies when only one block is
ever produced). Implementers must not "simplify" either construction (e.g. by skipping the
extraction and keying the expansion with Z directly).

### 2.2 Key hierarchy

K_dev is never used directly as a CMAC or CCM key. At boot (and on claim / rotation) the device
derives, and the client derives when it stores the key:

```
uid    = 8-byte device identity (§ 4.2)
K_ext  = KDF(K_dev, "OD2/k-ext",  uid)   salt for session extraction
K_conf = KDF(K_dev, "OD2/k-conf", uid)   key-confirmation tags (claim, rotation)
K_lan  = KDF(K_dev, "OD2/k-lan",  uid)   TLS-PSK for the LAN transport (§ 11)
```

### 2.3 Label registry

Every Label is used for exactly one purpose. Context length is fixed per Label.

| Label (ASCII, no NUL) | K_in | Context | Output use |
|---|---|---|---|
| `OD2/k-ext` | K_dev | uid (8) | K_ext |
| `OD2/k-conf` | K_dev | uid (8) | K_conf |
| `OD2/k-lan` | K_dev | uid (8) | K_lan |
| `OD2/wrap` | PRK_claim | Tc (179) | k_wrap, one-shot CCM key that carries K_dev |
| `OD2/h2d` | PRK_claim or PRK_auth | Tc (179) or Ta (178) | host->device record key |
| `OD2/d2h` | PRK_claim or PRK_auth | Tc or Ta | device->host record key |
| `OD2/sid` | PRK_claim or PRK_auth | Tc or Ta | 16-byte session id (binding for SEC_KEY, never on the wire) |
| `OD2/claim-c` | K_conf | Tc | client's claim confirmation tag |
| `OD2/claim-d` | K_conf | Tc | device's claim completion tag |
| `OD2/auth-c` | PRK_auth | Ta | client's handshake proof |
| `OD2/auth-d` | PRK_auth | Ta | device's handshake proof |
| `OD2/rotate-c` | K_conf_new | uid (8) \|\| sid (16) | client's rotation confirmation |

Tc and Ta begin with distinct ASCII prefixes (`OD2/claim`, `OD2/auth`) and differ in length, so a
claim derivation and an authentication derivation never share an input even under one PRK.

Two non-KDF uses of CMAC exist, both copied in structure from Bluetooth LE Secure Connections
numeric comparison (Core Spec v5.x Vol 3 Part H § 2.2.6 f4 and § 2.2.9 g2):

```
Cd       = CMAC(Nd, "OD2/commit" || uid || Qd || Qc)          commitment, keyed by the secret Nd
code_raw = CMAC(Nc, "OD2/code"   || uid || Qc || Qd || Nd)     confirmation-code source
```

---

## 3. Wire framing overview

All SEC2 opcodes follow the protocol's universal framing (`opendisplay_protocol.h`, UNIVERSAL
FRAMING): requests `[cmd_hi][cmd_lo][payload]`, responses `[status][cmd_lo][data]`, multi-byte
fields little-endian unless stated. Points `Q` and CMAC/KDF outputs are byte strings and are
transmitted in the byte order defined above (no endianness conversion).

### 3.1 New opcodes (provisional)

`0x0054..0x0057` are free in the header (`opendisplay_protocol.h` 2.4: 0x0053 is the last of the
0x005x family; no RESP_ value 0x54..0x57 exists; no shipped firmware or py-opendisplay uses them —
checked by grep of Firmware, Firmware_NRF54, Firmware_Silabs, Firmware_NRF, py-opendisplay).

| Opcode | Name | Framing | Allowed when |
|---|---|---|---|
| `0x0054` | `CMD_SEC_STATUS` | plaintext, single frame | always |
| `0x0055` | `CMD_SEC_CLAIM` | plaintext, fragmented (§ 3.3) | unclaimed device, claim window open |
| `0x0056` | `CMD_SEC_AUTH` | plaintext, fragmented | claimed device |
| `0x0057` | `CMD_SEC_KEY` | **sealed only** | claimed device, live session |

`CMD_AUTHENTICATE 0x0050` keeps its number on 3.0 only to give old clients a clean refusal
(§ 12.2). New status byte `AUTH_STATUS_V2_REQUIRED = 0x05` (provisional).

### 3.2 SEC2 response and error shape

```
success   [0x00][cmd_lo][sub:1][frag:1][body chunk...]        (CLAIM / AUTH, § 3.3)
          [0x00][0x54][status body]                            (STATUS, § 5)
error     [0xFF][cmd_lo][sub:1][err:1]                         4 bytes, always plaintext
```

`err` is scoped to opcodes 0x0054..0x0057 (one shared namespace `OD_SEC_ERR_*`):

| err | Name | Meaning / client action |
|---|---|---|
| 0x01 | MALFORMED | wrong length, bad fragment, unknown version/sub. Fix the client. |
| 0x02 | BAD_STATE | message out of order (e.g. NONCE without BEGIN, FINISH without HELLO, pending state expired or discarded). Restart from the first message. |
| 0x03 | RATE_LIMITED | > 10 handshake starts in 60 s. Wait 60 s. |
| 0x04 | **REBOOT_REQUIRED** | claim window closed. Ask the user to power-cycle / press reset, then retry within 10 minutes (§ 7.2). |
| 0x05 | ALREADY_CLAIMED | the device has a key. Use SEC_AUTH; if the user has no key, factory reset ([FACTORY_RESET.md](FACTORY_RESET.md)). |
| 0x06 | NOT_CLAIMED | SEC_AUTH / SEC_KEY on a device without a key. Device is open; claim it if wanted. |
| 0x07 | AUTH_FAILED | proof/confirmation tag mismatch. Wrong key (AUTH) or code check bypassed/MITM (CLAIM). Do not retry automatically more than once. |
| 0x08 | EXPIRED | the step arrived after its timeout (§ 13). Restart. |
| 0x09 | BAD_POINT | peer public key failed validation. Client bug or attack. |
| 0x0A | CRYPTO_ERROR | device engine failure (RNG, ECC). Retry later; persistent = hardware fault. |
| 0x0B | STORAGE_ERROR | persisting the key failed; the device stays unclaimed / keeps the old key. Retry. |
| 0x0C | BUSY | another long operation (display pass, flash erase) holds the device. Retry after it. |

### 3.3 Handshake fragmentation (CLAIM and AUTH only)

Handshake messages are up to 97 bytes; a BLE notification at ATT MTU 23 carries 20 value bytes.
Each handshake message is therefore carried as fragments:

```
H->D  [0x00][cmd_lo][sub:1][frag:1][chunk: 0..(write_max-4) bytes]
D->H  [0x00][cmd_lo][sub:1][frag:1][chunk: 0..(frag_max-4) bytes]
frag  bit 7 = LAST; bits 0-6 = fragment index, 0,1,2,... in order
```

- The message **body** is the concatenation of the chunks, in index order.
- **H->D**: the client may use any chunk size up to its usable write size (20 at MTU 23, up to
  249 at the protocol's 253-byte frame ceiling). Each fragment is one write-with-response. The
  device sends **no** notification for a non-LAST fragment (the ATT write response is the
  flow control); it answers the LAST fragment with the response message, or with a 4-byte error
  at any fragment that is malformed (index out of order, body would exceed the message's fixed
  length, sub differs from fragment 0's). An error discards the partial message.
- **D->H**: the device splits its response so that every notification is at most `frag_max`
  bytes **including** the 4-byte header. `frag_max` is declared by the client in the first
  message of every handshake (byte 1 of BEGIN / HELLO), range 20..244; values outside are
  MALFORMED. Clients that do not know their MTU MUST send 20.
- Every message has a **fixed body length** (tables below); a LAST fragment that leaves the body
  short or long is MALFORMED. There is no length field.
- Reassembly state is per connection, one message at a time; the next fragment must arrive
  within 5 s of the previous one or the reassembly is discarded (no reply).
- Fragmentation applies only to SEC_CLAIM and SEC_AUTH. Sealed records (§ 8) are never
  fragmented by this layer.

---

## 4. Device state and identity

### 4.1 Persistent and volatile state

| State | Where | Lifetime |
|---|---|---|
| `K_dev` + origin (CLAIMED / LEGACY) | secret record, separate from the config record (§ 9) | until rotation, UNCLAIM or factory reset |
| claim window deadline | RAM; carried across a *planned* software reset in retained RAM (§ 7.2) | 600 s from a physical boot |
| pending claim (Qc, Qd, ephemeral private key, Nc, Nd, K_dev candidate) | RAM only, never persisted | until CONFIRM, disconnect, timeout or a new BEGIN |
| pending handshake (expected auth-c tag, derived record keys) | RAM | until FINISH, disconnect, timeout or a new HELLO |
| session (k_h2d, k_d2h in HAL slots; counters; sid; start time) | RAM | until teardown (§ 8.6) |

Device security states:

```
           +-------------------- factory reset / UNCLAIM ---------------------+
           v                                                                  |
     UNCLAIMED --BEGIN (window open)--> CLAIM_PENDING --CONFIRM ok + persisted--> CLAIMED
       ^   ^                               |   |                                ^   |
       |   +-- disconnect / timeout / ------+   |                                |   | ROTATE
       |       bad confirm / new BEGIN          |                                |   v
       |       (new BEGIN = new K_dev)          +- (the pending key is never     +-- CLAIMED(K_new)
       |                                           persisted, so nothing strands)
       +-- first 3.0 boot with no 2.x key ------------------------------------------
     (first 3.0 boot WITH an enabled 2.x key -> CLAIMED, origin LEGACY, rotation required, § 9.4)
```

### 4.2 Device identity `uid`

8 bytes, stable for the life of the silicon, not secret, wire-visible in SEC_STATUS:

| Target | uid |
|---|---|
| efr32bg22-slc | `SYSTEM_GetUnique()` as 8 bytes big-endian |
| nordic-zephyr (nRF52840, nRF54) | FICR DEVICEID[1] \|\| DEVICEID[0], each big-endian (Zephyr `hwinfo_get_device_id`, first 8 bytes) |
| esp32-idf | `0x00 0x00` \|\| the 6-byte factory MAC (`esp_efuse_mac_get_default`) in transmission order |

The uid binds every key and transcript to one device and fixes the roles (the device's uid is
always the one inserted). The client takes the uid from SEC_STATUS at claim time, stores it with
K_dev, and uses the **stored** uid for every later handshake; a mismatch with the uid a device
reports makes the handshake fail (that is a different device, or an attacker).

`dflags` — device capability byte, included in every transcript:

| Bit | Name | Meaning |
|---|---|---|
| 0 | HAS_PANEL | a display is configured and the device will try to render the claim code |
| 1 | HAS_RESET_PIN | the reset pin is enabled in 0x27 |
| 2 | KEY_LEGACY | K_dev was migrated from a 2.x config (AUTH only; § 9.4) |
| 3 | reserved | 0 |
| 4-7 | reserved | 0 |

---

## 5. `CMD_SEC_STATUS` 0x0054 (plaintext, always allowed)

```
request   [0x00][0x54]
response  [0x00][0x54][sec_ver:1][state:1][flags:1][window_s:2 LE][uid:8][timeout_s:2 LE][dflags:1]
          18 bytes total (fits one notification at MTU 23)
```

| Field | Values |
|---|---|
| sec_ver | 0x02 |
| state | 0 UNCLAIMED, 1 CLAIM_PENDING (a claim was started on this connection and is not yet confirmed), 2 CLAIMED |
| flags | bit0 CLAIM_WINDOW_OPEN, bit1 SESSION_ACTIVE (this connection), bit2 RESET_PENDING (never seen by a client in practice), bit3 LEGACY_SECRETS_OPEN (unclaimed but the stored config holds secrets, § 9.4), bit4 ROTATION_REQUIRED (legacy key, § 9.4), bit5 KEY_CORRUPT (LOCKED, § 9.1), others 0 |
| window_s | seconds left in the claim window, 0 when closed (and always 0 when CLAIMED) |
| timeout_s | the session lifetime a new session will get (§ 8.6) |
| dflags | § 4.2 |

SEC_STATUS is unauthenticated and therefore advisory: a client uses it to choose what to do next;
every security decision is re-made inside the authenticated transcript (uid and dflags are in Tc
and Ta).

### 5.1 Discovery before and right after connecting

| Signal | Source | Meaning |
|---|---|---|
| firmware major >= 3 | `CMD_FIRMWARE_VERSION 0x0043` (plaintext, unchanged) | the device speaks SEC2 only. Clients MUST NOT use v1 with it |
| MSD `status` bit 3 (`OD_MSD_STATUS_ENCRYPTION_ENABLED`) | advertisement, `CMD_READ_MSD` | on 3.0: **1 = CLAIMED** (a session is required). Same bit, same client behaviour as 2.x ("needs a key"). No new advertising bits (decision 4) |
| READ_IDENT flags byte (trailing, `od_cmd_ident.c`) | `CMD_READ_IDENT 0x0046` | new bits (provisional): **bit2 SEC_V2** (always 1 on 3.0), **bit3 CLAIMED**, **bit4 CLAIM_WINDOW_OPEN**, **bit5 LEGACY_SECRETS_OPEN**. Bits 0-1 are CONFIG_DRIVEN / NEEDS_MIGRATION (PROTOCOL_CHANGES_3.0 #4) |
| SEC_STATUS | 0x0054 | authoritative state, uid, window seconds |

The claim-window bit is not in the advertisement (no free status bit: bits 0-2 are taken and
bits 4-7 are the loop counter; `dynamic[11]` is config-owned). A client that wants to show
"ready to claim" in a scan list must connect and read READ_IDENT or SEC_STATUS.

---

## 6. Gating table — every opcode

Columns: **U** = unclaimed device; **C/no** = claimed device, no live session on this
connection; **C/s** = claimed device with a live session. "open" = plaintext, executed as today.
"FE" = refused with the 3-byte plaintext `[0x00][cmd_lo][0xFE]` (SESSION_REQUIRED, the existing
`RESP_AUTH_REQUIRED` shape every client already recognises), command not executed. "sealed" =
must arrive sealed (§ 8); a plaintext frame gets FE.

| Opcode | Name | U | C/no | C/s | Notes |
|---|---|---|---|---|---|
| 0x000F | REBOOT | open | FE | sealed | a planned reboot carries the claim window (§ 7.2) |
| 0x0040 | CONFIG_READ | open | FE | sealed | **a claimed device's config is readable only in a session**; that read is complete — WiFi password, FindMy keys and the 0x27 key filled in from the secret record (§ 9.2, decided: no write-only secrets). An unclaimed device cannot hold new secrets (§ 9.3) |
| 0x0041 | CONFIG_WRITE | open, **but a blob carrying a secret is NACKed 0x10 SECRET_NEEDS_CLAIM** (§ 9.3) | FE | sealed; 0x27 key must be zero or equal K_dev, else NACK 0x11 (§ 9.2) | Nordic's REWRITE_ALLOWED exemption is **removed** on every target (decided, § 9.2): no unauthenticated config write on a claimed device |
| 0x0042 | CONFIG_CHUNK | as 0x0041 (checked at the last chunk) | FE | sealed | as 0x0041 |
| 0x0043 | FIRMWARE_VERSION | open | open | **plaintext** (never sealed) | unchanged |
| 0x0044 | READ_MSD | open | open | **plaintext** | the same 16 bytes are broadcast anyway; made plaintext-exempt on every target (was Silabs-only) |
| 0x0045 | CONFIG_CLEAR | open | FE | sealed | erases the config record only, **never K_dev** (§ 9.5, FACTORY_RESET.md § 6) |
| 0x0046 | READ_IDENT | open | open | **plaintext** | carries the new SEC flags (§ 5.1) |
| 0x0047..0x004D | FLASH_* / ACTION_* | open | FE | sealed | image store, action table |
| 0x0050 | AUTHENTICATE (v1) | `[00][50][03]` NOT_CONFIG | `[00][50][05]` V2_REQUIRED | `[00][50][05]` (plaintext; does **not** tear the session down) | the v1 machine is deleted; this is the refusal old clients see |
| 0x0051 | ENTER_DFU | open | FE | sealed | SMP / AppLoader exposure: § 6.1 |
| 0x0052 | POWER_OFF | open | FE | sealed | |
| 0x0053 | DEEP_SLEEP | open | FE | sealed | |
| 0x0054 | SEC_STATUS | open | open | **plaintext** | |
| 0x0055 | SEC_CLAIM | open (window) | ALREADY_CLAIMED | ALREADY_CLAIMED | REBOOT_REQUIRED outside the window |
| 0x0056 | SEC_AUTH | NOT_CLAIMED | open (handshake) | open; a HELLO tears the live session down (§ 8.6) | |
| 0x0057 | SEC_KEY | NOT_CLAIMED (4-byte error) | FE | sealed | rotation / unclaim |
| 0x0070..0x0072 | DIRECT_WRITE_* | open | FE | sealed | |
| 0x0073 | LED_ACTIVATE | open | FE | sealed | |
| 0x0075 | LED_STOP | open | FE | sealed | |
| 0x0076 | PARTIAL_WRITE_START | open | FE | sealed | |
| 0x0077 | BUZZER | open | FE | sealed | |
| 0x0080..0x0082 | PIPE_WRITE_* | open | FE | sealed | PIPE DATA (0x0081) frames are sealed records like any other |
| 0x0083 | NFC_ENDPOINT | open | FE | sealed | |
| 0x0084 | SPI_RAW | open | FE | sealed | debug escape hatch; open on unclaimed devices by decision 2 |
| 0x0085 | DRIVE_WRITE_START | open | FE | sealed | |
| any other | unknown | target's unknown-opcode behaviour (Unified: no reply) | FE | sealed, then unknown-opcode behaviour | the gate runs before routing so an unknown opcode cannot probe |
| LAN TLS origin | — | — | — | TLS-PSK frames bypass the BLE gate as today (`od_dispatch.c` origin check), PSK = K_lan (§ 11) | |

Rules:

1. The **plaintext set** on a claimed device is exactly {0x0043, 0x0044, 0x0046, 0x0050 refusal,
   0x0054, 0x0055 refusal, 0x0056}. These are always sent and answered in plaintext, even inside a
   session; a client MUST NOT seal them and the device MUST answer FE to a sealed one (it would
   otherwise be a second encoding of the same command).
2. A device MUST NOT execute any part of a gated handler (including partial parsing with side
   effects, such as arming a flash PRELOAD) before the frame's tag verified.
3. A claimed device answers FE, never a NACK or silence, to a plaintext gated command, so that a
   client always learns "session required" in one shape.

### 6.1 DFU services on a claimed device

- **nordic-zephyr SMP and esp32-idf SMP**: not reachable (hidden / refused) until a sealed
  ENTER_DFU; then open until reboot as today. Binding the unlock to the unlocking connection and
  a time limit is pending **Q6**; this spec's conservative default for implementation is: relock
  on the unlocking connection's disconnect and after 300 s, refuse SMP from any other connection.
- **efr32bg22-slc**: sealed ENTER_DFU reboots into the Gecko AppLoader, which accepts any peer
  and any unsigned GBL. Not fixable without a new bootloader (pending Q5/Q6).
- On an **unclaimed** device all DFU paths stay open, as today (decision 2).

---

## 7. Claim protocol — `CMD_SEC_CLAIM` 0x0055

### 7.1 Messages

All plaintext, fragmented per § 3.3. Lengths are body lengths.

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

(At MTU 23 a NONCE request is exactly one 20-byte write: 4 header + 16.)

### 7.2 The claim window and REBOOT_REQUIRED

- The window is **600 s** from a **physical boot**: power-on reset, brown-out reset, external
  reset-pin (nRESET) reset, or a wake from deep sleep / power-off / EM4 / System OFF **caused by a
  button**. These are read from the reset-cause registers through a new HAL
  (`od_hal_reset_cause()`, see the plan).
- **Not** a physical boot, and therefore **no new window**: software reset (`CMD_REBOOT`, a
  reboot after config write, after OTA), watchdog, panic/lockup, timer wake from deep sleep. A
  remote peer must not be able to reopen the window, and CMD_REBOOT is open on unclaimed devices.
- **Planned software resets carry the remaining window**: immediately before a firmware-initiated
  reset (CMD_REBOOT, post-config-write reboot, factory-reset completion), the device writes
  `{magic, remaining_ms, crc}` to retained RAM (ESP32 `RTC_NOINIT_ATTR`, Zephyr `__noinit`
  section, BG22 BURAM retention registers). The next boot, if its reset cause is "software"
  and the record verifies, resumes the window with that remaining time; any other cause clears
  the record. This is what lets the usual first-time flow (power on, write config, device
  reboots, claim) finish without a second power cycle.
- **A factory reset opens a fresh 600 s window** on its completion reboot (the user is
  physically present; FACTORY_RESET.md § 5).
- The window is checked when **BEGIN** arrives. A pending claim may complete after the window
  closes, within the § 13 timeouts.
- Outside the window, BEGIN is answered `[FF][55][01][04]` REBOOT_REQUIRED. Nothing else in the
  device changes. The client tells the user, in these words or close: *"For security, a display
  can only be claimed within 10 minutes of being switched on. Please power-cycle the display (or
  press its reset button) and try again."* — then polls SEC_STATUS (reconnecting as needed)
  until CLAIM_WINDOW_OPEN.
- On a claimed device, BEGIN is ALREADY_CLAIMED regardless of the window. A lost key is recovered
  only by factory reset (FACTORY_RESET.md).

### 7.3 Device and client computations

```
BEGIN (client):     dc, Qc  <- fresh P-256 key pair        (never reused)
BEGIN (device):     check: window open, not claimed, rate limit, ver == 0x02, frag_max in 20..244
                    discard any pending claim (its key candidate is forgotten)
                    validate Qc                              -> BAD_POINT
                    dd, Qd  <- fresh P-256 key pair
                    Nd      <- RAND(16)
                    K_new   <- RAND(16)                      the device key candidate
                    Cd      =  CMAC(Nd, "OD2/commit" || uid || Qd || Qc)
                    reply [dflags][Qd][Cd]
NONCE (client):     Nc      <- RAND(16); send
NONCE (device):     Z       =  ECDH(dd, Qc); destroy dd
                    Tc      =  "OD2/claim" || 0x02 || dflags || uid || Qc || Qd || Nc || Nd   (179 B)
                    PRK     =  CMAC(0^16, Z)
                    k_wrap  =  KDF(PRK, "OD2/wrap", Tc)
                    W       =  CCM(k_wrap, n = 0x00 * 13, a = "OD2/wrap" || uid, p = K_new)   (16 + 12)
                    code    =  Code(Tc)       (§ 7.5)
                    reply [Nd][W]; THEN start rendering the code (§ 7.5)
NONCE reply (client): check ct_equal(Cd, CMAC(Nd, "OD2/commit" || uid || Qd || Qc)) else abort
                    Z = ECDH(dc, Qd); destroy dc; same Tc, PRK, k_wrap
                    K_new = CCM-decrypt(W)  else abort
                    code  = Code(Tc); prompt the user (§ 7.5)
                    on a correct code: persist {uid, K_new, state = UNCONFIRMED} locally FIRST
                    K_conf = KDF(K_new, "OD2/k-conf", uid)
                    tag_c  = KDF(K_conf, "OD2/claim-c", Tc); send CONFIRM
CONFIRM (device):   K_conf = KDF(K_new, "OD2/k-conf", uid)
                    ct_equal(tag_c, KDF(K_conf, "OD2/claim-c", Tc)) else AUTH_FAILED, discard pending
                    persist secret record {K_new, origin CLAIMED} (atomic) else STORAGE_ERROR
                    tag_d  = KDF(K_conf, "OD2/claim-d", Tc)
                    session keys: k_h2d = KDF(PRK, "OD2/h2d", Tc), k_d2h = KDF(PRK, "OD2/d2h", Tc),
                                  sid   = KDF(PRK, "OD2/sid", Tc); counters = 0; session OPEN
                    reply [tag_d][timeout_s]
CONFIRM reply (client): ct_equal(tag_d, KDF(K_conf, "OD2/claim-d", Tc)) else treat as not claimed
                    mark the stored key CONFIRMED; derive the same session keys
```

The CONFIRM reply is the device's last plaintext word; everything after it on this connection is
sealed (§ 8).

The single-use `k_wrap` is why a constant CCM nonce is acceptable for W (expert review E10).

### 7.4 Why the commitment

A plain "code = f(transcript)" is broken: an attacker in the middle, playing the device towards
the client, receives Qc and can grind its own Qd' offline until the client-side code equals the
device-side code (10^6 trials of ECDH + CMAC: seconds on a PC). Following Bluetooth LE Secure
Connections numeric comparison, the device commits to Nd (inside Cd) before it learns Nc, and
the client reveals Nc only after receiving Cd:

- Playing the device towards the client, the attacker must fix Qd' and Nd' (via Cd') before it
  sees Nc; afterwards nothing it controls changes the client's code.
- Playing the client towards the device, the attacker must send Nc' before it learns Nd; the
  device's code is then a fresh pseudo-random function of Nd it could not predict.
- The two codes therefore match with probability about 10^-6 per attempt.

A known-key CMAC is **not** a binding commitment (CBC-MAC can be inverted when the key is known,
so "CMAC(public key, Qd || r)" can be opened to anything). The commitment is binding because Nd
is the CMAC **key** and stays secret until the reveal; opening Cd to a different (Nd', Qd') means
finding a key collision of AES-CMAC on a chosen message. This is the f4 construction. Expert
review E3.

### 7.5 The confirmation code

```
code_raw = CMAC(Nc, "OD2/code" || uid || Qc || Qd || Nd)
code     = (uint32 big-endian of code_raw[12..15]) mod 1 000 000      six decimal digits, zero-padded
shown as "123 456" (a space after the third digit)
```

- **Length and alphabet**: 6 decimal digits (bias of the mod: 2^32 mod 10^6 = 967 296, relative
  bias < 2.3e-4 — negligible against 10^-6). Digits survive every panel, font and language and
  are what HA, phone keyboards and the user already know from Bluetooth pairing.
- **Display**: the device renders the code right after it has queued the NONCE reply (the reply
  must not wait for the refresh: refreshes take 1-40 s). It uses the boot-screen path in **display
  mode 0** (`od_display_boot()`), with the boot layout's text block replaced by:

  ```
  full layout                     reduced layout (small panels)
  "PAIRING CODE"                  "CODE"
  "123 456"   (largest scale)     "123 456"
  "Enter in your app"             "OD1A2B3C"
  "ID:   OD1A2B3C"
  ```

  The QR keeps pointing at the landing page (never carries a key, § 9.6). Header and footer are
  the boot screen's.
- **How long**: e-paper keeps it until the next refresh. The device does **not** redraw on
  success, failure or timeout (saves a refresh; the code is worthless after the claim ends — it is
  a function of public values plus Nc/Nd, all of which went over the air). The next upload or boot
  screen replaces it.
- **Client**: displays "Enter the 6-digit code now shown on the display (this can take up to a
  minute to appear)." The user **types** the code; the client compares it with its own `code`
  (constant time not required — it is not secret). On match → CONFIRM. On mismatch → send ABORT,
  do not store the key, and warn: *"The code does not match. Someone nearby may be interfering.
  The display may now be claimed by someone else — if so, factory-reset it (see the recovery
  page) and try again."* Three wrong entries of the same claim count as a mismatch.
  HA and py-opendisplay MUST implement the entry; the website SHOULD (plan § 6).
- **Never** auto-confirm, never pre-fill the field, never offer "skip" when HAS_PANEL = 1.

### 7.6 What the claim guarantees (and what not)

- Against P: K_new is wrapped under a key derived from an ECDH secret P does not have (G4).
- Against A, if the user typed the right code: the client never accepts a K_new an attacker
  knows (G5, ~10^-6 per attempt).
- **Not**: an attacker in the middle who obtains the device-side K_new can send its own valid
  CONFIRM and become the device's owner. The owner's client sees the code mismatch and says so.
  This is detection, not prevention (§ 1.4 item 4). A device with a button could require a press
  to accept CONFIRM, which would close this gap; that is a possible later extension and is listed
  as expert-review item E8 / maintainer question Q8.

### 7.7 Devices without a panel

`dflags.HAS_PANEL = 0` when no display is configured, the boot path reports no display, or the
config's display has no usable mode 0. Then:

- the device still computes everything and still accepts CONFIRM; it shows nothing (if an LED is
  configured it blinks the claim pattern of FACTORY_RESET.md § 3.3 while a claim is pending);
- the client MUST NOT ask for a code. It MUST show: *"This display reports that it cannot show a
  pairing code. Claiming is then protected only by the 10-minute power-on window. Continue only if
  the display really has no screen and you are not in a public place."* and require an explicit
  acknowledgement;
- the HAS_PANEL bit itself is unauthenticated before the claim; an attacker can clear it. The
  acknowledgement text is the only defence: a user who can see a screen on their display should
  stop. Residual risk § 1.4 item 5.

### 7.8 State machine rules

- Each sub is accepted once per pending claim, in order BEGIN → NONCE → CONFIRM; a repeated
  NONCE or CONFIRM is BAD_STATE and discards the claim (so W is computed exactly once per
  k_wrap).
- One pending claim at a time. **A new BEGIN discards the pending claim and generates a new
  K_new.** A pending claim is also discarded on disconnect, on ABORT, on a failed CONFIRM tag, on
  any MALFORMED/BAD_STATE in its sequence, and on timeout (§ 13).
- The pending K_new exists only in RAM. The device persists it only after a valid CONFIRM, so a
  lost NONCE reply or an abandoned claim cannot leave a key that nobody holds.
- The client persists K_new **before** sending CONFIRM. If the CONFIRM reply is lost, the client
  reconnects, reads SEC_STATUS: CLAIMED → run SEC_AUTH with the stored key; success → mark it
  CONFIRMED; AUTH_FAILED → the device was claimed with a different key (an attacker, or a newer
  claim) → delete the stored key and tell the user. UNCLAIMED → the device never committed; delete
  the stored key and restart the claim.
- A failed persist answers STORAGE_ERROR and leaves the device UNCLAIMED.
- After a successful claim the device is CLAIMED immediately: every later connection needs
  SEC_AUTH. The open session created by CONFIRM lets the client continue (e.g. write the config)
  without a second handshake.

---

## 8. Session establishment and records — `CMD_SEC_AUTH` 0x0056

### 8.1 Decision: device key + fresh ECDH (forward secrecy)

Session keys are derived from K_dev **and** a fresh ephemeral-ephemeral ECDH, not from K_dev and
nonces alone. Justification:

- Forward secrecy against X (G3): a stolen HA backup or a future client compromise must not
  decrypt recordings of every past session, which v1 allowed (review W8).
- An unauthenticated active peer must not be able to obtain material for offline guessing of
  K_dev (G6): with the client proving first (§ 8.3), the device reveals nothing K-dependent until
  the client has proven knowledge of K_dev.
- The code is already there: the claim needs ECDH on every target, and on BG22 and nRF the PSA
  ECDH/keygen/export functions are already placed for the BLE security manager.

Cost, per connection, on top of key-only: one P-256 key generation and one ECDH on each side;
+160 bytes over the air (two points) and one extra handshake round trip compared with a 1-RTT
key-only design (2 RTT total). Latency: ECC on BG22 CRYPTOACC, nRF CC310/CRACEN and ESP32 mbedTLS
is estimated at tens of milliseconds to ~200 ms per operation (**not measured**; the plan has a
benchmark step); via an ESPHome proxy each RTT is ~50-150 ms. Expected handshake time:
0.3-1 s. Battery cost: negligible against a refresh. A key-only fallback is not specified.

### 8.2 Messages

| Sub | Direction | Body | Len |
|---|---|---|---|
| 0x01 HELLO | H->D | `[ver=0x02][frag_max:1][Qc:64][Nc:16]` | 82 |
| 0x01 HELLO reply | D->H | `[dflags:1][Qd:64][Nd:16]` | 81 |
| 0x02 FINISH | H->D | `[tag_c:16]` | 16 |
| 0x02 FINISH reply | D->H | `[tag_d:16][timeout_s:2 LE]` | 18 |

### 8.3 Computations

```
HELLO (client):   dc, Qc <- fresh; Nc <- RAND(16)
HELLO (device):   claimed? else NOT_CLAIMED. rate limit. tear down any live session (§ 8.6).
                  validate Qc -> BAD_POINT
                  dd, Qd <- fresh; Nd <- RAND(16)
                  Z  = ECDH(dd, Qc); destroy dd
                  Ta = "OD2/auth" || 0x02 || dflags || uid || Qc || Qd || Nc || Nd     (178 B)
                  PRK = CMAC(K_ext, Z)
                  precompute exp_c = KDF(PRK, "OD2/auth-c", Ta), tag_d = KDF(PRK, "OD2/auth-d", Ta),
                  k_h2d, k_d2h, sid; keep only these (not Qc/Qd/Z/PRK)
                  reply [dflags][Qd][Nd]
HELLO reply (client): Z = ECDH(dc, Qd); destroy dc; uid = the STORED uid for this key
                  same Ta, PRK (K_ext from the stored K_dev), tag_c = KDF(PRK, "OD2/auth-c", Ta)
                  send FINISH
FINISH (device):  ct_equal(tag_c, exp_c) else AUTH_FAILED (counts towards the rate limit;
                  discard pending)
                  session OPEN: install k_h2d, k_d2h in HAL slots; tx_ctr = rx_last = 0
                  reply [tag_d][timeout_s]
FINISH reply (client): ct_equal(tag_d, KDF(PRK, "OD2/auth-d", Ta)) else abort ("device failed
                  mutual authentication": wrong device, attacker) and disconnect
```

The device proves second: an attacker who connects and sends HELLO learns Qd and Nd only, which
do not depend on K_dev. An attacker who impersonates a device to a real client learns tag_c, a
function of K_ext and of an ECDH secret the attacker shares — enough for an offline guess of a
weak key. Claimed keys are 128-bit random and unguessable; migrated legacy keys may be weak
(§ 9.4, expert-review item E9).

### 8.4 Sealed record format

Keys: `k_h2d` and `k_d2h` (different for every session and direction). Counters: 32-bit,
per direction, the first sealed frame of a session in each direction uses counter **1**.

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

- **What each frame authenticates**: the opcode (AAD, H->D) — or the whole response including
  its status and echo (plaintext, D->H); the exact payload length (CCM encodes the message length
  in its first block B0, so the v1 inner `[len:1]` byte is dropped); the direction (separate key
  and the `dir` nonce byte); the counter (nonce); the session (keys are unique per handshake); the
  protocol version (AAD byte 0x02).
- `0xE5` is a status value no response uses (0x00 ACK, 0xFF NACK, 0xFE auth required; BG22's
  defective 0x01). On a connection with a live session, every device->host frame is either sealed
  (first byte 0xE5) or one of the plaintext frames of § 8.5.
- Empty payloads are legal (CCM with zero-length plaintext): e.g. a sealed CONFIG_READ request is
  `[00][40][ctr:4][T:12]`, 18 bytes. The current HAL contract ("at least one ciphertext byte")
  must be widened (plan WP2).
- **Tag length 12 bytes (96 bits).** Forgery probability per attempt 2^-96; a session is torn
  down after 3 bad tags (§ 8.6), so an online forger's total success probability is ~2^-94.4.
  96 bits keeps the existing HAL slot policy (`OD_HAL_CRYPTO_TAG_LEN 12`, PSA imports the key with
  that exact shortened-tag algorithm) and is within SP 800-38C's guidance for tags of at least 64
  bits; 128 bits would add 4 bytes per frame for no meaningful gain at these strike limits. Expert
  review E5.
- **Overhead**: H->D 2 + 4 + 12 = 18 bytes; D->H 1 + 4 + 12 = 17 bytes (v1: 29). Largest sealed
  payload in one 253-byte frame: H->D 235 bytes, D->H a 236-byte plaintext response. The existing
  "encrypted chunk budget" of 154 bytes (header CHUNK BUDGETS) may be raised to 230 for
  DIRECT_WRITE data under SEC2; until the header says so, clients keep 154.
- **MTU**: sealed frames are not fragmented. A sealed frame needs `len + 3 <= ATT MTU`, exactly
  like the plaintext protocol today, whose config and image chunks already need a large MTU (or
  ATT long writes for writes). SEC2 adds 18/17 bytes to every frame; at MTU 23 only commands with
  payloads <= 2 bytes and responses <= 3 bytes fit sealed. That is no worse in kind than today;
  ESPHome proxies and every supported central negotiate >= 185.

### 8.5 Receive rules

**Device (host->device):**

1. If the opcode is in the plaintext set (§ 6 rule 1): process as plaintext; a frame for such an
   opcode that is long enough to be sealed is still parsed as plaintext.
2. Otherwise, with no live session: FE.
3. With a live session, a gated frame shorter than 18 bytes (cannot be a sealed record) is
   answered with the sealed NACK `[FF][cmd_lo][0xF1]` (`OD_SEC_ERR_RECORD`, provisional), not
   with FE (the session exists), and counts nothing.
4. Counter check **before** decrypt: `od_nonce_window` semantics, unchanged — accept a counter
   greater than rx_last, or within the 256-entry backward window and not yet seen; reject 0.
   A rejected counter is answered with the sealed NACK `[FF][cmd_lo][0xF2]` (REPLAY) and does
   not count as a strike (lossy-link rule of `od_session.h`).
5. CCM decrypt-and-verify. Failure: sealed NACK `[FF][cmd_lo][0xF3]` (INTEGRITY), strike + 1;
   at 3 strikes tear down (§ 8.6). Plaintext of a failed frame is never read.
6. Only after the tag verified: commit the counter to the window, then dispatch.

**Client (device->host)**, while a session is live:

1. First byte 0xE5 and length >= 17: counter must be **strictly greater** than the last accepted
   device counter (the device emits in order on one ATT bearer; a gap is allowed, a repeat or
   decrease is not). Decrypt-and-verify; on failure discard the frame, count it, and after 3
   failures disconnect. Then parse the decrypted plaintext exactly like an unencrypted response.
2. Exactly `[0x00][cmd_lo][0xFE]` (3 bytes): **session gone** (expired, torn down, device
   rebooted). Drop session state; § 8.7.
3. A plaintext response to an opcode of the plaintext set (§ 6 rule 1) that the client sent:
   accept as plaintext (it is unauthenticated by design and must not be trusted for security).
4. A 4-byte `[FF][cmd_lo][sub][err]` to a SEC opcode the client sent: accept.
5. **Anything else is discarded** (this replaces py-opendisplay's "any notification < 31 bytes is
   plaintext" rule, review W7).

Device-sent sealed NACK codes 0xF1..0xF4 are carried inside a sealed D->H frame, so a client can
trust them. 0xF4 ROTATION_REQUIRED answers any command other than the B1 allow-list in a
legacy-key session (§ 9.4).

### 8.6 Session lifetime and teardown

- **Lifetime**: absolute, from FINISH (or claim CONFIRM). `timeout_s` = `SecurityConfig
  .session_timeout_seconds`; **0 means the default 3600 s** on 3.0 (it meant "never" on 2.x);
  values 1..59 are raised to 60. Snapshotted at session open and reported in the FINISH/CONFIRM
  reply and SEC_STATUS.
- **Teardown** (device): BLE disconnect of the session's connection; lifetime reached (checked on
  every inbound frame, as `od_session_alive` does now); 3 bad tags; a counter reaching
  0xFFFFFFFF in either direction; a HELLO (a new handshake replaces the session immediately — the
  client asked for it); a successful ROTATE_CONFIRM or UNCLAIM (after the sealed reply is
  queued); a factory reset; a config save does **not** tear down (the key is no longer in the
  config, § 9). Teardown releases both HAL slots and zeroes the state.
- **One connection, one session**: a session never survives the connection it was made on. The
  ESP32 target runs with `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1` (CONNECTION_POLICY.md); every target
  binds the session to the connection handle it was established on and tears it down when that
  handle disconnects.
- On disconnect the device MUST also discard any pending claim/handshake/reassembly and **drop
  notifications still queued for that connection** (od_txq), so nothing of an old connection can
  be delivered on a new one.
- **Deep sleep / power-off / reboot**: all RAM state is gone; the client's next connection starts
  with a HELLO. There is no session resumption.

### 8.7 Client rules for reconnects (py-opendisplay#141, opendisplay.org#95)

1. Session state lives in the connection object. On **any** disconnect (including the client's
   own, a proxy drop, the device going to sleep) the client deletes it — keys, counters, sid.
2. On every new connection to a claimed device the client sends SEC_AUTH HELLO before its first
   gated command. It does not "try the old session first".
3. Before HELLO the client drains pending notifications. While a handshake is running it accepts
   only fragments with the expected opcode and sub; anything else is discarded, and a handshake
   that does not complete within § 13 limits is restarted once from HELLO, then reported.
4. On SESSION_REQUIRED during a session: delete the session, run one HELLO, then retry the
   failed command **once** if it is idempotent (reads, config write as a whole, image upload from
   its START); a mid-stream PIPE/direct-write data frame is not retried — the transfer restarts.
5. Proactive renewal: at 90 % of `timeout_s`, between transfers, run HELLO (which replaces the
   session).
6. A client never sends v1 (0x0050 step 1/2) to a device it has ever seen report firmware major
   >= 3 or SEC_V2, and never uses a SEC2 K_dev as a v1 key (pinning, plan § 5.2).

Because every handshake binds both fresh ephemeral keys, both nonces, uid and dflags, a reply
from an earlier handshake or connection cannot complete a new one: it fails the tag check with
AUTH_FAILED instead of producing a confusing "mutual authentication" error on a session that
half-exists.

---

## 9. Key storage, migration and config interaction

### 9.1 The secret record

K_dev lives in its **own** persistent record, never in the config record:

```
offset 0  magic  "OS2K" (4)
       4  version 0x01
       5  origin  0x01 CLAIMED, 0x02 LEGACY
       6  reserved 0x00 0x00
       8  K_dev (16)
      24  crc32 (ISO-HDLC, as od_config_store) over bytes 0..23 (4)
      28 bytes total
```

| Target | Medium | Notes |
|---|---|---|
| esp32-idf | NVS, own namespace `od_sec`, key `k` (blob) | plaintext at rest (flash/NVS encryption not enabled; out of scope § 1.4.3). Erase = write zeros then `nvs_erase_key` (best effort, like `od_hal_nvs_secure`) |
| nordic-zephyr | NVS/settings, own ID distinct from the config record | the config-replace path that retires the old record on -ENOSPC (2x4 kB NVS) must never touch this ID |
| efr32bg22-slc | NVM3 object with its own key | ~28 B + NVM3 overhead; must be included in the page-capacity budget that gates the 3120 B config (plan WP2) |

Writes are atomic in the `od_hal_nvs` sense (whole old or whole new record readable after a
power failure). A record with a bad magic/CRC is treated as absent **only if** the config also
carries no legacy key; otherwise migration (§ 9.4) runs again. A record that is **present but
corrupt** (magic found, CRC or length wrong, or the medium reports a read error) puts the device
in a **LOCKED** state: it behaves as CLAIMED with a key nobody has (gated commands FE, SEC_AUTH
AUTH_FAILED, SEC_STATUS state 2 with flags bit 5 `KEY_CORRUPT`) until a factory reset. Fail-closed,
because the config of a claimed device may hold secrets that an "open" fallback would expose to
anyone. Only a record that is cleanly absent means UNCLAIMED. Maintainer question Q9 / expert
review E12.

### 9.2 Config packet 0x27 on 3.0 — the key field

Maintainer decision (2026-11-11): **no write-only secrets** on a claimed device. A sealed
CONFIG_READ returns the complete config, key included, so a backup taken over a session is a
complete restore image. K_dev is still *stored* only in the secret record; the config record's
0x27 key bytes are zero at rest, and the read path fills them in.

**CONFIG_READ (read side, "truthful")**

| Device | 0x27 `encryption_enabled` | 0x27 `encryption_key` |
|---|---|---|
| CLAIMED (sealed read; plaintext read is FE) | 1 | K_dev |
| UNCLAIMED (open read) | 0 | 16 x 0x00 |

On a CLAIMED device whose stored config has no 0x27 packet, the read inserts one (defaults:
timeout 0, flags 0, reset_pin 0xFF) at 0x27's canonical position, so every backup of a claimed
device carries its key; `total_len` in chunk 0 accounts for it. No CRC is computed over the read
image, so nothing else changes.

**CONFIG_WRITE (write side)** — the key is set **only** through the claim (§ 7) and rotation
(§ 10). The 0x27 key field of a written config must **match**:

| Device | 0x27 in the written config | Result |
|---|---|---|
| CLAIMED | key all-zero, or key == K_dev | accepted; the key is unchanged; `encryption_enabled` in the blob is **ignored** (any value; it cannot unclaim) |
| CLAIMED | any other non-zero key | **NACK `OD_ERR_CONFIG_KEY_MISMATCH` (0x11)**, nothing stored |
| UNCLAIMED | `encryption_enabled == 0` and key all-zero | accepted |
| UNCLAIMED | `encryption_enabled != 0` or a non-zero key | **NACK `OD_ERR_CONFIG_SECRET_NEEDS_CLAIM` (0x10)**, nothing stored (§ 9.3) |

"Must match" rather than "ignored" because a silently ignored key is a device the user believes
carries the key they wrote; a mismatch is loud. Accepting the all-zero key on a claimed device
lets clients that blank the field (and old read-modify-write tools, which never get a session and
so never reach this) write without knowing the key; accepting an equal key makes read → edit →
write round-trip exactly. Restoring a backup onto a *different* (or re-claimed) device: the client
must zero the 0x27 key and `encryption_enabled` before writing (else 0x10 / 0x11).

The config record is stored with 0x27's key zeroed and `encryption_enabled` 0, whatever was
written. `session_timeout_seconds`: § 8.6. `flags` bits 2-5 and `reset_pin`: FACTORY_RESET.md § 2.
`flags` bit 0 `OD_SECURITY_FLAG_REWRITE_ALLOWED` becomes **reserved** (maintainer decision
2026-11-11): Nordic's "erase and replace the config without a session" behaviour
(`targets/nordic-zephyr/src/od_cmd_config.c:142-155, 373-388`) is deleted, and no target allows an
unauthenticated config write on a claimed device. A 3.0 device that receives a config with bit 0
set **accepts the write** (if it passes the other rules), stores the byte as sent, returns it as
sent, and gives the bit **no effect**; clients write 0 and the protocol header renames it
reserved. Recovery from a lost key is the factory reset. Bit 1 SHOW_KEY_ON_SCREEN is likewise
reserved and ignored: the panel never shows the key (§ 9.6).
Factory-embedded configs (ESP32 `tryProvisionFactoryEmbed`, Nordic `factory_config.c`) must carry
neither a key nor any § 9.3 secret; a factory config that does is rejected like a write (logged,
not loaded).

### 9.3 Secrets need a key

Maintainer decision (2026-11-11): an **unclaimed** device refuses to **store** secrets. It stays
fully open otherwise.

**What counts as a secret.** There are no FindMy/FMDN provisioning opcodes in the protocol
(header 2.4; BG22 `opendisplay_ble.c` only syncs `od_net_a`/`od_net_g` from config 0x2D), so every
secret enters through a config write (0x0041/0x0042) or a factory config:

| Packet | Field(s) | "Carries a secret" when |
|---|---|---|
| 0x26 WifiConfig | `password[32]` | any of the 32 bytes is non-zero |
| 0x27 SecurityConfig | `encryption_enabled`, `encryption_key[16]` | `encryption_enabled != 0` or any key byte non-zero (§ 9.2) |
| 0x2D FindMyConfig | `eik[32]` (network G identity key), `net_a_adv_key[28]` (network A key), `advertisement_key[20]` (static network G EID) | any byte of any of the three non-zero |

Checked over **every** instance of the packet in the blob (singletons are not trusted to be
single). Not secrets: the SSID, `encryption_type`, `server_host`/`server_port` (an open network
with an SSID and no password is storable), FindMy flags/`fmdn_k`/curve/time bases with zero key
material, and everything else in the config. `net_a_adv_key` and `advertisement_key` are
broadcast and so not confidential; they are gated anyway (as the maintainer listed "network A and
G key material") because an open device must not be turnable by anyone in range into a beacon
registered to *their* tracker account, nor have its owner's FindMy registration replaced.

**What the device answers.** The whole write is refused: NACK
`[FF][41][0x10]` (single-frame) or `[FF][42][0x10]` (on the last chunk of a chunked write, where
the complete blob is first known), `OD_ERR_CONFIG_SECRET_NEEDS_CLAIM` (provisional, new
CONFIG_WRITE error byte; 0x10/0x11 chosen clear of the bare NACK and of the gate's 3-byte
`[00][cmd][FE|FF]` shapes). Nothing is stored; the previous config stays. Clients turn 0x10 into
"Claim this display first, then write the WiFi/FindMy settings."
Rejecting the whole write rather than dropping the secret fields is deliberate: a dropped field is
a device that ACKed a WiFi password and then never joins the network, with nothing telling the
user why; and it would break the 3.0 rule that a config is stored exactly as sent
(PROTOCOL_CHANGES_3.0 #10). Old clients see a 3-byte NACK, which they report as a failed write —
loud, not silent.

**On a claimed device** secrets are stored normally (the write is sealed).

**Removal.** Factory reset erases the whole config (FACTORY_RESET.md § 6). SEC_KEY UNCLAIM
(§ 10) **scrubs** the secrets first — rewrites the config record with 0x26 `password`, the three
0x2D key fields and 0x27 zeroed, everything else kept — and only then erases the secret record, so
a power failure between the two leaves a claimed device without secrets, never an open device
with them.

### 9.4 Devices upgraded from 2.x

A 2.x config is stored verbatim and may hold a v1 key (0x27), a WiFi password and FindMy key
material. 3.0 never writes them away silently at upgrade.

**(a) 2.x device without a v1 key, with stored WiFi/FindMy secrets.** Options:

| Option | Behaviour | For | Against |
|---|---|---|---|
| **A1 (recommended)** keep working, flag it | secrets keep working (WiFi joins, FindMy advertises); SEC_STATUS flags bit 3 `LEGACY_SECRETS_OPEN` and READ_IDENT flags bit 5 tell clients; any *new* write that carries a secret gets 0x10, so the first config edit in a new client forces the claim | no installed device breaks on OTA; the exposure is exactly what 2.x had (secrets readable by anyone in range) and closes at the claim | the exposure continues until the user claims |
| A2 require claiming | WiFi/FindMy disabled until claimed (secrets kept at rest) | no open device holds live secrets | every WiFi/FindMy device stops working at upgrade |
| A3 erase at upgrade | secrets zeroed on first 3.0 boot | nothing exposed | data loss, user must re-enter |

New clients show "This display stores a WiFi password / FindMy keys but is not claimed; anyone
nearby can read them. Claim it now?" whenever the flag is set.

**(b) 2.x device with a v1 key.** The device generates its key under SEC2; the 2.x key was chosen
(possibly typed, put in `?key=` URLs or the 2.x boot QR, read back by CONFIG_READ — review
W3/W9/W11) by a client. Options:

| Option | Behaviour | For | Against |
|---|---|---|---|
| **B1 (recommended)** migrate as a one-time credential, force rotation | first 3.0 boot moves the key into the secret record, origin LEGACY, and zeroes it in the config (two idempotent steps; a power cut between them repeats the first). The device is CLAIMED with `dflags.KEY_LEGACY = 1` and SEC_STATUS flags bit 4 `ROTATION_REQUIRED`. SEC_AUTH accepts the legacy key, but such a session only admits the plaintext set, SEC_KEY ROTATE / ROTATE_CONFIRM and CONFIG_READ (so a backup is possible); everything else answers the sealed NACK `[FF][cmd][0xF4]` ROTATION_REQUIRED. After ROTATE_CONFIRM the device holds a device-generated key and behaves normally | every device ends up with a device-generated key; secrets stay protected throughout (never open); one extra round trip in the client, done once | old clients are locked out (expected, decision 1); scripts that hard-code the old key break after rotation unless the library hands the new key back |
| B2 migrate and keep the legacy key as K_dev | as B1 without forced rotation; clients merely suggest rotating | least friction | weak/leaked 2.x keys live on indefinitely |
| B3 lock until factory reset | the device refuses everything gated until reset | simplest | every encrypted 2.x install needs physical access at upgrade |
| B4 drop the key | device opens at upgrade | — | security regression: the WiFi password etc. become readable by anyone. **Not acceptable** |

Client behaviour for B1: HA rotates automatically at the first successful SEC_AUTH and updates the
config entry before using the device; the website rotates and updates its stored key; py-opendisplay
raises `KeyRotationRequired` unless the caller passes a `on_key_rotated(new_key)` callback (it never
rotates silently, because the caller owns the key). Maintainer question Q10.

### 9.5 CONFIG_CLEAR 0x0045

Erases the config record only (and with it any secrets). K_dev, the claimed state and the image
store are untouched. A claimed device stays claimed with an empty config; the owner reconfigures it
over a session. Removing ownership remotely is SEC_KEY UNCLAIM (§ 10).

### 9.6 Where the key can be read

- Over the air the key appears only (1) wrapped in the claim reply (§ 7), (2) sealed in the
  ROTATE reply (§ 10) and (3) sealed in a CONFIG_READ on a claimed device (§ 9.2) — i.e. only to a
  peer that already holds it or is becoming its holder.
- The panel **never** renders the key and the boot QR **never** carries it: 3.0 deletes the
  `SHOW_KEY_ON_SCREEN` rendering and the key bytes of the boot payload
  (`od_boot_payload.c` `od_boot_payload_build` / `od_boot_url_build`, BG22
  `opendisplay_display.cpp`); the payload's key field is sent as zeros.
- Clients MAY show a key to their own user to move it to a second client (HA options flow "show
  key", `opendisplay key export`); that is client UX and the user's responsibility.

### 9.7 Keys in website URLs (`?key=`)

Kept, by maintainer decision (2026-11-11): restoring a device from a link that carries its key is
wanted. The exposure is real and documented here: a key in a query string lands in browser
history and sync, in the web server's and any proxy's access logs, in the `Referer` of requests
the page makes, in screenshots, and in every place the link is pasted or shared; anyone with the
link holds the device. Optional recommendations that limit it without removing the feature:

1. After reading the parameter, remove it from the address bar (`history.replaceState`) so it is
   not kept in history or re-shared by copying the address.
2. Never put a key into a link the site generates unless the user explicitly asks for a "link
   with key", and label that link as containing the key.
3. Also accept the key in the fragment (`#key=`), which browsers do not send to the server, and
   prefer it in generated links; keep accepting `?key=` for existing links.
4. Send `Referrer-Policy: no-referrer` (or `same-origin`) on the toolbox pages.
5. Offer "forget this key" for the browser's stored keys (`localStorage['opendisplay.deviceKeys']`).

---

## 10. Key rotation and unclaim — `CMD_SEC_KEY` 0x0057 (sealed only)

Sealed requests `[00][57][ctr][CCM(sub || body)][T]`; decrypted plaintext response frames are
`[00][57][sub][...]` / `[FF][57][sub][err]` inside a sealed D->H frame.

| Sub | Request body | Response body (sealed) |
|---|---|---|
| 0x01 ROTATE | empty | `[K_new:16]` — the device generates K_new = RAND(16), holds it pending in RAM |
| 0x02 ROTATE_CONFIRM | `[tag:16]`, tag = KDF(K_conf_new, "OD2/rotate-c", uid \|\| sid), K_conf_new = KDF(K_new, "OD2/k-conf", uid) | empty; then the device persists K_new (atomic replace) and tears the session down |
| 0x03 UNCLAIM | `[0xA5][0x5A]` (guard against a stray frame) | empty; then the device (1) scrubs the secrets from the config (§ 9.3: 0x26 password, 0x2D key material, 0x27 zeroed; atomic rewrite), (2) erases the secret record (secure erase where the HAL supports it), (3) tears the session down. The device is UNCLAIMED and open; the claim window is **not** reopened. A failure in (1) answers STORAGE_ERROR and leaves the device claimed |

Rules:

- Rotation follows the claim's pending/confirm pattern: a lost ROTATE reply or an abandoned
  rotation leaves the old key in force; a new ROTATE replaces the pending K_new; the pending key
  dies with the session. The client persists K_new (state UNCONFIRMED, old key kept) before
  ROTATE_CONFIRM, then re-runs SEC_AUTH with K_new; on AUTH_FAILED it tries the old key once and
  then keeps whichever worked.
- `sid` binds the confirmation to this session: a confirmation captured from one session is
  useless in another.
- After rotation, a client that held the old key (M) can no longer authenticate.
- **Re-claim** (new owner, lost key): factory reset (physical), then a normal claim. There is no
  remote re-claim of a claimed device.

---

## 11. LAN transport

Encrypted LAN stays TLS-PSK (protocol SECTION 9), with PSK = K_lan (§ 2.2) instead of v1's
`CMAC(K, "opendisplay-tls-psk")`. The TLS ciphersuite should be an (EC)DHE-PSK suite for forward
secrecy (not verified on the ESP32 build; out of scope here). The LAN transport is unverified on
hardware.

---

## 12. Compatibility

### 12.1 Clients x firmware

| Client \ device | 3.0 unclaimed | 3.0 claimed | 2.x (any) | legacy Firmware_NRF |
|---|---|---|---|---|
| new (SEC2 + v1) | open; may claim | SEC2 | v1 + mitigations | v1 + mitigations |
| old (v1 only) | open; a config carrying a key, WiFi password or FindMy keys is NACKed 0x10 (§ 9.3) | refused: v1 step 1 → `[00][50][05]`; gated commands → FE | v1 | v1 |

### 12.2 What an old client sees

- `CMD_AUTHENTICATE [00 50 00]` on a claimed 3.0 device: `[00][50][05]` AUTH_STATUS_V2_REQUIRED;
  py-opendisplay 7.x raises on the unknown status; HA reports a failed authentication. On an
  unclaimed 3.0 device: `[00][50][03]` NOT_CONFIG, which is what 2.x devices without a key answer.
- MSD bit 3 = 1 on a claimed device makes old clients ask for a key; the key they have (if the
  device was migrated) does not help them.
- HA ordering hazard: HA pushes BG22 OTAs itself. The HA release that offers 3.0 firmware MUST
  carry the SEC2-capable py-opendisplay (plan WP10).

---

## 13. Timing limits and rate limits

| Limit | Value |
|---|---|
| claim window | 600 s from a physical boot (§ 7.2) |
| BEGIN reply → NONCE | 30 s |
| NONCE reply → CONFIRM | 300 s (the user reads a code a slow panel may take 40 s to draw) |
| HELLO reply → FINISH | 30 s |
| fragment → next fragment of the same message | 5 s |
| device reply latency after a LAST fragment (ECC included) | SHOULD be <= 2 s; client timeout 10 s |
| handshake starts (BEGIN + HELLO) | <= 10 per rolling 60 s per device (as v1's limiter); the 11th → RATE_LIMITED. Each AUTH_FAILED also counts. |
| session lifetime | `timeout_s`, default 3600 s, absolute |
| bad tags per session | 3 → teardown |
| counter ceiling | 2^32 - 1 per direction → teardown, new HELLO |

The rate limiter is per device, not per peer (one central at a time). It is a DoS lever (§ 1.4.7).

---

## 14. Test vectors (must be produced by the reference implementation)

Real vectors cannot be computed by hand. **py-opendisplay's SEC2 module is the reference
implementation**; it generates `sec2_vectors.json` with fixed inputs, and the file is committed to
`Firmware_Unified/tests/vectors/` for the firmware host tests and to the website tests. The
website's JS implementation must reproduce every vector independently (WebCrypto ECDH + its own
CMAC/CCM), so two independent implementations agree before firmware is tested against them.

Each vector records every input and every intermediate value:

| Group | Inputs | Outputs to check |
|---|---|---|
| V0 primitives (sanity) | RFC 4493 CMAC vectors; SP 800-38C / CAVP CCM vectors with a 12-byte tag and 13-byte nonce, including zero-length plaintext; CAVP P-256 ECDH (KAS ECC CDH) vectors | as published |
| V1 KDF | K_in, Label, Context for every label of § 2.3 | KDF output; plus the full CMAC input bytes |
| V2 key hierarchy | K_dev, uid | K_ext, K_conf, K_lan |
| V3 claim, happy path | uid, dflags, dc, dd (private scalars), Nc, Nd, K_new, frag_max = 20 | Qc, Qd, Cd, Tc (hex), Z, PRK, k_wrap, W, code_raw, code (6 digits), K_conf, tag_c, tag_d, k_h2d, k_d2h, sid; **every fragment of every message as wire bytes** at frag_max 20 and 244 |
| V4 auth, happy path | uid, dflags, K_dev, dc, dd, Nc, Nd | Qc, Qd, Ta, Z, PRK, tag_c, tag_d, k_h2d, k_d2h, sid; wire fragments |
| V5 records | k_h2d, k_d2h; H->D frames for cmd 0x0040 (empty), 0x0041 (200 B), counters 1, 2, 255, 256, 2^32-1; D->H frames for an ACK, a NACK, a 0x73 notification | wire bytes |
| V6 rotation | K_dev, sid, K_new | K_conf_new, rotate tag |
| V7 negative | each of: Qc not on curve; Qc = identity encoding; X >= p; Cd mismatch; W with one flipped bit; tag_c from the wrong Tc (one byte of Nd changed); wrong uid; H->D frame sealed with k_d2h (reflection); replayed counter; counter 0; AAD byte 0x01 instead of 0x02; truncated tag; fragment index gap; body one byte long/short | the exact error (`OD_SEC_ERR_*`, sealed NACK code, or "client discards") |
| V8 migration | 2.x config blob with 0x27 enabled + key (+ WiFi password, + 0x2D keys) | resulting secret record bytes, rewritten config bytes, SEC_STATUS flags; the B1 allow-list (each opcode: allowed / 0xF4) |
| V10 secret detection | config blobs: empty WiFi password with SSID; one non-zero password byte at offset 31; two 0x26 packets, only the second with a password; 0x2D with only `advertisement_key[19]` set; 0x27 enabled with zero key; 0x27 key == K_dev on a claimed device; 0x27 other key; chunked write whose secret is in the last chunk | accept / NACK 0x10 / NACK 0x11, and the exact NACK frame (`[FF][41][10]` vs `[FF][42][10]`) |
| V11 config read | claimed device with and without a stored 0x27 | the exact read image incl. the inserted 0x27 and `total_len` |
| V9 code edge cases | code_raw values giving 000 000, 999 999, and the bias boundary | formatted code strings |

Firmware host tests (`tests/host/`) drive `od_sec2` with the HAL crypto bound to a host
implementation (OpenSSL or a vendored portable P-256 for the host only) and reproduce V1-V9 byte
for byte. The firmware cannot fix the device's random choices in production; the host tests inject
`dd`, `Nd`, `K_new` through a test-only RNG/keygen seam.

---

## 15. Needs expert review

Points where this document's cryptographic reasoning is **not** settled. Each must be checked by
someone with protocol-cryptography expertise before the wire format is frozen.

| # | Item | Why unsure |
|---|---|---|
| E1 | AES-CMAC as the 800-56C extraction step, with salt 0^16 (claim) and secret salt K_ext (auth), over a 32-byte P-256 x-coordinate | 800-56C permits CMAC for extraction; whether 128-bit output from a ~256-bit-entropy x-coordinate via CBC-MAC meets the extractor assumptions, and whether a secret salt changes the analysis, is asserted, not proven here |
| E2 | Single-block SP 800-108r1 counter-mode KDF with CMAC; SP 800-108r1's caution about CMAC-based KDFs (the modification that adds a "K0" value) | I believe the concern applies to multi-block outputs and that L = 128 (one block) sidesteps it; not verified against the document text |
| E3 | Commitment Cd = CMAC keyed by Nd (f4 pattern) and code = CMAC keyed by Nc (g2 pattern), with full 64-byte points and uid in the input; claimed ~1e-6 MITM success | the pattern is Bluetooth's and has been analysed in that setting; my adaptation (inputs, order, output truncation) has not been |
| E4 | Is a separate `OD2/commit` domain enough to keep the Nd-keyed CMAC from interacting with any other use of Nd? (Nd is also in Tc) | Nd is used as a key once and as data in Tc; believed harmless |
| E5 | CCM with 96-bit tags, 13-byte nonce built from dir + 32-bit counter + zeros, zero-length plaintexts, uniqueness of (key, nonce) relying on fresh per-session per-direction keys | standard, but the zero-padding and the reliance on key freshness for nonce uniqueness should be confirmed |
| E6 | Key hierarchy: one K_conf used for both claim tags and rotation tags (different labels and contexts); K_ext used as an extraction salt while K_conf is a KDF key | label separation should suffice; confirm |
| E7 | Peer-point validation actually performed by `psa_raw_key_agreement` on BG22 CRYPTOACC, NCS (Oberon/CC310/CRACEN) and ESP-IDF mbedTLS; P-256 point at infinity encodings | invalid-curve attacks would expose ephemeral keys (only ephemeral, but still); must be tested on each target with V7 vectors, not assumed |
| E8 | Device-side takeover in a MITM'd claim (the MITM sends a valid CONFIRM) — accepted as detectable-not-prevented; whether a button-press confirmation should be mandatory on devices with a button | a design decision with a crypto consequence |
| E9 | Client-proves-first ordering in SEC_AUTH to deny unauthenticated peers offline-guessing material; residual offline guessing by a device impersonator against weak legacy keys | believed correct; confirm there is no reflection of tag_c/tag_d between roles (labels differ) |
| E10 | W = CCM with a constant all-zero nonce under the single-use k_wrap | single-use key makes nonce reuse impossible; confirm no path reuses k_wrap (a retransmitted NONCE reply must be byte-identical, never re-encrypted with a new K_new under the same Tc) |
| E11 | 6-digit code: entropy (~19.9 bits) per attempt, the 10^-6 bound, and the effect of letting the user retry entry three times on the same claim (the code does not change between entries, so retries do not help an attacker) | confirm |
| E12 | The fail-open behaviour on a corrupted secret record (§ 9.1) | security vs. availability; flagged as Q9 too |
| E13 | Constant-time handling: tag compares use `ct_equal`; ECC scalar multiplication on ESP32 (software mbedTLS) and BG22 (CRYPTOACC) side-channel properties | not evaluated |
| E14 | LAN TLS-PSK with a static PSK derived from K_dev | no forward secrecy unless an (EC)DHE-PSK suite is negotiated |
