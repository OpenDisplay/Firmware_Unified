# targets/telink-tlsr

OpenDisplay firmware for **Telink TLSR825x** e-paper tags: the chip family
[ATC_BLE_OEPL](https://atc1441.github.io/ATC_BLE_OEPL_Image_Upload.html) runs on (Hanshow/Solum-style
ESLs, TLSR8258/8359). Bare metal on Telink's `tc_ble_single_sdk`: no RTOS, no Kconfig, one
superloop, like `efr32bg22-slc`.

> **Status: runs on one tag.** A Hanshow 2.66" BWR ESL (ATC type 9) installs it over ATC's OTA,
> takes a config, draws the boot screen, shows an uploaded image correctly and updates itself over
> BLE. Open: encrypted sessions untested on silicon, no other panel verified.
> `docs/HARDWARE_VERIFICATION_CHECKLIST.md` § `telink-tlsr` has the rows.

## Build

```bash
targets/telink-tlsr/build.sh          # -> build/telink-tlsr/opendisplay_tlsr.bin
```

`tools/fetch_deps.sh` (run by `build.sh`) fetches two pinned dependencies into
`build/deps/telink-tlsr/` (git-ignored, and outside `targets/` so `tools/check.sh`'s ratchets never
scan Telink's tree): Telink's [`tc_ble_single_sdk`](https://github.com/telink-semi/tc_ble_single_sdk)
(Apache-2.0) and the tc32 toolchain from [`flyskywhy/tc32`](https://github.com/flyskywhy/tc32)
(`macos` / `linux` branch). The macOS toolchain is an x86-64 binary and runs under Rosetta on Apple
Silicon.

The post-build step fails the build unless the `.bin` has `KNLT` at offset 8 and its length at
offset 24, which is what both Telink OTA and ATC's `sendFw()` check before accepting an image.

## What works and what does not

| Area | State |
|---|---|
| BLE peripheral, GATT service/characteristic `0x2446` (write, write-no-rsp, notify) | implemented |
| 247-byte ATT MTU, 251-octet DLE | configured; slave requests DLE if the central has not after 1 s |
| Advertising: flags + 16-byte OD MSD, scan response `ODxxxxxx` + UUID `0x2446` | implemented; battery and temperature filled as below, 0 V / -40 C until first measured |
| Shared dispatch, RX ring, TX queue, config read/write/chunk/clear, session auth + CCM | implemented over shared/ |
| Identity strings | DataExtended (0x2C) parsed and stored; manufacturer and model head the boot screen on panels of 400x300 and up |
| Config storage | two 4 KB sectors at `0x7A000` (see `od_hal_tlsr.c` for the flash map) |
| Crypto | TLSR825x AES engine + `od_aes_modes.c` (CMAC, CCM); engine byte order found by a FIPS-197 known-answer test at first use |
| Randomness | AES-CTR generator seeded from the SDK's analog-noise `rand()`; the noise source is unmeasured |
| Display transfer (full image, direct and compressed) | Firmware_NRF52's UC81xx / SSD16xx / JD796xx drivers over bit-banged SPI; see "Panels" below |
| Partial refresh | not offered (`partial_enabled = false`) |
| Boot screen | shared `od_boot_screen`, drawn once per boot from the main loop; skipped when the config sets `CLEAR_ON_BOOT` or the previous run ended in a watchdog reset. Outcome in MSD byte 4 (`0xb2` drawn, `0xe1..0xe5` failing hook, `0xef` refused before any hook) |
| DFU (`0x0051`) | arms the Telink OTA service for the current connection; see "Flashing" |
| LED (`0x0073` / `0x0074`) | shared pattern runner (`od_led.c`) through `od_led_tlsr.c`; LED pins are outputs only while a pattern runs, and PA7 (blue on ATC boards, also SWS) goes back to SWS after every run. A short green blink at boot. ATC boards' LEDs are active-low: `led_flags` 0x7 |
| Buzzer, power-off, deep sleep | NACKed |
| Battery voltage | SAR ADC on `power_option.battery_sense_pin` (PB0-7, PC4, PC5; ATC tags use PB3), Telink's drive-high-and-measure method: once after the config loads, then every 60 s while idle and disconnected; the advert is republished when the 10 mV value changes |
| Temperature | the panel controller's own sensor, read at the start of every refresh (SSD16xx: temperature-sensor read after a `0xB1` load update; UC81xx: TSC); readings outside -30..70 C or exactly 0 are discarded. The TLSR825x has no usable die sensor |
| Watchdog | 4 s, fed from the main loop and while the stack is serviced |
| Logging | not wired |
| Power | suspend between radio events; no deep-retention sleep (shared/ state is not in retention RAM) |

## Layout and the one rule it adds

Two compile domains, deliberately. The prebuilt BLE library was built with
`-fpack-struct -fshort-enums`, so everything that includes an SDK header is too (`main.c`,
`app*.c`, `tlsr_port.c`). `shared/` and this target's `od_*.c` are not. `src/tlsr_port.h` is the
only header both sides include, and it carries no struct and no enum, because either would change
layout across the boundary without a diagnostic.

| File | Side | Role |
|---|---|---|
| `main.c`, `app.c`, `app_att.c` | SDK | reset entry, BLE bring-up, GATT table, link events |
| `tlsr_port.c` | SDK | timer, flash, AES engine, noise, reboot |
| `od_tlsr_app.c` | OD | config lifecycle, MSD, the RX-drain pump, session/rxq app seams |
| `od_cmd_tlsr.c` | OD | command hooks (BG22's config and auth policy) |
| `od_hal_tlsr.c` | OD | `od_hal_time`, `od_hal_nvs`, `od_hal_radio` |
| `od_hal_crypto.c`, `od_aes_modes.c` | OD | crypto HAL; modes are host-tested in `tests/host/tlsr_aes_modes_test.c` |
| `od_xfer_tlsr.c` | OD | od_xfer image hooks over the panel drivers; NFC and inflate seams |
| `epd/UC81xx.c`, `epd/SSD16xx.c`, `epd/epd_models.c` | OD | panel drivers and model table, imported unchanged from Firmware_NRF52 `EPD/` at `71b870c1` |
| `epd/EPD_driver.h` | OD | same import; only the nRF include block, `EPD_DEBUG` and the Arduino wrappers changed |
| `epd/epd_io.c`, `epd/epd_port.h` | OD | the `EPD_*` primitives: bit-banged mode-0 SPI (3-wire read), reset, busy wait |
| `od_tlsr_rt.c`, `od_tlsr_fmt.c`, `tc32_compat.h` | OD | toolchain gaps, below (`od_tlsr_fmt.c`: snprintf without FP helpers, host-tested against libc) |

## Panels

The display config's `panel_ic_type` must be in canonical PanelIC 1000-1030, the Firmware_NRF52
model line (e.g. 1022 = `SSD1619_026_BWR`), or one of this target's Hanshow additions below.
Raw 1-31, which that firmware also accepted, is refused: canonically 1-51 are bb_epaper panels.
The config's `pixel_width`/`pixel_height` and `color_scheme` must match that model:
the table fixes the controller's RAM layout, so a mismatch is refused at transfer start rather
than streamed into the wrong layout. Schemes: MONO for BW models, BWR or BWY for the two-plane
models, BWRY for the JD796xx ones.

Hanshow glass found on ATC tags, all provisional numbers until opendisplay-protocol assigns them:

| PanelIC | Model | Native size | ATC type | Held | `rotation` |
|---|---|---|---|---|---|
| 1031 | `SSD16XX_HS_266_BWR` | 152 x 296, 8-pixel source offset | 9, "266 HS BWR SSD" | landscape | 1 (90) |
| 1032 | `SSD16XX_HS_200_BWY` | 200 x 152 | 5, "200 HS BWY SSD" | portrait | 3 (270) |
| 1033 | `UC8151_HS_350_BWY` | 184 x 384 | 1, "350 HS BWY UC" (Nebular 350Y-N) | landscape | 1 (90) |
| 1034 | `TI_970_BWR` | 960 x 672, two controllers | 14, "970 TI BWR" (TC097SC1B8) | landscape | 2 (180) |

Native size is sources x gates. ATC reports the 2.0" BWY glass as 152x200; driven that way only the
152x152 overlap reached the glass. The two SSD glasses scan gates upward and write the B/W RAM
uninverted; the UC glass runs on the imported UC8151 settings unchanged.

The 9.7" (`epd/TI97xx.c`, ATC's TI sequence transcribed) is split over two controllers: each
120-byte row goes 60 bytes to `cs_pin` (left) and 60 to `DisplayConfig.cs_pin_2` (right; JSON
`reserved_pin_2`), and its second supply switch is `SystemConfig.pwr_pin_2`. Both are read only for
this model -- `cs_pin_2` is a zero byte in older configs and `pwr_pin_2` is otherwise the
power-latch pin. Panel parameters come from the master's OTP at every start; only the master's
busy line is waited on, as ATC does.
`rotation` makes the tag's natural face upright: hosts add it to the requested rotation and the
boot screen is drawn with it, so images need no `--rotate`.

Pins are `DisplayConfig`'s `data_pin`, `clk_pin`, `cs_pin`, `dc_pin`, `reset_pin`, `busy_pin`,
plus `SystemConfig.pwr_pin` for a panel power switch, numbered as in `tlsr_port.h`
(`port * 8 + pin`: PA0 = 0, PB4 = 12, PD7 = 31).

**`pwr_pin` is active-low on this target.** The config has no polarity field, and every ATC board
read so far switches the panel supply through an active-low enable (PC5), so the firmware asserts
the pin LOW while driving the panel and holds it HIGH otherwise -- including from boot, as soon as
a config is loaded, so the supply never floats. Other OpenDisplay firmwares drive `pwr_pin` HIGH;
the host cannot tell which convention a device uses. A board with an active-high enable needs a
build with `-DOD_TLSR_PWR_ACTIVE_LOW=0`. `atc-ble od-config` (py-atc-ble-oepl) only emits
`pwr_pin` for tags whose ATC config reports the enable as inverted.

The image bytes go to the controller unchanged, exactly as Firmware_NRF52 wrote them, and a
refresh (up to ~65 s busy) keeps the BLE stack running while it waits. Before the refresh the END
acknowledgement is flushed for up to 2 s; if it has not left by then the refresh proceeds anyway,
as `od_txq.h` specifies (BG22 aborts instead).

`tests/host/tlsr_display_test.c` runs this whole path -- hooks, drivers, SPI bit-banging -- on
the host against a fake port that decodes the wire back into command/data bytes, and checks every
image byte lands after the right RAM command, once, followed by refresh and sleep.

## Flashing

- **Update a tag already running this firmware, wireless:**
  `tools/ble_ota.py --device ODxxxxxx build/telink-tlsr/opendisplay_tlsr.bin`. It sends OpenDisplay's
  ENTER_DFU (`0x0051`), which arms the Telink OTA service for that connection only -- without it the
  service ignores every write, so nobody in range can reflash the tag -- then streams the image with
  Telink's legacy OTA protocol. The new image goes to the other 192 KB bank (`0x0` / `0x40000`); the
  tag checks its CRC32 and reboots into it only if it passes, so a failed or interrupted update leaves
  the old firmware running. Images must be 4 mod 16 bytes long (body padded to 16, plus the CRC):
  the OTA server aborts on any other length, and `finish_image.py` guarantees it. Keyed (encrypted)
  tags are not supported by the tool yet.
- **From ATC_BLE_OEPL, wireless:** `uv run --with py-atc-ble-oepl tools/atc_install.py --device ADDRESS
  build/telink-tlsr/opendisplay_tlsr.bin`, or ATC's web uploader → "Select Firmware". One way: ATC
  copies the image over itself.
- **Over SWS, wired:** `tools/sws_flash.py --port /dev/cu.usbserial-XXXX build/telink-tlsr/opendisplay_tlsr.bin`.
  USB-serial TX -> 1..1.8 kOhm -> SWS (PA7, also the blue LED line), GND, 3.3 V logic. Write-only:
  no RX needed, nothing is verified over the wire. Works on any firmware state.
  Going back to a firmware that lives at 0x0 (ATC) on a tag updated over the air: add
  `--invalidate-bank2`, which erases the header of the image at 0x40000 so only the one just
  written can boot.
  To power-cycle a tag powered from the adapter, unplug the adapter: removing only its 3.3 V wire
  leaves the chip half-powered through the idle-high TX line and the SWS pin.

Then give the tag its config (`atc-ble od-config` in py-atc-ble-oepl, read *before* flashing) with
py-opendisplay's `write_config`, connecting with `config=` so it does not try to read the absent one.

## Diagnostics

MSD bytes 0..3 (the config-driven area) carry `d1 <step> <resets> <build>`: the frame-path step
the previous run died in and a watchdog-reset count, both kept in analog registers that only a power
cycle clears. Steps: `0x01..0x03` ATT write callback, `0x04/0x05` around dispatch, `0x06` RX consumed,
`0x07` radio send, `0x08/0x09` around the notify call, `0x10/0x11` around `blt_sdk_main_loop`,
`0x20/0x21` link up/down. Only a watchdog reset should leave a non-zero step behind: `sws_flash.py`
clears it before restarting the MCU, since an SWS halt otherwise looks like a hang in `0x10`.

## tc32 toolchain gaps

- **Never build at `-Os`.** tc32 lays switch tables out as byte offsets but dispatches them as
  32-bit addresses; the first switch taken jumps into garbage. It links, passes every host test and
  hangs the chip. The OD side builds at `-O2` like the SDK, and `tools/check_jump_tables.py` fails
  the build if any table points outside its function.

tc32-elf-gcc is GCC 4.5.1.

- **No `__atomic_*` builtins** (added in GCC 4.7). `tc32_compat.h`, force-included on the OD side,
  maps the load/store forms od_rxq and od_xfer use to volatile accesses between compiler
  barriers: TC32 is single-core and the only concurrency is the BLE ISR.
- **libgcc's assembly helpers are one object repeated.** Every `_*.o` member (`_divsi3.o`,
  `_clzsi2.o`, `_arm_fixunssfsi.o`, ...) is the same blob defining `__divsi3`, `div`, `__ashldi3`,
  and so on, which collides with the SDK's RAM-resident `common/div_mod.S` and cannot be relocated
  against it. `od_tlsr_rt.c` defines the four symbols that would otherwise pull it in
  (`__ashldi3`, `__lshrdi3`, `__clzsi2`, `__fixunssfsi`), so the linker never does. The genuine
  soft-float members (`addsf3.o`, `gesf2.o`) and `_muldi3.o` are used as-is. Check the map
  (`grep 'libgcc.a(' build/telink-tlsr/opendisplay_tlsr.map`) after adding float or 64-bit code.
- **No `memmove`** in the SDK's libc; `od_tlsr_rt.c` has one.

## Open before first flash

- Confirm the busy-line polarity and SPI timing on a real panel; the host test cannot.
- Confirm the target chip: the startup code is `MCU_STARTUP_8258`. TLSR8359 tags need their SRAM
  size and flash map checked against `boot.link` (the image currently uses ~33 KB of RAM).
- ATC's firmware writes its tag type at `0x79000`; that block is left untouched so a tag can go
  back. Reading it to seed a first OD config (panel, pins) is the obvious next step.
- UART recovery path for a tag that boots OD and then misbehaves (ATC's web flasher works over
  UART too).
