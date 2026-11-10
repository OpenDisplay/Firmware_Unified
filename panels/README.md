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
  generated/
    bbep_streams.json       command streams captured from bb_epaper on the host
    drive_db.json           per-panel derived drive data (modes as step lists)
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

1. `tools/capture_bbep.py` compiles bb_epaper for the host against a recording
   SPI/GPIO/delay backend (`tests/panels/harness/bbep_rec_io.inl`) and runs, for every panel
   enum, the call sequence the 2.x firmware (`Firmware` `src/display_service.cpp`) performs:
   full refresh, fast refresh (if bb_epaper has a fast init), full-frame partial refresh (if it
   has a partial init), sleep. Output: `generated/bbep_streams.json`.
2. `tools/derive.py` turns each captured stream mechanically into od_epd sequences (one per
   mode), moving large register payloads (LUTs) into a 0xAF slot streamed with `LUT_SLICE`.
   Where the stream needs something the sequence language cannot express, the panel is marked
   `blocked` with the missing primitive named -- nothing is approximated.
3. `tools/equivalence.py` runs the REAL `shared/core/od_epd.c`, host-compiled, over the encoded
   packets against the same recording mock, and diffs the two normalised streams.
4. `tools/gen_presets.py` writes `generated/drive_presets.json` and `generated/coverage.md`.
   `tools/gen_all.py` runs 1-4; `tools/gen_all.py --check` fails if anything committed is stale.

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

## `generated/drive_presets.json` -- schema v1 (STABLE CONTRACT)

Consumers: the website's auto-migration (2.x config -> 3.0 config on connect) and
py-opendisplay's migrate function/CLI. Additive changes bump `schema_minor`; anything that
removes or changes the meaning of a field bumps `schema_version`.

```jsonc
{
  "schema": "opendisplay.drive_presets",
  "schema_version": 1,
  "schema_minor": 0,
  "generator": "panels/tools/gen_presets.py",
  "sources": {
    "bb_epaper": "4edfa7f",            // revision the streams were captured from
    "panels_yaml_sha256": "…",
    "structs": "opendisplay_structs.h 2.x"
  },
  "caps": {                             // config byte ceilings (whole config blob)
    "bg22": 3120, "esp32": 4096, "nordic": 4096,
    "typical_base_config_bytes": 0      // legacy-only config used for the fits check
  },
  "panels": {
    "0x001E": {                         // key: legacy panel_ic_type, "0x%04X"
      "panel_ic_type": 30,              // the SAME number, as an int. Keep writing it into 0x20.
      "name": "EP29YR_168x384",         // stable panel name (= bb_epaper enum name)
      "aliases": [],                    // other panel_ic_type values that mean this panel (e.g. 1004 -> 0x003A)
      "legacy": {                       // what a 2.x / old client must see in 0x20
        "panel_ic_type": 30,
        "color_scheme": 3               // ColorScheme for 0x20; old clients pick colour codes by panel_ic_type+scheme
      },
      "width": 168, "height": 384,      // native controller orientation, pixels
      "color_scheme": 3,                // ColorScheme the panel renders natively (0x20 value)
      "controller": "UC81xx",           // "UC81xx" | "SSD16xx" (bb_epaper chip_type family)
      "bbep_flags": ["4COLOR"],         // bb_epaper flag names, informational
      "verified": "glass",              // "glass" | "equivalent" | "untested"
      "status": "ok",                   // "ok" | "blocked"
      "blocked_on": [],                 // primitive ids (see "Primitives") when status=blocked
      "requires_escapes": [],           // escapes a target must implement, e.g. ["SET_PIN"]
      "modes": [                        // in mode_index order; mode 0 is the default
        {"index": 0, "name": "full",  "refresh": "full",    "passes": 1, "color_scheme": 255},
        {"index": 1, "name": "fast",  "refresh": "fast",    "passes": 1, "color_scheme": 255},
        {"index": 2, "name": "partial","refresh": "partial","passes": 1, "color_scheme": 255,
         "planes": 2}                   // partial: frame = old plane + new plane (IMAGE_SLICE 0/2, 1/2)
      ],
      "packets": [                      // ready to append AFTER every legacy packet, in this order
        {"id": 177, "number": 0, "body_hex": "…"},   // 0xB1 first, then 0xAE…, 0xAF…, 0xB0…
        {"id": 174, "number": 0, "body_hex": "…"}
      ],
      "framed_hex": "…",                // concatenation of [number][id][len:2 LE][body] for all packets
      "drive_bytes": 0,                 // len(framed_hex)/2
      "fits": {"bg22": true, "esp32": true, "nordic": true},  // typical_base + drive_bytes <= cap
      "equivalence": {"full": "match", "fast": "match", "sleep": "match"},
      "differences": ["D1", "D2"],      // accepted differences that apply (README § Normalised stream)
      "notes": []
    }
  },
  "names": {"EP29YR_168x384": "0x001E"} // name -> key
}
```

Field rules a consumer may rely on:

- `packets[].id` is the packet-type id (174 0xAE, 175 0xAF, 176 0xB0, 177 0xB1). `body_hex` is
  the BODY only (no 4-byte `SinglePacketHeaderEx`). `number` is the value this generator puts in
  the header's first byte (instance for 0xB1, mode_index for 0xAE, lut_slot for 0xAF, mode_index
  for 0xB0); readers ignore it, so a consumer may renumber.
- All packets are for display instance 0. For instance N, rewrite `body[0]` of every 0xB1, 0xAE,
  0xAF and 0xB0 body to N. 0xAF slot ids are global on the device (looked up by slot alone), so a
  second display must also renumber its slots (0xAF `body[1]` and the 0xAE `lut_slot[]` bytes at
  body offsets 19..26) to avoid colliding with the first display's.
- A migrating client removes any existing 0xAE/0xAF/0xB0/0xB1 packets for that instance (and the
  burned ids 0x2E-0x31) before appending these.
- `status: "blocked"` entries carry `packets: []` -- writing nothing is the honest outcome; the
  device will report the panel undrivable rather than drive it wrongly.
- `verified`: `glass` = this exact data was seen working on a real panel; `equivalent` = the od_epd
  interpreter emits the same normalised stream bb_epaper emits for the 2.x flow (host-proven, not
  seen on glass); `untested` = generated but not proven equivalent (see `equivalence`/`notes`).
- Mode names are at most 11 ASCII characters. `refresh` is one of `full`, `fast`, `partial`,
  `multipass`. For a CMD_DRIVE_WRITE_START (0x0085) a host picks `mode` = `index`.
- `fits` assumes `caps.typical_base_config_bytes`; a consumer with the real config should check
  `len(config) + drive_bytes <= cap` itself.

## Primitives (`blocked_on` / "Needed from others" ids)

| id | missing in od_epd / od_epd_cfg |
|---|---|
| `SEQ_MAX` | a mode sequence longer than `OD_EPD_SEQ_MAX` (128 B) or 64 steps |
| `SLEEP_MAX` | sleep sequence longer than `OD_EPD_SLEEP_SEQ_MAX` (16 B) |
| `SET_PIN_RESET` | a mid-sequence hardware reset: needs `SET_PIN` with `OD_SEQ_PINREF_RESET` implemented by the target io (`od_epd_cfg` leaves `set_pin` NULL) |
| `CS_BOTH` | one transfer to both controllers at once (`CS_SELECT` accepts only 0/1) |
| `PIXEL_XFORM` | firmware-side per-pixel repacking/clamping of host bytes (SSD2683 1->2 bpp, 4BPP_DATA, Spectra nibble clamp) |
| `READBACK` | runtime controller detection by register read-back (SSD2677 sticky variant) |
| `WINDOW` | windowed partial refresh: RAM window coordinates substituted per update |
| `IT8951` | command protocol with HRDY handshake (see NOTES_special.md) |
| `PARALLEL` | parallel-bus panel driven by FastEPD (see NOTES_special.md) |
