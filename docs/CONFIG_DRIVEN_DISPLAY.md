# Config-driven display path

Every panel command a device sends comes from the config's drive packets: 0xAE display_mode,
0xAF display_lut, 0xB1 display_panel (0xB0 calibration is host-only). Firmware holds no panel
table and no framebuffer. This file is the contract between that firmware path, the panel
database that generates the packets (`panels/` in the panel-data stream), and hosts.

Status: host-tested (`tests/host/display_test.c`, `epd_seq_test.c`, `xfer_split_test.c`) and
building on Nordic (nRF52840 MCUboot, nRF54L15, nRF54LM20A), BG22 and all 13 ESP32 boards.
**Not verified on hardware on any target.**

## Layers

| layer | file | owns |
|---|---|---|
| od_display | `shared/core/od_display.c` | session per operation: resolve panel + mode, temp policy, rail up/down, stream, refresh tail, sleep. Uploads (0x70/0x76/PIPE), 0x0085 drive transfers, the boot screen, the image store |
| od_epd_cfg | `shared/core/od_epd_cfg.c` | binds od_epd to the stored config (od_cfg_index reads bodies on demand); marker-budget stream writer; mode by name; temp policy |
| od_epd | `shared/core/od_epd.c` | the sequence interpreter: validate before any I/O, walk, pause at image markers |
| target | `od_drive_app.h`, `od_display_app.h`, `od_xfer_app.h` seams | rail and settle times, SPI bus, pins, die sensor, watchdog phase, flash diverts |

Nordic (`targets/nordic-zephyr/src/opendisplay_display.c`) and ESP32
(`targets/esp32-idf/src/od_display_esp.cpp`, `display_service.cpp`) take all three shared
layers. BG22 keeps its own adapter over od_epd for flash. ESP32 additionally keeps FastEPD for
the panels a built-in controller driver drives (IT8951, parallel); see "Built-in controller
drivers" below.

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
planes first, as bb_epaper did. In the panel database (schema minor 1, `panels/generated/`) the
default class (ESP32, Nordic) gives 28 panels' partial modes WINDOW (+ FILL), so they accept any
byte-aligned rectangle; BG22's class has no WINDOW/FILL, so there partial modes accept only a
whole-panel rectangle. EP213ZZ (0x0049) keeps a whole-panel partial on every target (its RAM X
offset is not expressible as WINDOW geometry).

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
| mode sequence length | no cap (the record) | no cap (the record) |
| `OD_EPD_MAX_SEQ_STEPS` | 128 | 128 |
| `OD_EPD_SEQ_WINDOW` (sequence bytes resident in RAM) | 128 B | 128 B |
| `OD_EPD_SLEEP_SEQ_MAX` | 16 B | 16 B |
| `OD_EPD_LUT_BYTES` | 535 B | 535 B |

A mode sequence is not copied into RAM: od_epd reads it from the stored record through
`io->seq_read` into a 128-byte window as it walks, for validation and execution alike (the sleep
sequence is read from the descriptor's copy). The only bound is the step cap, 128 on every
target. It exists to stop a runaway or hostile sequence, not to save memory; the largest panel
in the database has 79 steps. 128 B is the window because it holds every escape with its
arguments whole (the largest, SUBST or WINDOW with 32 pairs, is 67 B) and every step of 70 of the
77 database panels. The other seven (0x001B, 0x0020, 0x002F, 0x0043, 0x004B, 0x004C with a
154-byte 0x32 waveform-register step, 0x0044 with 229 B) send that one step as the command plus
two data runs, which is how a LUT slot and the frame already reach the bus (one `data()` call
per run, D/C held high). The bytes are the same as 2.x (the panel replay compares them); the
split itself has not been checked on glass for those seven panels. A host cannot query these
limits; it should refuse more than 128 steps before writing.

A pass reads the stored record from begin to the end of its tail, pausing at the image marker
while the frame arrives. For that whole span the display binding holds config writes:
`od_config_store_save()` returns `OD_CONFIG_STORE_BUSY` and the CONFIG_WRITE is NACKed as a
failed save, so the host retries after the refresh. `od_config_store_clear()` is never held (it
is the security erase ahead of an unauthenticated rewrite), so od_epd also compares the store's
generation counter before every read: a moved config, or a storage read that fails or comes back
short, fails the pass (`OD_EPD_RC_STALE`, `OD_EPD_RC_IO`) and, if the panel has already seen I/O,
runs the sleep sequence before returning; the caller's own sleep afterwards is absorbed so the
panel sees it once. A failure before the first panel I/O touches nothing, as any refusal.

## Escape support per target

| escape | Nordic | ESP32 | BG22 | notes |
|---|---|---|---|---|
| SET_PIN | yes | yes | yes | Nordic and ESP32 drive the pin as a GPIO output, level 2 releases to input |
| CS_SELECT | yes | yes | yes | offered only when `cs_pin_2` is wired and distinct |
| READ_SCRATCH | **no** | **no** | yes | the Nordic and ESP32 panel buses are write-only (no MISO) |
| WRITE_SCRATCH, LUT, LUT_SLICE, IMAGE_SLICE, SUBST, DELAY, BUSY | yes | yes | yes | LUT steps work after the image marker too |
| WINDOW, FILL | yes | yes | no | provisional |

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

### Config writes are stored exactly as sent

Firmware does not merge or carry anything across a config write. A 2.x client that rewrites the
config without drive packets leaves a device with none: it draws nothing, uploads NACK at START
(above) and READ_IDENT reports NEEDS_MIGRATION, which is how a 3.x client detects the device and
repairs it by writing a complete config. A config write never reads the stored record, so a
damaged record (bad CRC, magic or length) never blocks its own replacement.

## READ_IDENT (0x0046)

Shared handler `shared/core/od_cmd_ident.c` on every target that sets `OD_CAP_READ_IDENT`
(BG22, Nordic, ESP32). Plaintext, pre-gate:
`[ACK][0x46][proto_minor][fw_target][count][7 B per display: ic_type LE, w LE, h LE, scheme]`.
`fw_target` on Nordic: nRF52840 answers 0x01 (FIRMWARE), nRF54 answers 0x02 (NRF54); ESP32
answers 0x01 (FIRMWARE). This is a maintainer question.

On ESP32 (`OD_IDENT_NATIVE_PANELS=1`) a display on a built-in controller driver (IT8951,
parallel) never sets NEEDS_MIGRATION: it draws without drive packets. CONFIG_DRIVEN is still
set, because every SPI panel is driven from drive packets; whether a host needs a separate
"built-in driver" bit is a maintainer question.

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
| SEQ_MAX | gone: sequences stream from the record on every target, 128-step cap |
| LUT_AFTER_IMAGE | done: LUT context survives the image pause |
| SET_PIN_RESET | done on Nordic and BG22 (SET_PIN with OD_SEQ_PINREF_RESET) |
| WINDOW | provisional escape 0x0C (+ FILL 0x0D); the generator does not emit it yet |
| CS_BOTH | not added, deliberately: the panel database expands every both-CS transfer into a cs0 copy then a cs1 copy (its D6), so no flow needs it. Making CS_SELECT take 2 would be a value BG22 accepts at validation and then fails at runtime |
| PIXEL_XFORM | missing: firmware-side repacking (SSD2683 1->2 bpp, 4BPP_DATA, Spectra nibble clamp); hosts must send controller format |
| READBACK | missing: controller detection by register read-back (SSD2677 sticky variant); needs a conditional/branch primitive, and READ_SCRATCH, which Nordic cannot do |
| IT8951, PARALLEL | out of scope for the sequence language; ESP32 selects a built-in driver by 0xB3 (below) |
| sleep > 16 B | not seen in the database yet; `OD_EPD_SLEEP_SEQ_MAX` would need the same treatment |

## ESP32 target

`targets/esp32-idf/src/od_display_esp.cpp` implements the od_drive_app / od_display_app /
od_ident_app seams, CMD_SPI_RAW and the boot policy; `display_service.cpp` the od_xfer_app glue;
`od_epd_esp.cpp` the panel bus (SPI2, 8 MHz, mode 0, write-only; D/C, CS, CS2, RST, BUSY as
GPIO). No panel library is linked for SPI panels (`tools/check.sh`: "esp32: no panel library").

Build switches (`targets/esp32-idf/main/CMakeLists.txt`), all default 0 elsewhere:

| macro | effect |
|---|---|
| `OD_CAP_READ_IDENT=1`, `OD_IDENT_TARGET=OD_IDENT_TARGET_FIRMWARE` | 0x0046 answered |
| `OD_IDENT_NATIVE_PANELS=1` | `od_ident_app_native_panel()`: built-in-driver panels are not NEEDS_MIGRATION |
| `OD_DISPLAY_SHARED_BOOT_SINK=0` | od_display exports its boot sink as `od_display_boot_sink_*`; `boot_screen.cpp` picks od_display or FastEPD per frame |
| `OD_XFER_SPLIT_HALVES=1` | `od_xfer_panel_info_t.split_halves_ok`: SPLIT_HALVES uploads (dual-controller Spectra 6, 0x002B / 0x0042) start instead of being refused |
| `OD_CFG_INDEX_CONTROLLER=1` | od_cfg_index records 0xB3 |

Behaviour that is target logic rather than drive data:

| behaviour | where | why not data |
|---|---|---|
| Rail, AXP2101 bring-up, 800 ms settle; keep-alive window (`screen_timeout_seconds`, max 30 s, 0 on AXP2101) | `pwrmgm()`, `rail_on/rail_release` | board power, not panel |
| 200 ms bleed between the sleep sequence and the rail cut when a `pwr_pin` or AXP2101 cuts power | `rail_off()` | the 4.2in SSD16xx (0x0002) greys out with less; its discharge commands are in its 0xB1 sleep sequence, but a 200 ms DELAY does not fit SLEEP_MAX (16 B). Applied to every panel on a switched rail |
| Boot presence probe: reset, BUSY must reach idle within 2 s, else no boot screen and a raw rail cut | `boot_panel_responds()` | skipped for split panels, no BUSY/RST, or a 0xB1 that says BUSY is not usable |
| reTerminal Sticky SD-card park (SCK 13 / MOSI 14 / CS 15: GPIO8 high, GPIO10 low) before any panel SPI | `park_shared_sd_card()` | a board quirk |
| Boot screen deferred after a config write that brings drive packets | `displayServiceDeferredBoot()` from `loop()` | as Nordic |
| Watchdog fed at every od_display phase change | `od_display_app_phase()` | multi-pass refreshes run inside one write |

Panel behaviour bb_epaper carried that the panel data does not yet encode (no target code
re-creates it, by design):

- 0x0027 (4.26in SSD1677): the register partial LUT and 0x22=0xDC trigger. The preset has only a
  "full" mode, so partial uploads are not offered on it.
- EP397 / EP426 partial RAM windows (Y-decrement, X-mirror): need WINDOW from the generator.
- SSD2677 vs SSD1677 on the Sticky (register 0x70): needs READBACK.

## Built-in controller drivers (0xB3 display_controller, PROVISIONAL)

**For the protocol owner: a wire addition, not in opendisplay-protocol.** The id is a fallback
`#define` in `shared/core/od_cfg_index.h`; it goes when the canonical header assigns one.

Two panel classes cannot be driven by a command walk (analysis: the panel-data stream's
`panels/NOTES_special.md`). IT8951 speaks a 16-bit word protocol with a handshake per word and
needs values read back at runtime; a parallel panel has no controller, and the MCU clocks every
frame of the waveform. So the config does not describe their bytes. It **selects a built-in
driver and parameterises it**, which is what a board-id switch did before:

`[n][0xB3][len:2 LE]` + body (length-prefixed range, so older firmware steps over it):

| byte | field | |
|---|---|---|
| 0 | instance_number | the 0x20 display it describes |
| 1 | version | 0 |
| 2 | controller_class | 0 command-driven (drive packets), 1 IT8951 (SPI), 2 parallel (MCU-timed) |
| 3 | flags | bit 0 MIRRORED (rows reversed), bit 1 GRAY16 (4 bpp frame whatever the scheme), bit 2 TEMP_SENSOR (refresh temperature from the configured sensor) |
| 4-5 | vcom_mv LE | IT8951; 0 keeps the controller's stored VCOM |
| 6 | default_temp_c | int8, used without a sensor reading; 0x7F = the driver's default |
| 7 | parallel_panel | class 2: 1 Inkplate 5 V2, 2 Inkplate 10, 3 M5Stack PaperS3 (the board's bus/PMIC wiring, which FastEPD owns) |
| 8 | miso_pin | IT8951 read line, 0xFF none |
| 9 | en_pin | IT8951 board supply enable, 0xFF none |
| 10 | ite_en_pin | IT8951 controller enable, 0xFF none |
| 11 | reserved | 0; a longer body is accepted, the tail reserved |

SCLK, MOSI, CS, BUSY (HRDY) and RST stay in 0x20. ESP32 reads it through
`targets/esp32-idf/src/od_controller_esp.cpp`; a FastEPD build drives class 1 and 2, every other
build treats them as undrivable (NEEDS_MIGRATION). A config **without** 0xB3 is described from
`panel_ic_type` for the panels that shipped before it (3000/3001 ED103TC2, 3005 M5Paper, 3002 /
3003 Inkplate, 3004 PaperS3): that table is now the only place a panel id selects a driver, and
it exists so fielded configs keep working. Removing it is a maintainer decision.

Not data, and not proposed as data: the IT8951 word protocol and its DEV_INFO / LISAR readback,
the IT8951 waveform-mode choice per refresh (INIT/GC16/DU/A2 stay in `display_it8951.cpp`), and
the parallel waveform engine. Like every packet, 0xB3 is stored only if the config write
contains it, so a 2.x config write drops it and the legacy table takes over again.

## FastEPD parallel: why it stays on FastEPD

PaperS3, Inkplate 5 V2 / 10 (S3 boards) and the M5Paper's IT8951 (wrover build) stay on FastEPD,
selected by 0xB3 or the legacy table. A parallel panel's "drive data" is a waveform matrix plus
a frame engine: the MCU strobes CKV/SPV and latches the source drivers for every row of every
frame, with microsecond timing and DMA over the LCD/i80 peripheral. A forward-only command walk
cannot express that, and the matrices alone (the one part that could be 0xAF-sized data) do
nothing without the engine. Moving them would add a second copy of FastEPD's tables to the
config for no behavioural gain, so the driver stays linked and the config only selects and
parameterises it.

