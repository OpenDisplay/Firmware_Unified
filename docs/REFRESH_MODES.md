# Refresh modes: provisional wire layouts and device rules

The lab handover's refresh modes (`HANDOVER_refresh_modes.md`, proposal
`presets_home_proposal.md` § 5.3 F1/F2/F5/F7) need four additive format changes. **None is in
`opendisplay_structs.h` / `opendisplay_protocol.h` yet.** They match the py-opendisplay presets
test branch (`local/presets-test`, REPORT.md § "Provisional byte layouts") and are listed here
so they can be moved into `../opendisplay-protocol` once Jonas accepts them. Firmware that
predates them degrades safely: an unknown tail is ignored, a compressed or high-id slot reads as
absent, and the new START flag is refused.

## Layouts

All little-endian.

**0xAE mode tail** (after `seq`), v1 = 4 bytes, host information only (the device ignores it):
`[ext_version=1][mode_flags: bit0 EXPERIMENTAL][budget_cost][scrub_before: mode_index | 0xFF]`.

**0xB1 panel tail** (after `sleep_seq`), v1 = 9 bytes:

| off | field | read by the device |
|---|---|---|
| 0 | `ext_version` = 1 | yes |
| 1-2 | `drive_preset_key` | no |
| 3-4 | `drive_rev` | no |
| 5 | `drive_flags` bit0 HAND_EDITED | no |
| 6 | `budget_limit` (0xFF = none) | no |
| 7 | `lut_family`: 0 none, 1 S6-800, 2 JD-BWRY-535, 3 x4-536 | yes: lint + never-send list |
| 8 | `busy_timeout_s` | yes: wins over `busy_timeout_ms` when non-zero |

**0xAF `lut_slot`**: bits 0-3 = slot id (16 slots), bit 7 = body is raw deflate (RFC 1951,
10-bit window), bits 4-6 reserved (a set bit makes the packet match no slot). Stored and
inflated bodies are both ≤ 1024 B (`OD_EPD_LUT_BYTES`). `crc16` (CCITT-FALSE) covers the
**stored** bytes.

**CMD_DRIVE_WRITE_START flag 0x04 `INLINE_LUT`**: the two reserved bytes become `inline_len`
(u16, must be non-zero). The data stream starts with `inline_len` bytes of entries
`[slot_id][len:2][crc16:2][table: len bytes]`, then the frames, so the expected stream size is
`passes × frame + inline_len` (the 4-byte `uncompressed_size` of a compressed stream counts the
prefix too). The tables shadow the stored slots for this transfer only; nothing is written to
flash. Pass 0 begins once the prefix is in.

## Device rules (F2, F7)

Enforced only when the 0xB1 tail declares a `lut_family` (configs written before the tail run
as before). Every refusal happens **before any panel I/O** and returns `OD_EPD_RC_LINT` (-10),
reported on the wire as `OD_ERR_DRIVE_SEQ_UNSUPPORTED` (0x07).

- **Table lint** on every stored or inline table: S6-800 = 799 or 800 B, header not all zero, no
  8 zero bytes in a row (handover 4.3, lutlint L7). JD-BWRY = exactly 535 B, and in each of the
  6 blocks the first row whose timing sums to zero has M = 0 (the block terminator; row 7 is
  exempt). x4 = 535 or 536 B. A failing slot is absent.
- **A mode that uploads a table (`LUT` / `LUT_SLICE`) whose slot is absent is refused** rather
  than run with the panel's OTP waveform under custom-table registers.
- **Never-send list**: 0x90, 0x91, 0xA0-0xA3, 0x8F, 0xFB, 0xFF in any step (plain, SUBST,
  WRITE_SCRATCH, WINDOW).
- **No TSC read (0x40) between a table upload and the next 0x12** (handover 4.4).
- `busy_timeout_s` lifts the BUSY ceiling past the u16 ms field (handover 4.7: ≥ 120 s).

## Decisions and conflicts for Jonas

1. **0x4D and 0xB6 are not on the never-send list.** The handover lists "+4D/B6/8F/FB/FF on
   BWRY/EE02", but the stock 2.9in BWRY init sends `4D 78` and the EE02 init sends `B6 07`
   (both verified on glass). The firmware cannot refuse them without refusing the stock modes.
2. **Inline tables are sent raw, not deflated.** The proposal says the stream starts "with the
   deflated table(s)". The 0x0085 stream already has its own zlib option (flag 0x01), so a
   second deflate layer gains nothing and would need a second inflate state during the stream.
3. **Lint failures reuse `OD_ERR_DRIVE_SEQ_UNSUPPORTED` (0x07)**, not a new error code. A host
   cannot tell "table refused by the lint" from "escape not supported here".
4. **BG22 and Telink have no LUT cache** (`OD_EPD_LUT_CACHE=0`, RAM): compressed slots read as
   absent there and `INLINE_LUT` is NACKed with `OD_ERR_DRIVE_UNSUPPORTED`. Panels driven by
   BG22 boards must store their tables uncompressed (the 2.9in and 7.5in BWRY entries in
   `panels/generated/refresh_modes.json` do). BG22 also has no WINDOW escape, so both BWRY
   windowed partials (`partial_bw`) are written as a literal full-panel 0x83 with hold codes for
   unchanged pixels. The 7.5in loads its table after the whole init including the booster tail,
   the only placement that panel honours, then sets PLL 02 for the table's 20 ms frames.
5. **`mode_count` in the 0xB1 head** is written as highest index + 1 by the generator.
6. The EE02 7.09in entry is built on the lab-order mode 0 (`tests/host/ee02_spectra_preset.inc`),
   not on 0x0042 (shared with the 13.3in), so its drive key is 0. Its stock path through this
   firmware has not run on glass.

## Multi-pass Spectra 6 (`colour32`, `colour20`, EE02 7.09in)

Names follow `colour16`: `colourN` = an N-colour palette drawn in several passes. Mode 5
`colour32` runs the same custom-table sequence three times (set-4 split rails `0F 00 1A 24 30 38`,
PLL 08, POF on both chips after each pass), one table per pass: slot 0 G13 (a full refresh that
gives every pixel a defined stock-like colour, so no scrub and no history), slot 2 PS_best_A,
slot 3 MIXg. The two PLL-04 tables carry a rate-code-4 LUTC (13.6 ms) so all passes share PLL 08.
Pass frames are ordinary BWGBRY_SPLIT nibbles (mode `color_scheme` 0xFF); code 4 holds.

Mode 6 `colour20` runs the first two passes only (G13, PS_best_A; about 27 s).

Each mode's 0xB0 holds a maximin palette (`panels/palettes/ee02_709_colour32.json`, 32 entries,
min dE00 8.6; `ee02_709_colour20.json`, 20 entries, min dE00 8.0) with
`cal_flags` bit 1 `CODES_3BIT` (structs 2.4, opendisplay-protocol PR 17): pass p's code in bits
[3p+2:3p]. Lab is D50 media-relative (stock white = L* 100). The palette and its counts are from a
rigcam search over all code tuples (294 triples, 42 pairs) of three candidate table sets (camera-provisional);
`shared/protocol/opendisplay_structs.h` does not have the flag yet, and the firmware never reads
0xB0.

## Temperature windows (enforced)

Every experimental mode now carries an enforced window with `temp_policy` SUBSTITUTE
(`panels/tools/pdlib.py` `CUSTOM_WINDOW_C`, decision 2026-11-25): **15-30 C**, narrowed to the
mode's own lab band where it has one (`faster_cool` 15-24, `faster_warm` 25-30 on the 7.3in;
`colour16` 18-30 from its measured gamut gate). Outside its window the device runs mode 0, the
stock full, and the START ACK reports mode 0, so the host sends mode 0's frames. Mode 0 never
carries a window (the panel-level `temp_gate` no longer applies to it), so the fallback always
exists. A temperature the device cannot read (0x7F) skips the check, as before.

Bands of one mode (`*_cool` / `*_warm`, `*6` / `*7`) stay separate modes, one table each; the
host picks the one whose window holds the temperature (py-opendisplay `drive_plan`), and the
device's window is the safety net when the host's reading is stale.

## Drive data

`panels/tools/gen_refresh_modes.py` writes `panels/generated/refresh_modes.json` and
`tests/host/refresh_modes.inc` (`--check` gates both). Every register stream is the one the lab
drove on glass, with the rails, VCOM and PLL each table was gated with; the generator names the
source of each. `tests/host/refresh_modes_test.c` runs every mode through the executor, with and
without the LUT cache.
