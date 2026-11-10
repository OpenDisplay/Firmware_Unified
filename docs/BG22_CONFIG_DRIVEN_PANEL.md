# BG22: the panel is driven from config now — a config update is REQUIRED

`targets/efr32bg22-slc` no longer builds bb_epaper. There is no compiled-in panel table: the
init sequence, waveform, pass count, BUSY policy and colour code map all arrive in the device
config as the Rev 2 drive packets.

| packet | carries |
|---|---|
| `0xAE` display_mode | one drive mode: name, colour scheme, passes, waveform slots, temperature window, drive sequence |
| `0xAF` display_lut | one waveform slot at its real length |
| `0xB0` display_calibration | measured CIELAB + per-pass codes (host-side today) |
| `0xB1` display_panel | BUSY trust and sense, colour code map, sleep sequence |

## What happens to a device without them

**It cannot drive its panel at all.** It boots, advertises, connects, accepts a config write
and accepts an upload — and the panel stays blank. There is nothing to fall back to.

The firmware refuses rather than pretending:

* `od_xfer_app_begin_full()` returns false when no `0xB1` is indexed, so the upload **fails**
  instead of completing against a panel that never changes;
* `opendisplay_display_boot_apply()` skips the boot screen for the same reason — without
  latching, so the next config write that brings drive packets draws it (after that write's
  ACK, from the superloop) with no power cycle.

That is the honest behaviour, but it does mean a unit is dark between the OTA and the config
write. Order matters:

```
1. read the existing config off the device and KEEP IT
2. OTA                      # no panel from here until step 3
3. write the config WITH the drive packets
4. read back; confirm 0xB1 and at least one 0xAE are present
5. upload one image and LOOK AT THE PANEL
```

Generate the packets with `newdriver/Firmware_Silabs/panels/migrate_drive_config.py`, which
emits them from `panels.yaml`. A 2.6" config with two modes and two waveforms is ~1.7 KB
against this target's 3120-byte cap (`OD_CONFIG_MAX_SIZE`, raised from 2048 in `6d5f262`; a
16-colour 2.9" config measures 2112 B).

## Why bb_epaper had to go rather than stay as a fallback

It did not fit. The BG22 application slot is 0x44000, and the NVM3 region lives inside it, so
the real ceiling for application text is **253952 bytes**. Before this change the image was
252512 — **1440 bytes of headroom**, not the 26016 the build script reports (it subtracts the
app image from the slot without accounting for `.nvm`; see the note below). Adding od_epd's
5256 bytes overflowed by ~4 KB.

Dropping bb_epaper freed 14760 bytes, leaving 237752 B and ~16 KB of headroom at that commit.
**Later features have spent most of it:** at `6df2d6e` the image is 247,104 B against the
253,952 B ceiling, which leaves **6,848 B**. Porting FMDN (network G) costs about another 1.9 KB.

**The build script's "Free in application FLASH region" is overstated by 24576 bytes** on this
target. It is the number you would reach for before adding a feature, and it will tell you
there is 26 KB free when there is 1.4 KB. Treat `253952 - <image size>` as the real figure
until the script is fixed.

## Scope

**esp32-idf and nordic-zephyr still use bb_epaper and are unaffected.** Neither has an
`od_hal`-style IO backend for od_epd, so removing it there would leave them with no panel
driver at all. `third_party/bb_epaper` stays vendored for them.

Driving a panel from config on those targets needs an od_epd IO implementation first — the
BG22 one is `targets/efr32bg22-slc/od_epd_io_silabs.c`, ~315 lines over `em_gpio`/`em_cmu`.
