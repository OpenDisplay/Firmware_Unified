# panels/ -- the panel drive database

Firmware 3.0 drives every SPI e-paper panel from config packets (0xAE display_mode, 0xAF
display_lut, 0xB0 display_calibration, 0xB1 display_panel; `shared/protocol/opendisplay_structs.h`
§ "0xAE-0xB1 PANEL DRIVE DATA"). No target links bb_epaper. This directory is where the drive
data for every panel lives, how it is generated, and the artefact hosts consume.

```
panels/
  README.md                 this file (database schema + drive_presets.json contract)
  NOTES_special.md          IT8951 / FastEPD parallel panels: can they be config-driven?
  panels.yaml               hand-authored, ON-GLASS-VERIFIED entries (6 BWRY panels). Frozen.
  panel_ids.json            legacy panel_ic_type id -> bb_epaper enum, scheme, preset ids
  palettes/                 measured BWRY 16-colour palettes (0xB0 source)
  waves/                    the two 535-byte BWRY16 waveforms (0xAF source)
  tools/                    generators (stdlib Python + a host C compiler)
    pdlib.py                encoders, capture/replay drivers, normaliser, deriver
    gen_all.py              capture -> derive -> replay -> generated/*  (--check: staleness)
    build_panel_ids.py      rebuilds panel_ids.json from the sibling repos (git show only)
  generated/
    bbep_streams.json       normalised bb_epaper streams per enum, + drift vs the vendored copy
    drive_presets.json      THE HOST ARTEFACT (schema below)
    coverage.md             per-id status table
  legacy/                   lab scripts imported verbatim (see Provenance)
```

## Provenance

`panels.yaml`, `palettes/*.json` and `legacy/*` were imported from
`newdriver/Firmware_Silabs` branch `feat/od-epd-driver` at `76691a3` (2026-11-06,
"cal: the ODBEEBA6 photo palette is the reference for every BWRY 16-colour panel"), directory
`panels/`. The two waveforms in `waves/` are `COLOR16_WAVE_COLOUR_DEFAULT` /
`COLOR16_WAVE_EDIT_DEFAULT` from `en05_ep397yr_uart/include/color16_waves.h`, read through
`lut_lab.waves.default_waves()`; they are byte-identical to the 0xAF bodies shipped in the
website presets (`opendisplay.org` branch `feat/od-epd-driver`). The PNG evidence files of the
source directory were not imported.

`legacy/` scripts are kept for the lab workflow they belong to (they need py-opendisplay's
`panel_drive`, `lut_lab` and a BLE device). Their absolute paths were replaced by environment
variables: `OD_WORKSPACE` (default: the directory holding this repo), `OD_LAB_HOST`,
`OD_SILABS_SCRIPTS`. `tools/` supersedes `legacy/gen_simple_config_drive.py` for the
presets; `legacy/migrate_drive_config.py` is superseded by the host migrate functions that read
`generated/drive_presets.json`.

## How the data is made

`python3 panels/tools/gen_all.py` (needs a C/C++ compiler and the bb_epaper clone, default
`../bb_epaper` next to this repo or `OD_BBEP_UPSTREAM`; read with `git archive`, never checked out):

1. **capture** -- bb_epaper at `4edfa7f` (the revision `Firmware` origin/main pins; the vendored
   `third_party/` copy `5dccfbb` is captured too and per-panel drift is listed in
   `generated/coverage.md`) is compiled for the host against a recording SPI/GPIO/delay backend
   (`tests/panels/harness/bbep_rec_io.inl`). `tests/panels/harness/bbep_capture.cpp` runs, per
   panel enum, the call sequence `Firmware` `src/display_service.cpp` performs, including its
   overlays (EP397/EP426 RAM alignment, the EP426 partial LUT, split-panel CS handling): full
   refresh, fast refresh (if bb_epaper has a fast init), full-frame partial refresh (if it has a
   partial init and the panel is mono), sleep.
2. **derive** -- each flow is normalised (below) and turned mechanically into od_epd steps.
   Register payloads move into a 0xAF slot streamed with `LUT_SLICE` until the sequence fits
   `OD_EPD_SEQ_MAX`; only payloads AHEAD of the first image marker can move, because od_epd
   resumes after a marker without the pass's slot. Anything the sequence language cannot
   express blocks the mode with the missing primitive named -- nothing is approximated.
3. **replay** -- `tests/panels/harness/od_epd_replay.c` links the REAL `shared/core/od_epd.c`,
   parses the framed packets with the `opendisplay_structs.h` offsets (static-asserted), drives
   `od_epd_select_mode`/`begin_pass`/`next_plane`/`finish_pass` and `od_epd_sleep`, and prints
   the bus stream, which is normalised and compared with step 1.
4. **emit** -- `generated/drive_presets.json`, `generated/bbep_streams.json`,
   `generated/coverage.md`. `gen_all.py --check` exits 1 if any committed output is stale.

`tests/panels/test_panels.py` (host gate step "panel data") re-runs step 3 against the committed
streams, so it needs no bb_epaper checkout; `OD_PANELS_STALE=1` adds the `--check` run.

The six `panels.yaml` entries are NOT re-derived: their packets are encoded from `panels.yaml`
exactly as the website ships them today (asserted byte-for-byte by the tests).

## Normalised stream (what "equivalent" compares)

Both sides are reduced to a token list:

| token | meaning |
|---|---|
| `RESET` | hardware reset pulse (timing not compared; see differences) |
| `BUSY` | wait until BUSY reads idle |
| `DELAY n` | fixed wait of n ms |
| `CMD cc dd..` | command byte with DC low followed by its data bytes, with the chip-select set (`cs0`, `cs1`, `cs01` both) |
| `IMAGE k/n bytes` | frame bytes from the host, slice k of n |
| `PIN p l` | config-referenced pin p set to level l |

Accepted, documented differences (a stream that differs only by these is `equivalent`):

- **D1 reset timing.** bb_epaper pulses RST low 20 ms / high 20 ms; od_epd calls the target's
  `reset(10)`. Pulse shape is a bus-backend property.
- **D2 CS framing.** bb_epaper's `bbepWriteCmdData` keeps CS asserted across command and data;
  od_epd issues `cmd()` then `data()` and the target bus decides the CS framing.
- **D3 coalesced resets.** A reset immediately followed by another reset (bb_epaper `bbepWakeUp`
  followed by an init sequence that starts with `EPD_RESET`) is one reset.
- **D4 pre-reset traffic.** 2.x's cold path runs `pInitFull` inside `bbepInitIO` and then resets
  the controller (`bbepWakeUp`); everything before that reset is discarded by the reset and is
  not reproduced. The reference flow therefore starts at `bbepWakeUp`.
- **D5 BUSY poll cadence.** bb_epaper waits 10+1 ms then polls every 20 ms with a 5/35 s cap;
  od_epd polls every 5 ms up to `busy_timeout_ms`. Only the BUSY position is compared.
- **D6 both-CS transfers.** On the dual-controller Spectra panels bb_epaper sends some commands
  to both controllers at once; od_epd has no both-CS select, so the same transfer goes to cs0
  then cs1. The reference stream is expanded the same way before comparing.
- **D7 Spectra nibbles.** 2.x `split_panel.cpp` clamps pixel nibbles 4 and 7-15 to 0 before
  writing; od_epd streams host bytes untouched, so the host must send only valid Spectra codes.

The leading `RESET BUSY` of every mode flow is dropped on both sides: `od_epd_begin_pass`
supplies it and bb_epaper's `bbepWakeUp` does the same thing.

## `generated/drive_presets.json` -- schema v1 (STABLE CONTRACT)

Consumers: the website's auto-migration (2.x config -> 3.0 config on connect) and
py-opendisplay's migrate function/CLI. Additive changes bump `schema_minor`; anything that
removes or changes the meaning of a field bumps `schema_version`.

```jsonc
{
  "schema": "opendisplay.drive_presets",
  "schema_version": 1,
  "schema_minor": 0,
  "sources": {
    "bb_epaper": "4edfa7f",            // revision the streams were captured from
    "bb_epaper_vendored": "5dccfbb",   // third_party/ copy (drift listed in coverage.md)
    "panel_ids": {"Firmware": "…", "opendisplay.org_2": "…", "Firmware_Silabs": "…"},
    "panels_yaml_sha256": "…"
  },
  "caps": {                             // config byte ceilings (whole config blob) + od_epd limits
    "bg22": 3120, "esp32": 4096, "nordic": 4096,
    "typical_base_config_bytes": 417,   // legacy-only config used for the fits check
    "seq_max": 128, "max_steps": 64, "sleep_max": 16, "lut_max": 535
  },
  "names": {"EP29YR_168x384": "0x001E"},// name -> key (aliases excluded)
  "panels": {
    "0x001E": {                         // key: legacy panel_ic_type, "0x%04X"; all 77 ids present
      "panel_ic_type": 30,              // the SAME number, as an int. Keep writing it into 0x20.
      "name": "EP29YR_168x384",         // stable panel name (bb_epaper enum, or the panels.yaml name)
      "aliases": [],                    // other keys that mean this panel (0x003A <- 0x03EC)
      "alias_of": "0x003A",             // only on an alias; its drive data is the target's copy
      "legacy": {                       // what a 2.x / old client must see in 0x20
        "panel_ic_type": 30,
        "color_scheme": 3,              // old clients pick colour codes by panel_ic_type+scheme
        "pixel_width": 168, "pixel_height": 384,   // website preset geometry (null if no preset)
        "presets": ["…"]                // website preset ids using this panel_ic_type
      },
      "width": 168, "height": 384,      // bb_epaper native size
      "color_scheme": 3,                // native ColorScheme of mode 0
      "controller": "UC81xx",           // "UC81xx" | "SSD16xx"
      "bbep_flags": ["4COLOR"],         // bb_epaper flag names, informational
      "status": "glass",                // "glass" | "equivalent" | "differs" | "blocked" | "not_attempted"
      "verified": "glass",              // "glass" | "equivalent" | "untested"
      "blocked_on": [],                 // primitive ids (see "Primitives"); non-empty iff status=blocked
      "blocked_detail": {"fast": ["SEQ_MAX"]},     // per flow (full/fast/partial/sleep), why it was dropped
      "requires_escapes": [],           // "SET_PIN", "CS_SELECT": the target io must implement them
      "modes": [                        // shipped modes, in mode_index order; mode 0 is the default
        {"index": 0, "name": "full", "refresh": "full", "passes": 1, "color_scheme": 3, "planes": 2}
      ],                                // planes = image markers per pass (partial: old + new)
      "modes_blocked": [                // 2.x flows NOT shipped, with why and what would make them fit
        {"name": "fast", "blocked_on": ["SEQ_MAX"], "seq_bytes": 140, "steps": 39,
         "fits_with": {"seq_max": 144, "steps": 39}}
      ],
      "packets": [                      // ready to append AFTER every legacy packet, in this order
        {"id": 177, "number": 0, "body_hex": "…"},   // 0xB1, then 0xAE…, 0xAF…, 0xB0…
        {"id": 174, "number": 0, "body_hex": "…"}
      ],
      "framed_hex": "…",                // concatenation of [number][id][len:2 LE][body] for all packets
      "drive_bytes": 0,                 // len(framed_hex)/2
      "fits": {"bg22": true, "esp32": true, "nordic": true},  // typical_base + drive_bytes <= cap
      "equivalence": {                  // replay verdicts; per mode index and for sleep
        "modes": {"0": {"rc": 0, "equal": true}},
        "sleep": {"rc": 0, "equal": true},             // null when sleep is blocked
        "all_equal": true               // on mismatch an entry adds first_diff/ref/got tokens
      },
      "differences": ["D1", "D2", "D3", "D4", "D5"],  // accepted differences that apply
      "notes": []                       // doubts and caveats, plain text
    }
  }
}
```

Field rules a consumer may rely on:

- `packets[].id` is the packet-type id as an int (174 0xAE, 175 0xAF, 176 0xB0, 177 0xB1). `body_hex` is the BODY only (no
  4-byte `SinglePacketHeaderEx`). `number` is what this generator puts in the header's first
  byte (instance for 0xB1, mode_index for 0xAE, lut_slot for 0xAF, 0xB0 for 0xB0 -- the website
  bytes for the glass entries); readers ignore it, so a consumer may renumber.
- All packets are for display instance 0. For instance N, rewrite `body[0]` of every 0xB1, 0xAE,
  0xAF and 0xB0 body to N. 0xAF slot ids are global on the device (looked up by slot alone), so a
  second display must also renumber its slots (0xAF `body[1]` and the 0xAE `lut_slot[]` bytes at
  body offsets 19..26) to avoid colliding with the first display's.
- A migrating client removes any existing 0xAE/0xAF/0xB0/0xB1 packets for that instance (and the
  burned ids 0x2E-0x31) before appending these.
- `status`: `glass` = the panels.yaml data seen working on a real panel (hand-written, NOT the
  bb_epaper stream; `equivalence` is recorded for information and differs); `equivalent` = every
  shipped mode and the sleep sequence replay through od_epd to the same normalised stream as the
  2.x flow (host-proven, never seen on glass); `differs` = shipped but a replay mismatches;
  `blocked` = the full flow or sleep cannot be expressed -- `packets: []`, write nothing, the
  device then reports the panel undrivable rather than driving it wrongly; `not_attempted` = no
  bb_epaper enum to capture.
- `verified` is `glass`, `equivalent` (same meaning as the status) or `untested`.
- A panel with `requires_escapes` is equivalent on the host but only drives on a target whose io
  implements those escapes (`od_epd_cfg` currently leaves `set_pin`/`cs_select` NULL, so od_epd
  refuses them with `RC_UNSUP` at mode select). They are not blockers: the data is complete.
- Mode names are at most 11 ASCII characters. `refresh` is `full`, `fast` or `partial` (glass
  multi-pass modes report `full` with `passes` > 1). For a CMD_DRIVE_WRITE_START (0x0085) a host
  picks `mode` = `index`. A partial mode here is FULL-FRAME: the frame is old plane + new plane
  (`planes: 2`, IMAGE_SLICE 0/2 and 1/2); windowed partial needs `WINDOW`.
- `fits` assumes `caps.typical_base_config_bytes`; a consumer with the real config should check
  `len(config) + drive_bytes <= cap` itself.

## Primitives (`blocked_on` / "Needed from others" ids)

| id | missing in od_epd / od_epd_cfg |
|---|---|
| `SEQ_MAX` | a mode sequence longer than `OD_EPD_SEQ_MAX` (128 B) or 64 steps even after moving payloads into a slot; `modes_blocked[].fits_with` gives the cap that would fit |
| `LUT_AFTER_IMAGE` | the sequence only fits if a payload AFTER an image marker is streamed from the slot; od_epd's `next_plane`/`finish_pass` resume without the pass's slot (`RC_LUT`) |
| `LUT_MAX` | the moved payloads exceed one 535-byte slot |
| `LUT_SLOTS` | more than 8 distinct slots for one panel |
| `SLEEP_MAX` | sleep sequence longer than `OD_EPD_SLEEP_SEQ_MAX` (16 B); no current panel hits it |
| `CS_BOTH` | a transfer that must reach both controllers simultaneously (none in the 2.x flows: D6 covers them) |
| `CS_EVERY_BYTE` | CS toggled around every byte (bb_epaper flag); no legacy id uses it |
| `DATA_WITHOUT_CMD` | a data burst not preceded by a command on the same CS |
| `IT8951` / `PARALLEL` | not SPI command-stream panels (see NOTES_special.md); none has a legacy id |
| `NO_FULL_FLOW` / `NO_BBEP_ENUM` | nothing to derive from |

Escapes a target must implement (`requires_escapes`, not blockers): `SET_PIN` with
`OD_SEQ_PINREF_RESET` (a mid-sequence hardware reset, emitted as pin 0 / 20 ms / pin 1 / 20 ms)
and `CS_SELECT` (dual-controller panels).

Not expressible and therefore only noted, never silently approximated: `PIXEL_XFORM`
(firmware-side repacking, e.g. the SSD2677 2bpp repack), `READBACK` (runtime controller detection
by register read, 2.x EP397 SSD1677/SSD2677), `WINDOW` (per-update RAM window coordinates).
