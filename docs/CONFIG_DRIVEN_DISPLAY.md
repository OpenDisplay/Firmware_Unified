# Config-driven display path

Every panel command a device sends comes from the config's drive packets: 0xAE display_mode,
0xAF display_lut, 0xB1 display_panel (0xB0 calibration is host-only). Firmware holds no panel
table and no framebuffer. This file is the contract between that firmware path, the panel
database that generates the packets (`panels/` in the panel-data stream), and hosts.

Status: host-tested (`tests/host/display_test.c`, `epd_seq_test.c`) and building on Nordic
(nRF52840 MCUboot, nRF54L15, nRF54LM20A) and BG22. **Not verified on hardware.**

## Layers

| layer | file | owns |
|---|---|---|
| od_display | `shared/core/od_display.c` | session per operation: resolve panel + mode, temp policy, rail up/down, stream, refresh tail, sleep. Uploads (0x70/0x76/PIPE), 0x0085 drive transfers, the boot screen, the image store |
| od_epd_cfg | `shared/core/od_epd_cfg.c` | binds od_epd to the stored config (od_cfg_index reads bodies on demand); marker-budget stream writer; mode by name; temp policy |
| od_epd | `shared/core/od_epd.c` | the sequence interpreter: validate before any I/O, walk, pause at image markers |
| target | `od_drive_app.h`, `od_display_app.h`, `od_xfer_app.h` seams | rail and settle times, SPI bus, pins, die sensor, watchdog phase, flash diverts |

Nordic takes all three shared layers (`targets/nordic-zephyr/src/opendisplay_display.c`).
BG22 keeps its own adapter over od_epd for flash. ESP32 still runs bb_epaper/FastEPD; its port
onto od_display is the next stream (od_drive_app.h is unchanged so its 0x0085 path keeps
compiling).

## Mode conventions (proposed; maintainer decision)

Modes are matched by their 12-byte 0xAE name, case-insensitively.

- **mode 0** is the plain full refresh. Ordinary uploads and the boot screen use it, and it
  must be single-pass and render the panel's own scheme (`color_scheme` 0xFF or equal to 0x20).
- **"fast"**: when the 0x0072 refresh byte is FAST (1) on a full upload, mode 0's head and image
  run, then the *tail* of "fast" (the steps after its last image marker). Without a "fast" mode
  FAST falls back to mode 0's own tail. This mirrors bb_epaper's refresh-mode-specific init.
- **"partial"** drives 0x0076 / PIPE partial updates. Its existence is what makes
  `od_xfer_app_panel_info().partial_enabled` true; the 0x20 flag alone does not.
- Any other mode (e.g. "colour16", multi-pass) is reached only through CMD_DRIVE_WRITE_START.

## Image markers and plane order

The frame bytes for a pass are split across the sequence's image markers in order. Each marker
takes a budget:

- `IMAGE` takes one controller plane on a CONTROLLER_PLANES frame (BWR, BWY, GRAY4: plane 0 then
  plane 1, as on the wire), and the whole frame otherwise (MONO, BWRY, 4 bpp).
- `IMAGE_SLICE part/of` takes frame/of, as the protocol says, on every layout; on SPLIT_HALVES
  `x/2` takes that half's exact size.

So a two-plane frame reads identically as `IMAGE, IMAGE` or `IMAGE_SLICE 0/2, 1/2`. A sequence
with fewer markers than the layout needs fails at the plane boundary instead of overrunning.
Multi-pass: a frame boundary is a pass boundary; the pass's tail (refresh) runs inside the write
that completes it, then the next pass's head.

**Partial wire format**: OLD plane, then NEW plane, `plane_bytes` each (1 bpp, byte-aligned
rectangle). The "partial" mode's two markers take them in that order. A rectangle smaller than
the panel needs a sequence carrying `WINDOW` (it is refused UNSUP otherwise, before any I/O,
because the controller would be written from the origin). Sequences should `FILL` both RAM
planes first, as bb_epaper did. The panel database's partial modes are full-frame (no WINDOW),
so today they accept only a whole-panel rectangle.

## Provisional escapes (need opendisplay-protocol adoption)

| esc | args | meaning |
|---|---|---|
| 0x0C WINDOW | `[n][(kind,val)*n]` | one step of n bytes (cmd first); kind 0 literal, kind 1 geometry: bits 2..0 select x0, x1, y0, y1, W-1, H-1, w, h; bit 3 /8; bit 4 mirror; bit 5 high byte |
| 0x0D FILL | `[cmd][value][flags]` | cmd + a plane of `value`; flags bits 1..0 bpp (1/2/4), bit 7 window instead of panel |

The window is the partial rectangle during a partial update and the whole panel otherwise.
Both compile out on BG22 (`OD_EPD_WITH_WINDOW=0`), where a sequence using them is refused
SEQ_UNSUPPORTED. The numbers are provisional until the canonical `DriveSeqEscape` enum takes
them.

## Limits

| | default (Nordic, ESP32, host) | BG22 |
|---|---|---|
| `OD_EPD_SEQ_MAX` (one mode's sequence, held in RAM per pass) | 512 B | 128 B |
| `OD_EPD_MAX_SEQ_STEPS` | 96 | 64 |
| `OD_EPD_SLEEP_SEQ_MAX` | 16 B | 16 B |
| `OD_EPD_LUT_BYTES` | 535 B | 535 B |

512 / 96 covers every flow the panel database encodes (longest 424 B, 80 steps). On BG22 a
longer mode is refused at select. A host cannot query these yet; regenerate the panel data
against these values.

## Escape support per target

| escape | Nordic | BG22 | notes |
|---|---|---|---|
| SET_PIN | yes | yes | Nordic drives the pin as a GPIO output, level 2 releases to input |
| CS_SELECT | yes | yes | offered only when `cs_pin_2` is wired and distinct |
| READ_SCRATCH | **no** | yes | the Nordic panel bus is write-only (SPIM MOSI/SCK) |
| WRITE_SCRATCH, LUT, LUT_SLICE, IMAGE_SLICE, SUBST, DELAY, BUSY | yes | yes | LUT steps work after the image marker too |
| WINDOW, FILL | yes | no | provisional |

An unsupported escape anywhere in a sequence refuses the whole sequence before the panel is
touched (wire: OD_ERR_DRIVE_SEQ_UNSUPPORTED under 0x0085).

## A config without drive packets ("needs migration")

A 2.x config after an OTA has a display (0x20) but no 0xAE/0xB1. Firmware then draws nothing:

- **Upload starts NACK `0x08` (OD_ERR_NEEDS_MIGRATION, provisional)**:
  `[FF][70][08][00]` (0x70 START; its other refusals stay the 2-byte `[FF][70]`), `[FF][76][08][00]`,
  `[FF][80][08][00]` (PIPE START). 0x08 is free in both the OD_ERR_PARTIAL_* and the
  OD_ERR_PIPE_START_* namespaces. 0x0085 keeps OD_ERR_DRIVE_NO_CONFIG (0x02).
- The **boot screen is skipped**, and not latched: the first config write that brings drive
  packets shows it (Nordic, from the loop once the link is idle).
- The image store refuses to display a slot.
- Hosts learn the state from READ_IDENT's NEEDS_MIGRATION flag (below). The MSD has no free status bit (bit 3 is
  ENCRYPTION_ENABLED, bits 4-7 the loop counter), so it does not carry this.

## READ_IDENT (0x0046)

Shared handler `shared/core/od_cmd_ident.c` on every target that sets `OD_CAP_READ_IDENT`
(BG22, Nordic). Plaintext, pre-gate:
`[ACK][0x46][proto_minor][fw_target][count][7 B per display: ic_type LE, w LE, h LE, scheme]`.
`fw_target` on Nordic: nRF52840 answers 0x01 (FIRMWARE), nRF54 answers 0x02 (NRF54); this is a
maintainer question.

**Trailing flags byte (provisional)**, after the records:

| bit | name | meaning |
|---|---|---|
| 0 | CONFIG_DRIVEN | this firmware drives panels only from drive packets (`OD_IDENT_CONFIG_DRIVEN`, default 1) |
| 1 | NEEDS_MIGRATION | a configured display has no 0xB1 + 0xAE for its instance: write a config with drive packets |
| 2-7 | reserved, 0 | |

A host that reads `count * 7` bytes after the header never sees it, and a 2.x firmware simply
does not send it, so "no flags byte" means "not config-driven / unknown".

## code_map

0xB1 carries `code_map[4]` (logical colour to panel 2-bit code). Firmware stores it and exposes
it (`od_epd_cfg_code_map()`) but **does not apply it**: frame bytes reach the panel as the host
sent them, so hosts send panel codes. Whether firmware should remap instead is an open decision.
Codes in 0xB0 are panel codes already and must never be remapped.

## Primitives the language still lacks

From the panel database's `blocked_on` ids, against this firmware:

| id | status |
|---|---|
| SEQ_MAX | lifted to 512 B / 96 steps (BG22 unchanged) |
| LUT_AFTER_IMAGE | done: LUT context survives the image pause |
| SET_PIN_RESET | done on Nordic and BG22 (SET_PIN with OD_SEQ_PINREF_RESET) |
| WINDOW | provisional escape 0x0C (+ FILL 0x0D); the generator does not emit it yet |
| CS_BOTH | missing: one transfer to both controllers (CS_SELECT takes 0/1 only) |
| PIXEL_XFORM | missing: firmware-side repacking (SSD2683 1->2 bpp, 4BPP_DATA, Spectra nibble clamp); hosts must send controller format |
| READBACK | missing: controller detection by register read-back (SSD2677 sticky variant); needs a conditional/branch primitive, and READ_SCRATCH, which Nordic cannot do |
| IT8951, PARALLEL | out of scope for the sequence language |
| sleep > 16 B | not seen in the database yet; `OD_EPD_SLEEP_SEQ_MAX` would need the same treatment |
