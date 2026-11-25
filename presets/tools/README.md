# presets/tools -- waveform table tools

Offline Python tools for the waveform tables a 0xAF `display_lut` slot carries: decode, encode,
compute the charge a table leaves on the ink, and lint it before it reaches a panel.

Standard library only, Python 3.10+. Nothing here talks to hardware.

```
presets/tools/
  lutlint.py          command line: lint | charge | show | seq
  odlut/
    s6_codec.py       800 B Spectra 6 0x20 image <-> rows / segment lists (exact byte round trip)
    s6_lutc.py        the LUTC (VCOM) block: fields, timeline, table builders
    s6_charge.py      per-slot charge report, the charge gate, a cumulative ledger
    s6_lint.py        table rules L0-L8, upload-ordering rule
    bwry535.py        535 B BWRY register LUT codec
    bwry_lzss.py      decompressor for the BWRY controllers' compressed waveform store
  tests/test_odlut.py
```

## Use

```
python3 presets/tools/lutlint.py lint table.bin --panel 7.3 --mode custom --pwr "3F 00 32 2A 0E 2A"
python3 presets/tools/lutlint.py charge table.bin --panel 7.09 --pwr "0F 00 28 2C 28 38"
python3 presets/tools/lutlint.py show panels/waves/wave_colour.bin
```

Exit 0 is a pass, 1 a fail. `--json` gives the full report. From Python:

```python
from odlut import s6_lint
r = s6_lint.lint_table(table_bytes, panel="7.3", mode="custom", pwr="3F 00 32 2A 0E 2A")
if not r.ok: print(r.text())
```

`mode` is how the table will run. `custom` is a register table (LUT_EN set): the weak rails come
from the PWR bytes the firmware writes, so they must be given. `stock` is a copy of a built-in
table: the rails come from the table header.

## Rules

| Rule | Fails when |
|---|---|
| L0 length | a block is longer than 20000 frames (corrupt or mis-decoded; the charge rules are skipped) |
| L1 longest block | `--busy-timeout` is shorter than the predicted refresh. Warns below max(90 s, 1.5 x refresh), and when an unused slot is the longest block |
| L2 equal lengths | stock only: the non-empty blocks differ in length |
| L3 DC | a used or border slot leaves more than 1.0 S-frame of net charge (stock: 1.5), in nominal volts; or the LUTC VCOM swing does not balance |
| L4 end frame | custom only: a used or border slot ends on a driven level |
| L5 PWR | custom only: no PWR bytes, or not 6 of them |
| L6 border | custom only: the slot CDI selects for the border is empty |
| L7 hang | 8 zero bytes in a row, or an all-zero header |
| L8 byte gate | opt-in (`--forbid`): the table holds a byte value the upload path refuses |
| S1 order | a reset or temperature read sits between a table upload and its refresh |

## Limits

- The electrical constants in `s6_charge.PANELS` (rail voltages, frame times, VCOM behaviour) were
  measured on one unit each of the 7.3" and the 7.09" Spectra 6 panel. Unit-to-unit spread is not
  characterised.
- The rules are lab rules derived on those units. Passing the lint is a necessary check, not
  proof that a table is safe for a panel over its lifetime.
- L4 is under review: whether a zero end frame is required, or power-off right after BUSY is
  enough, is not settled.
- Which reference is DC-neutral for the ink (0 V or VCOM) is open. The gate uses 0 V and reports
  the VCOM-referenced charge next to it.
- `bwry535` decodes structure only. The electrical meaning of the BWRY level bits is not modelled
  here, so there is no BWRY charge rule yet.

## Tests

```
python3 -I presets/tools/tests/test_odlut.py
```

`tools/check.sh` runs it as "presets tools: waveform table lint".
