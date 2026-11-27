# panels/nrf52811/: drive presets for nRF52811 devices (Firmware_NRF)

The source of truth for the nRF52811 presets. Firmware_NRF holds no copy: its host tests and its
README read `panels/generated/drive_presets_nrf52811.json` from here.

```
presets.yaml      per preset: id, name, size, colour scheme, BUSY idle level, status/verified,
                  notes, the 2.x id (legacy), `palette` (0xB0 source), and the drive steps:
                  `init` (mode 0 "full") and `sleep`, in the panels.yaml step strings
provenance.json   frozen evidence from the derivation, copied into the output unchanged:
                  `deviations` (where a preset deliberately leaves its 2.x source) and
                  `alternatives` (the uncorrected packets, with their equivalence result)
```

Regenerate with `python3 panels/tools/gen_nrf52811.py` (`--check` reports a stale output;
`tests/panels/test_panels.py` runs it). Each preset is encoded as 0xB1 + 0xAE (+ 0xB0), replayed
through `shared/core/od_epd.c` built without WINDOW/FILL (as Firmware_NRF builds it), and the
replayed bus must equal the steps, for the mode and for sleep.

## What these presets are

- **Ids are provisional** (0xE0xx from the 2.x C drivers, 0xE1xx from op lists): the 2.x ids
  1000..1030 collide with registry ids.
- **Only presets seen right on glass are here** (`status: glass`, `verified` says where).
  To add one, write its steps here and check it on a real panel with a Firmware_NRF build.
- **Config cap**: 2000 B (Firmware_NRF `OD_CONFIG_MAX_SIZE`). `fits_target` assumes 600 B of
  other config.
- **0xB0**: `palette: {fleet: <file in ../palettes>, unit: <device>}` gives mode 0 that unit's
  i1Pro stock colours (`lab_rel_white`), a type default (cal_flags bit0 = 0). Colours are matched
  by name and coded as the two-plane panel code: plane 0 = bit 0, plane 1 = bit 1, so black 0,
  white 1, red or yellow 3 (the swatch charts were sent with the colour setting both planes). A
  unit marked `use: exclude` is refused (0xE01B has none: its unit drew the chart faded).

## History

Exported 2026-11-27 from Firmware_NRF `v3/nrf52811` at `87e6808`, where `tools/gen_presets.py`
derived them: the 2.x drivers compiled against a recording SPI mock, the bus stream turned into
od_epd steps, the documented transforms T1-T6 and the deviations applied, and the packets
replayed against the 2.x stream. That derivation stays in Firmware_NRF's history; it is not
needed to change a preset.
