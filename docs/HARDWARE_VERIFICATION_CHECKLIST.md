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

### ESP32 + Nordic — shared `od_epd_cfg`: CMD_DRIVE_WRITE_START + CMD_SPI_RAW, 2026-11-10

Both opcodes now route on ESP32 (SPI panels only; FastEPD/IT8951 boards refuse) and Nordic
through `shared/core/od_epd_cfg.c` + `od_cmd_drive.c` + `od_cmd_spi_raw.c`. Builds pass
(ESP32 `s3-n16r8-extuart`, `c3-n4`; Nordic `xiao_nrf52840` single- and dual-slot); host gate green.
Nordic's 0x0085 was on glass before the move to shared code; nothing below has run since.

- [ ] Nordic `xiao_nrf52840` (EN04, 2.9" BWRY): 16-colour 0x0085 through the shared path, on glass
- [ ] Nordic: CMD_SPI_RAW OPEN (plain and PANEL_INIT), CMD, DATA, DATA_NOACK, BUSY, RESET, CLOSE
- [ ] Nordic: BLE disconnect mid raw session closes it (rail down)
- [ ] ESP32 SPI-panel board with drive packets: 0x0085 on glass
- [ ] ESP32: CMD_SPI_RAW sub-opcodes, and refusal on a FastEPD/IT8951 board
- [ ] ESP32: ordinary bb_epaper upload still works after a drive or raw session

### ESP32 (`s3-n16r8`) — EE02 dual-CS 7.09in Spectra 6 on glass, 2026-11-25

EE02 (XIAO ESP32-S3 Plus, BLE `OD1488B0`) + GDEB0709E01 1200x1600, panel_ic 0x0042, scheme 8
split, `ee02_709_spectra6` drive config from `panels/generated/refresh_modes.json` (pins RST 38,
BUSY 4, DC 10, CS 44 / CS_2 41, MOSI 9, SCK 7, PWR 43). Evidence: `v3_worktrees/reports/
refresh_modes_desk/` (`ee02_dualcs_trace_*.txt` console traces with per-command logging from a
debug build, `photos/ee02_dualcs_*.jpg` rigcam crops). Two defects, both needed:

- **CS framing (`od_epd_esp.cpp`).** CS was released between a command byte and its parameters.
  The panel ignored them: PON worked (BUSY low 180 ms), DRF `12 01` never pulled BUSY low, and
  every write ACKed in ~9.5 s with nothing drawn. CS now stays low from the command through its
  data (2.x framing, README D2).
- **DRF order (README D9).** DRF to cs0 then cs1: both halves draw, then BUSY stays low past the
  120 s cap. DRF to cs0 only: 30.2 s, but only the main half changes. DRF to cs1 then cs0: both
  halves draw and BUSY releases (write ACK 39.8 s including the upload).

- [x] Boot screen draws on both halves (framing fix, slave-first DRF config), 2026-11-25
- [x] Stock `full` (mode 0, lab-order stream) via CMD_DRIVE_WRITE (0x0071/0x0072, compressed):
      grid with all six inks, then the same grid rotated 180°; every cell changed on both halves,
      ACK 39.8 / 39.9 s, 2026-11-25 (`photos/ee02_dualcs_3*`, `_4*`)
- [ ] The generated `0x0042` default preset (PON before the frame) on this glass
- [ ] `0x002B` EP81 Spectra 1024x576 (same D9 change) and the 13.3in T133A01 — no such panel here
- Mode names below are the ones tested; renamed 2026-11-25 (`pll04` = `full_quick`, `hold47` =
  `changedonly`, `fastg13` = `faster_cool`, `partialmix` = `partial4ink`).
- [x] Mode 1 `pll04`: write ACK 32.8 s vs `full` 39.8 s; dE00 [cam] vs `full` median 0.60, max
      2.27 (repeat `full` vs `full`: 0.72 / 1.96), 2026-11-25 (`runee02_run3.out`)
- [x] Mode 2 `hold47`: code-4 cells kept their ink (max 1.63), driven cells match `full` (max
      0.46), 2026-11-25
- [ ] Mode 3 `fastg13`: not run. deskee02.py needs a panel temperature for its -40..25 C window,
      and this path reports none (`temp_c` null: no SHT40, and the bus has no MISO read)
- [x] Mode 4 `partialmix` (K/W/Y/B targets, as designed): 3 updates with easy and hard
      transitions, dE00 [cam] vs `full` median 0.35, max 2.06, 0/48 cells over 3; write ACK 19.4 s,
      2026-11-25. The table has no R/G drive: deskee02.py's +2-ink step asked for R and G, and
      exactly those 8 of 36 changed cells kept their old ink (max 17.2); all 28 K/W/Y/B targets
      landed
- [x] Mode 3 `faster_cool` (G13, set-4 rails, PLL 08) draws from stored deflated slot 0: write ACK
      20.2 s; with one inline table 20.3 s, 2026-11-25 (`ee02_spectra709/host/results/mp709`)
- [x] INLINE_LUT on the split panel: before the fix every drive write with inline tables failed
      `pass 0 refresh failed` (both halves sent to the main chip, OD_ERR_SEQ at the slave DTM);
      after it a single pass + inline 20.3 s, 2-pass 39.6 s, both halves drawn (rigcam), 2026-11-25
- [x] Mode 5 `morecolours` (3 passes, slots 0/2/3 stored deflated: 3 tables through the 2-entry
      LUT cache): write ACK 34.5 s; the 32-entry 0xB0 palette grid drawn through py
      `prepare_multipass`: 31 distinct at dE00 5 and 8 (stock `full`, same image: 9 / 8), patch dE00
      vs palette [cam] median 4.2, p90 7.7, max 10.3, 2026-11-25. Camera-provisional; no TSC on
      this path (`temp_c` null)
- [ ] Nordic: `od_epd_nordic.c` still frames CS per cmd/data call; a dual-CS panel there needs
      the same fix

### EFR32BG22 (`efr32bg22-slc`) — 3.0 rollout on six units, 2026-11-13

Release image `v3/head2` `3c5e1f7` (gbl sha256 `5366fdf2…`, RAM gate heap 9,664 B), delivered by
BLE OTA through the AppLoader. Each unit then got a fresh website-preset config (board + panel +
power, BG22 drive class), left open and unclaimed. Evidence: `reports/silabs_validation/`
(`LOG.md`, `REPORT.md`, per-unit logs) outside the repo. "Glass" = Jonas looked at the panel.

| Unit | Board | Panel | Update from |
|---|---|---|---|
| ODF4DA34 | M3 Pro | 0x001E EP29YR 2.9in | 2.2.1 |
| OD1F9C0E | M3 Pro | 0x0038 EP154YR 1.54in | 2.2.1 |
| OD01AAA9 | M3 Pro | 0x003A EP42YR 4.2in (BUSY unusable) | 2.2.1 |
| OD6FACF2 | M3 Pro | 0x001C EP266YR 2.66in | 2.2.1 |
| OD507E22 | M3 Core | 0x0036 EP35YR 3.5in | 0.25 bg22-dev (4-page NVM3 prototype) |
| ODBF064E | M3 Pro | 0x0040 EP75YR 7.5in | 0.25 bg22-dev, encrypted |

- [x] **2.2.1 -> 3.0 keeps the config**: 289 B (x3) and 579 B byte-identical after the OTA, on four
      units; READ_IDENT flags 0x0B (NEEDS_MIGRATION), SEC2 UNCLAIMED.
- [x] **0.25 -> 3.0 keeps the config on OD507E22**: 1772 B with display modes/LUT packets,
      byte-identical.
- [x] **0.25 -> 3.0 brought back an OLDER config on ODBF064E**: `BG22_NVM3_OTA_LAYOUT.md` case (b)
      row 2, now observed. The 2072 B open config written and read back on 0.25 was gone; the
      2138 B encrypted one it replaced came back, and 3.0 migrated its key into SEC2 (CLAIMED).
- [x] Same-image re-flash after every update (six units) - the units stay updatable.
- [x] Fresh config write + byte-identical read-back (2032-2110 B), six units.
- [x] Compressed, raw and 0x0085 colour16 uploads ACKed on all six panels (4.2in: START ACK
      only after panel power-on, beyond the client's 10 s; colour16 221 s).
- [x] Image store mounts after a reboot (not after the config write) and serves FLASH_PRELOAD /
      FLASH_DISPLAY slot 0 on all six (16/12/32/20/20/96 KiB slots).
- [x] **Glass: yellow and red were swapped** with `code_map` [0,1,3,2] on all six BWRY panels;
      **correct on all six** after the identity map (fixed panel data, config rewritten). Only the
      4-colour full mode was looked at; colour16 output was not checked by eye on any unit.
- [x] Health gate on ODF4DA34: 24 links (incl. 4 consecutive quiet 15 s links), no AppLoader
      entry. Counters not read (RTT only).
- [x] **SEC_KEY UNCLAIM on BG22 (ODBF064E)**: accepted, unit UNCLAIMED, 0x27 disabled and zeroed.
      **Its next boot never advertised**: the storage purge looped in `nvm3_repack`
      (SWD PC samples + stack), NVM3 pages ended at ~29,200 erases each (spec 10k). Recovered
      by erasing 0x50000-0x56000 over SWD; the unit then booted factory-fresh into claim mode,
      the 10-minute window timed out unclaimed, and it was configured normally.
- [ ] **Purge fix (`od_nvm3_purge.c`) on hardware**: UNCLAIM or factory reset on a configured BG22
      running the fixed image, reboot watched under J-Link: must advertise within seconds, erase
      counts up by about one per page. Not run.
- [ ] Factory reset (R6 sets the same purge flag) on BG22: not run.
- [ ] colour16 output on glass; the 4.2in colour16 palette is still the 2.9in copy.

### EFR32BG22 (`efr32bg22-slc`) — link starvation after a config write, 2026-11-11

Unit **OD13FA1E** (M3 Silabs Pro, EP29YR 2.9in BWRY, 168x384), encrypted, FMDN + FindMy on,
LTO image `bg22-test-B-fix` (v3/reconnect-fix), 2274 B config, delivered by BLE OTA only.

- [x] **Boot screen renders on BWRY** (was: no blank second plane for a single packed plane;
      the render failed after a 21 s stall at boot and after every config write).
- [x] **No refresh while a link is open; advertising paused during it.** After a config write
      the writer stayed connected and got FIRMWARE_VERSION and encrypted READ_MSD in 0.09 s each
      (session kept: security packet unchanged). After it disconnected, adverts stopped for
      ~24 s while the boot screen drew, then resumed; connects at t=15..100 s all served,
      a connect at t=5 s failed cleanly at the client's 20 s timeout (no half-open link).
- [x] Config write/restore round trip byte-identical (2274 B); 0x0085 16-colour ACK in 28.9 s;
      compressed upload; OTA of the image onto itself, first reconnect clean.
- [x] **Image store mounts with flash MISO = PC0** (`0x20` in the 0x26 packet's MISO byte):
      31 slots x 16 KiB; preload slot 0 -> VALID with digest, display accepted, erase -> empty,
      display of the erased slot refused. The shipped config/preset has MISO 0xFF, so the store
      stays unmounted until the preset is fixed. Panel output not camera-checked.
- [ ] Not seen on glass by a camera; not watermarked for stack.

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
- [x] **CMD_DRIVE_WRITE_START, 16 colours.** Build `9.9.12`, plaintext, uncompressed direct path,
      mode 1 `colour16`, 3 passes x 16128 B, 21 C die. `render16.py --pattern index16` grid
      confirmed by eye as correct; no camera or colorimetry.

- [x] **XIAO QSPI flash deep power-down at boot.** PPK2 at 4.0 V, USB unplugged, advertising with
      `sleep_timeout_ms` = 2000: mean 150.8 -> 141.0 uA. Same build with a 40 s refresh: 136.0 uA.
      A temporary System OFF build still drew 113.7 uA, so that is board and module hardware.

- [x] **Image store on the XIAO module flash (0x0047-0x004B).** Build `9.9.24`, plaintext. Preload of
      two 16128 B frames over compressed PIPE, about 1.2 s each and no refresh; both VALID with
      distinct digests, kept across a reboot; empty slot refused; slot displayed and confirmed
      correct by eye; single-slot erase. Idle afterwards 141.5 uA.

Open:

- [ ] `replace_without_overlap()` restoring the old record when the new one still does not fit.
      Not exercised; a record that passes NVS's size check yet fails after the delete was not
      constructed.
- [ ] The original failure itself (1978 B over 1980 B NACKing on `9.9.10`) did not recur in four
      later attempts on `9.9.11`, and its errno was never captured.
- [ ] Boot refresh takes ~34 s on this panel, past the 30 s limit, so each boot logs
      `boot display: still running` and BLE starts 30 s in.
- [ ] The drive path on nRF54L15 / nRF54LM20A (built, not run), compressed 0x0085, encrypted 0x0085,
      and 0x0085 arriving during an active PIPE upload. The panel run predates removing
      `od_xfer_reset()` from `od_cmd_drive.c`, which affects only a start during an active transfer.
- [ ] What makes up the ~113 uA hardware floor below (PMIC, DC-DC, module LDO, `P0.17` LED path).
- [ ] Image store: `FLASH_ERASE 0xFF` (py-opendisplay misreads the echoed `0xFF` as an integrity
      failure), a preload interrupted mid-transfer, encrypted use, and why the first 60 s after a
      BLE session averaged 178 uA.
- [ ] Battery idle current with the settling change. nRF54L15 / nRF54LM20A take the same `main.c`
      and have not been run.

---

## ESP32-S3 reTerminal Sticky — executor fixes S1 / S3 (2026-11-15)

One board, the reTerminal Sticky (OD6C79FC, EP397 800x480 SSD16xx, config-driven), image
`s3-n32r8-extuart` from `fix/esp32-display-review` at `89ddb5b`, ESP-IDF 5.5.5, plaintext,
unclaimed. A/B against origin/main `a519151` on the same board and config. S1/S3 bus evidence
is from a test-only build that logs each panel command byte (not committed). No camera: "refresh"
below means the command bytes on the bus, not the glass.

- [x] Config with the board's drive packets written over BLE; boot screen, full upload (0x0070)
      and 0x0085 mode 0 refresh on the fix build.
- [x] **S1, extra image marker in mode 0.** Base: frame sent (`24`), no refresh command, only the
      sleep sequence, and the host gets a success ACK. Fix: the same bus traffic, then
      "refresh did not complete" and 0x74 to the host, on both 0x0085 and 0x0070.
- [x] **S3, BUSY never idle after the reset (inverted idle level, 3 s timeout), 0x0085.** Both
      builds answer PANEL_BUSY after 4.0 s. Base sends no command after the reset; the fix sends
      the sleep sequence (`3f 22 20 10`) once, twice repeated.
- [ ] The native-driver fixes (N1-N5): this board has no FastEPD/IT8951, so they are not on
      hardware. Needs an E1003 or a parallel board.
- [ ] S3 on the ESP32 SPI_RAW open path; the inverted-BUSY trick also makes the sleep sequence's
      own BUSY step return early, so the sleep's timing was not checked.

## ESP32-S3 reTerminal E1003 — FastEPD 16-gray (2026-11-08)

One board: ESP32-S3R8, 32 MB flash, ED103TC2 1872x1404, panel 3001. Image `s3-n16r8-extuart` at
`858f2cb`, built with ESP-IDF 5.5.5 (pin is 5.5.4), plaintext.

- [x] Config written over BLE, parsed at boot, panel initialised through FastEPD.
- [x] Compressed PIPE full frame, 1,283 kB in 4.8 s, 7.1 s including refresh. 16 gray bands and
      fine stripes confirmed by eye; no camera or colorimetry.
- [x] **IT8951 on `display_it8951.cpp`, FastEPD no longer used for this panel.** 16-gray full frame
      streamed with no framebuffer: 5.3 s against 7.1 s, 8.36 MB PSRAM free against 6.39 MB. 1-bit
      full frame and 0x76 partial update also confirmed by eye. Boot screen not separately checked.
- [x] **Refresh temperature from the SHT4x, not a forced value.** Strips at forced 14/22/30 C: 22 C best
      by eye. Real reading (24 C) sent before each refresh; grays and 1-bit partial confirmed by eye.
      VCOM is still a fixed -1.5 V rather than the value labelled on this panel.
- [x] **BLE OTA over mcumgr SMP** (`s3-n16r8-extuart`, plaintext). 1.28 MB in 75 s, booted and
      confirmed. A build that never confirms ran once, then rolled back by itself on the next reset.

Open:

- [ ] A variant with FastEPD, 32 MB and the CH343 console; `s3-n32r8-extuart` has no FastEPD.
- [ ] Whether a config stored by older firmware survives the move to this firmware. This unit came
      up with no stored config in NVS.
- [ ] ESP32 OTA with encryption on (unlock through `CMD_ENTER_DFU`), a corrupt image, a host other
      than BlueZ, and any ESP32 variant besides this S3 on hardware.

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
- [ ] FMDN / network G (`shared/core/od_net_g.c`, set `OD_HAL_ADV2_SET_NET_G`) — ported,
  host-tested only (`net_a_test.c` pins the 29-byte SECP160 frame against 2.3.0). Open: a 2.3.0
  unit provisioned for network G, OTA'd to 3.0, still advertises the same frame on the same
  address (scanner), and network A + G + OpenDisplay advertise concurrently (3 sets).
  SECP256R1 is not advertised (2.3.0 could not either: no SHA-256 in its PSA build).

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

## `telink-tlsr` on tag 42D241 (TLSR825x, 2.66" BWR SSD) — bring-up (2026-11-14)

| Row | Result |
|---|---|
| The legacy firmware v108 updates onto itself via `tools/legacy_ota.py` (OEPL block transfer, type 0x03) | **PASS**: back at fw 0x006C, 0x0005 and 0x0011 byte-identical before/after |
| The legacy firmware installs the probe (`opendisplay_tlsr_probe.bin` @ 81cfdab, 44,100 B) | **PASS**: 00C9, image boots |
| Probe advertises | **PASS**: `ODPROBE69ED53`, steady, no reboot seen in 20 s. Address is the random fallback, so 0x76000 is blank on this tag; the legacy firmware's 34:64:60:42:D2:41 comes from elsewhere |
| Probe accepts a connection | **FAIL**: connects time out. Cause: the link-layer ACL FIFOs (`blt_rxfifo`/`blt_txfifo`) were never defined, so the linker made them 1-byte/zero-sized. Fixed in `src/od_tlsr_fifo.c`, and `tools/check_ll_fifos.py` now fails a build that lacks them. The fix is **not yet on the tag** |
| Way back (probe → the legacy firmware through `tlsr_ota.py`) | **BLOCKED**: unreachable without a connection. Needs SWS |
| Full image, display, SEC2, OTA power-cut fallback | not run |

## `telink-tlsr` on tag 03BE11 (TLSR825x, 2.66" BWR SSD, legacy v98) — bring-up (2026-11-14)

| Row | Result |
|---|---|
| Legacy v98 updates onto itself via `tools/legacy_ota.py` (`2025-05-27_9c5f268282.bin`) | **PASS**: back at fw 0x0062, 0x0005 and 0x0011 byte-identical |
| The legacy firmware installs the probe @ e61640d (44,132 B, FIFOs defined, GAP count fixed) | **PASS**: boots as `ODPROBE6B42B5` |
| Probe accepts a connection, GATT discovery | **PASS**: GAP, GATT and the updater service (value handle 11, write + notify) |
| Updater STATUS | **FAIL**: replies "unknown opcode" for a byte that changes per packet. Cause: the SDK and liblt_825x.a are built with `-fpack-struct`; unpacked, `rf_packet_att_write_t.value` is at offset 10 instead of the wire's 9, so every write is read one byte late. Fixed by compiling the SDK with `-fpack-struct` and reading its headers under `pack(1)`; a compile-time assert pins offset 9. **Not on the tag** |
| Way back | **BLOCKED**: the updater cannot parse a command. Needs SWS |

## `telink-tlsr` on OD05B911 (TLSR825x, 2.0" BWRY JD, legacy v108) — bring-up (2026-11-14)

| Row | Result |
|---|---|
| Legacy v108 updates onto itself via `tools/legacy_ota.py` | **PASS**: back at fw 0x006C, 0x0011 byte-identical; 0x0005 byte 35 went 0 → 1 (it reads 1 on both other tags; runtime state, not stored config) |
| The legacy firmware installs the probe @ f2fda19 (44,148 B, packed SDK structs) | **PASS**: boots as `ODPROBE84DCED` |
| Probe connects; updater STATUS | **PASS**: `idle, 0, 0, ok`, twice. The write path reads the value at the right offset |
| Stage legacy v108 through the updater (no install) | **INCOMPLETE**: BEGIN's staging erase completed; then 15-byte writes, because the tool failed to acquire the MTU, ran for 500 s and the tool timed out mid-transfer |
| Tag afterwards | **SILENT**: no advertising under any `ODPROBE`/`A4:C1:38` address in 25 s and 60 s scans. A reboot would show a new random address. Nothing outside staging was written and no INSTALL was sent. Not yet explained: a coin cell sagging under ~8 min of continuous connection plus erases, or a lockup the watchdog did not clear (**open**) |
| After a manual power cycle | **PASS**: back as `ODPROBEAECA0C`; STATUS `idle, ok`. The probe image was intact |
| Stage legacy v108 at MTU 247 (239-byte chunks, ~60 s) | **PASS**: `staged and verified` on the device (CRC pass, held-back boot flag written and read back); STATUS after reconnect `verified, 126388, 126388, ok`, so a verified image survives a disconnect |
| **Way back: INSTALL legacy v108 through our updater (RAM install routine, first run)** | **PASS**: the tag rebooted into legacy v108 (MSD fw 0x006C, hw 0x6C); 0x0011 byte-identical to before any update, 0x0005 identical to after the legacy firmware self-update |
| Round trip: legacy v108 installs the probe again (same f2fda19 image, sha256 d5d2c50b…) | **PASS**: boots as `ODPROBEF12E9D`; STATUS `idle, 0, 0, ok` |
| Updatability after the second probe flash: stage legacy v108 (no install) | **PASS**: `staged and verified`; STATUS `verified, 126388, 126388, ok`. Left staged; the probe stays on |
| Probe updater installs the full image @ 43fc6ea (90,052 B, sha256 4973d105…) | **PASS**: boots as `OD9933A8` (A4:C1:38:99:33:A8); MSD 0x2446 `ffffffffffffffffffffff500064`, service 0x2446 advertised; GATT has GAP, GATT, 0x2446 and the updater service |
| Full image: updater STATUS | **PASS**: `idle, 0, 0, ok` |
| Full image: `CMD_FIRMWARE_VERSION` 0x0043 and `CMD_READ_IDENT` 0x0046, plaintext | **PASS**: `0043 03 00 08 "tlsr-dev"` (3.0) and `0046 04 06 00 29 a0 0f 80 00 00` (fw_target 0x06). Only these two read-only commands ran; config write, SEC2 and every transfer opcode are untested here |
| Full image: updatability, stage legacy v108 (no install) | **PASS**: `staged and verified`; STATUS `verified, 126388, 126388, ok`. Left staged; the full image stays on |
| Full image: `CMD_CONFIG_READ` 0x0040 with nothing stored, plaintext | **PASS**: `ff400000`, the fleet's no-config answer (same as Nordic and Silabs; py-opendisplay reads it as "no stored configuration") |
| Full image: chunked `CMD_CONFIG_WRITE` 0x0041 + `CMD_CONFIG_CHUNK` 0x0042, 229 B, plaintext | **PASS**: `00410000`, `00420000`. Config = the corpus's `minimal-required-singletons` packets plus three 0x20 display packets (200×152, every pin 0xFF), built with py-opendisplay's serializer. Nothing on this target applies config to hardware |
| Full image: config readback after write | **PASS**: 229 B byte-identical; py-opendisplay `parse_config_response` gives 3 displays at 200×152. READ_IDENT went from 0 to 3 displays without a reboot, so the write reloads config live |
| Full image: `CMD_REBOOT` 0x000F, then config readback | **PASS**: the tag came back as `ODDA7248` (a new random address, because 0x76000 is blank); READ_IDENT still 3 displays; config 229 B byte-identical. Config persists across a reboot |
| Full image after reboot: updater | **PASS**: STATUS `idle, ok` (the first connect failed with a BlueZ `EOFError`, the retry worked: contention). The reboot took the updater from `verified` back to `idle` (that state is RAM-only); legacy v108 staged and verified again, and left staged |
| Not run: `CMD_CONFIG_CLEAR`, a single-frame write, a config with a security packet, power-cut during a config write, SEC2 | **OPEN** |
| Full image @ 0674cd2 (address from the flash UID, 90,276 B, sha256 0fefaaf1…): first install attempt | **FAIL, recovered**: the host got ATT error 0x0e mid-transfer and the tag rebooted into the previous image (new random address `OD014F08`, STATUS `idle, ok`). A reset during an upload is **open**, alongside the earlier silent-after-abort row |
| Same image, second install | **PASS**: boots as **`OD05B911` at `FA:FB:3E:05:B9:11`**, the address the legacy firmware used on this tag, and the name carries the legacy firmware's three bytes |
| UID address across `CMD_REBOOT` | **PASS**: back at `FA:FB:3E:05:B9:11` / `OD05B911` |
| Config across the image change and the reboot | **PASS**: the 229 B config reads back byte-identical; READ_IDENT 3 displays |
| Updatability on this image: stage legacy v108 (no install) | **PASS**: `staged and verified`; STATUS `verified, 126388, 126388, ok`. Left staged |
| Full image @ 1198c6b (boot state in DEEP_ANA_REG1, reset cause in STATUS; 90,388 B, sha256 ab6ca765…): install | **PASS**: running (STATUS carries the new fields). The host lost its D-Bus connection (`EOFError` in dbus_fast) during the disconnect after INSTALL; bluetoothd did not restart. Host-side, the second time today |
| STATUS boot info right after that install | `unplanned, 6`: **expected artefact**, not six crashes. The previous image's boot mark 0xA5 reads in the new layout as "booted, not planned, 5", and the previous updater's install did not mark its reboot planned |
| `CMD_REBOOT`, then STATUS | **PASS**: `planned, 6`. A planned reboot is told apart and leaves the unplanned count alone; address, READ_IDENT and the 229 B config unchanged |
| Updatability on this image | **PASS**: legacy v108 `staged and verified`; STATUS `verified, …, planned, 6`. Left staged |
| Next disturbed transfer: read STATUS **before** any power cycle | **OPEN**: `power-on` means a brownout (coin cell), `unplanned` with the count up by one means the watchdog or a crash |
| Full image @ 55071ad (panel path through `od_display`, 131,412 B, sha256 2d5ab067…): install | **PASS**: staged and verified at MTU 247, installed; STATUS after reboot `idle, ok, planned, 6` (the install's reboot is marked planned; no unplanned reset) |
| Updatability on this image: stage legacy v108 (no install) | **PASS**: `staged and verified`; STATUS `verified, 126388, 126388, ok, planned, 6`. Left staged. One status read and one config read lost the host's D-Bus connection (`EOFError`); the retries worked |
| Old config (no drive packets) on the panel image | **PASS**: READ_IDENT tail flags 0x0B, NEEDS_MIGRATION set; nothing drawn |
| Display config, 270 B, plaintext chunked write: SystemConfig `pwr_pin` PB5 (13) with DeviceFlags 0x41 (PWR_PIN + provisional PWR_PIN_ACTIVE_LOW), one 0x20 (152×200, panel 0xF001, BWRY, CLEAR_ON_BOOT, pins as in `targets/telink-tlsr/README.md`), 0xB1 + one 0xAE from `panels.yaml` | **PASS**: `00410000`, `00420000`; readback byte-identical; READ_IDENT `0101f09800c80003` + tail flags 0x09 (NEEDS_MIGRATION clear). The reload parks the panel pins and drives the rail pin to off; CLEAR_ON_BOOT kept the boot screen from drawing. Tag healthy afterwards (STATUS `verified, …, planned, 6`) |
| First refresh (0x0070 raw 7,600 B, FULL), go-ahead given; sequence as first committed (PON then `@busy`) | **FAIL, diagnosed**: START NACK `ff700900` (OD_ERR_PANEL_BUSY) after 31.4 s. No reset (STATUS `verified, …, planned, 6`) |
| Same, BUSY declared unusable (24 s fixed waits; diagnostic config) | Flow completes: START 49 s, END `0072`, `0073` 24 s later. Proves the transfer path only; BUSY ignored |
| Image @ 2e43465 (adds CMD_SPI_RAW; 132,516 B, sha256 9698ba96…): install, then stage legacy v108 | **PASS**: STATUS `idle, ok, planned, 6`; the legacy firmware `staged and verified`, left staged |
| Raw SPI: BUSY after power + pins / after RESET / after a bare PON | low / **released at once** / still low after 3 s. The rail (active-low PB5), the pin map and BUSY on PA1 are right; the controller answers RESET |
| Raw SPI: the legacy firmware's exact register list, then PON | BUSY still low after 10 s, and after 60 s |
| Legacy v108 timing (disassembly, 0xa698 at 0x8c58 / 0xbbe4) | The legacy firmware caps every BUSY wait at 400 ms (after reset, PON, POF) and carries on; its bit-bang SPI (0xa46c) is the same mode 0, MSB first, no added delay |
| Raw SPI refresh: registers, DTM + 7,600 B pattern, PON (400 ms cap), DRF 12 00, POF, deep sleep | **BUSY released 16.7 s after DRF**; POF released at once. BUSY on this glass holds low from PON until the refresh ends |
| 0xF001 sequence changed: DTM + image before PON, `@delay 400` after PON, `@busy` after DRF only; config rewritten (272 B, readback identical) | **PASS**: 0x0070 START `0070` in 1.0 s, 7,600 B by 4.6 s, END `0072`, REFRESH_SUCCESS `0073` at 21.8 s (≈17 s BUSY-sensed refresh). Repeated: 1.0 / 4.7 / 22.0 s. Tag healthy (STATUS `verified, …, planned, 6`) |
| `CMD_REBOOT`, then config readback, then 0x0070 upload | **PASS**: same address and name; the 272 B config reads back byte-identical to what was written; CLEAR_ON_BOOT kept the boot screen off; upload START 1.1 s, `0073` at 21.8 s. STATUS `idle, …, planned, 6`; legacy v108 staged and verified again and left staged |
| Boot screen: config rewritten with CLEAR_ON_BOOT clear (272 B, readback identical), then `CMD_REBOOT` | **PASS on glass** (Jonas): the boot screen draws at startup on the 200×152 BWRY panel. STATUS after the reboot `idle, ok, planned, 6` (the blocking draw on the loop caused no watchdog or unplanned reset); legacy v108 staged and verified again, left staged. Battery and temperature fields have no sensor behind them on this target yet |
| Image with battery ADC, panel TSC temperature, PIPE W=6 (138,132 B, sha256 b6e9b715…): install, then stage legacy v108 | **PASS**: STATUS `verified, 126388, 126388, ok, planned, 6`; the legacy firmware left staged |
| Config 272 B with PowerOption `battery_sense_pin` PB3 (11) and 0x20 `transmission_modes` STREAMING \| ZIP \| PIPE_WRITE | **PASS**: `00410000`, `00420000`, readback identical |
| MSD after the reload's panel-up, decoded by py-opendisplay `parse_advertisement` | **PASS, plausible**: battery 2990 mV, later 2980 mV (coin cell); temperature 24 °C from the JD controller's TSC (cmd 0x40, 3-wire read). Neither was cross-checked against a meter or thermometer |
| py-opendisplay `upload_image`, compressed (418 B, 9-bit window), PIPE off | **PASS**: 18.7 s end to end |
| py-opendisplay `upload_image`, compressed (416 B), PIPE on | **PASS on glass** (Jonas): negotiated W=6 N=6 frame 244, selective; 18.9 s |
| py-opendisplay `upload_image`, uncompressed 7,600 B, PIPE on | **PASS on glass** (Jonas): W=6; 20.1 s end to end, about 1 s more than compressed. STATUS afterwards `verified, …, planned, 6`: no reset across the three uploads |
| Not run: encrypted uploads, partial, setup-family screens, PIPE retransmit (no loss was induced), the boot screen's battery/temperature text on glass | **OPEN** |
| What the glass shows at 152×200 (Jonas's photo) | Colour map **correct** (black, white, yellow, red as sent; code_map identity confirmed), text upright and unmirrored. Geometry **wrong**: the glass is 200 wide × 152 tall, so 48 columns on the right stayed blank and the last 48 rows (most of the red band) were lost |
| 0xF001 renamed EP20YR_200x152, TRES `61 00 C8 00 98`; config 0x20 200×152 (272 B, readback identical, READ_IDENT `01f0c80098`) | Upload START 1.0 s, `0073` at 22.0 s. **PASS on glass** (Jonas): the four bands fill the panel, triangle top-right, colours right |

## `telink-tlsr` on OD060F40 (TLSR825x, 3.5" HS BWY UC, legacy screen type 1) — install (2026-11-14)

The legacy firmware's 0x0011 on this tag: 384×184 (the legacy firmware's `h` is the glass width), 2 colours (BWY), OEPL type 0x60. EPD pins RESET PD4, DC PD7, BUSY PA1, CS PB4, CLK PB5, MOSI PB6, enable PC5 with `enable_invert` 1, and a second enable `enable1` PB7, which our config has no field for. Battery ADC PB3; LED PD2/PD3/PA7.

| Row | Result |
|---|---|
| The legacy firmware's updater installs the full image (138,132 B, sha256 b6e9b715…), with no probe first | **PASS**: boots as `OD060F40` at the legacy firmware's own address 3D:67:58:06:0F:40. The host's D-Bus link dropped (`EOFError`) at the disconnect after the transfer; the install had completed |
| Updater STATUS, then stage legacy v108 (no install) | **PASS**: `idle, ok, power-on, 0`, then `staged and verified`; left staged |
| Config 133 B, plaintext single-frame write: rail PC5 active-low, battery PB3, one 0x20 (384×184, BWY, panel id 0, no drive packets) | **PASS**: `00410000`, readback identical; READ_IDENT 1 display, NEEDS_MIGRATION set; nothing drawn |
| MSD | **PASS, plausible**: battery 2950 mV (no meter check); temperature at the "no reading" floor, as designed before any panel-up |
| Sequence: legacy v108's UC driver (0xbbcc, family 0 of the table at flash 0x1e8b8), default branch for type 1; provisional panel 0xF002 EP35Y_184x384 | Transcribed; see `panels/panels.yaml` |
| Config with 0xF002, rail PC5 active-low, PSR `AF 0F` (as first transcribed) | **FAIL**: upload reports success in 0.7 s, glass unchanged. Raw SPI: PON raises BUSY for 23 ms; DRF never raises it |
| Same with PC5 active-high; with PB7 as the rail (active-high); with a 20 ms delay before the post-DRF BUSY poll | **FAIL**, same symptom each time |
| Raw SPI with PB7 as the rail: PON, PSR `AF 0F` / `8F 0F`, DRF | `AF 0F`: BUSY never raised. **`8F 0F` (OTP LUT): BUSY held 27.9 s**, a real refresh. PON took 81-83 ms with PB7, against 23 ms in every PC5 run |
| Upload with PSR `8F 0F`, PB7 rail, no CDI | Refreshes (28.5 s). **B/W inverted** on glass (Jonas), yellow right |
| CDI `50 87` / `50 A7` / `50 97` | 87: B/W still inverted. A7: yellow inverted instead. **97: PASS on glass** (Jonas): geometry 184×384, orientation (triangle top-right), black, white and yellow right |
| BUSY timeout raised to 60 s; config 229 B rewritten (readback identical); py-opendisplay compressed + PIPE (W=6) upload | **PASS**: 30.3 s end to end. STATUS `verified, …, planned, 0`: no unplanned reset; legacy v108 still staged |
| MSD after panel-ups | battery 2960 mV; temperature 20 °C from the UC controller's TSC (3-wire read), plausible, not checked against a thermometer |
| One upload stalled with no reply for 90 s (host `BLETimeoutError`) right after a config write; STATUS afterwards `verified, planned, 0`, and the retry passed | **OPEN**: cause unknown (the tag did not reset) |
| Not run: whether PC5 is needed alongside PB7 now that the PSR is right; uncompressed, plaintext-only and encrypted uploads; the boot screen at 184×384; LED | **OPEN** |

### OD060F40 — idle power and the GPIO sub-op (2026-11-14)

PPK2 as source meter at 3.0 V into the battery terminals, sampled at ~86 kS/s.

| Row | Result |
|---|---|
| Baseline, image b6e9b715… (suspend disabled, 100 ms advertising), idle | 2.9 mA mean, p10 2.76 mA: no suspend at all |
| Suspend when idle, 100 ms advertising | 219 µA mean, floor (median) 49 µA |
| Suspend + 1000 ms advertising (20-30 ms for 3 s after boot / disconnect) | **65 µA mean**, median 47.8 µA, p10 45.2 µA (25 s window). Legacy v108 on the same tag: under 20 µA mean (Jonas) |
| Pull-downs on 18 spare pins | Floor unchanged (45.2 µA): no floating-pin leakage; the floor is the suspend mode |
| Install via `tlsr_ota.py`, then stage legacy v108, with suspend on | **PASS**: `verified, 126388, planned`; done three times |
| py-opendisplay compressed PIPE (W=6) upload with suspend on | **PASS**: refresh complete, 28.5 s |
| 3 min idle | **PASS**: no unplanned reset, "verified" (RAM-only) survived |
| GPIO sub-op: read PA1/PC5/PB7, set PD2 high/low/pull-up/pull-down/float with readback; PA7 and pins ≥ 32 refused | **PASS**: `00 84 07 pin level`; refused pins `FF 84 01 07`. PD2 reads 1 under its pull-down: an external pull-up (LED) |
| GPIO pull-down on PE0 (before port E was refused) | **FAIL**, one unplanned reset: PE0..PE3 are the flash bus. Port E is now invalid for config and GPIO alike |
| A read pass over every pin, then idle | **PASS** after the fix: floor 47 µA. Before it, a read left PB/PC inputs enabled (their input-enable is an analog register) and the floor rose to 168 µA |
| Power-floor test image (`-DOD_TLSR_PMTEST=ON`: probe cycling A 100 ms adv + suspend / B suspend, radio off / C deep sleep, 32 KB retention / D deep sleep, no retention), 3 cycles | A floor 49.5 µA (mean ~195). **B 45 µA**: the suspend floor is the chip's suspend mode. **C 3.6 µA** (4 µA mean with a timer wake every 4 s). **D 1.7 µA**: the board itself. Deep retention is the missing piece; with 1000 ms advertising (~17 µA) it predicts ~20 µA, the legacy firmware's figure |
| Test image updatable in phase A; OpenDisplay image reinstalled from it, then legacy v108 staged | **PASS**: STATUS answered 13 s after install; reinstall `planned, 0`; `verified, 126388` |

### OD060F40 — deep retention in the OpenDisplay image (2026-11-14)

Every static in the 32 KB retention bank (`tools/retention_link.py`; 32,080 B used), retention
while advertising only, from 60 s after a boot with no unplanned reset since power-on.

| Row | Result |
|---|---|
| Install (158,164 B image, ROM loads ~32 KB at cold boot), cold boot, STATUS | **PASS**: `planned, 0`; and again after a power-on boot |
| Retention starts 60 s after boot | **PASS**: per-second floor 47 µA → 4.0 µA at the 60 s mark |
| Idle, 1000 ms advertising, 21.6 s window | **24 µA mean**, median 4.0 µA, p10 3.6 µA (was 65 µA mean with suspend). Legacy v108: under 20 µA |
| Uptime runs through retention sleeps: PPK2 supply 3.0 → 2.7 V while in retention, MSD battery (60 s cache) | **PASS**: 2930 → 2640 mV within 25 s; the cache could only expire on a running clock |
| Connect from retention: py-opendisplay compressed PIPE upload; config read; GPIO sub-op reads | **PASS**: refresh 28.5 s; config 229 B identical; GPIO replies |
| Stage legacy v108, then ~75 s of retention, STATUS | **PASS**: `verified, 126388` survives the sleeps; unplanned 0 |
| Floor after a refresh (rail pin held off by the pull-down through deep sleep) | **PASS**: 3.9 µA; MSD temperature 21 °C from the panel |
| Fallback: fault-test image (`-DOD_TLSR_FAULT_TEST=ON`), GPIO pin 0xFE arms a hang in the next retention wake | **PASS**: ~4 s at 2.9 mA (the hang), watchdog reset, STATUS `unplanned, 1`; floor stays 47 µA (suspend only) for minutes past the 60 s mark; updater answers |
| Update from the fallback state, then power cycle (PPK2 off ~5 s) | **PASS**: build a1a1a1a1 installed (`planned, 1`, still suspend-only); after the power cycle `power-on, 0` and retention back ~66 s after power-up |
| Self-update, OpenDisplay over OpenDisplay, no legacy image staged: a1a1a1a1 → b2b2b2b2 from the retention state | **PASS**: READ_FW_VERSION `3.0.0 build b2b2b2b2` after the reboot |
| Corrupted image (one byte flipped mid-image), sent past the host's check | **PASS**: FINISH refused `crc`, STATUS `failed, …, crc`; build b2b2b2b2 keeps running; the next install (the permanent image, sha256 84aebe19…) is clean |
| Idle connection held 30 s (BlueZ's parameters) | 305 µA mean, 48 µA median: suspend between connection events, as designed |
| OD060F40 made permanent (Jonas): OpenDisplay image sha256 84aebe19… installed, no legacy image staged | Done |
| OD05B911 (active-low rail PB5, battery): install, upload | **PASS** in suspend-only mode: refresh complete at 22 s; STATUS `planned, 6`. Its 6 unplanned resets since the battery went in (bring-up) keep retention off until a power cycle; legacy v108 staged again |
| OD05B911 after a battery cycle (Jonas) | STATUS `power-on, 0`: retention armed |
| Retention on OD05B911, detected without a meter: PA2 (floating) set pull-down + output high over the GPIO sub-op, link closed, 90 s idle, read back | **PASS**: read 1 before, 0 after, while STATUS stayed `power-on, 0` (no reboot): only a retention wake resets the digital GPIO registers |
| Two uploads with retention between them (active-low rail PB5, pull-up while off) | **PASS**: both refreshes complete at ~22 s, no reset; MSD 2960 mV, 25 °C. The rail being off *during* sleep is not measured on this tag (no meter) |
| Boot screen on the fix image (0ff7a43 + no boot screen after an unplanned reset; sha256 644164c4…); config already without CLEAR_ON_BOOT | **PASS**: boots and stays up, STATUS `planned, 0`; MSD temperature 23 °C with no upload since the install, so the panel ran at boot. Glass not checked. Does **not** prove the watchdog fix: this 200×152 panel renders under 4 s, and it booted the boot screen on earlier images too. Legacy v108 staged again |
| Review-fix image @ 3b81ebd (M1 supply floor, M2 install read-back, L1 reset byte, L3-L5, L7, L8 bring-up caps off; 158,548 B, sha256 1dd4ca03…), from STATUS `verified, planned, 0` with legacy v108 staged (2026-11-15) | **PASS**: staged and verified at MTU 247, installed with the new read-back install routine; STATUS after reboot `idle, ok, planned, 0` at FA:FB:3E:05:B9:11; legacy v108 staged and verified again (BEGIN passed the supply check), left staged. Only the updater ran: no config read, upload, glass, SEC2, reset-pin or power-cut row on this image |
| Not run: hours-long soak (deferred by Jonas) | **OPEN** |

### OD060F40 — factory reset and SEC2 claim (2026-11-14)

Power cycles are the PPK2 cutting DUT power for 1.5 s; on-periods 4.5 s (inside T_ARM 2 s ..
T_CLEAR 10 s). Host: py-opendisplay `v3/py` (f838fe0) CLI. Rows 1-3 on the permanent image
(84aebe19…), the rest on the image with the setup screens wired.

| Row | Result |
|---|---|
| A 1.5 s PPK2 cut reads as a power-on (DEEP_ANA_REG1 lost): planned reboot, cut, STATUS | **PASS**: `planned` → `power-on, 0` |
| Factory reset: five quick cuts, config present, unclaimed | **PASS**: config gone (read returns none), STATUS `planned` (R8 reboot), SEC_STATUS `UNCLAIMED`, claim mode, 3 renders |
| Below the pattern: three quick cuts after restoring the config | **PASS**: claim mode only; config 229 B identical |
| Claim gesture (two quick cuts): SETUP screen drawn | **PASS** on the meter: ~30 s refresh at 5-13 mA once past T_CLEAR; not advertising during it. Screen not photographed |
| `opendisplay claim`: CODE screen, code typed from the glass (Jonas) | **PASS**: `Claimed 00003d6758060f40`; SEC_STATUS `CLAIMED`. The CODE refresh ran inside the open connection |
| Encrypted session: `opendisplay info` | **PASS**: config read, "Encryption Enabled" |
| Encrypted upload: `opendisplay upload` (184x384 BWY, compressed) | **PASS**: done in 48 s; idle back to ~28 µA mean. Glass not checked |
| Claimed, no session: plaintext 0x0040 config read, 0x000F reboot, 0x0084 raw SPI | **PASS**: each `[00][cmd][FE]` (auth required); 0x0046 and 0x0043 answer, as specified |
| Updater on the claimed tag, ungated image | STATUS answers; the updater took a plain install (this is how the gated image got on) |
| Gated image, claimed, no session: `tlsr_ota.py stage` | **PASS**: BEGIN refused `locked`; STATUS answers; plaintext 0x0051 → `[00][51][FE]` |
| `tlsr_ota.py --sec2 install` (session via py-opendisplay, sealed ENTER_DFU, same link) | **PASS**: ENTER_DFU ACK, 239-byte chunks, staged, verified, installed; READ_FW_VERSION `3.0.0 build c3c3c3c3`; still `CLAIMED`, encrypted `info` works |
| The unlock does not outlive the link: plain stage after that reboot | **PASS**: `locked` again |
| Back to the dev build the same way | **PASS**: `3.0.0 build tlsr-dev` (sha256 5e939cf7…) |
| Key rotation: `opendisplay rotate-key` | **PASS**: the stored key changes (fingerprint compared, not the key); encrypted `info` with the new key works; the old key gets `0x0056` AUTH_FAILED |
| Unclaim: `opendisplay unclaim` | **PASS**: SEC_STATUS `UNCLAIMED`, no claim mode; plaintext config read answers (295 B); plain `tlsr_ota.py stage` verifies (updater open again); the key store forgot the uid |
| Not run: reset pin (no 0x27 pin on this tag), session timeout, replay | **OPEN** |
| Boot screen on power-up (CLEAR_ON_BOOT cleared in the stored config, CMD_REBOOT), image 43c7946 | **FAIL**: watchdog reset loop from ~1 s after every boot (~3.6 mA, ~5.5 s period = 4 s watchdog + boot), never advertises; the five-cycle factory reset does not count (render starts before T_ARM). Tag needs SWS. Fix 0ff7a43 (watchdog fed from data writes) not yet run on a tag |
