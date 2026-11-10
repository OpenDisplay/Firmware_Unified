# IT8951 and FastEPD parallel panels: can they be config-driven?

Analysis only. Nothing here is implemented or tested. Sources: `third_party/FastEPD/src`
(`FastEPD.inl`, `FastEPD.h`), `third_party/bb_epaper` (`arduino_io.inl`, `bb_ep.inl` 4edfa7f),
`targets/esp32-idf/panel/od_panel_fastepd.cpp`, `shared/core/od_epd.{c,h}`.

The panels in question are panel_ic_type 3000/3001 (ED103TC2 behind an IT8951, SPI) and
3002/3003 (Inkplate 5 V2 and Inkplate 10, driven directly over a parallel bus). None of them has a
2.x bb_epaper id, so none appears in `generated/drive_presets.json`. Today all four are claimed by
`od_panel_fastepd.cpp` on esp32-idf only.

## Short answer

- **IT8951: partly.** Bring-up, VCOM and refresh triggering are a short fixed command script and
  could be config data. The image path cannot, because it depends on values read back from the
  controller at runtime and on a 16-bit word bus protocol with a handshake on every word. The
  sensible split is a target-side IT8951 *bus backend* under od_epd, with the drive script carried
  in 0xAE. That needs four new primitives (below), and the gain over today's FastEPD path is small.
- **FastEPD parallel (Inkplate): no.** There is no controller. The MCU is the timing controller:
  it clocks every row of every frame of the waveform over an 8/16-bit i80/LCD bus, with
  gate-driver strobes (CKV/SPV) and source-driver latching, for every update. The "drive data"
  is a waveform matrix plus a frame-sequencing engine with microsecond timing and DMA, which is
  not something a forward-only command walk can express. Its waveform *matrices* could move into
  config (0xAF-sized blobs); the engine stays target code.

## IT8951 in detail

The wire protocol (FastEPD `it8951WriteCmdCode`/`it8951WriteData`/`it8951ReadData`, bb_epaper
`bbepWriteIT8951Cmd`):

- Every transaction is CS-framed and starts with a 16-bit preamble: `0x6000` command, `0x0000`
  data write, `0x1000` data read (followed by a dummy word). There is no DC pin.
- Commands and arguments are 16-bit big-endian words (`__builtin_bswap16` in bb_epaper).
- **HRDY** (wired to the BUSY input) must read high before *every* transaction, not only after a
  refresh (`it8951WaitForReady`, 3 s cap, polled every 10 ms).
- Bring-up: `SYS_RUN`, `GET_DEV_INFO` (0x0302) followed by an `IT8951DevInfo` word read that returns the panel
  width/height and the **image buffer address**. That address is then written into `LISAR`
  (0x0208/0x020A) before every image load. Then `I80CPCR` (packed write) and VCOM.
- Image: `LD_IMG_AREA` with five words (endian/bpp/rotate, x, y, w, h), then the pixel words
  (1bpp or 4bpp packed, byte-swapped per 16-bit word), `LD_IMG_END`, then `DPY_AREA` with
  x, y, w, h and the waveform mode (INIT/DU/GC16/A2), then HRDY.

Mapping onto od_epd:

| need | od_epd today | primitive |
|---|---|---|
| no DC pin; 16-bit preamble per transaction | `cmd()`/`data()` imply DC | `IT8951` bus backend: the target io encodes cmd as `6000 cccc` and data as `0000 …`; the sequence stays byte-oriented |
| HRDY before every transaction | BUSY only where a step says so | backend property (wait inside io->cmd/io->data) -- keeps the sequence short |
| read DEV_INFO, write the buffer address into LISAR | `READ_SCRATCH`/`WRITE_SCRATCH` exist, but `od_epd_cfg` leaves `read_bytes` NULL | `READBACK` via the backend's read preamble, then scratch substitution |
| x/y/w/h in LD_IMG_AREA and DPY_AREA | fixed bytes only | `WINDOW` (needed for partial updates; a full-screen update can hard-code the panel size) |
| byte-swapped 16-bit pixel words | host bytes streamed raw | host-side: the host can pre-swap, as with the Spectra nibbles (D7) |
| waveform mode per update | one mode per 0xAE | fine: one 0xAE per waveform mode (INIT, GC16, DU, A2) |

With an `IT8951` backend plus `READBACK` and scratch substitution, a full-screen GC16 update would, by
estimate (not encoded), fit in well under 100 bytes of sequence, under `OD_EPD_SEQ_MAX`. Partial updates additionally need `WINDOW`.
The other costs are that the frame (1872x1404 at 4bpp is about 1.3 MB) only works on PSRAM
targets, and that the multi-attempt probe and power sequencing in `bbepInitIT8951` (EN and
ITE_EN pins, retries on HRDY timeout) would still be target code. **Recommendation:** keep IT8951
on FastEPD until a second IT8951 product exists. Config-driving one panel buys nothing.

## FastEPD parallel panels in detail

`FastEPD.inl` drives the panel itself:

- The bus is an ESP32-S3 i80 LCD peripheral (`esp_lcd_i80_bus`) carrying source data, plus GPIO
  strobes for the gate driver (CKV, SPV, SPH as source CS, LE). `pfnRowControl` sequences each
  row, and `s3_notify_dma_ready` pipelines rows over DMA.
- The panel power rails come from a PMIC over I2C (TPS65186 on Inkplate 10) (`pfnEinkPower`), with board
  specific sequencing.
- Grey levels come from a **waveform matrix** (`u8GrayMatrix`, `u8Ink5V2Matrix`,
  `u8TenPointThreeMatrix`, …: 16 levels x N frames of 2-bit push values). Every update runs N
  full-panel frames, and each frame re-encodes the whole image through the matrix at row rate.
  `s3_diff.S` holds ESP32-S3 SIMD versions of those conversion routines.

None of that is a command stream. Things that *could* be data:

- the waveform matrix, either per panel or per temperature (a few hundred bytes, which fits a
  0xAF slot);
- the panel geometry and bus width, already in the 0x20 display config;
- the PMIC voltages.

The frame engine, the row timing and the DMA pipeline must stay target code: a per-target
`od_panel_ops` backend, as today. Marking these `PARALLEL` in the primitive table means "not
expressible", not "missing one escape".
