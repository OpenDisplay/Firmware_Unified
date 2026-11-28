# Proposal: frame layout fields in DisplayConfig (0x20)

Status: implemented for **3.0**, before the v3 migration ships, so it changes the 3.0 wire format
directly and needs no compatibility period of its own. Canonical: OpenDisplay/opendisplay-protocol#18
(structs 2.5), synced into `shared/protocol/`. Host: py-opendisplay fork `local/presets-test`
(OpenDisplay/py-opendisplay#266). Firmware: `od_color_frame_layout()` / `od_color_display_geometry()`
and the frame walker in `shared/core/od_color.c`, used by `od_boot_screen.c`. Glass: open (step 5).

## Problem

The device has no framebuffer and no pixel transform: every IMAGE / IMAGE_SLICE byte goes to the
panel as the host sent it. Some panels want an order the host cannot produce today:

| Panel | Needs |
|---|---|
| GDEY0579F52 5.79in BWRY | two controllers, 396 columns each; rows sent as pairs (271-i, i) |
| CrowPanel EP579_792x272 (0x0033) 5.79in mono | two SSD1683s, 400 columns each, second starts at column 392; column-major |
| GDEY0579T93 / Z93 5.79in | two SSD1683s, 396 columns padded to 400 |

`color_scheme` already describes *what* a pixel is (bits, planes). These fields describe *where*
the bytes go. Like `color_scheme`, they are a fixed property of the glass, so they belong in 0x20
next to it.

## Wire layout

Defined in OpenDisplay/opendisplay-protocol#18 (structs 2.5). Taken from `reserved[13]` (struct
offsets 33-41). All-zero is today's behaviour, so every existing config is unchanged and the
struct stays 46 bytes.

| Offset | Field | Type | Values |
|---|---|---|---|
| 33 | `frame_split` | u8 enum | 0 NONE; 1 HALVES (left part, then right part, one IMAGE_SLICE each) |
| 34 | `frame_scan` | u8 bits | bit0 COLUMN_MAJOR; bit1 MIRROR_X; bit2 MIRROR_Y; bit3 LSB_FIRST; bits 4-7 0 |
| 35-36 | `part_width` | u16 le | columns each part carries on the wire; 0 = width / parts |
| 37-38 | `part_stride` | u16 le | panel column where the next part starts, relative to this one; 0 = `part_width` |
| 39 | `plane_invert` | u8 bits | bit0 invert plane 0, bit1 invert plane 1 |
| 40 | `pixel_pack` | u8 enum | 0 SCHEME (as `color_scheme` packs); 1 NIBBLE (one pixel per nibble, 2-bit code in the low bits) |
| 41 | `row_order` | u8 enum | 0 LINEAR; 1 FOLD (h-1, 0, h-2, 1, ...); 2 INTERLEAVE (rows y, y+1 as one stream: per column, (x, y+1) then (x, y)) |
| 42-45 | `reserved[4]` | | must be 0 |

Order of application: split into parts, then row order and scan within each part, then byte
packing (nibble, invert, bit order). Columns a part covers past the panel's edge are sent blank
(white).

### Not defined yet

These were found in vendor code but are left out of 2.5. Because undefined values are refused,
each can be added later without misdrawing on an older reader:

- dense 3 bits per pixel without the UC8159 filler bits (a `pixel_pack` value);
- two planes with different formats: 2-bit codes in plane 0 plus an inverted 1-bit colour plane,
  as UC8154 BWY tags use (a new scheme or a `pixel_pack` value);
- four controllers (`frame_split` 2), until a sample shows each quadrant's scan direction;
- a part boundary off a byte. GDEY0579T93 (1 bpp, second part at column 396) needs one: 396 / 8
  is not whole, so every row of the second part would have to be bit-shifted. It is refused
  today; a sample decides whether its controller can instead start at 392 or 400.

A dual-CS panel that switches CS halfway through every row needs nothing new. Each controller
keeps its own RAM address counter, so sending part 0 whole and then part 1 (`frame_split = 1`
with CS_SELECT in the sequence) writes the same RAM.

The one-byte-per-pixel expansion is dropped: IT8951 panels are driven by the device's own
controller driver (0xB3), not by a forwarded stream.

### The four panels

| Panel | split | scan | part_width | part_stride | Wire bytes |
|---|---|---|---|---|---|
| GDEY0579F52 (2 bpp) | 1 | row_order FOLD | 396 | 0 | 2 × 26,928 |
| CrowPanel 0x0033 (1 bpp) | 1 | COLUMN_MAJOR | 400 | 392 | 2 × 13,600 |
| GDEY0579T93 (1 bpp) | 1 | (mirror per sample, to confirm) | 400 | 396 | not expressible yet (above) |
| every current panel | 0 | 0 | 0 | 0 | unchanged |

## Compatibility

- **Panels without quirks:** the new bytes are 0 in every existing config, because they were
  `reserved`. All-zero packs, sizes and boots exactly as today. No preset, no stored config and
  no host change for those panels.
- **`BWGBRY_SPLIT` (scheme 8) does not exist in 3.0.** Its panels become `color_scheme = BWGBRY`
  (4) + `frame_split = 1`. The v3 migration (`panels/migration.py`, and `od-v3.js` on the
  website, which it ports) already rewrites 0x20 for every 2.x config (`panel_ic_type` remap);
  it also writes the layout fields from the panel's preset entry and turns scheme 8 into 4.
  3.0 firmware and host refuse scheme 8 at config load, with a reason that points to the
  migration. The scheme 8 special cases go (`od_color.c`, the `od_boot_app_segments()`
  derivation in `od_display.c`, the BG22 and ESP32 display paths, `images.py` `half_planes`).
  The enum value stays reserved so it is never reused.
- **Units already on 3.0** (the bench units, the six BG22s of the 2026-11-13 rollout): they have
  no scheme 8 panel and every new byte is 0, so nothing changes for them. A preset update writes
  the fields for any unit that does get a split panel.
- The migration also runs when a 3.0 config's layout fields differ from its preset entry, the
  same way it already replaces drive packets that don't match.

## Who reads what

**Host (py-opendisplay)** reads all fields and packs the frame. One pass after dither and
`code_map` (`encoding/images.py`): cut parts, then apply row order / scan / mirror / bit order /
invert / nibble packing per part, then concatenate. Each step is a pure reorder or a fixed expansion, tested
against the vendor reference code (the Good Display A32 sample, bb_epaper `bbepWriteHalf`).

**Firmware, upload path**, reads only the fields that change the frame's *size*: `frame_split`,
`part_width`, `part_stride`, `pixel_pack`. They feed `od_color_direct_geometry()`, so `part_bytes[]` /
`total_bytes` match what the host sends and the existing START size check still holds. With
`frame_split = 1`, any scheme gets `OD_COLOR_LAYOUT_SPLIT_HALVES`, so `marker_budget()` already
budgets one part per `IMAGE_SLICE n/2`. Uploaded bytes are still forwarded unchanged.

**Firmware, boot and setup screens** (`od_boot_screen.c`), read every field, because the device
produces these frames itself. The renderer decides each pixel from its (x, y) alone (text, QR,
logo and swatches are all coordinate tests), so it needs no framebuffer for any of the layouts:

| Field | Renderer change | Extra RAM |
|---|---|---|
| `frame_split` + `part_width` / `part_stride` | generalises today's `od_boot_app_segments()` slicing: render the row, send columns `[n*stride, n*stride + part_width)`, blank past the edge | row pitch grows to the widest part's end (EP579: 100 B instead of 99 B) |
| `row_order` FOLD / INTERLEAVE | row loop visits (h-1-i, i), or renders rows y and y+1 and merges them per column | INTERLEAVE: a second row of pitch bytes |
| MIRROR_Y / MIRROR_X | row loop reversed / x mapped as `w-1-x` (same place rotation is mapped) | none |
| COLUMN_MAJOR | loop byte-column outer, y inner; emit `h` bytes per column | one column buffer of `h` bytes (EP579: 272 B), inside `row_len` |
| `plane_invert` / LSB_FIRST | XOR / bit-reverse each byte before writing | none |
| `pixel_pack` NIBBLE | write each 2-bit code into a nibble | row pitch doubles |

A part boundary that is not byte-aligned at the scheme's bpp is refused at config load (all
known panels align: 392 / 8, 396 / 4). The boot screen draws whatever the layout fields
describe, so it cannot disagree with what the host sends.

## Rules

- Partial updates: geometry reports `partial_supported = false` whenever `frame_split != 0` or
  `frame_scan`, `row_order` or `pixel_pack` is non-zero. The host refuses rather than sending a window in the wrong order. Split
  partials (a window crossing the part boundary becomes one window per controller) are a later
  definition.
- Multipass: each pass is packed with the same layout, and passes are concatenated as today.
- Reserved values are refused: by the host at pack time, by firmware at config load, logged.
  This covers unknown enum values, reserved bits in `frame_scan` / `plane_invert`, and any
  non-zero byte in the remaining `reserved[5]`. That is what lets a later quirk use them: a 3.0
  device or host that doesn't know it refuses the config instead of silently drawing a wrong
  frame. 3.0 is the last point where this rule is free to add. Today nothing checks those bytes.
- The panel presets (`panels/`, `drive_presets.json`) carry these fields with the panel. The
  migration and the config builder write them into 0x20 together with `color_scheme`.

## Decided while implementing

Host and firmware agree on each of these; `tests/vectors/gen_frame_layout.py` holds the cases.

- `part_width` / `part_stride` without a split are refused.
- `plane_invert` bit 1 on a one-plane scheme is refused.
- `plane_invert` alone keeps the frame's order, so partials stay available.
- Row-end pad bits are 0, as in a linear frame, and are inverted with the rest of the byte.
- MIRROR_X / MIRROR_Y apply to a part's own columns and rows, before the row order.
- FOLD on an odd height ends on the middle row, sent once.
- A split panel's mode sequences carry IMAGE_SLICE 0/2 and 1/2, or no image at all (sleep);
  a whole-frame IMAGE or a single slice is refused at config load.
- The boot renderer writes a byte-aligned split, FOLD or MIRROR_Y straight from its drawn row,
  so those layouts need no RAM beyond today's row buffer.

## Left out on purpose

- Four controllers (`frame_split = 2`): reserved until a sample shows each quadrant's scan
  direction.
- Parallel / I2S panels (Inkplate, epdiy) and segment ICs: they can't be driven by a byte stream
  the device only forwards.

## Placement

The fields live in 0x20, not the 0xB1 head. They are a fixed property of the glass, like
`color_scheme`. The
price is that the layout and a panel's per-part sequence sit in different packets. Two rules keep
them in step: the migration and preset updates always write both together, and firmware refuses
at config load a sequence whose IMAGE_SLICE count differs from `frame_split`'s part count.

## Steps, all before 3.0

1. `../opendisplay-protocol`: carve the fields out of `reserved[13]` and retire scheme 8 —
   OpenDisplay/opendisplay-protocol#18. Sync the header here once it merges.
2. py-opendisplay: `DisplayConfig` parse/serialize, the packer, size accounting in
   `device/imaging.py`, the partial refusal, scheme 8 refusal, and the migration writing the fields
   and rewriting scheme 8. Mirror the migration in `od-v3.js`, through the website session.
3. Firmware: size geometry in `od_color.c`; the boot renderer's part / row order / mirror / column /
   invert / bit-order / nibble handling in `od_boot_screen.c`; scheme 8 refusal and removal of
   its special cases; a host test per layout that checks the boot frame byte for byte against
   py-opendisplay's packer on the same raster.
4. Presets: add the fields for `0x002B`, `0x0042` (formerly scheme 8), 0x0033 and the GDEY0579F52
   (which needs its new panel id). Hand-write the 5.79in per-half sequences.
5. Glass: re-check `0x002B` / `0x0042` (boot screen and upload), then the two 5.79in panels.
