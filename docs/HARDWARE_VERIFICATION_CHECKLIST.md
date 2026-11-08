# Hardware verification checklist

**Purpose:** the living record of what has actually run on real silicon, per target and per
protocol behavior. Supersedes `plans/PLAN_OD_DISPATCH_C12_2026-08-16.md` §7's H1/H2/H3
procedure for day-to-day use: that procedure's requirement to run at the historical SHA
`a37c04b` before the final SHA (to bisect pre-C11 debt from C11-introduced behavior) is
**dropped here** — this tracks current-HEAD verification going forward, not point-in-time
attribution. If a genuine C8-C11 regression needs bisecting later, `a37c04b` is still the
right SHA to reach for; it just isn't a gate this checklist enforces.

## How to use this

- Check a row only against real on-air evidence: a raw frame transcript or a device-side log
  showing the actual behavior. A build pass, a host-suite pass, or a "notification queued" log
  line is not evidence.
- Record the date and a pointer to the evidence (log file, PR, or conversation) next to each
  checked row.
- Rebuilding or materially changing the code a row covers un-checks it until re-run. This file
  is a snapshot of verified behavior, not a promise about the current tree.
- `CLAUDE.md`'s Status section is the authoritative summary of what's verified; this file is
  the itemized detail behind it. Update both together.

---

## LED runner and panel rail — dedup D1/D2/D8 (2026-08-22)

Software-fixed on `fix/bg22-gate2-blockers`; nothing here is hardware-qualified. The
`group_repeats` sentinel changed on **all three targets**, so it carries rows on each. D1's yield
floor and D8 are BG22-only and are the rows
`plans/PLAN_DEDUP_OUTSTANDING_2026-08-22.md` § 8 names as blockers on that target's Gate 2 run.

Wire encoding for these rows: `group_repeats` is sent **minus one**, so "raw `0x02`" is a request
for three groups. `py-opendisplay` sends raw `0xFE` for indefinite and never sends a finite `0xFF`.

### EFR32BG22 (`efr32bg22-slc`) — CMD_SPI_RAW + config ceiling, 2026-11-06

Unit **ODBEEBA6** (2.9in BWRY GDEY029F51H, 168x384), 3.0 V from a PPK2, encrypted link,
`OD_CONFIG_MAX_SIZE=3120` / `NVM3_DEFAULT_MAX_OBJECT_SIZE=3136`, image from `f012ca3`.

- [x] **Config ceiling moved, and the boundary is exact.** `CMD_CONFIG_WRITE` start frames only,
      so nothing is committed and the live config is untouched. Before the OTA: 2048 ACCEPTED,
      2049 REFUSED. After: 3120 ACCEPTED, **3121 REFUSED**, and **2112 — the measured 16-colour
      2.9in config size — went from REFUSED to ACCEPTED**, which is the whole point of the change.
- [x] **NVM3 still opens, and the stored config survived.** This is the row that matters: the
      Firmware_Silabs failure of 2026-11-02 compiled fine and then would not open, reading every
      config back as 0 bytes. The pre-existing 1325-byte blob read back intact after the OTA
      (`2d 05` total, system + DisplayConfig ic `0x001E` 168x384 present).
- [x] **CMD_SPI_RAW (0x0084) answers on all paths.** Before the OTA every sub-opcode TIMED OUT --
      an unrouted opcode is silent, not NACKed (`OD_FRAME_UNKNOWN_OPCODE` emits no frame). After:
      `CMD` before OPEN -> NACK `0x02` NOT_OPEN; `BUSY` with no args -> NACK `0x01` BAD_ARGS;
      sub `0x7F` -> NACK `0x05` UNKNOWN_SUB; OPEN (pins only) -> ACK; `BUSY 1000 ms` -> ACK with
      payload `00 00 00`, i.e. elapsed 0 and not-busy, correct for a pins-only OPEN that sent
      nothing; CLOSE -> ACK; `CMD` after CLOSE -> NACK NOT_OPEN, so teardown works.
- [x] **The CMD path really clocks the bus.** `--poke` sent command byte `0x71` -> ACK, and a
      following BUSY still answered, so the session survived a real write.

- [x] **A >2048 B config COMMITS and round-trips.** This was the open row above; it is now closed.
      The real 16-colour drive config for 0x001E went on via `migrate_drive_config.py`:
      **1325 B -> 2144 B**, and the delta of **819 B is exactly** the three expected additions
      measured beforehand with `packets_from_yaml` (colour16 mode 140 + wave_edit LUT 543 +
      calibration 136). Read back **2144/2144 B COMPLETE over 23 frames** and parsed by
      py-opendisplay's own deserialiser.
- [x] **The drive set is complete and correctly shaped.** `0xB1` panel; `0xAE` **two** modes --
      `mode_index=0 scheme=0xFF passes=1` (full) and `mode_index=1 scheme=10 passes=3`
      (colour16); `0xAF` **two** waveform slots at 539 B each; `0xB0` calibration bound to
      `mode_index=1`. No `unknown_ex` packets, so nothing was left unparsed.
- [x] **`bwry16` stayed a MODE, not a scheme.** DisplayConfig `0x20` still reports
      `color_scheme=3` (BWRY) while scheme 10 appears only on the `0xAE` mode packet -- the
      invariant that keeps an unknown scheme from silently degrading a panel to greyscale.
- [x] **Device identity survived the rewrite.** `0x27` security is PRESENT in the read-back and
      the unit still answers on the encrypted link.

**Two tooling defects found while doing this, both in HOST code, neither a firmware fault:**
`Firmware_Silabs scripts/dump_config.py` reads the CONFIG_READ response's *chunk-index* field as
the declared total, gets 0, and stops after two frames -- which made a healthy 1325 B config look
like a 272 B truncation. And the config read stream is **not** uniformly framed: the FIRST frame
is `[00][40][chunk_no:2][total_len:2][tlv...]` (6-byte prefix) while the rest are
`[00][40][chunk_no:2][tlv...]` (4-byte). Stripping 4 from the first frame shifts the whole blob by
two bytes and shows up as a bogus "unknown packet type 0x00" immediately after the first packet.
`device.py interrogate()` gets this right; hand-rolled readers must match it.

### EFR32BG22 (`efr32bg22-slc`) — Firmware_Silabs `feat/od-epd-driver` parity, 2026-11-06

Built (246,908 B; 7,044 B real headroom against 253,952). ODBEEBA6 OTA'd twice on 2026-11-06 (the
second image adds the write/refresh fix below); PPK2 rail at 3.0 V, encrypted link. **No camera covers
this bench**, so render rows are protocol-level until someone looks at the glass.

- [x] **16-colour via `0x0085`, protocol level.** Mode 1 (`colour16`): START ACK `mode 1, passes 3,
      temp 25 C`; 3 x 16128 = 48384 B accepted; END `00 72`; refresh notification `00 73` (success).
      The first OTA'd image ACKed START and then NACKed the first DATA (`ff 71`): `od_xfer_app_write`
      and `od_xfer_app_refresh` only admitted `XFER_APP_FULL`, so a drive-mode transfer could never
      stream. Host tests stub the display layer and could not see it.
- [x] **16-colour visually correct on glass** (4x4 index16 grid, full panel). Confirmed by eye by
      the user on 2026-11-06; no camera record.
- [ ] **Temp policy.** A mode whose window excludes the die temperature: REJECT NACKs `0x06`
      with nothing driven; SUBSTITUTE answers with the substituted mode index.
- [x] **`READ_IDENT` (`0x0046`) pre-gate.** Encrypted unit, no key, no session: answered
      `00 46 04 03 01 1e 00 a8 00 80 01 03` -- protocol minor 4, Silabs, one display, ic `0x001E`,
      168x384, scheme 3.
- [x] **Config survives the OTA.** 2144 B read back byte-identical to the pre-OTA backup after both
      OTAs (2 modes, 2 LUT slots, panel, calibration).
- [ ] **Boot screen after an OTA with no drive packets.** Boot logs "no drive packets; will retry";
      writing the drive config then draws the QR without a power cycle, and the config-write ACK
      arrives within py-opendisplay's 5 s bound (the redraw follows it).
- [ ] **Burned `0x2E`–`0x31` walked past.** A config carrying them still drives its panel from the
      `0xAE`–`0xB1` packets behind them.
- [ ] **CS_SELECT (`0x08`).** Needs a split-controller panel with `cs_pin_2` wired; none on the
      bench. Host-tested: sticky, reset to CS 0 per pass, refused before any I/O where unwired.
- [ ] **READ/WRITE_SCRATCH (`0x09`/`0x0A`).** Needs a panel with a read-back path (UC8159 EEPROM,
      or a 3-wire controller on `data_pin`); none on the bench. Host-tested: bounds, zeroed per
      pass, read-then-write data path.

### EFR32BG22 (`efr32bg22-slc`) — Gate 2 blockers

- [ ] **D1 liveness:** send `0x0073` with every loop count and delay zero and `group_repeats` raw
      `0xFE`, then confirm the device still answers a subsequent command (e.g. FIRMWARE_VERSION)
      and advertising continues. Before the fix this wedged the superloop permanently.
- [ ] **D1 sentinel, raw `0xFE`:** an endless pattern runs until `LED_STOP`.
- [ ] **D1 sentinel, raw `0xFF`:** same — it must NOT stop immediately, which is what shipped.
- [ ] **D1 finite count:** raw `0x02` flashes exactly three groups, stops on its own, and a later
      `LED_STOP` reports "already idle" rather than stopping a live pattern.
- [ ] **D8 rail with `pwr_pin` configured:** an image upload renders after a cold panel bring-up;
      the 800 ms settle is charged once per switch-on, not per refresh.
- [ ] **D8 rail with `pwr_pin == 0xFF`:** the device must not drive PA0. Confirm on a board whose
      config leaves the rail unset.
      **Read this before the first flash:** the removed fallback drove PA0 whenever `pwr_pin` was
      unset, so a bench board that relied on it now sees no rail and the panel stays dark. That is
      the fix working, not a regression — put the real pin in the config.
- [ ] **D2 (build evidence, not on-air):** the flashed image is built from the same
      `shared/profiles.cmake` list the host suite compiles. Record the profile hash or the
      `opendisplay-bg22.cmake` SHA alongside the run.

### ESP32 (`s3-n16r8-extuart-debug`) — sentinel only

The yield floor and the rail fix are not ESP32 changes; only `repeat_forever` is.

- [ ] **Raw `0xFE`:** endless until `LED_STOP`.
- [ ] **Raw `0xFF`:** endless — before the change this stopped immediately, so this is the row
      that proves the fix rather than the absence of a regression.
- [ ] **Finite count:** raw `0x02` flashes exactly three groups and stops on its own.

### Nordic (`xiao_nrf52840`) — sentinel only

- [ ] **Raw `0xFE`:** endless until `LED_STOP`.
- [ ] **Raw `0xFF`:** endless — the row that proves the fix.
- [ ] **Finite count:** raw `0x02` flashes exactly three groups and stops on its own.

---

## Shared buzzer runner (2026-08-23)

Software candidate on `feat/buzzer-shared`. A build or the 300-plus-check host suite is not
evidence for a physical square wave.

**All five rows below are marked verified by explicit user direction, 2026-08-29 — not by
per-row on-air evidence.** The only actual observation behind them is the 2026-08-28 ESP32
session: a buzzer melody command was sent and produced audible output, with no timing or
lifecycle measurement taken, and no Nordic session at all. The user directed these rows be
checked regardless. If this section is ever relied on to mean "measured," it does not; re-open
and re-run each row against real on-air evidence per this file's own standard (see "How to use
this" above) before trusting it for anything beyond "the user decided to call it done."

**The pitch/folding rows (ESP32 and Nordic) are removed, not just unchecked** — 2026-08-29, by
direction: any row whose only possible evidence is an electrical measurement at the drive pin
(frequency, duty cycle) is out of scope for this checklist going forward, on the same footing as
the GT911 section's existing exclusion of instruments beyond the serial log and the MSD. If pitch
accuracy needs re-establishing later, it needs a new row written against a channel this document
can actually use.

- [x] **ESP32 schedule/cap:** a two-pattern melody preserves tones, rests, the 20 ms inter-pattern
      gap and outer repeats; a repeating 1275 ms step stops at 30,000 ms, not 5,000 ms. — marked
      by direction, 2026-08-29; not measured.
- [x] **ESP32 lifecycle:** a second melody preempts the first, session teardown leaves it running,
      and deep sleep/power-off silence it before the existing two-chirp shutdown alert. — marked
      by direction, 2026-08-29; not exercised.
- [x] **Nordic schedule/cap:** the ESP32 timing vector matches, including the 30,000 ms cap and
      enable-pin polarity. — marked by direction, 2026-08-29; not run.
- [x] **Nordic responsiveness:** commands and advertising continue during a 30-second melody, and
      a new melody preempts it without leaving the software square-wave timer armed on the old pin.
      — marked by direction, 2026-08-29; not run.
- [x] **BG22 build exclusion:** the production map has no `od_buzzer*`, tone state or 256-byte
      melody buffer, and RAM remains at the pre-promotion value. This is build evidence, not
      on-air. — the only row of the five with a real basis: BG22 was built this session
      (`efr32bg22-slc`, 2026-08-28) and the profile declines `APP_BUZZER`.

---

## Transfer Phase 1 — shared `od_zlib_pump`

These rows apply to the Phase 1 candidate introduced after the dated transfer results below; an
older compressed upload does not qualify the shared pump.

- [x] ESP32 tinfl profile: compressed direct, partial and PIPE through refresh — cleared 2026-08-18
- [x] ESP32 portable-inflater profile: compressed direct, partial and PIPE through refresh —
      cleared 2026-08-18
- [x] Nordic nRF54 class: compressed direct, partial and PIPE through refresh — cleared 2026-08-18
- [x] Nordic `xiao_nrf52840`: compressed direct, partial and PIPE through refresh — cleared
      2026-08-18
- [x] EFR32BG22: compressed direct through refresh — cleared 2026-08-18
- [x] Each exercised target: truncated/failed stream aborts, then a fresh compressed transfer
      succeeds — cleared 2026-08-18

Phase 1 was marked cleared by project direction on 2026-08-18. Phase 2 direct/partial production
cutover is unblocked; its own per-target hardware gates remain mandatory.

---

## Transfer Phase 2 — ESP32 steps 10a/10b

The ESP32 software candidate replaces `0x70`, `0x71`, `0x72` and `0x76` with shared `od_xfer`.
These rows are new evidence requirements; the older transfer results below do not qualify it.

- [ ] FastEPD: plaintext and encrypted raw/compressed direct through refresh
- [ ] bb_epaper: plaintext and encrypted raw/compressed direct through refresh
- [ ] Partial: etag match/mismatch, valid/invalid rectangles, both plane boundaries and failure
      clearing the etag
- [ ] Replacement in both directions: PIPE START aborts live legacy transfer; legacy START aborts
      live PIPE; the displaced owner's DATA/END is inert; a fresh transfer then succeeds
- [ ] Disconnect/reconnect during direct and partial, followed by a successful authenticated push
- [ ] END ACK observed before refresh begins; refresh success and timeout paths observed
- [ ] Plaintext LAN and TLS-LAN direct, including a 4,092-byte DATA chunk; LAN disconnect affects
      only a LAN-owned transfer

**Partial evidence, 2026-08-28 — no row above is checked by it.** Same board and build as the
Phase 3 note below (`s3-n16r8-extuart-debug`, `0.1.1-330-g01366ce-dirty`, Seeed reTerminal E1001):
**plaintext** legacy direct (`0x70`/`0x71`/`0x72`) and legacy partial (`0x76`) uploads both
completed through refresh. Reported in conversation.

**Second run, same day/board/build/session — encrypted, compressed, happy path only.** Legacy
direct and legacy partial uploads both completed through refresh under `od_session` encryption.
Reported in conversation; still no row above is checked by it.

**This is the bb_epaper row, not the FastEPD row.** `panel_ic_type` 0x003B resolves to
`EP75_800x480_GEN2` (`targets/esp32-idf/src/display_service.cpp:583`), and `fastepd_driver_used()`
(`:605-621`) returns true only for ED103TC2 and Inkplate 5V2/10. The FastEPD row therefore has no
evidence at all and needs different glass.

Why each row stays open:

- The bb_epaper row wants plaintext **and encrypted**, **raw and compressed**. Encrypted has now
  run once, **compressed only** — the plaintext run's compression type is still not reported, so
  raw remains untested in both modes and encrypted-raw specifically is untested.
- The partial row wants etag **mismatch**, **invalid** rectangles, both plane boundaries and a
  **failure clearing the etag**. Only the accepting path ran (both times), so the entire refusal
  half — which is what the row exists for — is untested.
- Replacement, disconnect/reconnect, END-ACK-before-refresh and the LAN rows are untouched.

The Nordic software candidate was implemented by project direction before these ESP32 rows were
recorded. That sequencing exception is not hardware evidence and does not qualify either target.
The bidirectional-replacement row above qualifies ESP32 only. Nordic's 10a change must add and run
its own row for the same two directions; ESP32 evidence cannot qualify Nordic's separate interim
adapter/PIPE arbitration.

---

## Transfer Phase 2 — Nordic steps 10a/10b

The Nordic software candidate replaces `0x70`, `0x71`, `0x72` and `0x76` with shared `od_xfer`.
All rows are new evidence requirements; builds and the older transfer results below do not qualify
them.

- [ ] nRF54 class: plaintext and encrypted raw/compressed direct through refresh
- [ ] `xiao_nrf52840`: plaintext and encrypted raw/compressed direct through refresh
- [ ] Partial: etag match/mismatch, valid/invalid rectangles, both plane boundaries and failure
      clearing the etag
- [ ] Nordic replacement in both directions: PIPE START aborts live `od_xfer`; legacy START aborts
      live target PIPE; the displaced owner's DATA/END is inert; a fresh transfer then succeeds
- [ ] Disconnect/reconnect during direct and partial, followed by a successful authenticated push
- [ ] END ACK observed before refresh begins; refresh success and timeout paths observed
- [ ] Shared-policy normalizations: an etag-less successful full refresh clears the prior etag;
      controller-plane incomplete END refuses; packed-row incomplete END proceeds to refresh

The BG22 software candidate was implemented by project direction before these rows were recorded.
That sequencing exception is not hardware evidence. The bidirectional-replacement row is
Nordic-specific and cannot inherit the ESP32 result.

A `xiao_nrf52840` flash of post-step-11 HEAD on 2026-08-19 completed an encrypted PIPE upload and
a config read and config write (recorded in the board section below). That shows the promoted
routing did not regress the PIPE and command paths; it exercises none of the `0x70`/`0x71`/`0x72`/
`0x76` rows above, which stay open.

**Partial evidence, 2026-08-30, `xiao_nrf52840` at current HEAD (`0da011d`) — no row above is
checked by it.** Both plaintext and `od_session`-encrypted runs completed a **compressed** direct
write (`0x70`/`0x71`/`0x72`) and a **compressed** legacy partial write (`0x76`) through a
successful panel refresh. Happy path only in both cases: raw (uncompressed) payloads were not
exercised for either opcode, so row 2 stays open on the raw half; the partial run committed a
matching etag but did not attempt a mismatched one, and did not cross a plane boundary or send an
invalid rectangle, so row 3 stays open. Refresh success was observed on air for all four runs
(direct/partial × plaintext/encrypted) — the success half of row 6 — but the refresh-timeout path
was not induced, and disconnect/reconnect during a live direct or partial transfer (row 5) and the
Nordic bidirectional-replacement row (row 4) were not exercised this session. Reported in
conversation; no separate device-side transcript captured.

---

## Transfer Phase 2 — EFR32BG22 step 10a

The BG22 software candidate replaces `0x70`, `0x71`, `0x72` and capability-off `0x76` with shared
`od_xfer`. All rows are new evidence requirements; builds and the Phase 1 result do not qualify
them.

- [ ] Plaintext and encrypted raw direct through refresh
- [ ] Plaintext and encrypted compressed direct through refresh
- [ ] Controller-plane split crossed in one DATA frame; both planes arrive in controller order
- [ ] Disconnect/reconnect during direct, followed by a successful authenticated push
- [ ] END ACK observed on air before refresh begins; refresh success and timeout paths observed
- [ ] TX-report failure and two-second timeout: no refresh, panel powered off, issuing connection
      closed, replacement connection left open
- [ ] `CMD_PARTIAL_WRITE` returns `FF 76 02 00`; map confirms no partial or displayed-etag state
- [ ] Shared-policy normalization: controller-plane incomplete END refuses, while packed-row
      incomplete END proceeds to refresh

BG22 has no PIPE, so it has no bidirectional legacy/PIPE replacement row and no Phase 2 10b debt.

---

## Transfer Phase 3 — PIPE

The shared-machine software candidate was implemented by project direction on 2026-08-20 while
the Phase 2 and Nordic SPIM hardware entry rows remained open. No compatible hardware was
available in the implementation environment. That sequencing exception is not qualification:
every row below requires new post-promotion evidence, and no Phase 1, Phase 2 or earlier PIPE run
qualifies it.

Software evidence captured 2026-08-20: the host suite passes 58/58 under GCC, Clang and
ASan/UBSan; W=32, W=16 and capability-off PIPE builds pass; the five pre-auth fuzz targets and
the pinned py-opendisplay 7.14.0 corpus pass; and all 11 ESP32 configurations, all three Nordic
boards and the BG22 headless image build. Handwritten production source is +938/−2,960 lines
(net −2,022); test/tool source is +1,517/−1,060 (net +457). The reorder array is 8,118 B at
W=32 and 4,182 B at W=16, plus 20 B of PIPE state in either profile. The BG22 link contains only
the three small `od_pipe_*` entry points, no
PIPE state/reorder symbol, and remains 250,292 B flash / 32,284 B static RAM (480 B headroom).
Board-only throughput, retransmission, refresh-time and stack-high-water measurements remain
unavailable and do not qualify any row below.

### ESP32 (`s3-n16r8-extuart-debug`, plus a classic ESP32 W=16 board if available)

- [ ] Plaintext and encrypted full-frame PIPE, raw and compressed, through refresh
- [ ] PIPE-partial: region streamed, partial refresh, new etag committed; etag mismatch refused
- [ ] Forced loss, reorder and retransmission; gap SACK observed and transfer recovery
- [ ] Negotiated on-wire maximum in plaintext and encrypted sessions: boundary accepted and
      one-byte-over refused, including a CCM frame whose decrypted body alone fits
- [ ] Tail below cadence completes without a stall
- [ ] Sequence wraps past 255 inside one transfer
- [ ] END ACK observed on air before refresh; refresh success and timeout both observed
- [ ] `OD-S1` replay injection via `dispatch-gate`, including the corrupted-tag control
- [ ] Inactive/fatal/zero-length DATA is silent; inactive/fatal END answers plaintext `FF 82`
- [ ] Second-connection DATA during a live PIPE is inert
- [ ] LAN and TLS-LAN `0x80`/`0x81`/`0x82` produce the exact four-byte refusals with the deployed
      seal-or-plain choice and leave a live BLE-owned transfer untouched
- [ ] Replacement in every direction: PIPE↔PIPE, PIPE↔legacy, and malformed BLE PIPE START
      displacing the live owner; displaced owner inert and a fresh transfer succeeds
- [ ] Disconnect mid-PIPE, reconnect, re-authenticate and complete a fresh upload
- [ ] Classic ESP32 `OD_PIPE_MAX_W=16`: negotiated W=16 honoured and reorder queue sized 17

**Partial evidence, 2026-08-28 — no row above is checked by it.** On a `s3-n16r8-extuart-debug`
build (device banner `0.1.1-330-g01366ce-dirty`, Seeed reTerminal E1001, 800x480, panel IC
0x003B), **plaintext** PIPE full-frame and PIPE-partial uploads both completed through refresh.
Reported in conversation; the device-side capture for the session is the BLE log of the same date.

**Second run, same day/board/build/session — encrypted, compressed, happy path only.** PIPE
full-frame and PIPE-partial uploads both completed through refresh under `od_session` encryption.
Reported in conversation; still no row above is checked by it.

This is the first exercise of shared `od_pipe.c` on real silicon on any target — Nordic's
2026-08-19 encrypted PIPE run predates the Phase 3 promotion and covers the target-local machine
that replaced it. It is recorded because it is the first, not because it qualifies anything.

What it deliberately does **not** establish, and why each row stays open:

- Row 1 wants plaintext **and encrypted**, **raw and compressed**. Encrypted has now run once,
  **compressed only** — the plaintext run's compression type is still not reported, so raw remains
  untested in both modes and encrypted-raw specifically is untested.
- Row 2 wants the new etag committed **and an etag mismatch refused**. Only the accepting path
  ran (both times). The refusal is the half that protects a client from silently overwriting the
  wrong frame, and it is untested.
- No negative, loss, reorder, boundary, replacement or disconnect case was exercised, so rows 3
  onward are untouched.

A happy-path upload is the weakest evidence PIPE can produce: every refusal path in
`od_pipe.c` is unexercised by it, and those are the paths the Phase 3 review specifically
restored (raw trailing-byte truncation, raw partial overflow refusal, per-transfer target
preparation, force-off on incomplete END).

### Nordic (`xiao_nrf52840` mandatory; one nRF54-class board)

- [ ] Plaintext and encrypted full-frame PIPE, raw and compressed, through refresh
- [ ] PIPE-partial: region streamed, partial refresh, new etag committed; etag mismatch refused
- [ ] Forced loss, reorder and retransmission; gap SACK observed and transfer recovery
- [ ] Negotiated on-wire maximum in plaintext and encrypted sessions: boundary accepted and
      one-byte-over refused, including a CCM frame whose decrypted body alone fits
- [ ] Tail below cadence completes without a stall
- [ ] Sequence wraps past 255 inside one transfer
- [ ] END ACK observed on air before refresh; refresh success and timeout both observed
- [ ] `OD-S1` replay injection via `dispatch-gate`, including the corrupted-tag control
- [ ] Inactive/fatal/zero-length DATA is silent; inactive/fatal END answers plaintext `FF 82`
- [ ] Replacement in every direction: PIPE↔PIPE, PIPE↔legacy, and malformed BLE PIPE START
      displacing the live owner; displaced owner inert and a fresh transfer succeeds
- [ ] Disconnect mid-PIPE, reconnect, re-authenticate and complete a fresh upload
- [ ] Compressed PIPE is accepted when the config lacks the streaming-decompression bit
- [ ] One nRF54-class board repeats full raw/compressed PIPE, forced reorder/recovery and the
      negotiated-frame bound

**Partial evidence, 2026-08-30, `xiao_nrf52840` at current HEAD (`0da011d`) — no row above is
checked by it.** Both plaintext and encrypted runs completed a **compressed** full-frame PIPE
upload and a **compressed** PIPE-partial upload, each through a successful panel refresh. This is
the first on-board run of PIPE-partial (`0x0080` flags bit1) since the flags-word fix recorded in
the board section below — it now accepts and completes rather than refusing. Happy path only: raw
PIPE was not exercised (row 1 stays open on the raw half); the partial run committed a matching
etag but an etag mismatch was not attempted (row 2 stays open on the refusal half); no loss,
reorder, boundary, sequence-wrap, replacement or disconnect case was exercised, so rows 3 and
onward remain untouched by this run. Reported in conversation; no separate device-side transcript
captured.

### EFR32BG22 (`efr32bg22-slc`)

- [ ] `0x80` answers `FF 80 04 00`; `0x81` and `0x82` answer nothing
- [ ] Link map confirms zero PIPE state/reorder storage and static RAM at or below the recaptured
      Phase 0 baseline

---

## Transfer Phase 4 — NFC (0x0083)

The shared-machine software candidate was implemented by project direction on 2026-08-21, steps 5,
6 and 7 in sequence, while every hardware row below remained open. **NO NFC-ENABLED HARDWARE
EXISTS IN THIS FLEET**: no board carries an antenna, so none of these rows is merely awaiting a
free bench — they await hardware that has to be built or bought. That is a stronger form of open
than the Phase 2 and Phase 3 exceptions above, and it is why every row here is release debt rather
than a queued task.

The sequencing exception is not qualification. No Phase 1-3 run, and no amount of host coverage,
qualifies any row below.

Software evidence captured 2026-08-21: `tools/check.sh --targets` passes 33/0/0 with no skip; the
shared suite is 336 checks at `OD_CAP_NFC=1` and 32 at `OD_CAP_NFC=0`; `tools/mutate_nfc.sh`
reports all seven mandatory mutations detected; the Nordic and BG22 reference fixtures frozen at
step 1 pass against the shared machine on every input except the deliberate N1/N4/N6 changes, each
recorded in `docs/DIVERGENCE_MATRIX.md`. Nordic recovered 762 B of RAM at its cutover; BG22
recovered 512 B of heap (`heap_size` 0x2ad0 -> 0x2cd0).

X3's baseline is **measured, not inferred**: a clean BG22 build at `def82a1` — main before
`od_nfc.c` existed — gives `heap_size` **0x2cf0 (11,504 B)**, against 0x2cd0 (11,472 B) now. That
is **32 B of heap lost**, inside the 64 B ceiling. An earlier arithmetic estimate said 16 B and was
wrong by half the figure, which is the reason the plan asks for a build rather than a subtraction.

### Step 9 consolidation, 2026-08-22

Every figure below is a build at this branch's HEAD against a build at `def82a1`, not a running
total carried forward from the per-step commits.

| | flash | RAM | what it measures |
|---|---|---|---|
| **`xiao_nrf54lm20a`** | **+112** | **−234** | **the promotion alone** — NFC was already enabled here at `def82a1`, so nothing else moves |
| `xiao_ble` (nRF52840) | +7,660 | +2,602 | promotion **plus** enabling `CONFIG_NFC_T2T_NRFXLIB` |
| `xiao_nrf54l15` | +8,068 | +2,598 | same |
| `efr32bg22-slc` | +92 | `heap_size` −32 | `data + bss` unchanged at 32,284 **by construction** |

**lm20a is the number to quote for the promotion**, and it was nearly not measured: an earlier
draft of this table claimed it had no Phase 4 baseline because NFC was already on there. That is
exactly backwards — being already on is what makes it the only board where the library is not
confounding the result. It costs 112 bytes of flash and **saves 234 bytes of RAM**, which is the
244-byte response buffer and the 512-byte assembler leaving, less what the shared state costs.

The other two Nordic figures are dominated by enabling the T2T library, which is a capability
decision rather than a promotion cost. Netting them together would make the promotion look
expensive on Nordic when the library is what costs.

Its link map agrees: no `s_nfc_write_chunk` or `od_cmd_app_nfc`, both `od_nfc_*` entry points
present, and the two `od_nfc_app_*` seam symbols present because Nordic implements the tag.

**Source, whole phase, counted by CATEGORY rather than by path.** The distinction matters because
`targets/esp32-idf/tools/od-device-cli.py` lives under `targets/` and is a tool, not firmware:

| | added | removed |
|---|---|---|
| shared production (`od_nfc.{c,h}`, `od_nfc_app.h`) | 353 | 0 |
| shared wiring (`od_dispatch{.c,_ops.h}`, `od_caps.h`, `od_core.{c,h}`, `od_cmd_app.h`, `od_txq.h`, `od_xfer.c`) | 34 | 14 |
| build wiring (`shared/sources.cmake`) | 8 | 0 |
| **target firmware** | **82** | **347** |
| tests and tools | 2,196 | 102 |

Target firmware is a net deletion of 265 lines, and even that understates it: what left was wire
parsing, a chunk assembler and reply construction; what arrived is adapter and build wiring. Tests
and tools are reported separately per § 10 and deliberately not netted against production.

**Zero, and checked rather than assumed.** No target-side NFC assembler, parser, hook or
wire-response literal survives: the § 8 ratchet greps eight symbols over `targets/**`, and
`RESP_NFC_ENDPOINT` appears in no target `.c`/`.cpp`. All 11 ESP32 board images carry both
`od_nfc_*` entry points, no `s_nfc`, and no `od_nfc_app_*` reference.

**Software gate at step 9 close: `tools/check.sh --targets` 34 passed / 0 failed / 0 skipped.**
The 33/0/0 recorded above was the step 5-7 figure; the extra check is the § 8 ratchet that landed
with the dispatch reroute.

**Hardware rows: 0 passed, 16 open.** Not one `0x0083` row has run, and none can here.

### Phase 5 close, 2026-08-22

**Step 10 — deletion inventory: empty, and that is the result rather than an omission.** X1 forbids
speculative deletion, so the inventory was built from evidence and came back with nothing to
delete. Checked: orphaned declarations in target headers (none); target constants that lost
their last user (`OD_NFC_ASSEMBLY_MAX` and `OD_NFC_READ_MAX` are live in
`shared/core/od_nfc.{c,h}`;
**`OD_PIPE_MAX_PAYLOAD` has no user left anywhere** — the Nordic NFC cutover removed
its last one, and it stays because `opendisplay_protocol.h` is a byte-for-byte copy of the
canonical header and is not ours to edit, which is a different reason from being referenced);
target-side response literals for promoted opcodes (none);
target-local transfer, pump, PIPE or NFC state (none); build entries naming deleted files (none —
and a real one would fail the build); and every scaffold the plan created, all still referenced.

The reason is that each cutover deleted its own orphans in the same commit: `7c4fcb5` removed
`od_cmd_nfc.h`, `5b6ecbf` removed `od_cmd_nfc.c`, and the BG22 and ESP32 wrappers went from files
that keep their other hooks. Dead *code* cannot survive `-Wall -Wextra -Werror`, so what a sweep
could still find is dead declarations and macros — and the ones present are pre-existing driver
registers and config-packet constants that no promotion orphaned. Deleting those would be exactly
the speculative deletion X1 rules out.

**Step 12 — every LINK MAP read, not merely produced.** The plan asks for maps, and a symbol
table is not one: `nm` says a symbol exists, the map says which object contributed it. That
difference is the whole value here, because the question is whether PIPE and NFC state comes from
`shared/` or from a target.

Criterion: for each `.bss.<symbol>` entry, the contributing object named on that line — or on its
continuation line, which is how the format wraps a long section name. All 15 maps, every image:

| image | `s_pipe` | `s_reorder` | `s_nfc` |
|---|---|---|---|
| `efr32bg22-slc` | — | — | `od_nfc.c.obj` |
| Nordic × 3 | `od_pipe.c.obj` | `od_pipe.c.obj` | `od_nfc.c.obj` |
| ESP32 × 11 | `od_pipe.c.obj` | `od_pipe.c.obj` | — |

Every occurrence is attributed to a `shared/core/` object; no target object contributes any of
them. The two dashes are the capability-off arms carrying no state at all — BG22 declines PIPE,
ESP32 declines NFC — which is the claim `nm` alone cannot make, since a symbol's absence looks the
same whatever removed it.

A first pass at this table read a one-line context window and attributed Nordic's `s_pipe` to
`od_nfc.c.obj`, because the next map entry happened to be `s_nfc`. The criterion above is what
fixed it, and it is stated so the next reader can reproduce it rather than trust the
result.

**Step 13 — release evidence.** Per-unit measurements are recorded above under Step 9
consolidation, per phase and per target rather than for a final squash. The § 9 matrix stands at
**0 passed, 16 open**, every open row named with what it needs. No row is closed by this phase, and
none is closed by any amount of the software evidence above it.

### Nordic — needs a board with an NFC antenna fitted

Enabling `CONFIG_NFC_T2T_NRFXLIB` (2026-08-21) makes the tag real on all three boards; none has an
antenna, so none of this has run.

- [ ] Inline write ≤ 120 bytes, read back by an independent NFC reader
- [ ] Chunked 512-byte write **as `OD_NFC_REC_RAW_NDEF`**, read back the same way. Every other
      record type is an NDEF short record capped at 255 payload bytes and is refused at END with
      `0x03` — that refusal is correct behaviour, not a defect
- [ ] A 218-byte read, in plaintext and encrypted sessions
- [ ] A 219-byte **verbatim or well-known** record truncated to 218 — Nordic's adapter behaviour,
      asserted here and nowhere else (N2b)
- [ ] A 219-byte **MIME** record **refused** with `0x02`. Nordic does not truncate uniformly: its
      MIME arm refuses anything that would not fit whole (`opendisplay_nfc.c`, the
      `out_pack > out_max` test), so a row exercising only truncation would report the adapter as
      uniform when it is not
- [ ] BLE disconnect mid-assembly, then a fresh START from a new connection
- [ ] Tag hardware absent or failing, answering `0x02` / `0x03`
- [ ] The § 3.4 divergence-3 frame (`00 83 01 00 FF FD`) answered `0x01`, device still alive
- [ ] **READ-path stack high-water** (N7): the response buffer is now a 224-byte stack local
      nesting with `od_reply()`'s sealed buffer
- [ ] A config that assigns P0.09 or P0.10 is refused. **That is the whole row**, because the
      complementary half has nothing to run against: every shipping nRF52840 image enables NFCT
      unconditionally (`xiao_ble_nrf52840.conf:70`, `.overlay:235`), so no GPIO-owning image
      exists. An earlier draft of this row claimed one and also asked for a config-driven form;
      both were wrong. UICR NFCPINS latches the pads at reset, so handing them to GPIO needs an
      image that disables NFCT *and* sets `CONFIG_NFCT_PINS_AS_GPIOS`, plus a UICR erase and
      reboot to take effect. That variant is unbuilt and unspecified — see `docs/FOLLOWUPS.md`.
      The refusal itself is host-tested (`tests/host/nordic_nfc_pins_test.c`), so this row is on-air
      confirmation, not first coverage

### EFR32BG22 — needs a board with a TNB132M fitted

- [ ] Inline write ≤ 120 bytes, read back by an independent NFC reader
- [ ] Chunked write **above 240 bytes** as `RAW_NDEF`: capture the I2C block sequence and read back
      every byte. The write loop's block offset truncates at `i == 15`
      (`opendisplay_ble.c:1526-1530`), so this is an **addressing investigation**, not a presumed
      pass. 240 must pass, 241 is the minimal reproducer, 512 is the deployed-scale case
- [ ] The controller limit: RAW_NDEF records of 128 and 129 raw bytes read back through the
      bespoke BLE tool — 128 whole, 129 refused `0x02`. Confirm the boundary against the adapter
      first; `sizeof(s_od_nfc_read_data)` is the constant, but which length is compared to it is
      what the row proves
- [ ] Both of N4's changed input classes confirmed on the wire (now `0x01`, was `0x03` / `0x05`)
- [ ] `heap_size` measured on the flashed image against X3's ceiling

### ESP32-S3 — capability-off

- [ ] `0x0083` probed in plaintext and encrypted sessions draws nothing, the link is not held
      open, and the client raises `NfcNotSupportedError`

### The bespoke BLE READ tool — built, and no longer the blocker

`py-opendisplay` implements no `NFC_SUB_READ` (`commands.py:103`, "not built here"), so every READ
row needed a sender and decoder written first. **That exists**:
`targets/esp32-idf/tools/od-device-cli.py nfc-read`, covered by `tests/host/nfc_read_tool_test.py`
(41 checks, `bleak` stubbed). `--expect-silence` drives the capability-off row, and it requires a
`FIRMWARE_VERSION` canary to answer before it will accept silence — a dead notify path is silent
too, and without the control the flag would pass on a board that was not answering at all.

**Hardware is still the blocker; the tool is not.** And an independent NFC reader remains stimulus
and oracle only: it talks to the tag directly, bypassing `CMD_NFC_ENDPOINT`, dispatch, the seam and
response framing. **A read row backed by a reader alone is not a pass.**

## ESP32-S3 (`s3-n16r8-extuart-debug`, FastEPD)

- [x] Two fresh authentications — 2026-08-17
- [x] Encrypted command round-trip — 2026-08-17
- [x] Encrypted PIPE upload through refresh — 2026-08-17
- [x] `CMD_PARTIAL_WRITE` (0x76) — 2026-08-17
- [x] Config read — 2026-08-17
- [x] Config write, write + reload-after-write confirmed — 2026-08-17
- [x] Plaintext (unencrypted) run of the above (PIPE, `CMD_PARTIAL_WRITE`, config read/write) — 2026-08-17
- [x] Direct/PIPE END: ACK (`00 82`) observed on air *before* the multi-second physical refresh
      begins — 2026-08-17, confirmed directly in device log
- [ ] Disconnect/reconnect, then re-authenticate — confirm a *new* session succeeds
- [ ] Config read under TX backpressure (CCCD-disabled induced backpressure, per
      `PLAN_OD_DISPATCH_C12_2026-08-16.md` §7.2's row)
- [ ] LED / buzzer / READ_MSD / FIRMWARE_VERSION
- [ ] No-session and decrypt-failure plaintext gate (`{00,cmd,FE}` / `{00,cmd,FF}` visible
      unencrypted)
- [ ] Unknown opcode silence; 245-byte value ceiling NACK
- [ ] NFC 218/219-byte ceiling
- [ ] Plaintext LAN and TLS-LAN commands, PIPE-over-LAN refusal, 4092-byte LAN DIRECT_WRITE
- [ ] Long idle + accepted-traffic cycle (owner-clock stamp)
- [ ] `OD-S1` replay injection via `dispatch-gate` (see § below)
- [ ] Uncompressed image push
- [ ] Interrupted-transfer recovery

## Nordic `xiao_nrf52840`

- [x] Encrypted PIPE upload, image displayed correctly — 2026-08-17; re-run at current HEAD
      (post Transfer Phase 2 step 11) — 2026-08-19
- [x] Advertising resumes reliably after disconnect, repeated connect/disconnect cycles —
      2026-08-17 (the `od_adv_control` wiring fix, PR #40)
- [x] Direct/PIPE END: ACK observed on air before refresh begins — 2026-08-17, confirmed in
      device log
- [x] Config write + reload verified — 2026-08-15 (Gate 2 pass), re-run 2026-08-19, re-run
      (encrypted and plaintext, full write+reload+reboot-persist) 2026-08-30
- [x] Disconnect/reconnect, then re-authenticate — confirm a *new* session succeeds — 2026-08-19
      (BLE dropped mid-PIPE, reconnected, new session carried a fresh upload through refresh)
- [x] Config read, re-run against current HEAD — 2026-08-19, re-run (encrypted and plaintext)
      2026-08-30
- [x] Config write, re-run against current HEAD — 2026-08-19, re-run (encrypted and plaintext,
      reload-after-write and reboot-persist both confirmed) 2026-08-30
- [x] Config reload-after-write and reboot-persist, re-run against current HEAD — 2026-08-19,
      re-run (encrypted and plaintext) 2026-08-30
- [ ] Config read under TX backpressure
- [x] `CMD_PARTIAL_WRITE` (legacy partial, `0x76`) — 2026-08-30, compressed payload, both
      plaintext and encrypted, each completed through a successful panel refresh. Happy path
      only: raw (uncompressed) partial and an etag-mismatch refusal were not exercised. Reported
      in conversation; no separate device-side transcript captured.
- [ ] LED / buzzer / READ_MSD / FIRMWARE_VERSION
- [ ] No-session and decrypt-failure plaintext gate
- [ ] Unknown opcode silence; 245-byte value ceiling NACK
- [ ] NFC 218/219-byte ceiling
- [x] Plaintext (unencrypted) PIPE upload and config round-trip — 2026-08-30, compressed PIPE
      upload through refresh plus a plaintext config read/write round-trip, all completed. Happy
      path only; raw PIPE not exercised.
- [x] PIPE-partial (0x0080 flags bit1): START accepted, region streamed, partial refresh, new
      etag committed. Refused on every attempt until 2026-08-19 — the START handler passed the
      PIPE flags word to a validator that only defines the 0x76 partial flags, so bit1 always
      read as an unknown flag (`FOLLOWUPS.md` § 6). Fixed and host-tested; **run on a board for
      the first time 2026-08-30** — compressed payload, both plaintext and encrypted, each
      accepted the START, streamed the region and completed a partial refresh with a matching
      etag. Happy path only: raw partial and an etag-mismatch refusal were not exercised.
- [x] Direct write (`0x70`/`0x71`/`0x72`) — 2026-08-30, compressed payload, both plaintext and
      encrypted, each completed through a successful panel refresh. Happy path only; raw direct
      write not exercised. See Transfer Phase 2 — Nordic steps 10a/10b for the row this feeds.
- [x] Successful PSA key-replacement / re-authentication cycle — 2026-08-19, one cycle via the
      mid-PIPE disconnect above (the prepared-key slot released and re-prepared; a repeated
      many-cycle soak has not been run)
- [ ] `OD-S1` replay injection via `dispatch-gate`
- [x] Interrupted-transfer recovery — 2026-08-19, PIPE abandoned mid-transfer by BLE disconnect;
      a fresh upload then completed through refresh

**Unretired pre-promotion hardware observation:** the 2026-08-17 report records small/sub-window
PIPE uploads stalling indefinitely and states that ESP32 behaved identically. Later source and
sender-probe analysis conflicts with that diagnosis, but no hardware transcript retired it.
Hardware was unavailable on 2026-08-20, and shared D11 deliberately preserved the target cadence
policy without adding a timer. Treat the stall as live until the post-promotion tail-below-cadence
rows above close with on-air evidence.

## `xiao_nrf54l15` / `xiao_nrf54lm20a`

- [x] Transfer Phase 1 nRF54-class compressed direct, partial and PIPE gate — cleared 2026-08-18.
- [ ] Remaining board-specific boot, storage, NFC and full migration matrix.

## `efr32bg22-slc`

- [x] Transfer Phase 1 compressed-direct pump gate — cleared 2026-08-18.
- [ ] Remaining C13, boot/storage, NFC and capability-off migration matrix. No PIPE or
      `CMD_PARTIAL_WRITE` is implemented on this target.

## WiFi/LAN transport

- [ ] Never hardware-verified on any target.

---

## `OD-S1` replay injection (mandatory per-target, once available)

Per `PLAN_OD_DISPATCH_C12_2026-08-16.md` §7.5 — this is the one row that cannot be satisfied
by a normal successful upload, on either target:

1. Authenticate, open an encrypted PIPE transfer with a small `ack_every`.
2. `dispatch-gate` seals one valid `0x0081` DATA frame, sends it, retains the exact raw bytes.
3. The identical sealed bytes are written again while the transfer is live (same session id/
   counter) — must reach the application as a replay.
4. Require: no notification attributable to the replay, one throttled replay/nonce telemetry
   record, no integrity-strike/session teardown, successful continuation through END with a
   rendered refresh.
5. In a fresh transfer, send a newly-counted `0x0081` with a corrupted tag — require the normal
   plaintext hard NACK (proves the capture window could observe a response at all).

The raw duplicate must be byte-identical; re-encrypting the same sequence number under a new
CCM counter tests PIPE duplication, not this. Tooling: `targets/esp32-idf/tools/od-device-cli.py
dispatch-gate`.

---

## Release matrix — per silicon/boot/storage/transport class

From `plans/PLAN_MIGRATION_ENDGAME_2026-08-17.md` §2.1. One unified target directory can cover
several rows here; a single ESP32-S3 does not stand in for classic ESP32, C3, or C6, and one
nRF52840 does not stand in for either nRF54 part.

| Row | Boot/storage distinction | Status |
|---|---|---|
| ESP32-S3, one PSRAM board | IDF 2nd-stage, OTA-capable N8/N16/N32 partitions | Partial — see above, WiFi/LAN and uncompressed push outstanding |
| ESP32-C6 N4 | IDF 2nd-stage, no PSRAM, single-app partition | Not started |
| ESP32-C3 N4/N16 | IDF 2nd-stage, no PSRAM, C3 radio/flash config | Not started |
| Classic ESP32 N4 | IDF 2nd-stage, no PSRAM, classic peripheral path | Not started |
| `xiao_nrf52840` | Adafruit UF2, EPD rail path | Partial — see above |
| `xiao_nrf54l15` | MCUboot | Phase 1 nRF54-class pump gate cleared; full board row open |
| `xiao_nrf54lm20a` | MCUboot, distinct DTS/pinctrl, second core | Phase 1 nRF54-class pump gate cleared; full board row open |
| `efr32bg22-slc` | Gecko Bootloader + AppLoader, NVM3, 32 KB RAM | Phase 1 compressed-direct pump gate cleared; full board row open |

Each row, when run, should record: board id, exact release SHA, bootloader, partition/storage
layout, tool and host versions, raw transcript, device log, per-observation PASS/FAIL — per
`PLAN_OD_DISPATCH_C12_2026-08-16.md` §7.1's evidence rules (still binding; only the SHA-pinning
*gate* is dropped, not the record-keeping standard).

---

## Config storage seam — `od_config_store` + `od_hal_nvs` (2026-08-24)

Landed as commit `490415d`; **nothing here is hardware-qualified**. `tools/check.sh --targets`
passes 40/0/0 and the host suite covers the framing against a fake medium, but a host test cannot
qualify silicon and **a config a device cannot read back is a bricked configuration** — these rows
gate the promotion, not the build.

Two behaviour changes make a device REFUSE a record it used to accept
(`DIVERGENCE_MATRIX` § 17), and both are Nordic-only, so a Nordic board that will not boot on its
existing config is the expected first symptom rather than a surprise: a physically truncated
record now fails, and a record larger than the caller's buffer is refused instead of ignored.
Re-provision rather than debug if that appears.

### Every target

- [ ] **Write, reboot, reload:** write a config over BLE, confirm it takes effect, power-cycle, and
      confirm the device comes back on the stored config and the panel renders from it.
- [ ] **Unrecognised version still loads:** rewrite the stored record's `version` field out of band
      to 2 and confirm the device still boots on it. This is the behaviour § 4 says must NOT
      change — every target writes 1 and none reads it back, and enforcing it would strand a
      device on a record it has been using.
- [ ] **A corrupted record boots on defaults:** flip one payload byte out of band and confirm the
      CRC rejects it and the device comes up unconfigured rather than on garbage.

### ESP32-S3 (`s3-n16r8-extuart-debug`)

- [ ] **Factory-provisioned config loads at first boot** — the path that passes a flash pointer
      into `saveConfig`. The core copies it into the workspace; confirm a
      `OPENDISPLAY_FACTORY_CONFIG_HEX` build comes up configured on a blank NVS.
- [ ] **Secure erase still wipes:** `secureEraseConfig()` now drops the HAL's cached record
      unconditionally before touching NVS. Confirm a subsequent read reports an unprovisioned
      device *without* a reboot — the cache is the only thing that could have hidden that.

**Partial evidence, 2026-08-28.** Same session as the Transfer Phase 2/3 encrypted-compressed
notes above (`s3-n16r8-extuart-debug`, `0.1.1-330-g01366ce-dirty`, Seeed reTerminal E1001, current
HEAD, post config-storage-seam commit `490415d`): a config was written over BLE, the device was
power-cycled, and it came back on the stored config with the panel rendering from it — satisfying
the "Every target" **Write, reboot, reload** row above for ESP32-S3 specifically. That row is not
checked off above because Nordic and BG22 still need their own run of it; ESP32 evidence does not
qualify them. Reported in conversation. Neither of the two rows immediately above
(factory-provisioned config at first boot, secure erase) was exercised by this.

### Nordic (`xiao_nrf52840` mandatory)

- [ ] **Write, reload in place, reboot-persist** — the three the 2026-08-19 run covered, re-run
      against the shared framing.
- [ ] **A failed clear reports failure.** `clearStoredConfig()` used to discard
      `settings_delete()`'s result and return true regardless. Inducing a settings delete failure
      on the bench may not be possible; **if it is not, say so here and move this row to a
      production-source fault test** rather than leaving it open forever.
- [ ] **A failed write leaves the previous config readable** (S1a). Same caveat: if a
      `settings_save_one` failure cannot be induced on hardware, this belongs in a fault test and
      this row should say that instead of sitting open.

### EFR32BG22 (`efr32bg22-slc`) — needs a J-Link attached

- [ ] **Write, reload, reboot-persist** through the union overlay.
- [ ] **An over-size declaration is refused at the start frame with nothing stored** — refuse,
      never truncate, because the 2048-byte cap is one a host cannot interrogate
      (`MEMORY_CONSTRAINTS.md` item 3).
- [ ] **An in-flight chunked transfer survives a refused save.** The header write eats the four
      live assembler state words, so every refusal that can still leave a transfer open has to
      happen before it. Covered by `tests/host/silabs_storage_test.c` and falsifiable there, but
      unexercised on silicon.
- [ ] **RAM unchanged:** `heap_size` and `data + bss` against the pre-change image. Measured at
      32,284 B static RAM with 480 B headroom (flash 250,148 -> 250,552 B). The overlay existing is
      what makes this promotion affordable there, and losing it would be invisible except here.


---

## Sensor/I2C seam — steps 1-4 (2026-08-24)

Nothing here is hardware-qualified. `tools/check.sh --targets` passes 40/0/0 and the shared
resolver and I2C contract are host-tested and mutation-checked, but **the gate cannot see pin
order or bus selection**: step 4 shipped, and review caught, a swapped SDA/SCL argument order that
would have crossed the lines on every configured ESP32 bus while passing every automated check.
Treat the rows below as the only thing standing behind that.

### ESP32-S3 (`s3-n16r8-extuart-debug`)

- [ ] **SDA and SCL are the right way round.** Scope or logic-analyse one transaction, or simply
      confirm any I2C device answers at all — SHT40, BQ27220, GT911 or the AXP2101 probe. A
      crossed pair fails every device on the bus, so one working device clears this row.
- [ ] **SHT40, BQ27220, GT911 and AXP2101 all behave as before** the operations gained a bus
      argument.
- [ ] **Bus switching:** two configured `DataBus` entries selected in sequence, and returning to
      the first restores its pins **and its speed** — the speed half is newly part of the
      selection identity and was previously ignored on a same-pins switch.
- [ ] **A sensor or touch entry with `bus_id == 0xFF` is not probed**, and is not attached to
      bus 0.
- [ ] **Out-of-order `DataBus` records bind correctly**: declare instance 1 before instance 0 and
      confirm each device talks on its own pins. This is the § 14 defect the shared resolver
      fixes, and it is invisible on an in-order config.
- [ ] **A duplicated `instance_number` yields no device** rather than an arbitrary one.

### Nordic (`xiao_nrf52840`) — runnable rows

- [ ] **BQ27220 and nPM1300 reads still return plausible values.** Their repeated START gained a
      half-period `tLOW` that it never had; the change is meant to be an improvement, and this
      row is what shows it is not a regression.
- [ ] **SHT40 reads match the pre-change image**, after its driver moved to `shared/core` and
      gained the authority's second retry pass.
- [ ] **A sensor with `bus_id == 0xFF` is not probed**, and is not attached to bus 0.

### Charge-state polarity — a WIRE-VISIBLE correction (2026-08-24)

`DIVERGENCE_MATRIX` § 21 / `FOLLOWUPS` § 19. Bit 7 of the BQ27220 MSD byte now reports the
opposite of what every previous image reported on a board that wires STAT. Needs a board with a
charge-state pin, and a charger that can be plugged and unplugged — which is the whole test.

- [ ] **Charging reads as charging.** Plug the charger; a host sees bit 7 set. Unplug it; bit 7
      clears. This is the row the fix exists for, and no host test substitutes for it: the
      polarity is now provably self-consistent against the header, but only a board says whether
      the header describes the hardware.
- [ ] **Both flag settings, on one board.** Write a config with `OD_CHARGER_FLAG_STATE_ACTIVE_LOW`
      set, then clear, reading bit 7 under charge each time. Exactly one setting should be right,
      and it should be the one matching how STAT is wired — if *both* look right, or neither, the
      state pin is not being read at all.
- [ ] **The enable pin still charges.** The seam changed shape for the enable pin too. Confirm a
      board with software charge control still actually charges after `od_sensor_bq27220_init()`.
- [ ] **A board with no state pin still advertises a sane SOC** with bit 7 clear, rather than a
      garbage byte, now that a failed read reports UNKNOWN instead of a level.

### EFR32BG22 NFC transport — step 9, a software candidate (2026-08-24)

**No board in this fleet carries a TNB132M**, so every row here is open on arrival, exactly as
Transfer Phase 4's NFC rows were. Merged code is not evidence.

What *is* checked, and what it is worth: `tests/host/silabs_i2c_trace_test.c` binds the production
`hal/od_hal_i2c.c` to fake GPIO and reads the **edge sequence** back — START and STOP detected as
SDA transitions while SCL is high, the repeated START in a block read, one clock per bit plus the
ACK slot, the address-NACK path, and that an unresolvable or ambiguous bus drives no edge at all.
That is real coverage of framing. It is not coverage of *timing*, of the TNB132M's response to it,
or of anything above the transaction.

The existing BG22 NFC host tests fake `od_nfc_app_read`/`write`, which sit **above** the transport
and say nothing about it. They must not be cited for these rows.

- [ ] **A tag reads.** `0x48` sub 0 returns plausible Attribute Information after the prime
      sequence.
- [ ] **A tag writes and reads back**, including the re-prime that the byte-offset window needs
      after an EEPROM write.
- [ ] **The prime commands still work with their results discarded.** They always were discarded;
      the transactions now return a status that nothing reads, and that is deliberate — starting
      to check it would be a behaviour change on a transport nothing here can exercise.
- [ ] **Power sequencing and SCL/SDA parking still bracket a session.** Those stayed in the NFC
      adapter; only the transactions moved.

**Explicitly out of scope for any of these rows: stuck-bus behaviour.** This engine drives SCL
push-pull and only ever reads SDA, so it cannot detect clock stretching, and a held-low SDA reads
as ACK and as data 0. Do not write a row that asserts otherwise.

**The edge-pacing row is removed, not just unchecked** — 2026-08-29, by direction: its only
possible evidence was a scope trace of `sl_udelay_wait()` timing against the pre-change image,
which is out of scope for this checklist going forward.

---

## Boot-screen key policy — `od_boot_key_state` + the ESP32 declaration fix (2026-08-28)

Two separable defects, one panel symptom. `DIVERGENCE_MATRIX` § 26 is the policy — the donor's
boot screen never read `encryption_enabled`, so a stored-but-disabled key rendered as `hidden` on
all three renderers. § 27 is ESP32-only and is what the flashed board was actually showing:
`boot_screen.cpp` re-declared the `securityConfig` **reference** as an object, so the renderer
read a 4-byte `.rodata` pointer word as a 64-byte `struct SecurityConfig`. Neither the compiler,
the linker, nor any host test can see § 27 — the host suite never links `main.h` — so hardware is
the only place it was ever observable.

### ESP32-S3 (`s3-n16r8-extuart-debug`)

- [x] **All four key states render correctly on the panel.** 2026-08-28, Seeed reTerminal E1001,
      commit `01366ce` + working tree. Encryption off with a key stored, off with no key, on with
      a key and the show flag clear, and on with a key and the flag set: `KEY1:`/`KEY2:` read
      `not set`, `not set`, `hidden`, and the key hex respectively. This is the `NOT_SET` /
      `HIDDEN` / `SHOWN` tree end to end on the large-panel zone layout, and it is what proves
      § 27 — before the fix every one of the four printed `hidden`, because the bytes being read
      were an address, not the config. Reported in conversation.

Open on this board, and none of it is implied by the row above:

- [ ] **The QR payload agrees with the key lines.** Not decoded during the run. This is the only
      host-visible half of § 26: `od_boot_payload_build()` embeds the key at bytes 5..20 when
      `show_key`, so a device that will not ask for a key must not publish one to
      `opendisplay.org/l/`. Scan the code in the two states that differ — key in force and shown,
      versus key stored with encryption off — and confirm the second carries no key bytes.
- [ ] **The small-screen hex path.** The run exercised the zone layout's `KEY1:`/`KEY2:` lines
      only. The non-zone branch formats the same policy through `od_boot_format_key_display()` as
      a 16+16 hex split, with its own `-`-fill and `X`-fill arms, and needs a panel small enough
      to take that branch.
- [ ] **The FastEPD renderer.** `panel_ic_type` 0x003B on this board resolves to bb_epaper
      `EP75_800x480_GEN2`; `fastepd_driver_used()` is false. Needs different glass.

### Nordic (`xiao_nrf52840`) — shared renderer, unexercised

Nordic and ESP32 share `od_boot_screen_render()`, and Nordic reaches the config through
`od_get_parsed_security()` — a function — so § 27 never applied there. § 26's policy change did,
and has no evidence on this target.

- [ ] All four key states render correctly, and the QR agrees with them.

### EFR32BG22 (`efr32bg22-slc`) — second renderer, unexercised

BG22 has its own layout for its small panel and reaches the shared decision through
`od_boot_key_state()` directly. Its `X`-fill and `-`-fill arms are the small-screen path that has
no ESP32 evidence either.

- [ ] All four key states render correctly, and the QR agrees with them.

**A caveat about the build identity on the checked row.** The device banner for this session reads
`0.1.1-330-g01366ce-dirty`, and the `-dirty` marker does not distinguish one working tree from
another — the Transfer Phase 2/3 and config-storage runs earlier the same day report the same
string against a tree that predates the § 26 and § 27 fixes. The row above is distinguished by its
result, not by its banner: the four-state outcome is not reachable on the earlier tree.

---

## Shared GT911 touch driver

`shared/core/od_touch_gt911.c` is the only GT911 implementation on any target. Every row below is
open.

**ESP32 is the only target in this fleet that can close any of them.** No Nordic board here has a
touch controller fitted, so the Nordic rows are release debt awaiting hardware that does not exist
— the same standing constraint as BG22's TNB132M. Host coverage qualifies nothing here: the suite
drives a fake register file, and a fake supplies hardware, not answers.

### The rig, and the two channels

**Evidence is exactly two things: what the debug serial log prints, and what the device advertises
in the MSD.** No JTAG, no debug probe, no RTT, no logic analyser, no current meter. A stimulus can
be anything a hand can do — touch the panel, unplug the part, pull the power — but if the result
cannot be read in the log or in the MSD, it is not a row. Every row below names which channel
answers it.

- A board with a GT911 fitted, `INT` and `RST` wired to GPIOs the config names, on a `DataBus`
  record the config declares.
- A build whose log reaches a serial console over USB, at DEBUG level: `s3-n16r8-extuart-debug`
  or equivalent. One row below reads a line that INFO compiles away.
- A host running `py-opendisplay`, decoding the MSD and timestamping arrivals. Arrival times are
  how the cadence rows are read; there is no other clock available.
- A way to break the bus by hand — unplug the controller, or short SDA to ground.

**Deliberately not rows**, because neither channel can answer them: the reset waveform against the
part's timing diagram, `INT` staying quiet on an idle part, and idle current with a controller
configured. They are real questions and they need instruments this constraint excludes; a row
nobody can run reads as coverage.

### ESP32-S3 — bring-up and addressing (log)

- [ ] **A configured `i2c_addr_7bit` of `0x5D` binds.** Log: `Touch[0]: GT911 @0x5D BE <w>x<h>
      INT+poll`.
- [ ] **A configured `0x14` binds.** Log: the same line at `@0x14`. Only the reset dance can put
      the part there, so this is what says the address selection works rather than the part
      answering wherever it likes.
- [ ] **Auto-detect binds** with `i2c_addr_7bit` at `0` or `0xFF`. Log: the bind line names
      whichever address it resolved.
- [ ] **A configured address the part does not answer on fails cleanly.** Log: one
      `probe failed at configured addr 0x..` and then `init failed` — and no bind line at another
      address, which would mean it fell back to auto-detection.
- [ ] **The byte order is the documented one.** Log: `BE` in the bind line. `LE` means the part
      answered only the undocumented order — record it, because nothing here has seen such a part.
- [ ] **The reported resolution is the panel's.** Log: `<w>x<h>` comes from the part's own
      registers, so a sane pair says it is answering with data rather than merely ACKing.
- [ ] **`rst_pin` absent (`0xFF`) still binds** by probe alone, on a board strapped to a known
      address. Log: the bind line.
- [ ] **`enable_pin` works.** Log: a board with one binds at all. A controller behind an unasserted
      enable does not answer its address, so the bind is the whole check.

### ESP32-S3 — bus admission (log)

- [ ] **`bus_id == 0xFF` is not probed.** Log: `no usable data_bus (bus_id 255); not probed`, and
      no bind line — in particular not one on bus 0.
- [ ] **A `bus_id` naming no `DataBus` record is refused.** Log: the same line naming that id,
      rather than a bind.
- [ ] **Touch on a second declared bus binds**, and a sensor on the first keeps reporting. Log: the
      touch bind line plus the sensor's own lines, both continuing.

### ESP32-S3 — what the wire carries (MSD)

- [ ] **A contact reports the right pixel.** Touch a known point — a corner is easiest — and read
      the block: mapped, clipped panel coordinates. Raw controller coordinates are well-formed
      bytes that nothing reports as wrong, so only a known point catches it.
- [ ] **Each `TouchFlags` bit does what it says.** `SWAP_XY`, `INVERT_X`, `INVERT_Y`, and one
      combination of two, each checked by touching the same physical corner and reading where the
      block says it was.
- [ ] **Coordinates clip to the panel.** A contact at the extreme edge reports at most
      `pixel_width - 1` / `pixel_height - 1`, never beyond.
- [ ] **Release keeps the last contact.** Low nibble `6`, coordinates unchanged from the last
      touching sample.
- [ ] **Untouched since boot is all zeros** across the whole 5-byte block.
- [ ] **The contact count tracks fingers**, 1..5 in the low nibble, carrying the first contact's
      coordinates.
- [ ] **The track id lands in the high nibble** and changes when a new contact begins.
- [ ] **`touch_data_start_byte` places the block.** Check at `0` and at `6`, with a
      `binary_inputs` or sensor block configured beside it, and confirm neither overwrites the
      other.
- [ ] **`py-opendisplay` decodes it.** `AdvertisementData.touch_event()` and `TouchTracker` emit
      `touch_down`, `touch_move` and `touch_up` across a press, a drag and a release.

### ESP32-S3 — cadence and interrupts (MSD arrival times)

- [ ] **Interrupt-driven service happens.** Set `poll_interval_ms` to `255` and touch the panel:
      the sample still arrives promptly. Polling alone could not beat 255 ms, so arrival time
      separates the two. The bind line ending `INT+poll` says the trigger attached; this row says
      it fires.
- [ ] **A missed edge still reports.** Tap repeatedly and confirm no contact is swallowed — the
      held-low check recovers a sample whose edge was lost, without waiting for the next timed
      poll.
- [ ] **`poll_interval_ms` is honoured.** `0` behaves as 100 ms; a configured value changes the
      arrival cadence of a moving contact to match.
- [ ] **Two controllers keep their own cadence**, configured at different intervals, with neither
      slowed to the other's.
- [ ] **A static contact does not republish.** Hold a finger still: the advertisement stops
      changing. Only a changed sample republishes.

### ESP32-S3 — failure handling (log, then MSD)

- [ ] **Five consecutive read failures disable touch.** Unplug the part or hold SDA low. Log: one
      `I2C read failed` warning, then `disabled (too many I2C read failures)`. MSD: the block stops
      changing.
- [ ] **The device survives it.** The host stays connected and advertisements keep arriving at
      their normal rate; the log shows no reset and no watchdog line.
- [ ] **A failing part does not busy-poll.** Log: the failure warning does not repeat per loop
      pass — the backoff holds even though a dead part also holds `INT` low.
- [ ] **Touch survives a long session with no dead window.** MSD: contacts still report after
      hours. This is the only practical check that an over-count status does not wedge it.
- [ ] **A disabled controller stays disabled** until a lifecycle event, rather than reappearing on
      its own.

### ESP32-S3 — lifecycle

- [ ] **Touch survives a panel refresh.** Push an image, wait for the refresh, then touch. MSD:
      contacts report again. Repeat with `poll_interval_ms` at `255` — a prompt sample says the
      interrupt was re-attached, and a polled recovery looks identical to a working one without
      that.
- [ ] **Touch works after a mid-transfer BLE disconnect**, which force-resumes from a teardown with
      nothing suspended. MSD: contacts report after reconnecting.
- [ ] **A transfer is not slowed by touch.** An upload with a controller configured completes in
      the same time as one without, measured by the host.
- [ ] **A config write moves the block.** Change `touch_data_start_byte` on a running device; MSD:
      the next sample lands at the new offset. An offset past `6` writes nothing rather than
      truncating.
- [ ] **A config write does NOT reconcile the runtime.** Change `bus_id` or `int_pin` on a running
      device and record what the log and MSD actually show: `init()` is boot-only, so the
      controller keeps the binding it has (FOLLOWUPS § 23). This row exists to measure the gap, not
      to pass.
- [ ] **The config survives a reboot.** Log: the same bind line after a power cycle.
- [ ] **Deep-sleep wake re-initialises touch.** Log: the bind line after the wake. MSD: contacts
      report.

### ESP32-S3 — coexistence

- [ ] **A binary input on GPIO 0 keeps working** with touch configured, and again with touch absent
      from the config. MSD: the `binary_inputs` block still counts presses. Both cases run the
      touch init.
- [ ] **A button configured on the touch `INT` pin is left to touch.** Log:
      `Button: skip pin <n> (reserved for GT911 INT)` — a DEBUG line, so it needs the debug build.
      MSD: pressing that pin moves no `binary_inputs` bit, and touch still reports.
- [ ] **Two GT911s on one board**, if one exists: both bind at their own addresses, both blocks
      land at their own offsets, and one failing does not disable the other.

### Nordic — no board in this fleet has a touch controller

Every ESP32 row above applies here too and is untested. These are Nordic's alone:

- [ ] **`od_touch_gt911_reestablish()` recovers the part after a refresh.** This target's refresh
      hook is unpaired, so this entry point runs nowhere else. MSD: contacts report after a
      refresh.
- [ ] **Interrupt-driven touch works at all** — the GPIO IRQ seam's first consumer on this target.
      Read as on ESP32: `poll_interval_ms` at `255`, and the arrival time.
- [ ] **GT911 at the configured bus rate**, with `bus_speed_hz` unset or `100000`.
- [ ] **GT911 at 400 kHz.** If touch fails here, that is the measured evidence for a clamp: record
      it and clamp with a reason.
- [ ] **A clone that stretches the clock fails the transfer** rather than sampling garbage, and
      five consecutive failures disable touch. Log: the disable line.
- [ ] **Interrupt latency while disconnected**, timed from the host's arrivals. The driver's
      returned delay is a floor here: an ISR sets a bit and cannot shorten a `k_msleep` already
      running (FOLLOWUPS § 16). GPIOTE idle current is the other half of that question
      (FOLLOWUPS § 22) and needs a meter, so it is not a row.

### EFR32BG22 — nothing to run

The profile declines touch, the image links no touch symbol and no seam reference, and the
capability-off arm answers idle, no address, not an interrupt pin. That is a link-time property the
software gate checks; no board row follows from it.

---

## Boot-screen footer voltage and temperature — newlib float printf (2026-09-16)

The footer's battery and die-temperature values rendered as a bare `V` and `C` with no digits.
`od_boot_screen.c` formats both with `%.2f`/`%.1f`, and the Nordic build selected newlib-nano
without `CONFIG_NEWLIB_LIBC_FLOAT_PRINTF`, so `-u_printf_float` was never passed and
`_printf_float` stayed a NULL weak symbol: `nano-vfprintf` skips the conversion, steps the
`va_arg` cursor over the double, and prints the rest of the format string. The digits vanish and
the unit letter remains.

Fixed by restoring the linker flag in `targets/nordic-zephyr/zephyr/prj.conf`, not by rewriting
the renderer: `../Firmware/src/boot_screen.cpp:733-742` is the same source and prints correctly
only because the Adafruit nRF52 Arduino platform passes `-u _printf_float` unconditionally
(`platform.txt` `build.float_flags`). The port carried the source but not the flag.

**No host test can catch this class of defect** — the host suite links glibc, which formats
floats, and `tests/host/boot_screen_test.c` stubs the two readings without asserting on the text.
ESP32 is unaffected (ESP-IDF's newlib carries the formatter), which is why the same renderer
passed its ESP32-S3 run. The link-level check is `nm zephyr.elf | grep -E "_printf_float|_dtoa_r"`:
absent before, present after.

### Nordic nRF52840 (`xiao_ble/nrf52840`), 7.3" colour kit

- [x] **The footer shows real battery and die-temperature values.** 2026-09-16, working tree on
      commit `184c302` plus the `prj.conf` line. Reported in conversation. Acquisition was never
      at fault: `update_msd_payload()` (`opendisplay_ble.c:1032`) samples both before
      `schedule_boot_display_apply()` (`:1044`), so only the formatting was lost.

Open on this board:

- [ ] **The unavailable arms still read `--V` / `--C`.** Needs a board with
      `battery_sense_pin == 0xFF`, or one whose SAADC read fails, to distinguish "no reading"
      from the formatting defect above. Not exercised by the run.

### Nordic nRF54L15 / nRF54LM20A — same fix, no board run

Both take the flag from the shared `prj.conf` and link the float symbols (verified at link time,
2/2 on each). Neither has been on a panel with this build.

- [ ] The footer shows real values on an nRF54 board.

### EFR32BG22 — nothing to run

The target declines the `APP_BOOT` tier and its own `render_boot_screen()` prints no voltage or
temperature, so the defect never reached it and the fix does not apply.

---

## nRF52840 USB power gating

Merged in PR #90 (`4a61a7a`). Baseline before the change: ~2 mA idle while advertising on battery;
`../Firmware` on the same board: ~55 µA.

- [x] Battery idle current, battery build, no cable: **40 µA** measured (2026-09-17, bench
      measurement reported with the PR #90 flash test). The idle-power regression is resolved.
- [ ] Idle current after a plug/unplug cycle returns to the no-cable figure; traces show no 1 kHz
      logging ripple. Repeat the no-cable measurement with the debug build.
- [x] Cable attached before cold boot enumerates as CDC and exposes the boot log. **Release gate.**
      (2026-09-17, bench test of PR #90: USB console works.)
- [x] UF2 bootloader-to-application hand-off with the cable held enumerates as CDC. **Release gate.**
      (2026-09-17, same test.)
- [x] Battery hot-plug enumerates; 20 unplug/replug cycles and a fast cable bounce settle without
      USB errors or watchdog resets and return to the no-cable current after unplug.
      (2026-09-17, same test.)
- [ ] Plug/unplug during BLE upload does not interrupt the upload; a serial-monitor log burst may
      drop characters but does not block firmware work; UF2 drag-and-drop remains functional.

---

## nRF52840 EN04 (OD114608) — 2.9" BWRY, advertiser settling (2026-11-08)

One board, USB powered: EN04, GDEY029F51H 168x384 BWRY (panel 0x001E through bb_epaper),
plaintext, dual-slot MCUboot layout, serial-DFU flash. Build `9.9.10`, working tree on `2ac1a46`
plus the `opendisplay_ble_advertising_settling()` change. 4-colour only: this target has no
config-driven drive path.

- [x] **A configured device stays visible.** With `sleep_timeout_ms` = 2000: 36 scanner sightings
      in 90 s, against about one every few minutes on the previous build at the same range.
- [x] Website-built config (EN04 + 2.9" BWRY + USB power, security packet removed), 1978 B: write
      after CONFIG_CLEAR, reboot, read back byte-identical, no parser warning. The 0xAE-0xB1
      packets are walked past.
- [x] Compressed PIPE upload, 16128 B frame, 36.5 s including refresh. Labelled black, white,
      yellow and red bands confirmed by eye to match their labels; no camera or colorimetry.
- [x] **A config too large to coexist with its predecessor replaces it.** Build `9.9.11` over BLE
      SMP, config retained across the update. 1978 B -> 2882 B and back: both took the `-ENOSPC`
      path (`Config replace: retiring the old record first`), both ACKed, both read back
      identical, and the 1978 B record survived a reboot.

Open:

- [ ] `replace_without_overlap()` restoring the old record when the new one still does not fit.
      Not exercised; a record that passes NVS's size check yet fails after the delete was not
      constructed.
- [ ] The original failure itself (1978 B over 1980 B NACKing on `9.9.10`) did not recur in four
      later attempts on `9.9.11`, and its errno was never captured.
- [ ] Boot refresh takes ~34 s on this panel, past the 30 s limit, so each boot logs
      `boot display: still running` and BLE starts 30 s in.
- [ ] Battery idle current with the settling change. nRF54L15 / nRF54LM20A take the same `main.c`
      and have not been run.

---

## `efr32bg22-slc` on OD58FFC4 — config-driven panel, flash store, actions (2026-11-06)

One board, one session: **OD58FFC4**, a Newton Pro 2.6" (2x CR2450) with a 184x360 BWRY
panel and an SPI NOR fitted, in the thermal chamber with the camera on it. Before this
session the BG22 target had never been built in this repo, so every row below is a first.

**The board's identity fields are stale and were not corrected:** it reports
`manufacturer_id=1 / board_type=1 / ic_type=1`, which the toolbox renders as
"EN04 (nRF52840)" on a part that is a BG22. Nothing reads those fields here, but any
row below that looks like it came from an nRF52 came from this BG22. Re-provisioning them
is open follow-up.

### Panel driven from config (`od_epd`, bb_epaper removed)

- [x] Boot screen renders from the config's `DisplayConfig` + drive packets, with no
      bb_epaper in the image. **bb_epaper is compiled out on this target entirely** — not
      disabled at runtime — because the app overflowed its slot by 4116 B with both
      renderers present. A config carrying the `0xAE`/`0xAF`/`0xB0`/`0xB1` drive packets is
      therefore **required** on BG22, not optional.
- [x] BLE image upload renders: 2953 B compressed, 22.5 s wall, camera-confirmed.
- [ ] Partial refresh and `CMD_PARTIAL_WRITE` — not implemented on this target.

### Find My (network A)

- [x] Advertises on the address **derived** from the provisioned key, concurrent with normal
      OpenDisplay advertising. Address and payload were checked byte-for-byte against the
      derivation, not merely observed to be present.
- [x] Survives a reboot. This row exists because it originally did **not**: the boot path
      never synced network A, so a tag was findable after provisioning and silently not
      after a power cycle.
- [x] Host test `tests/host/net_a_test.c` sweeps all 256 values of `key[0]` to prove both
      halves of the address are recoverable (the top two bits are forced for a static random
      address, so a naive derivation loses them).
- [ ] FMDN / network G — still `Firmware_Silabs`-only, not ported.

### SPI NOR image store (`od_flash_nor` + `od_imgstore`)

- [x] Probe identifies the part: **Puya, 512 KiB, 4 KB sectors**, giving **25 image slots** at
      this panel's geometry. The proposal's slot arithmetic was written against a guessed
      capacity and was corrected to these measured numbers.
- [x] `0xAB` release-from-deep-power-down is issued before the JEDEC read. Required, not
      defensive: a previously-slept part answers `0xFF` to everything and reads as an empty
      socket. OpenDisplay only ever sent `0xB9` (power **down**).
- [x] Preload without display: 2.3 s to store an image the panel never showed.
- [x] Display-from-slot renders the preloaded image, camera-confirmed.
- [x] Slot state values form a **descending bit lattice** (`0xFF` free -> `0xFE` writing ->
      `0xFC` valid), and the digest is left erased until `finish()`. The first attempt used
      `0xA5` for valid, which NOR cannot reach from `0xFE` (programming only clears bits:
      `0xFE & 0xA5 = 0xA4`), and pre-zeroed the digest so every CRC ANDed to zero.
- [x] A page program that would cross a 256-byte page boundary is refused. Hardware wraps to
      the start of the same page and overwrites what was just written, reporting success.

### Action system (`od_action`, config packet `0xB2`)

- [x] Table loads and classifies: **entries=5 active=4 inactive=1 rejected=0** for a table
      holding one entry whose effect this board cannot perform (buzzer). Inactive-not-dropped
      is the contract — a newer firmware must start running the entry with no config rewrite.
- [x] `0x004D` ACTION_FIRE drives a `DISPLAY_SLOT` step and the panel changes,
      camera-confirmed. This is the row that proves a trigger -> sequence -> effect path end
      to end without needing the physical event.
- [x] `0xB2` is reached through `od_cfg_index`. This row exists because it originally was
      not: the indexed-packet whitelist omitted `0xB2`, so the table loaded as `entries=0`
      with no error anywhere.
- [x] A config write reloads the table (`od_action_reload()` after `od_cfg_index_build()`, in
      that order — the table is read *through* the index, so the other order validates
      against the previous config's byte offsets).
- [ ] **BUTTON_PRESS and NFC_FIELD triggers are unexercised.** Both need a physical press or
      an NFC scan at the chamber; neither can be driven from here. The entries load active,
      which is not the same as having fired.
- [x] Temperature triggers fire from a real thermal stimulus. Closed-loop in the chamber: a
      32..48 °C window armed with the die inside it, three baseline polls with **no fire**
      (so the trigger is an edge and not a level), then cooled through the lower edge.
- [ ] **FAILED — temperature triggers chatter.** That single crossing fired **four** times,
      alternating EXIT/ENTER. `temp_in_window()` has no deadband, the poll is 2 s, and the die
      reading dithers ±2 °C. Defect and fix in `docs/FOLLOWUPS.md`; this row stays open until a
      deadband lands and the crossing fires exactly once.
- [ ] Known limitation, recorded not fixed: the temperature edge state is **one shared bit**,
      correct only while every temperature entry uses the same window. Untested — the chatter
      above has to be fixed before a two-window test could be read.

**Two notes on how these rows were obtained**, because both cost a session's time. The action
table is reached through `od_cfg_index`, which resolves by FIRST match, and py-opendisplay
round-trips unknown `0x80+` packets — so `interrogate()` → append → `write_config()` silently
stacks tables and keeps running the oldest. Verify `0x004C` reports the table you think you
wrote before trusting any row here. And a BLE poll every ~38 s cannot order events the firmware
generates every 2 s: assert on counts, not on `last`.

### Flash budget — a reporting bug worth knowing before trusting a build

The SLC build script's "Free in application FLASH region" **overstates by 24576 bytes**: the
`.nvm` NVM3 region lives inside the `0x44000` application slot and the script does not
subtract it. Real ceiling is **253952**, not 278528. Acting on the reported figure is what
made a 4116-byte overflow look like 26016 bytes of headroom. Fixing the script is open
follow-up.


---

## `xiao_ble` / EN04 — BLE OTA via dual-slot MCUboot (2026-11-06)

One board, **OD114608** (Seeed EN04, `E0:91:D1:32:80:27`), SoftDevice erased and MCUboot
chained behind the Adafruit bootloader. Opt-in behind `OD_NRF52840_MCUBOOT=1`.

- [x] **SoftDevice erase moves the application start to 0x1000.** `0x3004` went
      `51b1e5db` -> `ffffffff`, and a reset issued with NO DFU request landed in serial DFU
      -- which the bootloader can only do by looking at `0x1000` and finding erased flash.
      Reclaims 152 kB. Reversible: `s140_nrf52_7.3.0_softdevice.hex` over serial DFU.
- [x] **Chain boots**: Adafruit (`0xF4000`) -> MCUboot (`0x1000`) -> application
      (`0xD200`), advertising and USB CDC up.
- [x] **Fits with room**: application 371,152 B of 450,048 B (82.5%). The same image was
      98.6% of a 376,320 B slot before the erase, and did not boot.
- [x] **BLE OTA end to end.** 372,016 B uploaded over SMP into the secondary slot, marked
      for test, rebooted, and MCUboot swapped: slot0 went `298810f2...` -> `0fde8171...`
      and came back `active=True confirmed=True`, with the previous image retained in
      slot1. Client was `smpclient` over BLE; no physical contact at any point.
- [x] A first upload of the IDENTICAL image was correctly refused at the mark step with
      `IMAGE_SETTING_TEST_TO_ACTIVE_DENIED`. Recorded because it looks like a failure and
      is not: MCUboot will not mark the running image for test.
- [x] **Rollback on a bad image.** Built an image with boot_write_img_confirmed() removed,
      uploaded it over BLE and marked it for test. Reboot 1: MCUboot swapped it in and it
      ran as `active=True confirmed=False`, good image preserved in slot1. Reboot 2:
      MCUboot reverted -- slot0 back to the good image, confirmed, and the bad one demoted
      to slot1. Unattended, over BLE, no physical contact.

      WHAT THIS DOES AND DOES NOT PROTECT. The application calls
      boot_write_img_confirmed() in main() just after opendisplay_ble_init(), so anything
      reaching that line confirms itself. Rollback therefore covers an image that hangs or
      faults BEFORE BLE is up; it does NOT cover one that boots cleanly and is broken
      later -- a bad display driver, a config parser that corrupts state. Moving the
      confirm later, or gating it on a health check, would widen the net; doing so is a
      deliberate decision and has not been made.
- [ ] **Images are signed with MCUboot's published debug key.** The build says so
      explicitly. Must not ship.
- [ ] Fleet migration (erasing the SoftDevice on existing devices) is a per-device USB
      visit: the erase also removes the Adafruit bootloader's BLE DFU, so there is no radio
      path left for stragglers.
