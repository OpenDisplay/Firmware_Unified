# Divergence matrix

Where the four repos' protocol semantics actually differ, verified by reading the
implementations side by side (2026-07-25). This is the input MIGRATION.md's "silent
behavioural divergence" risk asks for: every difference below must be resolved
*deliberately* when its subsystem is promoted to `shared/core`, and the resolution column
here is the proposed default. Citations are `repo-relative path:line` in the source repos;
`Firmware` cites are the local checkout unless marked `upstream/main`.

> **Currency: behavioural tables surveyed before `#124`; refreshed for `#124` on 2026-07-25.**
> `Firmware` is now at `2e2131b` (PR #124 — WiFi/LAN transport + tinfl inflate, +2007/-198).
> The §0 capability matrix, §3.1a/§3.1b (window and engine) and the new §9 (LAN transport)
> reflect it. Rows not
> marked `#124` were verified against the pre-#124 tree and are unaffected by that commit —
> it touched transport, inflate, and logging, not dispatch or TLV semantics. **`Firmware` is
> the reference implementation, so re-verify this file against it after every significant
> merge**, not only before a promotion.

Legacy `Firmware_NRF` is included only where it matters. Verified verdict: it is a **strict
subset** — no compression, no 0x76, no PIPE, no NFC, no LED_STOP, no CONFIG_CLEAR, no
READ_MSD handler, `ocrypto`-backed crypto, and its `CMD_DEEP_SLEEP` log string still says
"(0x0052)" (`Firmware_NRF/EPD/EPD_service.c:619-666`, `:658`). Dropping it loses no
protocol semantics.

## Reference implementation and direction of travel

**`Firmware` — specifically its ESP32 branch — models the feature set.** It is the most
actively developed, carries the widest capability coverage, and is where new protocol features
land first (WiFi/LAN transport and the tinfl inflate engine arrived in `#124`, 2026-07-25,
ESP32-only). When a behaviour is contested and no correctness argument decides it, `Firmware`'s
ESP32 shape is the default answer.

The other repos — `Firmware_NRF54`, `Firmware_Silabs`, `Firmware_NRF` — **should converge
toward it.** That is the default direction for anything they implement differently for no
reason beyond history.

Three qualifications, all load-bearing:

1. **"Reference" is about feature coverage, not correctness.** Where another repo is
   demonstrably right, it wins and `Firmware` changes — the resolution column below already
   does this repeatedly (NRF54's size-table TLV scan §2.2, its chunked-write validation §2.3,
   its mid-session plaintext rejection §1.5a). Modelling the feature set does not make
   `Firmware` the arbiter of every detail.
2. **Other repos will sometimes have *more*.** `CMD_NFC_ENDPOINT` is implemented on Silabs and
   NRF54 and *not* on `Firmware`; Channel Sounding is nRF54L15-only. Convergence is not
   one-way.
3. **Other repos will sometimes have *less*, permanently, and that is fine.** BG22 will never
   run PIPE or `0x76`; a gap is not a defect and not a backlog item. See SHARED_API_DESIGN.md
   § "Feature parity across targets is **not** a goal" for how the API expresses this.

**The canonical protocol headers are what bind them together.**
`opendisplay_protocol.h` and `opendisplay_structs.h` are the single contract every repo
implements a subset of: they own the wire values, the config TLV types, and the error codes.
A capability may be absent from a target, but its *opcode and encoding never differ* between
targets — a divergence in wire meaning is always a bug, whereas a divergence in coverage is a
design decision. That distinction is the organising rule for everything below.

### The opcode space is conservative

**Avoid introducing new opcodes.** Every opcode is permanent wire surface: it has to be
implemented or deliberately NACKed on four targets, taught to `py-opendisplay` and the HA
integration, and supported for the life of every deployed device. The cost is paid by
everything downstream, forever, and it is paid even by targets that will never implement it.

Before proposing one, exhaust the cheaper options in this order:

1. **An existing opcode with an existing field.** Most "new" needs are a value, not a verb.
2. **An existing opcode with a reserved or extension field.** Several responses already carry
   documented reserved bytes (the 4-byte ack's two trailing zeros, §1.4).
3. **A new error/status code in an existing namespace.** The "subsystem compiled out" case is
   exactly this — a target that lacks `0x76` or PIPE needs a *code* meaning "unsupported", not
   a new opcode (see §1 opcode coverage, where `OD_ERR_PIPE_START_BAD_HEADER` is currently
   misused for it).
4. **A new packet type in the config TLV** — cheaper than a new opcode *in principle*, but see
   the caveat below before relying on it. Prefer reserved bits, then reserved bytes, in an
   existing packet first (ARCHITECTURE.md § "The config binary is backward compatible").

   > **Caveat: unknown-type skip is only safe on NRF54 today.** `Firmware` and `Firmware_Silabs`
   > force skip-to-CRC on an unrecognised packet type, discarding everything after it —
   > including `0x27` security config if the new type is ordered ahead of it (§2.2). Adding a
   > packet type would therefore break deployed units on two of three targets. This option
   > re-opens once NRF54's size-table parser is the one in `shared/core`.
5. **A new opcode** — last resort.

**But do propose one when it genuinely simplifies the structure.** This is not a prohibition.
If a new opcode removes a class of special-casing, collapses several overloaded meanings of an
existing opcode, or replaces a fragile inference with an explicit signal, that is a real
simplification and worth the wire cost. Two live candidates from this document:

- **Capability / limits discovery.** ~~`MAX_CONFIG_SIZE` diverges (2048 on BG22, 4096
  elsewhere, §2.7) with no way for a host to learn the limit.~~ **This candidate lost its
  motivating case on 2026-07-25**: `MAX_CONFIG_SIZE` is now 4096 fleet-wide, so there is no
  per-device limit to discover and no oversized-config truncation to report. Limits discovery
  may still be wanted for the remaining values (max accepted `windowBits`, max frame size), but
  it must now be argued on those alone — which is a materially weaker case than the one that
  put it on this list. Whether it becomes a new opcode, an extension of
  `0x43 FIRMWARE_VERSION`, or a config TLV field remains the kind of trade-off to argue
  explicitly rather than settle by whoever implements first.
- **Explicit compression signalling.** Direct-write infers "compressed" from payload length
  rather than a flag (§3.1). It works, but it is inference where a bit would do.

A proposal must state: what it replaces, why options 1-4 cannot carry it, and the cost to every
target including those that will NACK it. Land the argument in these docs first — the canonical
headers are frozen, so no opcode can be added unilaterally regardless.

**This document is maintained, not a one-time survey.** Update it when a feature lands in any
repo, when a target adds or drops a capability, and when a subsystem is promoted to
`shared/core`. The capability matrix in § "Opcode coverage" is the quick answer to "what does
this repo support"; the behavioural tables are the detail.

## Reading the table

- **spec** = the canonical header (`shared/protocol/opendisplay_protocol.h`, 2.2+unreleased).
- "all three" = `Firmware` (ESP32/nRF52840), `Firmware_NRF54`, `Firmware_Silabs`.
- Where implementations unanimously contradict the spec, the fix is a **spec correction**
  (recommendation only — the header is frozen for this exercise), not three firmware changes.

## 0. Capability matrix — what each repo supports

The non-opcode half of "what does this repo do". Opcode-level coverage is in § "Opcode
coverage" below. `Firmware` is split where its two chip families differ, because they do.

| Capability | Firmware / ESP32 | Firmware / nRF52840 | NRF54 | Silabs | NRF legacy |
|---|---|---|---|---|---|
| BLE transport | NimBLE | Bluefruit | Zephyr BT host | Silabs BGAPI (`sl_bt_*`) | Nordic SoftDevice |
| WiFi / LAN TLS transport | **yes** (`#124`) | no | no | no | no |
| Inflate engine | **tinfl ROM** when `OPENDISPLAY_ENABLE_WIFI`, else uzlib (`#124`) | uzlib | uzlib | uzlib | **none** (uncompressed only) |
| Inflate RAM cost | **~11 KB** tinfl tables + 4 KB dict (9-bit window) vs ~2.5 KB uzlib | ~2.5 KB | ~2.5 KB | 1676 B measured | n/a |
| zlib window default | 512 B (32 KB on `esp32-s3-E1004` only) | 512 B | 512 B | 512 B | n/a |
| Panel backend | bb_epaper SPI + **FastEPD parallel** (S3) | bb_epaper SPI | bb_epaper SPI | bb_epaper SPI | direct driver |
| `MAX_CONFIG_SIZE` | 4096 | 4096 | 4096 | **2048** → 4096 (decided 2026-07-25, §2.7) | — |
| Crypto backend | mbedTLS CCM | mbedTLS CCM | PSA | PSA | `ocrypto` |
| PIPE sliding window `0x80`-`0x82` | **yes — only implementation** | no | no | no | no |
| Partial region `0x76` | yes | yes | yes | no (fail-fast NACK) | no |
| NFC endpoint (TNB132M) | no | no | **yes** | **yes** | no |
| Buzzer | yes | yes | yes | no (NACK) | no |
| Channel Sounding / ranging | no | no | **yes** (`device_flags` bit 5, default off) | no | no |
| Field OTA (host-driven) | **none** — `ENTER_DFU` only reboots | **yes** — Adafruit bootloader BLE DFU (`bledfu.begin()`, `CMD_ENTER_DFU` sets the bootloader magic), driven by `perform_nrf_dfu`; not wired into HA (verified 2026-07-25 — targets/nordic-zephyr/README.md § "Deployed nRF52840") | firmware-complete (MCUboot + SMP BT OTA DFU) but **undriveable** — `py-opendisplay` has no SMP/mcumgr client (targets/nordic-zephyr/README.md § Bootloader) | **yes** — `.gbl` via Silabs AppLoader, the only path wired into HA | library support exists (`perform_nrf_dfu`, legacy Nordic DFU `.zip`) but unused by HA |
| Kernel | Arduino loop / FreeRTOS | Arduino loop | Zephyr | **none** (superloop) | none |
| Config storage | LittleFS/NVS | NVS | Zephyr settings | NVM3 | flash |

Bold marks a capability unique to one column, in either direction — those are the cells that
show convergence is not one-way. `Firmware`'s ESP32 column is the widest, which is what
"models the feature set" means concretely; Silabs is the narrowest and defines the floor
`shared/core` must fit through.

Keep this table current as features land. A cell that changes without the table changing is
how the four repos drifted apart the first time.

## 1. Framing and dispatch

> **Status, 2026-08-16 (C8-C11).** This section was written against the four source repos before
> any promotion. Dispatch is now ONE implementation: `shared/core/od_dispatch.c` owns validation,
> tag liveness, producer conflict, budget, reservation, the gate and **the opcode map**; targets
> supply named per-command hooks (`shared/core/od_cmd_app.h`) and nothing else. The line references
> below point at the donor repos and are historical — read them as "where the divergence was", not
> as "where the code is". Resolutions marked below as adopted have been adopted on `esp32-idf` and
> `nordic-zephyr`; C13 moved `efr32bg22-slc` onto the same dispatcher in software on 2026-08-17,
> and every BG22 row below is hardware-unverified until the C13 Gate 2 run. Rows are CLOSED by
> stating what replaced the divergence, not by deleting the donor evidence that established it —
> this file is the record of how four repos drifted, so the forensics outlive the fix.
>
> Three rows have moved since: **1.6** no longer uses a global at all (§ 1.6), **1.7** is the
> shared `od_rxq` ring plus each target's pump (§ 1.7), and **0x52 on NRF54** is now the compliant
> NACK (opcode coverage table). NOT hardware-verified on either target.

| # | Behaviour | spec | Firmware | NRF54 | Silabs | Resolution for `shared/core` |
|---|---|---|---|---|---|---|
| 1.1 | Auth-required reply | `[0xFE][echo]` (status byte 0xFE) | `{0x00, cmd_lo, 0xFE}` — 0xFE as *data* (`src/communication.cpp:630,637`) | same (`src/opendisplay_pipe.c:277`) | same (`opendisplay_pipe.c:227`) | Implementations are unanimous and clients parse the shipped shape. **Correct the spec** to `[0x00][echo][0xFE]`; do not change firmware. |
| 1.2 | Decrypt-failure reply | not specified | `{0x00, cmd_lo, 0xFF}` (`communication.cpp:668`) | same (`opendisplay_pipe.c:1349`) | same (`opendisplay_pipe.c:1244`) | Adopt as-is; document in spec. |
| 1.3 | Oversize inbound frame | reject via ATT 0x0D (declared GATT max) | GATT `max_len` = `OD_BLE_MAX_FRAME` rejects at ATT layer (`src/ble_init.cpp:277-286`) | app-level NACK `{0xFF, cmd_lo, 0xFE}` (`opendisplay_pipe.c:1333-1337`) | same app-level NACK (`opendisplay_pipe.c:1231-1235`) | **DONE (C8/C12), and the shape needed settling too.** Both belts are kept: each target declares its GATT max and `od_dispatch.c` keeps the core-level check, which LAN needs because it has no ATT layer. The REPLY was the loose end this row left open -- it said to keep the check, never what to answer -- so C8 picked `[00][cmd][FF]` and C10 carried it onto Nordic, silently replacing the `[FF][cmd_lo][FE]` both donors shipped. That is a wire regression on two counts: a leading `0x00` is the status-as-data ACK family rather than a hard NACK, and `[00][cmd][FF]` is ALREADY the decrypt-failure answer (row 1.2), so a host could not tell an oversize frame from a failed decrypt. C12.1 restored the donors' bytes and `tests/vectors/dispatch.json` now pins all three of them, checked against production dispatch by `tests/host/corpus_runner.c`. |
| 1.4 | Ack frame width | 2-byte `[0x00][echo]` | config/LED/buzzer acks are **4-byte** `{status, echo, 0x00, 0x00}` (`communication.cpp:476-486`); direct-write acks 2-byte | same split (`opendisplay_pipe.c:811-812, 891-892, 743-755`) | same | Adopt the shipped shapes verbatim (clients depend on them); spec should document the two trailing zero bytes as reserved. |
| 1.5 | Plaintext exemptions when security on, pre-auth | AUTHENTICATE + FIRMWARE_VERSION always plaintext; Silabs also READ_MSD | exempts AUTH + FW_VERSION before the gate (`communication.cpp:613-621`); config write/chunk pass if `rewrite_allowed` flag, after secure erase (`:447-455, 508-517`) | same policy (`opendisplay_pipe.c:1187-1214`), plus rejects *plaintext* non-FW_VERSION commands **mid-session** (`:1356-1370`) | dispatch auth-gates everything except AUTHENTICATE (`opendisplay_pipe.c:1079-1084`); READ_MSD forced plaintext in `pipe_send` (`:532`) | **NRF54 is right** (most complete and matches spec). Shared dispatcher fixes both Silabs issues on adoption. |
| 1.5a | **Encrypted-frame detection when security on** | gate on `isEncryptionEnabled()` | length gate: decrypt only if `len ≥ BLE_CMD_HEADER+NONCE+TAG` else treat as auth-required (`communication.cpp:635`) | length gate `frame_len ≥ 31`, but **also** rejects short plaintext mid-session (`opendisplay_pipe.c:1342-1370`) — closes the hole | length gate `frame_len ≥ 31` with **no mid-session guard** (`opendisplay_pipe.c:1238-1251`) | **Security bug on Silabs**: once any client authenticates, a *short* (<31 B) plaintext command — REBOOT (2 B), DEEP_SLEEP, DIRECT_WRITE_END — bypasses CCM and executes, because `dispatch()` only blocks when there is *no* live session. NRF54's mid-session plaintext rejection is the correct model; shared dispatcher must gate on `sec_enabled()`, never on frame length. **C13 (2026-08-17):** closed on BG22 in software. Shared dispatch gates on `od_session_security_enabled()`, so the length gate is gone on all three targets and a 2-byte plaintext REBOOT mid-session is refused. Hardware row § 7 still outstanding. |
| 1.5b | NACK confidentiality | unspecified | NACKs (0xFF) sent unencrypted (`communication.cpp:218`) | same (`opendisplay_pipe.c:571`) | same — every NACK forced plaintext (`opendisplay_pipe.c:532`) | Unanimous; document. A NACK leaks only an opcode+error code, so plaintext is acceptable, but make it a deliberate rule in `shared/core`. |
| 1.6 | Origin-gated decrypt (LAN TLS bypasses CCM) | SECTION 9 rule 4 | was `g_commandOrigin`, a per-frame fact in a global | n/a (no LAN) | n/a | **DONE (C8/C11).** `od_dispatch_frame()` takes an `od_reply_t` (origin + tag) built by the ingress that has it — BLE from the RX slot's tag, LAN from the live owner word — and `shared/core/od_dispatch.c` gates decrypt on `rp->origin != OD_ORIGIN_LAN_TLS`. No global; `tools/check.sh` ratchets the old names' absence. |
| 1.7 | Execution context | unspecified | was a `commandQueue` plus a 10-slot response ring | was a `K_MSGQ` (40 × 509 B), main thread drained it | dispatch runs directly in the BGAPI event handler, i.e. superloop context (`opendisplay_pipe.c:1271-1298`) | **DONE (C9/C10/C11).** One SPSC `od_rxq` on both targets, peek/consume, every slot carrying its writer's identity; egress is `od_txq`. Each target keeps its own pump — ESP32's `serviceBleRx()` in `loop()`, Nordic's `opendisplay_pipe_process()` — because the surrounding work differs; both are bounded to one ring per pass. Depth derives from the target's own `PIPE_MAX_W + 2`. Silabs unchanged. **C13 (2026-08-17):** BG22 joins with NO application RX ring, deliberately. `sl_bt_can_process_event()` retains the vendor event while a config producer or a queued response is live, so arrival context is consumer context and `OD_FRAME_DEFERRED` stays unreachable; a 2,000 ms target-level hold deadline resets stuck command transport rather than holding BGAPI buffers indefinitely. Egress is `od_txq` on all three. |
| 1.8 | Long-write reassembly (ATT prepare/execute) | unspecified | not needed (NimBLE hands whole values) | rejects > 509 B at msgq bound (`opendisplay_pipe.c:75, 1380`) | explicit offset-based reassembly into `s_long_write_buf` (`opendisplay_pipe.c:1209-1227`) | Keep reassembly in the target BLE glue, not in core — it is a transport artifact. **C13 (2026-08-17):** BG22 raised the characteristic to 253 and selects max ATT MTU 256, then **removed** its `s_long_write_buf` staging — writes now dispatch only at `offset == 0`. That is sound for peers that negotiate the fleet maximum and NOT for a peer that negotiates lower and uses ATT prepare/execute: those fragments are dropped, and the offset-0 fragment is dispatched as if whole. Recorded as a deliberate capability reduction, not a no-op; restore staging if a low-MTU client is ever supported. |
| 1.9 | Unknown opcode | no reply specified | logs, **no reply** | logs, no reply | logs, no reply (`:1188-1190`) | **DONE (C11).** One answer, from the shared map's `default:` arm — no reply, and `od_frame_policy()` gives `OD_FRAME_UNKNOWN_OPCODE` no activity stamp, so unknown-command traffic cannot hold an exclusive link open. Nordic still logs the opcode, from its pump (the one place with both the bytes and the verdict); shared/ cannot log. Clients must use timeouts. |

### Opcode coverage (implemented handler exists)

| Opcode | Firmware | NRF54 | Silabs | NRF legacy | Notes |
|---|---|---|---|---|---|
| 0x0F REBOOT | yes | yes | yes | yes | |
| 0x40/41/42 CONFIG R/W/CHUNK | yes | yes | yes | yes | |
| 0x43 FW_VERSION | yes | yes | yes | yes | all reply `[00][43][maj][min][sha_len][sha≤40]` — the `sha_len` byte is not in the spec's layout (`communication.cpp:382-392`, `opendisplay_pipe.c:587-608`) |
| 0x44 READ_MSD | yes | yes | yes | **no** (macro only) | |
| 0x45 CONFIG_CLEAR | yes | yes | **no case — silent drop** (grep: zero hits in `opendisplay_pipe.c`) | no | **spec error**: `@targets` for 0x45 lists Silabs. Either implement on Silabs or fix `@targets`. **C13 (2026-08-17):** BG22 implements it — `od_cmd_app_config_clear` deletes the NVM3 record, queues the ACK under the old session, reloads and clears. A defined hook that returns `OD_CMD_UNKNOWN` would still be silent, so link success is not the proof; `tests/host/silabs_fault_test.c` asserts the success and persistence-failure orderings. The canonical header still contradicts itself (`:238` legend vs `:367` opcode block) — unchanged by C13. |
| 0x50 AUTHENTICATE | yes | yes | yes | yes | |
| 0x51 ENTER_DFU | yes | yes | yes | yes | |
| 0x52 POWER_OFF | yes (latch HW) | **NACK unsupported** — `{FF,52,OD_ERR_POWER_OFF_UNSUPPORTED,00}` (C11, `od_cmd_device.c`) | NACK unsupported (`:1114-1123`) | no | **CLOSED (C11).** The NRF54 gap was a silent drop, which left a host unable to tell "no rail cut here" from "firmware older than the command". Silabs was the compliant model and Nordic now matches it. The 4-vs-3-byte width contradiction in the canonical header is unresolved — `FOLLOWUPS.md` § 2.1. |
| 0x53 DEEP_SLEEP | yes | recognized, deliberately **no reply** (`:1248-1254`) | ACKs then EM4, ignores payload (`:1124-1130`) | yes (opcode drift, see above) | spec permits both ("may instead stay silent"); shared handler should emit the proper NACK namespace and make silence unnecessary. |
| 0x70/71/72 DIRECT_WRITE | yes | yes | yes | yes (uncompressed only) | |
| 0x73/0x75 LED | yes | yes | yes | 0x73 only | |
| 0x76 PARTIAL | yes | yes | NACK `{FF,76,07,00}` fail-fast (`:1177-1186`) | no | Silabs fail-fast NACK is correct behaviour for an unsupporting target — adopt as the default for any compiled-out subsystem. |
| 0x77 BUZZER | yes | yes | NACK `{FF,77,07,00}` | no | |
| 0x80-0x82 PIPE | yes | **yes** (`opendisplay_pipe_write.cpp`; the row's "no" predates the NRF54 port) | no (silent drop) | no | Silabs should NACK the 0x80 START rather than silently drop (`OD_ERR_PIPE_START_BAD_HEADER` is wrong; needs an "unsupported" code — spec gap). ESP32 refuses PIPE on LAN with that same reused code, post-gate, for want of a better one — SECTION 9 rule 2. **C13 (2026-08-17):** BG22 answers 0x0080 with `{FF,80,04,00}`, using the unused PIPE-start value 0x04 as a TARGET-SPECIFIC unsupported marker, and keeps 0x0081/0x0082 silent (a NACK to a DATA frame is fatal to an upload loop). 0x07 was rejected precisely because it means `OD_ERR_PIPE_START_RECT_INVALID` in the PIPE namespace, which would tell a host to retry with different geometry. This does not add a canonical symbol; a protocol-wide unsupported code would supersede it — FOLLOWUPS. |
| 0x83 NFC | **UNKNOWN, silent** — the capability-off arm of `od_nfc_frame()` | yes | yes | no | SUPERSEDED BY PHASE 4 (§ 10): the opcode routes to `shared/core/od_nfc.c` and no target defines a hook. ESP32 builds it at `OD_CAP_NFC=0`, whose arm answers nothing and stamps no activity — deliberately not an "unsupported" NACK, which would mean inventing a wire code the client cannot tell from firmware older than the command. |

## 2. Config TLV parsing

Three parsers (`Firmware/src/config_parser.cpp` 919 C++, `Firmware_NRF54/src/opendisplay_config_parser.c` 696 C, `Firmware_Silabs/opendisplay_config_parser.c` 529 C) shared the same outer container: toolbox-outer CRC16-CCITT-FALSE with the two length bytes fed as zeros (`config_parser.cpp:252-268`, NRF54 `:70-95`, Silabs `:17-45` — Silabs alone used the `OD_CONFIG_CRC_*` named constants, which were worth keeping).

All three have now been replaced by `shared/core/od_config{,_tlv}.c`. BG22's former 529-line parser
is a small NVM3/logging adapter as of C13 (2026-08-17), hardware-unverified.

| # | Behaviour | Firmware | NRF54 | Silabs | Resolution |
|---|---|---|---|---|---|
| 2.1 | Packet-type coverage | 14 types; **no 0x2A NFC** (`config_parser.cpp:329-614`) | size-table for all types, cross-checked against three sources (`:105-109`) | 15 types incl. 0x2A NFC; skips 0x26/0x28/0x29/0x2C as host-only (`:103-463`) | Shared parser covers the full `opendisplay_structs.h` set; per-target `#if` only for *applying* a packet, never for parsing it. **Firmware's missing 0x2A is a live defect, not a capability gap (established 2026-07-25).** `OD_PKT_NFC = 0x2A` is canonical schema, and Firmware forces skip-to-CRC on any unrecognised type (2.2) — so an ESP32 receiving a config that merely *mentions* NFC abandons the rest of the blob at that point, losing `0x2B flash_config` and `0x2C data_extended`. `0x27 security` survives only because it sorts ahead of `0x2A`; that is emission order, not a guarantee. ESP32 is entitled to not *support* NFC (`OD_NFC_ENABLE=0`); it is not entitled to lose the packets that follow. Fix on promotion via the NRF54 size table. |
| 2.2 | Unknown packet type | skip to CRC | **size-table skip**: known-size packets are stepped over; only genuinely unknown IDs force skip-to-CRC, plus a `rescan_security_packet` fallback so 0x27 after an unknown ID still loads (`:42-46, 621-623`) | skip handling per-type | **NRF54 is right** — its comment documents the exact bug the others have (a new packet type ahead of 0x27 silently drops security config). Take the size-table + ordered-scan design. |
| 2.3 | Chunked-write validation | does **not** validate declared total ≤ `MAX_CONFIG_SIZE` at START; commits when `receivedChunks ≥ expectedChunks` without checking `receivedSize == totalSize` (`communication.cpp:456-478, 527-537`) | validates total at START (`opendisplay_pipe.c:906`), rejects END with `received_size != total_size` (`:1004-1009`), binds the chunk context to a connection (`:901, 972`) | as NRF54 minus some checks | **NRF54 is right**; adopt all three tightenings. **C13 (2026-08-17):** closed by `od_config_asm` on all three. BG22 additionally rejects a declared total above its compiled 2,048-byte buffer at the start frame, before any byte is stored — the bound added in C13.0a, without which a 2,049..4,000 declaration overflowed the buffer. |
| 2.4 | Session invalidation after config save | `clearEncryptionSession()` in `reloadConfigAfterSave` (`communication.cpp:67`) | `clear_session()` on every save path (`opendisplay_pipe.c:939, 960, 1012`) | **never clears the session after a save** (`clear_session` call sites: `:118, 431, 621, 673, 1263` — none post-save) | **Security-relevant Silabs bug**: change the encryption key over an old session and the old session keeps working. Firmware/NRF54 behaviour wins. **C13 (2026-08-17):** closed on BG22. Order is persist → seal/queue the ACK under the OLD session → reload → `od_session_clear()`; a persistence failure emits no success ACK and performs neither reload nor clear, leaving the prior config and session usable. Pinned both ways by `tests/host/silabs_fault_test.c`. |
| 2.5 | Config-read scratch | shared 4 KB scratch, `getConfigScratch()` — deliberate stack-overflow avoidance (`communication.cpp:395-399`) | **own 4 KB static** `config_data[MAX_CONFIG_SIZE]` (`opendisplay_pipe.c:824`) on top of the 4 KB chunk buffer — ~20 KB of config buffers total across the file | shared scratch `opendisplay_config_buf()` (`opendisplay_pipe.c:725-727`) | One shared scratch (Firmware/Silabs pattern). On BG22 the NRF54 pattern would waste 4 KB of a 32 KB chip. **C13 (2026-08-17):** BG22 now has exactly ONE 2,064-byte config object: `struct od_config_asm` and the NVM3 record are a union sharing a buffer at offset 16, so the assembler state words become the record header at write time. Layout is held by two static asserts; the sequencing that makes it safe is pinned by `tests/host/silabs_storage_test.c`. A read is refused while assembly owns the buffer, and `od_cmd_mutates_config()` defers 0x41/0x42/0x45 while a read producer holds it. |
| 2.6 | Read chunk cap | `(MAX_CONFIG_SIZE+93)/94` (`communication.cpp:405-408`) | same, with the derivation comment (`:826-832`) | `ceil(MAX_CONFIG_SIZE / (MAX_RESPONSE_DATA_SIZE-6))` — same value, cleaner form (`:729-735`) | Silabs form. |
| 2.7 | **`MAX_CONFIG_SIZE`** | — | **4096** (`config_parser.h:7`) | **4096** (`opendisplay_config_storage.h:18`) | **2048** (`opendisplay_config_storage.h:7`, NVM3 record 2064 B) | **RESOLVED 2026-07-25 — 4096 fleet-wide, BG22 raised to match.** A single product-wide constant, *not* a per-target macro: any device accepts up to 4096 bytes of config, so the divergence is removed rather than made discoverable. Three consequences. (1) `py-opendisplay` needs no change — it already hardcodes 4096 (`config_serializer.py:672-675`), so the host-side work item F5 opened is closed by the decision instead. (2) `MAX_CONFIG_SIZE` drops out of the capability-reporting reserved bytes (ARCHITECTURE.md § "The gap, and a proposed fix"), since there is no longer a per-device number to report. (3) **The cost is BG22-only and material** — +2048 B of NVM3 record and +2048 B of read scratch against a 10 576 B heap; the two mitigations in MEMORY_CONSTRAINTS.md item 3 (shared scratch, bitmap replay window) become required rather than optional, and NVM3 max-object-size and instance capacity must be verified before the Silabs swap. **4096 is the ABSOLUTE ceiling — storable and transferable (2026-07-25).** Only 4000 is reachable today (`MAX_CONFIG_CHUNKS` 20 × 200 B, enforced on all three targets), so `MAX_CONFIG_CHUNKS` must become **21** = `ceil(4096/200)` — a queued canonical-header change, blocked by the freeze. Until then hosts must cap at 4000, and deployed units enforce 20 until reflashed. See FOLLOWUPS.md § 3.1. **C13 (2026-08-17):** **REVERSED for BG22.** `OD_CONFIG_MAX_SIZE` becomes an `#ifndef`-guarded target macro; BG22 sets 2048 and ESP32/Nordic keep 4096. The 2026-07-25 argument above is not withdrawn — it is outweighed on this part, where 4096 costs +2 KB of NVM3 record against a ~10.5 KB heap. The divergence a host cannot discover therefore RETURNS, and the mitigation is that BG22 refuses rather than truncates: a declared total above the cap is rejected at the start frame with nothing stored. That refusal depends on the runtime bound added in C13.0a — before it, a cap below 4,000 was a remotely triggerable buffer overflow. CLAUDE.md decision 12 is amended to match, and records both conditions rather than just the new number. |
| 2.8 | Outer CRC16 enforcement | advisory only — mismatch logs, config still applied (`config_parser.cpp:665-672` region) | advisory only (`opendisplay_config_parser.c:657-670`) | advisory only (`opendisplay_config_parser.c:490-498`) | **Unanimous** — the toolbox-outer CRC16 is never enforced anywhere; a second inner CRC32 in the storage record *is* enforced on load. So there is no divergence, but `shared/core` should decide deliberately whether to start enforcing CRC16 (it currently protects nothing). |

## 3. Direct-write 0x70/0x71/0x72

| # | Behaviour | Firmware | NRF54 | Silabs | Resolution |
|---|---|---|---|---|---|
| 3.1 | Compressed START detection | `len >= 4` ⇒ compressed, `[uncompressed_size:4 LE]` validated against computed panel geometry (`display_service.cpp:2138-2148`). **No flag byte** — length is the only signal | same wire contract via `opendisplay_display_direct_write_start` (`.cpp:771`) | same (`opendisplay_display.cpp:788`) | identical — promote as-is, but document that "compressed" is inferred from payload length, not a flag. |
| 3.1a | **zlib window (`OPENDISPLAY_ZLIB_WINDOW_BITS`)** | encoder must match | **9 (512 B)** on all current boards | **9 static** | **9 static** | All targets reject a stream whose CMF declares a window larger than their limit (`shared/core/od_zlib_inflate.c`, or the ESP32 tinfl adapter), so the encoder MUST cap `windowBits` at 9. The canonical portable engine treats the window as a per-target macro with floor 9; see MEMORY_CONSTRAINTS.md. |
| 3.1b | **Inflate engine (`#124`)** | not a wire property | **tinfl (ROM miniz)** when `OPENDISPLAY_ENABLE_WIFI`, via an unconditional compile-time remap of the `od_zlib_stream_*` call sites — so it serves direct-write, 0x76 **and** PIPE, not just LAN (`src/od_inflate_tinfl.h`). Other boards use the canonical portable bit-serial engine. | canonical portable bit-serial engine | canonical portable bit-serial engine | Engine choice is invisible on the wire provided the window contract holds. `shared/core/od_zlib_inflate.{c,h}` is the canonical portable implementation behind the streaming API; the tinfl adapter remains an ESP32 build-level alternative. Tinfl cannot be the shared default because its working set exceeds the BG22 budget. The `OPENDISPLAY_ENABLE_WIFI` gate remains a proxy for "can spare the RAM", not a transport filter; replacing that proxy belongs to the later engine-selection seam. |
| 3.2 | Uncompressed auto-complete | when `bytes_written == total`, END runs implicitly without a 0x72 (`display_service.cpp:2331-2332`) | *unverified at line level* (delegated into `opendisplay_display.cpp`) | *unverified* | Spec does not document auto-complete. Keep it (clients rely on it for full-frame pushes) and document it; verify NRF54/Silabs parity during their subsystem swaps. |
| 3.3 | END ack ordering | END-ack **before** the blocking refresh, then 0x73/0x74 (`display_service.cpp:2382-2385` comment + code) | same, explicitly, with a 20 ms TX drain gap (`opendisplay_pipe.c:796-799`) | **refresh happens first** — `opendisplay_display_direct_write_end` blocks through the ≤60 s refresh (`opendisplay_display.cpp:854-894`) and only then are `[00][72]` + `[00][73/74]` sent (`opendisplay_pipe.c:707-722`) | Spec and two of three say ack-then-refresh. **Silabs diverges**: its END ack can arrive a minute late; clients that time out on the END ack see false failures. Fix on adoption. **C13 (2026-08-17):** closed on BG22 in software, and the fix is larger than reordering. `opendisplay_display_direct_write_end()` splits into prepare/refresh; the ACK is queued after a successful prepare, drained to stack acceptance in-handler, and then a bounded `sl_bt_resource_get_connection_tx_status()` poll waits for ZERO packets pending on the same connection instance before the panel starts. Stack acceptance alone is explicitly not sufficient. Timeout, overflow/corrupt flags or an instance change abort the refresh. Sniffer corroboration still owed. |
| 3.4 | Refresh-mode byte on END | partial session: 0 FULL / 1 FAST / else PARTIAL (`display_service.cpp:2362-2364`); full: 0/1 | same contract | `payload[0]==1` ⇒ FAST else FULL (`opendisplay_display.cpp:875-877`) — no partial mode exists | consistent given capability differences. |
| 3.5 | Stray legacy frames mid-PIPE | 0x71/0x72 silently discarded while `pipeState.active` (`display_service.cpp:2280-2285, 2339-2343`) | n/a | n/a | Part of the PIPE state machine; promote with it. |

## 4. Partial-region 0x76

Only `Firmware` and `Firmware_NRF54` implement it. The `Firmware` handler is the reference
(`display_service.cpp:2173-2278`): 17-byte **big-endian** header, flags-whitelist →
etag (`old != 0 && old == displayed && new != 0`) → 1bpp-only → rect OOB → x/w alignment
(mult. of 8), then a two-plane stream (`plane_size * 2`, old plane then new). Error codes
are the `OD_ERR_PARTIAL_*` namespace, and any geometry/etag NACK clears `displayed_etag`.
NRF54 delegates to `opendisplay_display_partial_write_start(payload, len, &err_code_out)`
(`opendisplay_pipe.c:727-739`) with the same NACK framing; its internal validation order was
**not line-verified** here — check parity during the subsystem swap.

Two spec gaps found: 0x76 has no `RESP_*` mirror constant, so the echo byte is a raw
literal in firmware (TODO recorded at `display_service.cpp:2231-2234`) — add
`RESP_PARTIAL_WRITE_START` to the canonical header. And the PIPE-partial twin packs the
same geometry **little**-endian (`display_service.cpp:2483-2494`), which the spec does
document but which any shared header-parser must treat as two distinct layouts.

## 5. PIPE sliding window 0x80-0x82 (Firmware only)

The one subsystem with a single implementation, so no divergence — but its constants and
invariants must be recorded before promotion because no other repo can cross-check them:

- **Reorder queue**: `PIPE_REORDER_SLOTS` 33 (17 with `PIPE_SMALL_DRAM_WINDOW`), slot =
  4 + 248 data bytes ⇒ **~8.3 KB .bss** (~4.3 KB small) (`src/structs.h:32-53, 99-104`,
  `display_service.cpp:573-577`). `seq % SLOTS` is collision-free because a live window
  spans ≤ W < SLOTS.
- **Negotiation**: min-rule on window/ack-cadence/frame (`display_service.cpp:2725-2731`),
  response `{00,80,ver,W,N,frame:2LE,resp_flags}` (`:2785-2787`).
- **SACK**: 7-byte `{00,81,highest_seen,mask:4 LE}`; mask bit *i* = chunk
  `highest_seen-1-i` received; the accepted-prefix depth is bounded by `received_count` to
  avoid mod-256 phantom acks in the first 32 chunks (`:2512-2546`).
- **NACKs are fatal**: 8-byte `{FF,81,err,hs,mask:4}`; state deliberately not reset so the
  NACK payload reflects it; further 0x81 discarded until next START (`:2562-2566`).
- **The replay-window coupling is a hidden invariant**: the AES-CCM nonce counter advances
  at decrypt time for *every* 0x81 frame, including ones later queued or discarded, and the
  design depends on `in-flight ≤ W ≤ 32 ≤ ±32 replay window`
  (`communication.cpp:740-745`, `encryption.cpp:128-136`). If `shared/core` ever raises
  `PIPE_MAX_W` above 32 it must raise the replay window in lock-step. Encode this as a
  `static_assert` in the shared implementation.
- Uncompressed full-frame transfers auto-complete like legacy; **partial transfers never
  auto-complete** (`display_service.cpp:2481-2494, 2848`).

`shared/core` must make PIPE fully compile-out (`OD_PIPE_ENABLE=0`): Silabs cannot spend
8.3 KB (or even 4.3 KB) of RAM on it, and the spec's own `@targets` says only Firmware has
it. A non-implementing target should NACK the 0x80 START instead of today's silent drop —
which needs an "unsupported" code in the `OD_ERR_PIPE_START_*` namespace (spec addition).

## 6. Session auth / AES-CCM

The good news: the KDF chain is **byte-identical in all four repos** — label
`"OpenDisplay session"`, CMAC over `label||0x00||device_id||client_nonce||server_nonce||0x00 0x80`,
ECB finalization, session_id = first 8 bytes of CMAC(session_key, client||server)
(`encryption.cpp:61-107`, NRF54 `opendisplay_pipe.c:218-265`, Silabs same,
`Firmware_NRF/encryption.c:96-126`). Also identical: 30 s challenge window, 10-attempts/60 s
rate limit, ±32 counter window + 64-entry seen-list, 3-strike integrity teardown
(`encryption.cpp:526-560`, NRF54 `:407-438, 129-141`).

**Superseded 2026-08-15 on `esp32-idf` and `nordic-zephyr`** by `shared/core/od_session.c`. Rows
6.5–6.9 below are the promotion's behaviour changes; `efr32bg22-slc` and `Firmware_NRF` still ship
everything described above, so this section stays written in the present tense for them. The one
paragraph that is now wrong for the two swapped targets is the replay window: they no longer use
the ±32 window with a 64-entry seen-list. Upstream `Firmware` had already replaced it with a
256-bit backward bitmap and **no forward bound** (`src/nonce_window.h`), and that file was ported
verbatim as `shared/core/od_nonce_window.h`. A forward cap is not merely unnecessary there but
harmful — it strands a session permanently once a gap exceeds it, and it bounds nothing an
attacker cares about.

| # | Behaviour | Firmware | NRF54 | Silabs | Resolution |
|---|---|---|---|---|---|
| 6.1 | STEP-2 success frame | **19 bytes**: `[00][50][00][server_proof:16]` where proof = CMAC(session_key, server_nonce‖client_nonce‖device_id) (`encryption.cpp:626-641`) | identical (`opendisplay_pipe.c:714-724`) | identical | Spec documents only `[0x00][0x50][AUTH_STATUS_SUCCESS]` — the 16-byte mutual-auth proof is **undocumented**. Spec correction. |
| 6.2 | Session timeout basis | absolute from `session_start_time` (`checkEncryptionSessionTimeout`) | absolute, with a comment explaining why idle-based is wrong (`opendisplay_pipe.c:163-171`) | **idle-based** on `last_activity_ms` (`opendisplay_pipe.c:117`) — a continuously active session never expires | Firmware/NRF54 win; the NRF54 comment is the rationale. Fix Silabs on adoption. **RESOLVED 2026-08-15** as absolute, in the MILLISECOND domain, expiring on `elapsed >= timeout_ms`. Three bases shipped and the promotion took one piece from each argument, so state it precisely: Firmware's `>=` boundary, NRF54's ms domain, and neither's idle basis. Firmware divided BOTH timestamps by 1000 before subtracting (`encryption.cpp:266-267`), which is not wrap-safe — across the 49.7-day `uint32_t` rollover the current time restarts near 0 while the divided start stays ~4.29e6, so the age wraps huge and a healthy session is torn down. Taking NRF54's ms domain fixes that; taking Firmware's `>=` shifts NRF54 by 1 ms and Firmware by up to ~999 ms (its seconds floor could expire early). **Silabs' idle basis is a genuine security divergence, not a rounding one** — a continuously active session there never expires at all — and adopting absolute will change its behaviour outright. **C13 (2026-08-17):** closed on BG22 by the same `od_session` swap; its idle basis is gone. The seconds-domain wrap hazard described above never applied to BG22 and does not return. |
| 6.3 | Crypto backend | mbedTLS CMAC/CCM or CC310 (`#ifdef` arms, `encryption.cpp:15`) | PSA for CMAC/ECB/RNG, but **CCM hand-rolled** RFC 3610 over ECB (`opendisplay_pipe.c:282-405`) | same hand-rolled CCM | **RESOLVED 2026-08-15 for Firmware + NRF54** (`shared/hal/od_hal_crypto.h`). Both now use NATIVE CCM: mbedTLS on esp32-idf, `psa_aead_*` on nordic-zephyr. The hand-rolled RFC 3610 was not a missing capability — `CONFIG_PSA_WANT_ALG_CCM` was simply never set, while the Oberon and CRACEN drivers both implement it; enabling it costs **+2,320 B flash, +0 B RAM**. That code is deleted from the target and preserved verbatim as `tests/host/session_ccm_reference.inc`, the differential reference. `OD_CRYPTO_SOFT_CCM` is **deferred**, not adopted: with both targets native it would be a `shared/` source nothing compiles. Silabs still hand-rolls and is unchanged. **PROVEN ON SILICON 2026-08-15** — an `xiao_nrf52840` passed Gate 2 including the encrypted/authenticated path, so the shortened-tag key policy works against real traffic. mbedTLS on `esp32-idf` is still unverified. **C13 (2026-08-17):** BG22 is the third target on native CCM, via PSA over the CRYPTOACC transparent driver at the same 12-byte shortened tag. Enabling it required selecting an AEAD at all — the target had none, which is why the hand-rolled RFC 3610 existed here too. The linked map contains `sli_cryptoacc_transparent_aead_*` and no placed `mbedtls_ccm` code. `tools/check.sh` ratchets the tracked PSA override and the `.slcp` selection, because a clean checkout would otherwise silently disable session encryption. Unverified on silicon. |
| 6.4 | Legacy backend | — | — | — | `Firmware_NRF` uses Nordic `ocrypto_*` (`encryption.c:7-9`) — SDK-locked; irrelevant once the repo is dropped. |
| 6.5 | **Bidirectional nonce reuse** | present | present | present | **NOT FIXED, and not fixable here — see FOLLOWUPS.md § 5.** Inbound and outbound share one `session_id` and both counters start at 0, so the same `session_id‖counter` nonce is used in BOTH directions under one key. Catastrophic for CCM if a host ever decrypts with the same key in both directions. `od_session` reproduces it faithfully because changing it changes the wire; it needs directional key separation or a nonce-domain bit in the next protocol revision. |
| 6.6 | Replay at `diff == 0` | **accepted** (`encryption.cpp:178` skips the seen-scan when the difference is zero) | accepted (`:461`) | accepted (`:383`) | **FIXED** on both swapped targets. In a bitmap `d == 0` is simply bit 0, and every forward accept sets it, so there is no special case left to regress. A resend of identical sealed bytes is now `OD_SESSION_OPEN_REPLAY`. Host-compatible: py-opendisplay re-seals on every transmission including PIPE retransmits (`device.py:758-770`, pinned by `test_pipe_write_sender.py:532`), so no client depends on the old acceptance. **C13 (2026-08-17):** BG22 inherits the fix with `od_session`; its own `nonce_counter <= last_seen && diff != 0` hole is deleted. This is one of two changes that can refuse a frame the old BG22 accepted. |
| 6.7 | Window advance vs tag check | advances BEFORE decrypt (`encryption.cpp:740`, decrypt at `:763`) | advances before decrypt (`opendisplay_pipe.c:500`, decrypt at `:505`) | already split | **FIXED** on both swapped targets: `od_nonce_check()` is pure and `od_nonce_commit()` runs only after the tag verifies, so a forged frame at a high counter can no longer move `rx_last` and lock out the legitimate lower-counter frames still in flight. Commit still precedes the inner-length check — that frame carried a valid tag, so it is authentic and leaving it uncommitted would leave it replayable. **C13 (2026-08-17):** BG22 inherits the shared split. |
| 6.8 | Inner length byte | permissive `<=` (`encryption.cpp:767`) | permissive (`opendisplay_pipe.c:509`) | permissive | **TIGHTENED** to exact (`decrypted[0] == decrypted_len - 1`) on both swapped targets. The permissive form accepts authenticated trailing bytes the caller never sees — harmless today because no producer emits them, but it is slack in a length field on the pre-auth-adjacent path. If a device ever refuses a legitimate frame with `OD_SESSION_OPEN_BAD_LENGTH`, this is the line to look at first. **C13 (2026-08-17):** BG22 inherits the exact check. With 6.6 this is the second change that can refuse a frame the old BG22 accepted — neither has been exercised by a malformed/replayed-frame pass on hardware. |
| 6.9 | `encryption_enabled` test | `== 1` (`encryption.cpp:232`) | `!= 0` (`opendisplay_pipe.c:165`) | `!= 0` | **NRF54's `!= 0` adopted — the one place this promotion overrides the authority target**, so it is recorded rather than assumed. The wire contract defines only 0 and 1, and the two readings fail in opposite directions: under `== 1` a single corrupted byte silently turns encryption **off** on a device whose config says it is on; under `!= 0` a corrupt byte can only fail closed, and the client still authenticates normally because it holds the key. Fail-safe wins on a security gate. `od_session_security_enabled()` also folds in the zero-key rule, which Nordic and Silabs never applied. |

## 7. Advertisement / MSD

No semantic divergence — one 16-byte `struct MsdAdvertisement` (canonical
`opendisplay_structs.h`), company id `0x2446` little-endian, dynamic sensor bytes,
temperature byte, battery-voltage low byte + 9th bit in status, 4-bit loop counter:

- Firmware builds via the canonical struct and dedupes identical payloads before
  re-advertising (`display_service.cpp:1734-1821`).
- Silabs packs the same `msd_payload[2..15]` into the 0xFF AD record and moves the 16-bit
  service UUID `0x2446` to the scan response for space (`opendisplay_ble.c:1705-1754`,
  company id at `:22`).
- NRF54 uses the same company id and GATT UUID (`opendisplay_ble.c:33, 308-309`).

Resolution: the 16-byte payload build (sensor encode, voltage clamp to 511×10 mV,
temperature offset+40 ×2, status bits) is pure logic → `shared/core`. AD-record packing and
re-advertise mechanics stay per target (NimBLE vs Zephyr `bt_le_adv_update_data` vs
`sl_bt_legacy_advertiser_set_data` differ in whether the name fits the ADV PDU).

### 7a. BLE address derivation — a toolchain divergence, not a code one

**The one divergence in this document that no amount of reading the source could find.** It is
not in any repo's code; it is in a vendored binary framework's sdkconfig, and it decides the
device's identity to Home Assistant.

`CONFIG_<CHIP>_UNIVERSAL_MAC_ADDRESSES` sets how many interfaces get an IEEE-assigned MAC
derived from the factory base MAC, and therefore where Bluetooth lands:

| | STA | AP | BT | ETH |
|---|---|---|---|---|
| `FOUR` | base+0 | base+1 | **base+2** | base+3 |
| `TWO` | base+0 | local (U/L bit) | **base+1** | local |

HA's Bluetooth integration keys devices on the BLE address, so the offset *is* the fleet's
identity. What each build shipped:

| Chip | pioarduino (the fleet) | ESP-IDF default | Now pinned to |
|---|---|---|---|
| esp32s3 | **2** | 4 | **2** |
| esp32 | 4 | 4 | 4 |
| esp32c3 | 4 | 4 | 4 |
| esp32c6 | 4 | 4 | 4 |

**Discovered on hardware, 2026-08-03.** The S3 reappeared in Home Assistant as a brand-new
device after the ESP-IDF port. Base MAC `44:1b:f6:85:b1:b8`; the IDF build advertised
`...b1:ba` (base+2) against the fleet's `...b1:b9` (base+1). The device *name* was identical
throughout — `getChipIdHex()` reads the **base** MAC (`encryption.cpp:786`, `ESP.getEfuseMac()
>> 24`), which this setting does not touch — so name matched, address did not, and it read as a
stranger rather than as a fault.

**Why the S3 differs has no known rationale.** The one distinction that correlates — only the
classic ESP32 has an internal EMAC, so only it has a real fourth consumer — does not hold: the
C3 has the same profile as the S3 (SPI Ethernet only, whose controllers carry their own MACs)
and pioarduino gave it FOUR. Most likely an inconsistency in arduino-esp32's packaging.
Ruled out: IDF drift (`git log -p` on the chip `Kconfig.mac` files shows FOUR as the only
default ever set, so arduino's value is not a stale capture) and a smaller Espressif allocation
for the S3 (IDF's docs state every chip ships enough universal addresses for all internal
interfaces, and the knob is documented as being for *custom* MAC ranges — with a factory base
MAC either value is safe on any chip).

**Resolution: pin per chip to what shipped, in `sdkconfig.defaults.<chip>`, including where it
already equals IDF's default.** Fleet compatibility means matching observed behaviour, not a
rationale. Three of the four pins change nothing today and exist so an IDF upgrade cannot move
a fleet's addresses the way this one just did. The symbol cannot go in the common
`sdkconfig.defaults`: it is chip-prefixed, and the generic
`CONFIG_ESP_MAC_UNIVERSAL_MAC_ADDRESSES_*` is a bare bool with no prompt that the chip choice
`select`s.

### 7b. NimBLE host-stack log level — the same inherited-default trap

Second instance of 7a's mechanism, found the same week. The stack logs
`BLE_HS_LOG(INFO, ...)` from inside itself — `"GATT procedure initiated: notify"` plus
`att_handle=` on **every notify** (`ble_gattc.c:629`), and a five-line block per
advertise/stop-advertise (`ble_gap.c:3972`, `:4274`). A 414-chunk `PIPE_WRITE` emitted roughly
800 extra UART lines.

**It is not the NimBLE setting** — `CONFIG_BT_NIMBLE_LOG_LEVEL` is INFO in both builds. It is
the global ceiling:

| | pioarduino | ESP-IDF default | Ours before | Ours now |
|---|---|---|---|---|
| `CONFIG_LOG_DEFAULT_LEVEL` | 1 (ERROR) | 3 (INFO) | 3 | 3 |
| `CONFIG_LOG_MAXIMUM_LEVEL` | 1 (ERROR) | 3 (INFO) | 3 | 3 |
| `CONFIG_BT_NIMBLE_LOG_LEVEL` | 1 (INFO) | 1 (INFO) | 1 (INFO) | **2 (WARNING)** |

`MAXIMUM_LEVEL` is a compile-time ceiling, so at ERROR the preprocessor strips every `ESP_LOGI`
in the firmware — NimBLE's included. Arduino set it to ERROR; IDF defaults to INFO; this project
never named the symbol.

The cost that mattered was not noise. `od_log` takes a mutex and writes to UART0 while `ESP_LOG`
writes to the same UART with **no shared lock**, so records interleaved mid-line:

```
ERX 0x0040 (31 B): 00 40 C8 19 ... 27 B6 00I (137013) NimBLE: GATT procedure initiated: notify;
```

Every hex dump in a capture taken that way is suspect, which cost real debugging time during the
dark-panel investigation.

**Resolution: `CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING=y`, and deliberately NOT matching Arduino's
global ERROR ceiling.** Lowering the ceiling would also strip *our* useful `ESP_LOGI` lines —
`od_ble`'s resolved identity-address type and GATT `val_handle`, and the `bbep` BUSY-wait timeout
warning that made the dark-panel diagnosis possible. Silence the noisy component, not the whole
log. Verified by symbol inspection rather than config: the three NimBLE format strings are absent
from the linked image and the three of ours are present. 5 KB of flash reclaimed on the S3.

**The two-writers-on-one-UART problem is NOT fixed** and is now the remaining cause of corrupted
log lines. `od_log`'s mutex does not and cannot cover `ESP_LOG`. Either route both through one
sink or accept that any `ESP_LOG` at a visible level can splice a line. Worth doing before the
next hardware capture is trusted.

**The general lesson, which applies to the whole migration.** `docs/TOOLCHAINS.md`'s
PlatformIO-knob → sdkconfig translation table was built from `platformio.ini`'s `build_flags` —
i.e. from what the project *set*. This is a wire-visible behaviour the project silently
*inherited* from a precompiled framework sdkconfig it never saw, and now inherits differently.
Nothing in `Firmware`'s tree records that BT = base+1. The remaining ~1500 symbols in those
Arduino sdkconfigs are unaudited on the same basis; when the nRF52840 half moves to Zephyr,
expect the same class of surprise.

If a board ever needs FOUR *and* fleet-compatible BLE, `esp_iface_mac_addr_set(mac,
ESP_MAC_BT)` overrides one interface at runtime — but it must be called before `od_ble_init()`,
and it puts one wire fact in two places, so prefer the Kconfig pin.

## 8. Protocol-header state (report only — headers frozen)

Verified with `sync_protocol_header.py --check` and `git show`:

1. Canonical is 2.2 + an unreleased `OD_BLE_MAX_FRAME` addition. The vendored **protocol**
   copies in NRF54 / Silabs / NRF are one push behind (missing `OD_BLE_MAX_FRAME`);
   `Firmware` local and `Firmware_Unified/shared/protocol/` match canonical.
2. ~~**`upstream/main` of `Firmware` — the true ESP32 import source — is at 2.1**, missing
   all of 2.2 (SECTION 9 LAN) and the unreleased entries.~~ **WRONG — corrected 2026-07-25 at
   the Phase A import.** `upstream/main` is at **2.2**, and necessarily so: `#124`
   (`2e2131b`) *is* the LAN feature that SECTION 9 documents. Measured at import, its
   `include/opendisplay_protocol.h` differs from canonical by **ten lines, all inside a
   doc-only changelog entry** (the `0x43` trailing-patch-byte clarification), with no wire
   difference and no version difference. The import therefore did not land a stale header and
   nothing needed re-syncing. The migration risk built on this claim is void.
3. `upstream/main` hand-edited `include/opendisplay_structs.h` in PR #120 — three
   comment-only Seeed_GFX→FastEPD wording changes (PanelIC `@external` note, ids 3000/3001
   doc strings, `dc_pin` doc). No wire change. **Canonical still carries the old wording**,
   so the required sequence is: apply the FastEPD wording to the canonical structs header
   first, then `--push` — a bare `--push` today would *revert* upstream's edit.
4. `Firmware_Silabs` has a second, unmaintained `include/opendisplay_protocol.h` stale at
   2.1; nothing compiles it today only by include-path order. Do not carry it into
   `targets/efr32bg22-slc/` (see `targets/efr32bg22-slc/README.md`).
5. The sync tool's copy map still lacks `Firmware_Unified/shared/protocol/`.

Spec corrections accumulated from the matrix (all backward-looking documentation of shipped
behaviour, i.e. no-bump or MINOR under the header's own policy): 1.1 auth-required shape,
1.2 decrypt-failure shape, 1.4 reserved ack padding, 0x43's `sha_len` byte, 6.1 the STEP-2
server proof, 3.2 auto-complete, `RESP_PARTIAL_WRITE_START`, a PIPE-START "unsupported"
error code, the 0x45 `@targets` fix (or a Silabs implementation), and the **0x52 NACK-width
self-contradiction** — `@response` says `[FF][52][00][00]`, `@targets` says `[FF][52][00]`;
Silabs ships four bytes, so `@targets` is the line to correct (FOLLOWUPS.md § 2.1).

**One queued change is *not* backward-looking — it changes behaviour and must be sequenced
deliberately:** `MAX_CONFIG_CHUNKS` **20 → 21**, so that the 4096 `MAX_CONFIG_SIZE` ceiling is
actually transferable rather than capped at 4000 by the chunk count (decided 2026-07-25;
FOLLOWUPS.md § 3.1). It is a relaxation — a device accepts one additional chunk — so old hosts
are unaffected and it is a MINOR bump. But unlike everything above it, firmware must change to
match, and deployed units keep enforcing 20 until they are reflashed.

## 9. LAN / WiFi transport — `Firmware`/ESP32 only (`#124`)

New surface as of `2e2131b`. No other target has a second transport, so there is nothing to
diverge *from* yet — these rows exist to stop the ESP32's choices being adopted as universal by
default when `shared/core` gains an origin-aware dispatcher.

| # | Behaviour | Firmware / ESP32 | Others | Resolution for `shared/core` |
|---|---|---|---|---|
| 9.1 | Connection model | **The device is the server; the host connects to it.** It binds `WifiConfig.server_port` (default `OD_LAN_TCP_PORT` 2446 when 0) and listens | n/a — but **BLE is the same shape**: device is the peripheral, host is the central and initiates | **This is consistent across transports, not a LAN peculiarity.** On both BLE and LAN the device is passive and the host initiates, so `od_hal_radio` is uniformly passive: the transport delivers frames, the core replies on the same origin, and the core never initiates a connection on any transport. Do not model LAN as an exception. |
| 9.2 | `WifiConfig.server_host` | **DEPRECATED** (2026-07-25). Gates nothing and is not read. A leftover from an abandoned "tag pushes to an upload server" model that the device-as-server design never used; the name misleads, since `server_port` is the port *this device* binds | n/a | **Deprecated, not removed.** The 64 bytes stay in the `0x26` wire format at their current offset — deleting a field or shifting `server_port` would break the config-binary backward-compatibility rule (ARCHITECTURE.md) on shipped devices. Rules: firmware **must ignore** it; `py-opendisplay` and the HA config flow should stop writing it and must not surface it as a setting; new firmware must not repurpose the bytes (a deployed host may still send a non-zero value, so they are not free space — unlike `reserved[]`, which is contractually zero). Mark `@deprecated` in the canonical header when the freeze lifts. |
| 9.3 | TLS-PSK port | **derived, not configured**: `server_port + 1`, with no config entry (`OD_LAN_TLS_PORT` in the header agrees) | n/a | Derived ports are a wire contract as much as a stored one. `shared/core` must expose the derivation, not re-invent it per target. |
| 9.4 | PIPE over LAN | **not available** — "NO PIPE ON LAN" (`opendisplay_protocol.h`) | n/a | PIPE is BLE-only *by protocol*, not by capability. So `OD_PIPE_ENABLE` is necessary but not sufficient: the dispatcher must also refuse 0x80-0x82 on a LAN origin, on a target that supports both. |
| 9.5 | Credential logging | deliberately never logs SSID or password verbatim — presence and length only (`config_parser.cpp`, `#124`) | *unverified elsewhere* | **Now a rule**, not a convention — ARCHITECTURE.md § "Secrets are never logged verbatim". Applies to `shared/` at every log level and covers `0x26`, `0x27`, keys, nonces, MACs and raw TLV payloads. `Firmware`'s practice is the model; audit the other targets during promotion, since none is verified. |

**What generalises well:** the device is passive on every transport — BLE peripheral, LAN server
— and the host always initiates. `od_hal_radio` can therefore be uniformly passive with no
transport-specific initiation path, which is a simplification worth stating rather than
rediscovering.

**What generalises badly:** origin is already an explicit argument of frame RX in the proposed
core API (§1.6, SHARED_API_DESIGN.md) — good. But 9.4 shows origin also gates *which opcodes are
legal*, which the current design treats purely as a CCM-policy input. Extend it: origin selects
the reply transport, the encryption policy, **and** the permitted opcode set.

---

## 10. NFC endpoint 0x0083 — resolved by the Phase 4 promotion (2026-08-21)

Six divergences the shared machine settles. Rows 10.1-10.4 are wire-visible changes against what
shipped; 10.5-10.6 record differences the promotion deliberately did **not** normalise.

None is hardware-verified. No board in this fleet has an NFC antenna fitted, so every row below is
software-qualified only — see `docs/HARDWARE_VERIFICATION_CHECKLIST.md` § Transfer Phase 4.

| # | Behaviour | Before | After | Why |
|---|---|---|---|---|
| 10.1 | Chunk assembler ownership | **Neither port bound an owner.** Any connection could extend or commit another's assembly; both donors bind `connection` and reject foreign DATA/END with `0x07` | Bound to the full `od_reply_t` — origin *and* tag. Foreign DATA/END answers `FF 83 FF 07` and mutates nothing | A **restoration**, not an invention: both imports dropped a check both donors had, with no rationale in the tree. `{origin, tag}` is strictly stronger than the donors' connection byte because it also separates transports. A replacement START from any owner still displaces the incumbent, as the donors do |
| 10.2 | Inline-write length bound | Nordic evaluated `(uint16_t)(4 + declared) > body_len`, which **wraps**; BG22 widened it | 32-bit in every arm, and `od_span_split()` takes every cut | The wrap admitted a ~65 KB `memcpy` from a 256-byte RX slot into a 512-byte static, reachable unauthenticated with security disabled. Fixed on Nordic ahead of the promotion; the sibling `Firmware_NRF54` still carries it (`FOLLOWUPS.md` § 9) |
| 10.3 | Inline-write check order | **BG22 tested the record type first** and folded the length test into the tag call, so an over-declared length answered `0x03` and an invalid type with one answered `0x05` | Length first, as its own arm: both classes now answer `0x01` | Both donors test length first. `0x03` told a client its tag was broken when its frame was |
| 10.4 | Reply failure on START / DATA | Both ports discarded the queue result: the verdict stayed OK and the staged bytes survived a frame the client never saw answered | A failed ACK clears the assembler and the verdict is NACK | The client cannot know its frame landed and its only recovery is a fresh START; leaving bytes staged guarantees a later `0x08` or `0x09` on a transfer it believes never began. A deliberate divergence from the donors, which ignore the send result. READ / inline WRITE / END keep the opposite rule — the tag has already been touched, so a failed reply is reported and never reverted |
| 10.5 | Read cap requested from the tag | Both donors pass `OD_PIPE_MAX_PAYLOAD - 6` = **238** | Both ports request `OD_SESSION_PAYLOAD_MAX - 4` = **218**, unchanged by the promotion | A sealed 238-byte record exceeds one BLE frame. Applied in both plaintext and encrypted sessions so the answer's size never depends on whether the session happens to be encrypted. **A host cannot interrogate this**; restoring 238 is a protocol decision, not a firmware one |
| 10.6 | Behaviour above that cap | Nordic truncates for verbatim and well-known records and **refuses** an over-cap MIME record; BG22 refuses everything past its 128-byte staging buffer | Unchanged, and deliberately not normalised | Refusal versus truncation is a property of the record **and** the adapter together, not of the target. The shared machine passes the cap down and reports what came back — `false` becomes `0x02`, a short answer is sent at its true length. Normalising them is controller-code work on two targets with a hardware gate of its own |

---

## 11. LED_ACTIVATE 0x0073 — group repeats and the yield floor (2026-08-22)

Two defects the dedup survey found (`plans/PLAN_DEDUP_OUTSTANDING_2026-08-22.md` D1). 11.1 is
wire-visible on all three targets; 11.2 is a BG22-only liveness fix.

Neither is hardware-verified — see `docs/HARDWARE_VERIFICATION_CHECKLIST.md`
§ LED runner and panel rail, which carries sentinel rows for all three targets because 11.1
changed all three.

| # | Behaviour | Before | After | Why |
|---|---|---|---|---|
| 11.1 | `group_repeats` sentinel | All three runners store `(uint8_t)(raw + 1)` and treat internal 255 as endless. Raw `0xFE` therefore ran **forever** while raw `0xFF` — the value `opendisplay_structs.h:1114` documents as "repeat forever" — wrapped to 0 and stopped **immediately** | Raw `0xFE` **and** `0xFF` are both endless, held in an explicit `repeat_forever` flag rather than inferred from the wrapped count | `py-opendisplay` is the deciding evidence: `led_flash.py:102` encodes indefinite as `0xFE`, `:133` decodes both `0xFE` and `0xFF` as indefinite, and `:52` caps finite counts at 254 so `0xFF` is never a count. Accepting both matches the host exactly and keeps every deployed pattern working. The canonical header's "255" describes neither the host nor any shipped firmware; it is frozen, so the divergence is recorded rather than fixed there |
| 11.2 | Zero-delay step yield | BG22 alone re-entered its `for (;;)` phase loop on every zero-delay transition and at the group-closing edge, so a pattern with all delays zero and an endless group never returned. Superloop, no watchdog: recovery was removing power. Reachable from any `0x0073` frame | Every flash and the group-closing edge schedule at least `LED_MIN_STEP_DELAY_MS` and return | ESP32 and Nordic already do exactly this, and Nordic's own comment names the group-closing edge as "the only yield when every loop count is zero and no flash ever runs". A port of the authority's shape, not a new design. Pinned by `tests/host/led_test.c`, which drives the production machine and bounds emissions per service call, plus `silabs_led_adapter_test.c`, which proves BG22 re-arms the returned delay |

### 11.3 LED_ACTIVATE displacement — the outgoing mode nibble is left alone (2026-08-23)

Recorded with the LED runner promotion (`plans/PLAN_LED_RUNNER_2026-08-23.md`). Software only; the
rows in that plan's § 4 are open.

When a `0x0073` arrives while another pattern is running, all three donors park the outgoing
instance's LEDs. They disagree about its persisted mode nibble:

| Donor | On displacement |
|---|---|
| `../Firmware` / esp32-idf | `led_stop_internal(**false**)` — parks the pins, **leaves the nibble set** |
| nordic-zephyr | `opendisplay_led_stop(0,false)` — parks and **clears** the nibble |
| efr32bg22-slc | same as Nordic — parks and clears |

**Resolved to the authority: park, do not clear.** `CLAUDE.md` makes `../Firmware` the reference
where donors disagree, and here it also gives the better behaviour — because the adapter writes the
new payload into `reserved[]` *before* activating, clearing on displacement wipes the byte just
written whenever the client re-activates the **same** instance. On Nordic and BG22 that made a
re-sent pattern stop instead of restart; under the shared machine it restarts.

Unchanged, and deliberately so: an explicit `LED_STOP` clears the nibble on every target, natural
completion clears it, and an externally cleared nibble stops the run without the machine clearing
anything (there is nothing to clear, and re-clearing would race whoever cleared it).

---

## 12. `BinaryInputs.pins_used == 0` — an undocumented three-way split (recorded 2026-08-22)

Dedup survey Q8. **Recorded, not resolved:** which reading is normative is a host-visible decision,
and `opendisplay_structs.h` defines neither — the field's doc says only "per-pin used/invert/pull".
No firmware change is made here.

| Target | Code | `pins_used == 0` means |
|---|---|---|
| ESP32 | `device_control.cpp:743` — `if (input->pins_used != 0 && (input->pins_used & (1 << pinIdx)) == 0) continue;` | **all pins used** — the mask is skipped entirely |
| nordic-zephyr | `opendisplay_button.c:55` — same special case | **all pins used** |
| efr32bg22-slc | `opendisplay_ble.c:414` — `bool pin_used = (input->pins_used & (1u << bi)) != 0u;` | **no pins used** — the bit is tested directly |

So a config with a zero mask configures every pin on two targets and none on the third. Both
readings are defensible from the wire alone: zero as "unspecified, take the default" and zero as
"an empty set".

**Why it matters and why it is not fixed here.** `py-opendisplay` decides what a host actually
sends; changing either firmware without checking that is how a working deployment breaks. The
resolution belongs with a host-side check and, if the answer is "unspecified means all", a
sentence in the canonical header — which is frozen. Until then, a config that means to use every
pin should set the mask explicitly rather than rely on zero.

---

## 13. `bus_id == 0xFF` — five sites substituted bus 0 for "not configured" (recorded 2026-08-23)

Project ruling 2026-08-23, reaffirmed: **`0xFF` means unconfigured**, for every `bus_id`-shaped
field. It is the contract's pervasive absent sentinel (`opendisplay_structs.h:295-298`).

**Ruling extended 2026-08-24: the firmware correction does NOT wait on the host.** A `bus_id` of
`0xFF` makes the config invalid — the device was never assigned a bus — so refusing it is correct
on its own terms, and a substituted bus 0 is invalid whatever the host sends. `py-opendisplay`
defaulting an omitted touch `bus_id` to `0xff` (`config_json.py:643`) is a **host defect**
(`FOLLOWUPS` § 15), not a schedule constraint on this repo.

What that costs, stated plainly rather than discovered: a config that omits `bus_id` stops
configuring touch. That is the intended outcome — the alternative is probing an unassigned device
on somebody else's bus, where an address collision yields plausible-but-wrong readings instead of
a failure. **Configs already stored on devices that carry `0xFF` in that field need re-provisioning**,
and only the host can identify them.

The canonical header still documents `TouchController.bus_id == 0xFF` as "bus 0" (`:945`) and is
frozen — `FOLLOWUPS` § 14.

**Resolved 2026-08-24** by sensor-seam staging step 1. Every site below now refuses, matching what
Nordic touch always did. **There were five substituting sites, not four** — `touch_input.cpp` was
missed by the original survey and is the one that mattered most, because it also returned "bus ok"
when *no* `DataBus` record existed at all.

| Site | Behaviour before | Now |
|---|---|---|
| `esp32-idf/src/display_service.cpp:726` | substitutes bus 0, then validates | refuses |
| `esp32-idf/src/sensor_sht40.cpp:52` | substitutes bus 0 | refuses |
| `esp32-idf/src/sensor_bq27220.cpp:43` | substitutes bus 0 | refuses |
| `esp32-idf/src/touch_input.cpp:274` | substitutes bus 0 **and** accepts any bus when `data_bus_count == 0` | refuses `0xFF`; the no-records default-pin path is unchanged |
| `nordic-zephyr/src/opendisplay_sensor_common.h:22` | substitutes bus 0, then validates | refuses |
| `nordic-zephyr/src/opendisplay_touch.c:299` | **refuses** — the reference | unchanged |

Because the four substituting sites all validate afterwards, the misbehaviour requires a valid
`data_buses[0]` *and* a device that was never assigned a bus: that device is then probed on an
unrelated bus, where an address collision yields plausible-but-wrong readings rather than a clean
failure. `touch_input.cpp` is the fifth and does not fit that shape, which is why it was worse.

Same defect class as § 11.2 (`pwr_pin == 0xFF` driving pad `0x00` on BG22), fixed 2026-08-22 by
refusing. Correcting all five is `plans/PLAN_SENSOR_SEAM_2026-08-23.md` staging step 1, which no
longer waits on anything outside this repo.

### 13.1 The same defect wearing a different constant: ESP32's no-`DataBus` default-pin path

**Ruled 2026-08-24. RESOLVED 2026-08-28**, on both targets and for sensors as well as touch.
Distinct from the five sites above because the `bus_id` is *not* `0xFF`: a declared, non-sentinel
`bus_id` that matches no `DataBus` record used to transact anyway, on whichever bus the board last
selected.

| | `../Firmware` (donor) | `targets/esp32-idf` | nordic-zephyr | Shared |
|---|---|---|---|---|
| `bus_id == 0xFF` | substitutes bus 0 | **refuses** | **refuses** | **refuses** (`od_touch_gt911.c` `touch_bus_ok()`, `od_sensor_*.c`) |
| declared `bus_id`, no matching `DataBus` record | transacts on the board-default bus | **refuses** | **refuses** | **refuses** |

**The transport closed the second row, not the drivers**, and that is the mechanism this ruling
predicted: both I2C HALs resolve a `bus_id` through `od_config_data_bus()` and fail when nothing
answers to it — `selectConfiguredBus()` (`targets/esp32-idf/src/display_service.cpp:757`) and
`select_bus()` (`targets/nordic-zephyr/src/od_hal_i2c.c:23`). So no consumer of the shared
operations can reach an unnamed bus, whether or not it checks for itself.

Where the refusal lands still differs, and it matters when reading a log. The shared GT911 driver
checks admission and says so once — `Touch[N]: no usable data_bus (bus_id M); not probed` — and
never probes. `od_sensor_sht40.c` and `od_sensor_bq27220.c` refuse only `0xFF` themselves, so an
unnamed bus reaches them as a plain transaction failure at the first read.

ESP32's board-default pins survive in `initOrRestoreWireForOpenDisplay()` (`:784`) for the PANEL,
which has no `bus_id` of its own and takes the first record or the default. That is the one
remaining unkeyed consumer, and it is out of this ruling's scope.

The shared HAL is keyed by `DataBus.instance_number` (§ 14), so a bus that appears in no
`data_buses` entry **has no identity the seam can name** — there is no argument the shared driver
could pass to reach it. Preserving the behaviour would mean a second, unkeyed transaction path
existing solely for configs that decline to describe their own hardware.

And the failure mode is § 13's exactly: a device probed on a bus nobody assigned it answers if
some other part happens to sit at that address, so the result is a plausible-but-wrong reading
rather than an error. A config that declares a touch controller without declaring its bus is
invalid; the shared driver refuses it, matching Nordic.

---

## 14. `DataBus` reference: instance_number vs array index (recorded 2026-08-23)

The canonical header defines the key: `DataBus.instance_number` is *"0-based bus-block index;
referenced by SensorData.bus_id, TouchController.bus_id, etc."* (`opendisplay_structs.h:802`). So
a consumer's `bus_id` names an **instance_number**, not a position in `data_buses[]`.

**Resolved 2026-08-24** by sensor-seam staging step 1, alongside § 13 — the two defects shared
every call site, and correcting the sentinel alone would have left a refusal that still resolved to
the wrong bus. `shared/core/od_config.c`'s `od_config_data_bus()` is now the one resolution policy:
exactly one match, **NULL on no match and on ambiguity**. Refusing a duplicated `instance_number`
is a decision — nothing rejects a config declaring one twice, so "first match wins" would have
resolved by packet order, the same accident one layer up.

| Site | Before | Now |
|---|---|---|
| `efr32bg22-slc/opendisplay_ble.c:1101` (NFC) | scanned by `instance_number` — right key, but took the first duplicate and had no bound against a corrupted count | uses the shared resolver |
| `esp32-idf/src/display_service.cpp:729` | indexed `data_buses[bus_id]` | resolved |
| `esp32-idf/src/display_service.cpp:938` (AXP2101) | bounds-checked and indexed, so its prechecks could pass on one record while the bus call selected another | resolved |
| `esp32-idf/src/touch_input.cpp:282` | indexed | resolved |
| `nordic-zephyr/src/opendisplay_sensor_common.h:28` | indexed | resolved |
| `nordic-zephyr/src/opendisplay_touch.c:302` | refused `0xFF` correctly, but range-checked and indexed — so it rejected a sparse instance and picked the wrong record for out-of-order ones | resolved |

Index and instance agree only when records arrive in ascending order with no gaps, which is the
common case and why this has never been seen. A host that sends `instance_number` 1 before 0, or
that omits an instance, binds every indexing consumer to the **wrong bus** — a sensor then talks
on another device's pins, and an address collision yields plausible-but-wrong readings rather than
a failure.

`tests/host/config_test.c`'s `test_data_bus_lookup()` pins the decision table and is
mutation-checked: restoring the indexing implementation fails 11 assertions. **No hardware gate has
run** — `plans/PLAN_SENSOR_SEAM_2026-08-23.md` § 5's "unconfigured bus" row is open on both
targets.

---

## 15. Buzzer pitch, folding and total duration (resolved 2026-08-23)

Resolved by the shared runner in `plans/PLAN_BUZZER_2026-08-23.md`; hardware rows remain open.

| Behaviour | ESP32 / live `Firmware` | Nordic before promotion | Shared ruling |
|---|---|---|---|
| index to pitch | `round(100 * 13.75 * 2^(idx/24))` centi-Hz | linear interpolation from 400 to 12000 Hz | exponential table |
| indices outside 117..234 | shift by 24 until inside the window | no fold | octave-fold, preserving pitch class |
| total melody cap | 30,000 ms | 5,000 ms | 30,000 ms; clamp the final step to the remainder |
| owned payload cap | 256 bytes | 244 bytes | 256 bytes; the host's 120-step maximum is 244 |
| zero-duration step | skip without touching enable or tone output | briefly assert enable and start the tone | skip as a complete no-op |

This is not authority by name alone. `py-opendisplay` documents the same exponential inverse,
accepts raw indices 1..255, and says firmware performs the fold. The cap is not host-encoded, so
the standing live-`Firmware` donor rule decides it. `tests/host/buzzer_test.c` checks all 256
indices against an independent formula and pins the exact 30,000 ms schedule.

---

## 16. Buzzer — BG22 has none, by product decision (2026-08-24)

**The EFR32BG22 will never carry a buzzer.** Recorded so the absence reads as a decision rather
than an omission waiting to be "completed".

Consequences, all already true in the tree:

- `shared/profiles.cmake` sets `OD_CONFIG_WITH_BUZZER=0`, so `struct BuzzerConfig` is not a member
  of that target's `struct od_config`.
- `opendisplay-bg22.cmake` takes no `APP_BUZZER` tier; the image links no `od_buzzer` symbol,
  no 256-entry pitch table and no runner state. Verified by `nm` against the built image.
- `0x0077` is answered by the target's own capability hook rather than falling silent, so a host
  can distinguish "no buzzer" from "firmware older than the command".

Ratcheted by `tools/check.sh` § "silabs: BG22 has no buzzer runner", which fails if the tier is
taken, if the profile line is dropped, or — when an image is present — if any `od_buzzer` symbol
reaches the ELF. The source rules alone would not catch a symbol arriving through some other tier,
which is why the map check is part of it.

**Adding buzzer support to BG22 is not a matter of taking the tier.** It would cost a 256-entry
`uint32_t` pitch table (1 KB of flash) plus runner state on a part with 480 B of RAM headroom, and
the config struct's layout would change — which is an ABI break against every host test compiled
for that profile.

---

## 17. Stored config record — three loaders that disagreed about malformed input (2026-08-24)

Resolved by `shared/core/od_config_store.c`, which is now the only implementation of the record's
framing and validation. The record itself is unchanged and stays unchanged: `0xDEADBEEF`,
version 1, a 16-byte little-endian header, CRC-32 over the payload. What differed was **which
malformed records each target accepted**, and the shared loader is stricter than the loosest of
them. Written down because that is an observable acceptance change, not a refactor.

| Check | ESP32 | nordic-zephyr | efr32bg22-slc | Shared |
|---|---|---|---|---|
| `magic == 0xDEADBEEF` | yes | yes | yes | yes |
| `version` | **not checked** | **not checked** | **not checked** | **not checked** — see below |
| `data_len` against the build's cap | yes | yes | yes | yes |
| `data_len` against the **caller's** buffer | yes | **no** | yes | yes |
| declared payload actually present | yes | **no** | yes | yes |
| stored span not longer than a legal record | in its HAL | in its HAL | yes, before the header | **in the core** |
| CRC-32 over the payload | yes | yes | yes | yes |

Three consequences worth stating:

1. **Nordic accepted a physically truncated record.** It zeroes its whole 4 KB staging struct
   before loading and never compares the bytes it got against the header's `data_len`, so a
   short record whose CRC happens to cover the implicit zero tail passed. It now fails.
2. **Nordic ignored the caller's capacity.** The shared loader refuses rather than relying on
   every caller having passed a buffer of exactly `MAX_CONFIG_SIZE`.
3. **The over-long-span check moved into the core.** ESP32 and Nordic reject one inside their
   caching HALs as a side effect of buffer size; BG22 rejects it explicitly because NVM3 objects
   go to 2112 bytes against a 2064-byte record cap, so the window is physically reachable there
   and a device provisioned by a larger-cap build is how one arrives. Leaving it to the medium
   made the core's acceptance target-dependent, which is the thing this promotion removes.

**Nordic's failed clear used to report success.** `clearStoredConfig()` discarded
`settings_delete()`'s result and returned `true` regardless, so a device that could not erase its
config told the caller it had. It now reports what the medium did. ESP32 and BG22 already
reported it.

**`version` stays carried and unchecked, deliberately.** All three targets write 1 and none has
ever read it back. Enforcing it would be a new rejection introduced under a refactor: a device
holding a record this firmware did not write would stop booting on its stored config. Pinned by
`tests/host/config_store_test.c`, which loads records carrying version 2 and `0xFFFFFFFF`.

Validation **order** follows ESP32, the authority per CLAUDE.md: magic, then the cap checks, then
truncation, then CRC. BG22 tested truncation before the caller's capacity, so a record that is
both truncated and too large for the caller now reports the cap where BG22 would have reported
truncation. Every caller collapses the result to a refusal, so this is visible only through the
shared result enum — pinned so it stays a decision.

---

## 18. Nordic had two bit-banged I2C engines, and they disagreed (2026-08-24)

Resolved by sensor-seam staging step 2: `opendisplay_touch.c`'s private bit-banger is gone and
GT911 uses `opendisplay_i2c.c`, the same engine the sensors use. The framing is identical — both
did a repeated START for a register read — so the divergence was entirely in **edge timing and in
what each engine did about a clock stretch**.

| | Private touch engine | Shared engine |
|---|---|---|
| Half-bit period | fixed 5 µs | `500000 / bus_speed_hz` |
| Clock stretch during a **read** bit | result **discarded** — a timeout produced garbage data | fails the transfer |
| Clock stretch on a **slave ACK** | result **discarded** | fails the transfer |
| Clock stretch on the master ACK/NACK | result **discarded** | fails the transfer |
| `tLOW` before a repeated START | 5 µs | **none** — see below |

**Two things changed, and both are behaviour, not tidying.**

**A stretch timeout now fails the transfer.** The old touch engine ignored `scl_release_wait()`
everywhere, so a clone stretching past the 1 ms bound carried on and sampled whatever was on the
wire. GT911 reads now fail instead, which feeds the existing retry and the five-failure disable
latch. Failing is right — a plausible-but-wrong coordinate is worse than a dropped sample — but a
clone that legitimately stretches long will now disable touch where it previously produced
garbage.

**The shared engine had no low period before a repeated START, and that was a live defect in the
sensors, not something touch introduced.** `od_i2c_write(stop=false)` ends with SCL driven low and
`od_i2c_start()` released it again immediately, so the only thing between the two edges was GPIO
reconfiguration time — against a 4.7 µs `tLOW` minimum at 100 kHz. **BQ27220 and nPM1300 have
always read that way**, so this was affecting deployed sensor reads on Nordic before touch was
folded in. `od_i2c_start()` now opens with a half-period delay, which is harmless on a first START
and restores `tLOW` on a repeated one.

**Touch pinned 100 kHz at first, and no longer does.** The private engine's fixed 5 µs half-period
is 100 kHz, so step 2 reproduced it rather than change timing inside a mechanical fold. The
reconciliation happened at step 5, and the authority settles it: **there is no "touch rate"**.
`../Firmware` brings a bus up at `bus_speed_hz ? bus_speed_hz : 100000`
(`display_service.cpp:946`) and `touch_input.cpp` never names a clock. The private bit-banger
hardcoded a number instead of reading config, which is the divergence — not the shared engine.

Project ruling 2026-08-24: **GT911 follows `bus_speed_hz`**, like every other device on its bus.
A board declaring 400 kHz now clocks touch five times faster than the private engine did, and that
is a hardware row rather than an assumption.

`opendisplay_touch.c` went 710 → 589 lines; `xiao_ble` flash 332,444 → 332,004 B (−440), `bss`
140,477 → 140,509 (+32, four `struct od_i2c_bus` against four 2-byte `struct TouchBus`).

**NOT HARDWARE-QUALIFIED**, and this is the step where that matters most:
`plans/PLAN_SENSOR_SEAM_2026-08-23.md` § 3 step 2 requires a touch hardware check before the
cutover proceeds, and § 5's "Nordic touch cleanup" row is open.

---

## 19. SHT40 — the Nordic port had dropped the retry pass (2026-08-24)

Resolved by sensor-seam staging step 6: `shared/core/od_sensor_sht40.c` is the only SHT40 driver,
and it takes the authority's behaviour where the two ports disagreed.

| Behaviour | `../Firmware` / ESP32 | nordic-zephyr before | Shared |
|---|---|---|---|
| Address candidates | configured, then 0x44, then 0x45 | same | same |
| Retry | **two passes**, with a bus recovery between | **one pass** | two passes |
| CRC-8 words | checked **separately**, distinct error codes | checked together in one `if` | separately |
| Failure diagnosis | logged with cause, bus and pins | logged with no cause | logged with cause and bus |
| Conversion, clamps, TTL, MSD packing | identical | identical | unchanged |

**The retry is the one that matters.** `Firmware/src/sensor_sht40.cpp:128` runs the candidate
sweep twice, tearing the bus down and bringing it back up before the second attempt. The Nordic
port swept once, so a sensor that needed a bus recovery never got one — and on a target that
re-initialises per operation the difference is invisible in code review, because "retry" there
looks like it already re-opens the bus. It does not: the recovery is the point, not the re-open.

Recovery is a seam (`od_sensor_app_bus_recover`), because it means different things per target.
ESP32 caches one live IDF bus, so it tears down and re-selects — without that, its "retry" is a
repeat. Nordic re-initialises inside every operation and has nothing to tear down, so its
implementation is the settle alone.

**Checking both CRC words separately is not cosmetic.** A part answering with one good word and
one bad is reporting a real fault, and a combined test cannot say which.

**The error code is a LOG code, not a wire byte** — this row said otherwise in its first form and
that was wrong. Neither donor ever put it in the MSD: both failure writers emit `FF FF FF` and
clear `start + 3`, and the code (`0xFB` no bus, `0xFC` humidity CRC, `0xFD` temperature CRC,
`0xFE` read) only ever reached a warning line. The first version of the shared driver dropped that
line entirely and left the code computed and unused, so the real unrecorded change was the loss of
SHT40 failure diagnostics on both targets. The log is restored, once per failure run and reset on
the next success, as ESP32 had it.

`sht40_probe_bus_once()` is restored too — the address-only sweep of 0x44/0x45/0x51/0x55/0x6A that
the authority runs at init. It is diagnostic and never changes sensor state, but "nothing answered
anywhere on this bus" is the most useful line when a board comes up mute.

`tests/host/sensor_sht40_test.c` pins all of it — 54 checks — and is mutation-checked. Dropping
the second CRC word, collapsing the two transactions into a repeated START, reducing to one pass,
polling only the first configured sensor, reversing the address candidates, removing the
`start + 3 > 11` bound, dropping a temperature clamp, hardcoding bus 0, or comparing the TTL with
a signed-style test that breaks at the 32-bit wrap all fail it. The bus instance in every fixture
is deliberately **not** 0, because a driver that ignored `bus_id` passed the first version of this
suite.

**NOT HARDWARE-QUALIFIED.** Both targets' SHT40 rows are open in
docs/HARDWARE_VERIFICATION_CHECKLIST.md, and this change deletes both ports' driver policy.

---

## 20. GT911 register address byte order — the donor probes the undocumented order first (2026-08-24)

**Ruled 2026-08-24; implementation pending sensor-seam step 8.** Recorded now so the shared
driver's order reads as a decision rather than as a transcription slip.

GT911 register addresses are 16-bit and sent as two bytes before the data phase. GOODIX's
*GT911 Programming Guide* Rev.10 § 2.1 p.3 specifies the write frame as
`S | Address_W | ACK | Register_H | ACK | Register_L | ACK | Data...`, and § 2.2 p.4 sets the read
pointer the same way: **high byte first, unambiguously, in both directions.**

Both ports nevertheless try **low byte first** first. In the authority
(`../Firmware/src/touch_input.cpp:220-236`) that is two `if`s with early returns:

```c
if (gt911_read_reg(addr7, GT911_REG_PID, id, 4, /*reg_high_first=*/false) && match(id)) {
    *reg_high_first = 0;                      /* labelled "common" at touch_input.cpp:47 */
    return true;
}
if (gt911_read_reg(addr7, GT911_REG_PID, id, 4, /*reg_high_first=*/true) && match(id)) {
    *reg_high_first = 1;                      /* labelled "some GT911 / docs" */
    return true;
}
```

The in-tree ESP32 snapshot is the same logic with a bus argument added
(`targets/esp32-idf/src/touch_input.cpp:241`, label at `:49`). An earlier form of this entry
quoted the snapshot's signature and line numbers **as** the authority's, which inverts the rule
that the snapshot drifts and `../Firmware` decides.

| | `../Firmware` / ESP32 | nordic-zephyr | Shared (step 8) |
|---|---|---|---|
| First probe | low byte first | low byte first | **high byte first** |
| Fallback | high byte first | high byte first | low byte first |
| Bindable parts | both orders | both orders | both orders — unchanged |

**No source documents a low-byte-first GT911.** A targeted survey of the Goodix guides, the Linux
`goodix.c` driver and the ESP-BSP driver found the documented order in every one and no evidence,
authoritative or secondary, of a variant using the other. The donor's "common" annotation is
unsourced.

**Cost of the donor order on a conformant part: one wasted transaction per candidate address**, on
every boot and every re-resolve. `gt911_read_reg()` returns on the first successful *bus*
transaction and the `"911"` comparison happens above it in `gt911_probe_product()`, so a
byte-swapped pointer write that the part ACKs succeeds, returns four bytes that cannot match, and
falls through to the documented order. The three-attempt retry with its 500 µs spacing runs
only when the wrong-order transaction fails at the bus level — the uncommon case, not the ordinary
one. (An earlier form of this entry claimed the cascade ran every time; it does not, and the ruling
does not depend on it. **The saving is latency-shaped and small** — roughly one I2C transaction per
candidate address inside a reset path that already spends hundreds of milliseconds. The ruling
rests on following the documented order, not on the saving.)

**The two framings are ESP32's, not both donors'.** `Firmware_NRF54/src/opendisplay_touch.c:248`
retries three times with a **single** framing — its `gt911_read_reg_once()` always issues a
repeated START and has no STOP-separated alternative, which the in-tree Nordic port states outright
(`targets/nordic-zephyr/src/opendisplay_touch.c:112`). So the shared driver's framing fallback is
adopted from ESP32 alone, and the Nordic column below describes the byte order only.

**The fallback is retained, and that is the point of the ruling.** This is a reordering, not a
removal: a part answering only the undocumented form still binds, one probe later. Dropping the
fallback would be the change the evidence does not support — the survey establishes that nobody
has *documented* such a part, not that none exists, and no board in this fleet can settle it.

**NOT HARDWARE-QUALIFIED, and there is no row for it yet.** ESP32 is the only target that can
qualify any GT911 behaviour, and the checklist's GT911 rows today cover the bus rate, not the
probe. A row belongs with the step 8 driver that implements this, not with the ruling.

---

## 21. Charge-state polarity — every port read the flag backwards (fixed 2026-08-24)

**Fixed by project ruling.** `FOLLOWUPS` § 19 has the discovery and the outstanding config audit;
this records the resolution and the shape it took.

The canonical header defines the flag (`opendisplay_structs.h:482`):

> `OD_CHARGER_FLAG_STATE_ACTIVE_LOW` — *"charge-state (BQ25616 STAT) is active-low: charging when
> LOW"*

| `charger_flags` bit 1 | Header says charging when | Every port reported charging when |
|---|---|---|
| set (active-low) | pin LOW | pin **HIGH** |
| clear (active-high) | pin HIGH | pin **LOW** |

Both branches inverted, so the code was `!contract` for every input — not a mis-set flag, and not
a convention either. **The neighbouring flag settles that:** in the function immediately above it —
`initChargerGpio()` against `charger_gpio_charging()` in the authority, and
`od_sensor_app_bq_enable()` against `od_sensor_app_bq_charging()` in both ports —
`OD_CHARGER_FLAG_ENABLE_ACTIVE_LOW` was handled correctly everywhere. Nobody adopts an inverted
polarity convention for one charger pin and the datasheet's for the pin beside it; one operator was
simply wrong.

**The fix is an ownership change, not an operator flip.** Interpreting a config flag is policy, and
policy belongs in the shared driver — the two adapters each answering it is why one copy could
drift from the other, and why no test could see either. So the seam now carries levels:

| | Before | After |
|---|---|---|
| Enable | `od_sensor_app_bq_enable(bool on)` — target resolves polarity | `od_sensor_app_bq_enable_drive(bool level_high)` — target drives a level |
| State | `od_sensor_app_bq_charging(bool *charging)` — target resolves polarity | `od_sensor_app_bq_state_level(bool *level_high)` — target reports a level |
| `OD_CHARGER_FLAG_*` readers | both adapters | `shared/core/od_sensor_bq27220.c`, once |

`../Firmware` keeps the defect: sibling repo, reported not fixed.

**Wire-visible, deliberately.** Bit 7 of the BQ27220 MSD byte is the charging indicator, so any
board wiring STAT now advertises the opposite of what it did. That is the point — but a board whose
config was tuned by observation against the old firmware was double-inverted and correct, and is
now wrong. The config audit in `FOLLOWUPS` § 19.1 is the other half of this change and is not done.

**An unreadable state pin is UNKNOWN, and reaching that needed a pre-check rather than an error
test.** Both GPIO readers deliberately return `0` for a pin they cannot read, so a failure is
indistinguishable from LOW at the seam — and LOW is *charging* on an active-low board. Testing the
returned level for an error code is dead code on both targets; the adapters ask first instead
(`od_pin_decode()` on Nordic, a newly exported `od_hal_gpio_pin_valid()` on ESP32). `FOLLOWUPS`
§ 19.1 has the residue this does not close.

**Not hardware-qualified.** No board has confirmed the corrected reading; the row is open in
docs/HARDWARE_VERIFICATION_CHECKLIST.md.

---

## 22. GT911 `poll_interval_ms == 0` — the header says 25 ms, every firmware uses 100 (recorded 2026-08-25)

Cited by `shared/core/od_touch_gt911.c` and, until this entry, **cited at a section that did not
exist** — found in review of the promotion.

The canonical header documents the field as *"minimum time between polls; 0 = default 25 ms"*
(`opendisplay_structs.h:951`). No firmware has ever used 25.

| | `../Firmware` | `Firmware_NRF54` | Shared |
|---|---|---|---|
| `poll_interval_ms == 0` | 100 ms | 100 ms | 100 ms |
| Source of the number | `TOUCH_PROCESS_MIN_INTERVAL_MS`, the process-loop floor | same | same |

Both donors substitute their process-loop floor, not the documented default, and they do it for a
structural reason: the whole touch pass was rate-limited to 100 ms, so a 25 ms request could not
have been honoured even if the constant had said so. The shared machine returns a real delay and
could serve 25 ms now — but changing it would speed up every deployed board that left the field at
zero, which is a power change nobody asked for and which no board here can measure.

**Recorded, not resolved.** The host is the party that should move: `py-opendisplay` writing an
explicit `100` where it now writes `0` would make the config say what the firmware does. Until
then a config read shows `0` and the device polls at 100 ms, and the header's sentence is wrong
about every shipped device.

---

## 23. GT911 — waking INT destroyed the interrupt, and both donors re-attached only if they thought it was gone (fixed 2026-08-25)

Found in review of the shared touch promotion. **A deliberate departure from both donors**, and
the only behaviour change the promotion makes to a working controller.

**ONLY ONE DONOR HAD AN INTERRUPT AT ALL, and an earlier form of this entry said both did.**
`Firmware_NRF54` states outright that "INT-driven wakeups are not used here: like the button/led
modules this is a cooperatively polled module driven from the main loop" — it drives INT during
reset, because that selects the address, and never attaches a handler. The in-tree Nordic port it
fed did the same. So the defect below is the **ESP32 authority's**, and on Nordic the shared driver
introduces interrupt-driven touch that target never had. That is why the GPIO IRQ seam existed
dormant and why this promotion is its first consumer — recorded here because "Nordic gained a
capability" is a bigger statement than "a bug was fixed", and no board in this fleet can exercise
it.

Every resume path in the donor that *does* have interrupts wakes the line before re-attaching:

```c
gt911_int_wake(t);                       /* drive INT high, delay, reconfigure as input */
if (!rt->int_irq_attached) {             /* <-- still 1, so the attach is SKIPPED */
    attach_touch_int(idx, t->int_pin);
}
```
— `Firmware/src/touch_input.cpp`, and identically in the ESP32 port before this promotion.

**Reconfiguring the pad destroys the trigger on both stacks.** Zephyr's `gpio_pin_configure()`
removes the existing trigger and frees the GPIOTE channel; ESP-IDF's `gpio_config()` here sets
`intr_type = GPIO_INTR_DISABLE`. So after the wake the hardware has no interrupt, while
`int_irq_attached` still says it has one — and the guard then declines to restore it.

| | `../Firmware` / ESP32 | `Firmware_NRF54` / Nordic | Shared |
|---|---|---|---|
| Touch interrupt at all | yes | **none — polled only** | yes, on both |
| After a resume's INT wake | flag says attached, trigger gone | n/a | re-attached |
| Edges advance service | no | n/a | yes |
| Recovery | timed poll and held-low check only | polling was the design | interrupt-driven again |

**It is a latency regression, not a loss of function**, which is why it survived: the timed poll
and the held-low check still deliver every sample, so touch keeps working and simply stops being
interrupt-driven after the first panel refresh. Nothing in a log distinguishes that from a quiet
panel — and on the one donor that had interrupts, that is also the only donor anyone could have
noticed it on.

**A second stale-record case, fixed with it:** the runtime recorded only *that* an interrupt was
attached, not *which pin*. A config write can move the interrupt pin under a live controller and
nothing re-initialises touch on reload, so the new pin was attached and the old one kept its ISR
for the rest of the boot — spurious service on ESP32, and on Nordic one of only eight callback
slots consumed permanently, until no controller could attach at all. The runtime now carries the
attached pin and detaches a stale one before attaching.

The fix puts the clear where the damage is done — `gt911_int_wake()` takes the runtime and clears
`int_attached` itself, because the function that reconfigures the pad is the one that knows the
trigger is gone. `tests/host/touch_gt911_test.c` models the destruction in its GPIO fake, so a
driver that reverts to the donor form fails.

**Not hardware-qualified.** ESP32 is the only target that can exercise it.

---

## 24. GT911 post-refresh recovery — ESP32 brackets it, Nordic does not (recorded 2026-08-25)

The two ports drive the same recovery from different shapes, which the promotion had to serve
without picking one.

| | `esp32-idf` | `nordic-zephyr` |
|---|---|---|
| Before a refresh | `touchSuspendForEpdRefresh()`, nestable | nothing |
| After a refresh | `touchResumeAfterEpdRefresh()`, balanced | one unconditional call |
| Teardown | `touchForceResume()` collapses any depth | nothing |

The first cut of the shared machine offered only a suspend-counted `resume()`, which fits ESP32
and makes Nordic's recovery a **silent no-op** — its hook would return without probing anything,
so a controller disturbed by the panel stayed disturbed until enough status reads failed to
disable it. Review caught it before any board ran the code.

`od_touch_gt911_reestablish()` is therefore a separate entry point: unconditional, with no
reference to the suspend count. ESP32 keeps the counted `resume()`/`force_resume()` pair because
its brackets are real and its teardown force-resumes on paths that may not have suspended.

**Neither shape was normalised**, deliberately. Making Nordic suspend before a refresh would be a
behaviour change on a target where the refresh path was never audited for pairing — an unbalanced
suspend wedges touch for the rest of the boot, which is a worse failure than the one being fixed.

---

## 25. Touch advertising boost — adopted from the non-authority port, and reordered (2026-08-25)

Two changes the promotion makes to the touch publish path. Neither is visible on any board today,
which is exactly why they are written down.

**The boost on a touch edge is Nordic's, and the authority has no such call.** Neither
`../Firmware/src/touch_input.cpp` nor the ESP32 port it fed calls the boost from touch at all —
their touch path ends in `updatemsdata()`. `Firmware_NRF54` boosts, and the shared driver adopts
that. This is the **non-authority** side of a disagreement, taken deliberately: § 20 of the plan
records the reasoning, and the short form is that ESP32 advertises at 30-60 ms permanently and so
has no slow state a boost would rescue, while Nordic idles at 1000 ms and does.

Consequence to be aware of: on ESP32 `od_adv_app_boost()` is an empty function, so this is
behaviourally nil there **today**. It stops being nil the moment that target grows an
advertising-interval policy, which its own implementation comment anticipates.

**The order is boost-then-publish; the Nordic donor was publish-then-boost.** No target in this
repo can tell the difference: ESP32's boost is empty, and Nordic's publish only sets a pending flag
with the payload and the interval both applied later in the same loop pass. The order was chosen
because it is the one that stays correct if a target ever selects its interval *during* the
publish — the nRF/Bluefruit shape, where doing it the other way round produced a real defect
(a press advertised at the slow interval and only the release at the boosted one, so a host saw
"not pressed" reliably and "pressed" almost never; `device_control.cpp` carries the write-up).

An earlier form of the driver's comment asserted the order was load-bearing *here*. It is not, on
any target in this repo, and saying so invited a future reader to trust a mechanism that does not
exist.

## 26. Boot-screen key line — a stored key with encryption off read as "hidden" (fixed 2026-08-28)

**A deliberate departure from the authority.** The hunt started from a flashed ESP32 whose owner
had neither set a key nor enabled encryption and whose boot screen said `KEY1: hidden` regardless.
That board turned out to have a **second and independent** defect, § 27 below, and it — not this
one — is what produced the symptom there. This section stands on its own: the policy defect is
real, is the donor's, and reaches every renderer. It simply was not what that panel was showing.

`../Firmware/src/boot_screen.cpp:152-173` decides the key line from two inputs only — are the 16
key bytes all zero, and is `OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN` set. It never reads
`encryption_enabled`. The shared renderer was a faithful port of that and inherited it, so this is
not a port regression; both said "hidden" for any non-zero key, in force or not.

`encryption_enabled == 0` alongside a non-zero key is a **legal, reachable state**.
`od_config_apply_packet()` (`shared/core/od_config.c`, packet 0x27) normalises only the other
direction: a zero key clears the flag, because a device demanding authentication against a
guessable key reads as protected and is not. Nothing clears the key when the flag goes to zero,
and nothing should — an owner who disables encryption keeps the key for when they re-enable it.

So the screen claimed a protection the device does not enforce, and sent its owner looking for a
key that nothing would ask for. `od_boot_screen_render()` now derives `keyInForce` —
`encryption_enabled != 0` **and** the key is non-zero — and passes `zeroKey` when it is false, so
the two key lines, the small-screen hex and the **QR payload** all treat a key that is not in
force as absent. That last one is the only host-visible half: `od_boot_payload_build()` embeds the
key at bytes 5..20 when `show_key`, and a device that will not ask for a key no longer publishes
one to `opendisplay.org/l/`.

**Fixed at two sites, because there are two renderers.** ESP32 and Nordic share
`od_boot_screen_render()`; BG22 has its own for its small panel
(`targets/efr32bg22-slc/opendisplay_display.cpp`), reaching the same shared formatters through
`od_boot_url_build()` and `od_boot_format_key_display()`. It computed `key` and `show_key` exactly
the way the shared renderer did and carried the identical defect — rendering 32 `X`s and embedding
the key in the QR for a device that demands neither.

So the policy is not a condition each renderer derives — it is `od_boot_key_state()`
(`shared/core/od_boot_payload.c`), returning `NOT_SET` / `HIDDEN` / `SHOWN`, and both renderers
only spend the answer. Two inputs decide whether a key is in force; the show flag chooses only
between the latter two and cannot reach the answer while encryption is off or the key is absent.
The formatters could not host this — they are handed a key, not a policy — which is how the same
tree came to be written twice. A third panel layout now inherits it instead of re-deriving it.

Unchanged, deliberately: the flag itself is read correctly (bit 1,
`opendisplay_structs.h:903`) and its meaning is untouched, and `od_boot_format_key_line()` /
`od_boot_format_key_display()` keep the donors' exact strings and their `key_is_zero` + `show_key`
contract. The decision about what "the key" **is** belongs to the caller that knows the policy;
the formatters only render what they are handed.

The condition is not invented here. `py-opendisplay` already carries it as
`SecurityConfig.encryption_enabled_flag` — "True if encryption is both enabled and key is
non-zero" (`../py-opendisplay/src/opendisplay/models/config.py:828-830`) — so the host and the
boot screen now agree about what a key in force is.

`tests/host/boot_payload_test.c` walks all eight combinations of the three inputs, plus a NULL
config and a key whose only non-zero byte is the last — the zero test is over all 16 bytes, not a
prefix. `tests/host/boot_screen_test.c` pins the rendered result as an identity rather than a
second hash: a stored key with
encryption disabled renders byte for byte what a device carrying no key at all renders, and both
differ from the in-force hash. Its `make_case()` now sets `encryption_enabled = 1`, which is what
it always meant — the case was pinned against a renderer that never read the field.

Upstream keeps the defect; it is reported as `FOLLOWUPS.md` § 24.

## 27. ESP32 boot screen read a pointer as its `SecurityConfig` (fixed 2026-08-28)

**Not a divergence — a defect this repo introduced**, recorded here because it is what § 26's
flashed ESP32 was actually showing, and because § 26's fix could not have changed that panel by
itself.

`targets/esp32-idf/src/main.h` makes `securityConfig` an **alias**, not an object:

```cpp
struct SecurityConfig &securityConfig = globalConfig.security;
```

That is a Firmware_Unified adaptation — security moved inside the parsed aggregate so the zero-key
normalisation cannot be separated from the parse that applies it (`shared/core/od_config.h`), and
the old name was kept so consumers did not have to care. `targets/esp32-idf/src/boot_screen.cpp`
re-declared it as an object:

```cpp
extern struct SecurityConfig securityConfig;   /* it is a reference */
```

**This compiles and it links.** A variable's mangled name carries no type, so nothing in the
toolchain can object. A namespace-scope reference is stored as a pointer, and the map says so:
`.rodata.securityConfig 0x3c0fe464 0x4`. `&securityConfig` in that translation unit was therefore
the address of a 4-byte pointer word in flash, and `od_boot_screen_render()` read 64 bytes from
there as a `struct SecurityConfig` — a 60-byte out-of-bounds read of unrelated `.rodata`.

On `s3-n16r8-extuart-debug` those bytes were `9b 8a ca 3f 14 74 37 40 …`, the little-endian address
`0x3fca8a9b` of `globalConfig.security` followed by whatever the linker placed next:

| field | value read | consequence |
|---|---|---|
| `encryption_enabled` | `0x9b` | non-zero — "encryption on" |
| `encryption_key[16]` | `8a ca 3f 14 …` | non-zero — "key in force" |
| `flags` | `0x40` | `SHOW_KEY_ON_SCREEN` (bit 1) clear |

Which is `HIDDEN`, on every boot, for every config. The show-key flag could not have worked either,
and the QR payload described a device that did not exist. **Both the pre- and post-§ 26 policies
produce `hidden` from those bytes**, which is why the symptom survived that fix and why the two
defects are separable: § 26 is the policy, this is which object the policy was applied to.

The fix is one line — `boot_screen.cpp` includes `encryption_state.h`, which holds the single
correct declaration, and the local `extern` is gone. Nordic and BG22 were never exposed: both
reach the config through `od_get_parsed_security()`, a function, where the type is checked.

Ratcheted as "esp32: securityConfig declared only as a reference" in [tools/check.sh](../tools/check.sh),
because neither the compiler nor the linker can see this class of error and no host test can
either — the host suite never links `main.h`. The rule is an absence grep over `targets/`, so a
second consumer re-declaring the alias fails the gate rather than silently reading rodata.

## 28. Transfer rate ended after panel refresh instead of stream finalization (fixed 2026-09-03)

`DW complete` reports decompressed bytes written per transfer episode. Its elapsed interval must
therefore end when the byte stream is finalized, before the END acknowledgement, refresh barrier,
and panel refresh. `Firmware` measures that interval at finalization for direct, partial, and PIPE
uploads.

The shared transfer implementation captured its terminal snapshot after refresh, so a slow panel
inflated `t=` and understated `r=` even though `rx=`, `wr=`, `n=`, and `z=` were already settled.
The snapshot remains at the terminal outcome where it can be emitted exactly once; transfer state
now records a separate finalization timestamp and flag, and elapsed time uses that timestamp after
the stream has completed. Failures before finalization continue measuring to capture time.

Host refresh fakes advance the clock by 30 seconds. Direct, partial, and PIPE assertions pin `t=`
and `r=` to the pre-refresh interval, including PIPE's DATA-driven auto-END path.

## 29. Boot-screen QR floor on large panels (2026-09-30)

`Firmware`'s layout (`src/boot_screen.cpp`, `bootLayoutFit` and its caller) tries the middle-zone
text scale from the largest down and takes the first that fits beside a QR of *any* module size.
On a large panel with a short middle zone the largest text wins and the QR shrinks to nothing:
960x672 rendered scale-8 text beside a QR of 1-pixel modules (~49 px), which no phone reads --
found on the Telink 9.7" board. Every panel whose QR cap (`bootQrModuleMax`) is 8 or more,
i.e. >= 800x600, is exposed.

Here a first pass requires modules of at least half that cap; the authority's search is the
fallback, unchanged. Panels with a cap below 8 skip the floor: a floor there only shrank already
balanced text (400x300 and 600x448 went to scale 1). Measured on 26 geometries (128x296 to
1872x1404, several rotations): the seven with a speck-sized QR change -- 960x640, 960x672,
1024x758, 1200x825, 1304x984, 1600x1200 -- and the other 19 render byte-identical, 800x480
included. `tests/host/boot_screen_test.c` pins 960x672 and the moved 1600x1200 hash.

Reported upstream as FOLLOWUPS § 28.
