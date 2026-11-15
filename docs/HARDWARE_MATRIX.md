# Hardware matrix

Every board this repo builds, what it is, which kernel it runs on, and how its release and debug
variants are produced.

**The build files are ground truth, not this page.** Boards come from
`targets/esp32-idf/boards/*.cmake`, `targets/nordic-zephyr/build.sh`'s `OD_ALL_BOARDS`, and
`targets/efr32bg22-slc/opendisplay-bg22.slcp`. If this table and those disagree, they are right
and this is stale — see CLAUDE.md § "Headers beat design docs".

For what has actually run on silicon, see
[HARDWARE_VERIFICATION_CHECKLIST.md](HARDWARE_VERIFICATION_CHECKLIST.md). A board building is not
a board working.

## Summary

| Target | Boards | Chips | Kernel |
|---|---|---|---|
| `targets/esp32-idf/` | 11 (9 shipping + 2 debug) | ESP32-S3 / C3 / C6 / classic | **FreeRTOS**, via ESP-IDF |
| `targets/nordic-zephyr/` | 3, each × 4 profiles | nRF54L15, nRF54LM20A, nRF52840 | **Zephyr** |
| `targets/efr32bg22-slc/` | 1 | EFR32BG22 | **none** — bare-metal superloop |

Clock, flash and PSRAM below are read from each board's **resolved build config**
(`build/<board>/sdkconfig`, `build-<board>/zephyr/.config`), so they are what the firmware is
actually built for. Core counts and on-die SRAM are part properties from the vendor datasheets —
no build file states them, so treat that pair as the one unverified column.

## `targets/esp32-idf/` — FreeRTOS

| Board | Chip | Cores | Clock | SRAM | PSRAM | Flash | Ships | Notes |
|---|---|---|---|---|---|---|---|---|
| `s3-n16r8` | ESP32-S3 | 2 × Xtensa LX7 | 240 MHz | 512 KB | 8 MB OPI | 16 MB | yes | reference board; FastEPD |
| `s3-n8r8` | ESP32-S3 | 2 × Xtensa LX7 | 240 MHz | 512 KB | 8 MB OPI | 8 MB | yes | FastEPD |
| `s3-n32r8` | ESP32-S3 | 2 × Xtensa LX7 | 240 MHz | 512 KB | 8 MB OPI | 32 MB | yes | FastEPD |
| `s3-n16r8-extuart` | ESP32-S3 | 2 × Xtensa LX7 | 240 MHz | 512 KB | 8 MB OPI | 16 MB | yes | console on CH343P UART (GPIO43/44) |
| `s3-n16r8-extuart-debug` | ESP32-S3 | 2 × Xtensa LX7 | 240 MHz | 512 KB | 8 MB OPI | 16 MB | **no** | `OD_LOG_LEVEL=OD_LOG_DEBUG`; the hardware-verified board |
| `s3-n32r8-extuart` | ESP32-S3 | 2 × Xtensa LX7 | 240 MHz | 512 KB | 8 MB OPI | 32 MB | yes | reTerminal Sticky; SPI panel from the config's drive packets, **no FastEPD** |
| `s3-n32r8-extuart-debug` | ESP32-S3 | 2 × Xtensa LX7 | 240 MHz | 512 KB | 8 MB OPI | 32 MB | **no** | `OD_LOG_LEVEL=OD_LOG_DEBUG` |
| `c3-n4` | ESP32-C3 | 1 × RISC-V | 160 MHz | 400 KB | none | 4 MB | yes | no FastEPD (no LCD peripheral) |
| `c3-n16` | ESP32-C3 | 1 × RISC-V | 160 MHz | 400 KB | none | 16 MB | yes | DIO flash to free GPIO12/13 |
| `c6-n4` | ESP32-C6 | 1 × RISC-V (+LP core) | 160 MHz | 512 KB | none | 4 MB | yes | **shipped**; `min_spiffs_4MB.csv` — default 4 MB layout overflowed by 187 KB |
| `esp32-n4` | ESP32 (classic) | 2 × Xtensa LX6 | 160 MHz | 520 KB | none | 4 MB | yes | **no WiFi/LAN transport**; smallest image in the fleet |

PSRAM is `CONFIG_SPIRAM_TYPE_AUTO` — detected at boot — so 8 MB is the R8 module designation
rather than a compiled-in size. FastEPD needs PSRAM, so no C3/C6 board sets it. It is not
S3-only: the classic-ESP32 `esp32-wrover-e-n4r8` board (Inkplate 5 V2 / 10, M5Paper; on the
`v3/esp32-resync` branch until it merges) builds FastEPD's classic-ESP32 parallel path.

## `targets/nordic-zephyr/` — Zephyr

| Board (`-b` argument) | SoC | Cores | Clock | SRAM | Flash | Notes |
|---|---|---|---|---|---|---|
| `xiao_nrf54l15/nrf54l15/cpuapp` | nRF54L15 | 1 × Cortex-M33 (+ RISC-V VPR) | 128 MHz | 256 KB | 1428 KB RRAM | MCUboot |
| `xiao_nrf54lm20a/nrf54lm20a/cpuapp` | nRF54LM20A | 1 × Cortex-M33 (+ `cpuflpr`) | 128 MHz | 511 KB | 1940 KB RRAM | MCUboot; **a different part, not an L15 variant** |
| `xiao_ble/nrf52840` | nRF52840 | 1 × Cortex-M4F | 64 MHz | 256 KB | 1024 KB | Dual-slot MCUboot chained behind the Adafruit bootloader, SoftDevice erased (default); `OD_NRF52840_MCUBOOT=0` for the Adafruit-only/UF2 layout. EN04/EN05: serial DFU only, no UF2 drive. QSPI NOR enabled |

SRAM and flash here are Zephyr's `CONFIG_SRAM_SIZE`/`CONFIG_FLASH_SIZE` — the app-visible sizes
after partitioning, not the raw part capacity. The 128 MHz figure for the LM20A is in-tree
(`nrf54lm20a_cpuapp_common.dtsi`, `&hfpll clock-frequency = DT_FREQ_M(128)`).

Every board takes every profile, so the build count is 3 × 4 — see below.

## `targets/efr32bg22-slc/` — no kernel

| Board | Part | Cores | Clock | SRAM | Flash | App flash region |
|---|---|---|---|---|---|---|
| `opendisplay-bg22` | EFR32BG22C222F352GM40 | 1 × Cortex-M33 | up to 76.8 MHz | **32 KB** | 352 KB | `0x12000`–`0x56000` = 272 KB |

The first 72 KB of flash is bootloader + Gecko AppLoader. **32 KB of RAM total** is the constraint
that shapes `shared/` for the whole repo: no buffer may be sized for the largest target, and
`docs/MEMORY_CONSTRAINTS.md` exists for this part.

## "Debug" means three different mechanisms

This is the part a single column cannot carry, and getting it wrong wastes a build cycle.

### `esp32-idf` — debug is a separate BOARD

A `-debug` board is its own fragment in `boards/` that compiles `OD_LOG_DEBUG` in. It is a
distinct build target, not a flag on another board, which is why the two debug boards occupy their
own rows above.

```bash
cd targets/esp32-idf
./build.sh                       # all 11, debug boards included
./build.sh --release             # the 9 shipping boards -- excludes *-debug
./build.sh s3-n16r8 c6-n4        # just these (--release has no effect with explicit names)
```

Only `s3-n16r8-extuart` and `s3-n32r8-extuart` have debug counterparts. The other seven have none;
there is nothing to pass to get one.

### `nordic-zephyr` — debug is a PROFILE, orthogonal to the board

Every board takes any profile, so the real board count is 3 and the real build count is 3 × 4.

| `PROFILE` | Console | Notes |
|---|---|---|
| `battery` (default) | SEGGER RTT (J-Link) | UART off so an unpowered USB-UART bridge cannot sink current |
| `uart` | USB UART | RTT off; `prj_uart.conf` |
| `debug` | RTT + `CONFIG_LOG_DEFAULT_LEVEL=3` | `OD_DEBUG_BUILD=1`, `OD_LOG_LEVEL=OD_LOG_DEBUG`; artefacts get a `-debug` suffix |
| `quiet` | none by default | for µA measurement; detach the debugger |

```bash
cd targets/nordic-zephyr
./build.sh                                    # default board, PROFILE=battery
BOARD=xiao_ble/nrf52840 PROFILE=debug ./build.sh
./build.sh --all                              # all three boards at the current PROFILE
```

`PROFILE=debug` is the only profile that renames the artefact, so a debug image cannot overwrite
the production one.

### `efr32bg22-slc` — one build, no variants

There is no debug board and no debug profile: the SLC project produces a single image, and logging
is SEGGER RTT (`od_rtt.c`) with no UART console. Debug means attaching a J-Link, not rebuilding.

```bash
cd targets/efr32bg22-slc
./build-and-flash.sh --no-flash               # headless build, no board needed
```

## Kernels, and why they differ

| Target | Kernel | Consequence for `shared/` |
|---|---|---|
| `esp32-idf` | FreeRTOS, supplied by ESP-IDF | tasks and queues exist; the RX pump runs in `loop()` |
| `nordic-zephyr` | Zephyr | work queues and `k_msgq` exist; RX drains on the main thread |
| `efr32bg22-slc` | **none** — superloop + `sl_power_manager` | no thread to defer to, so arrival context *is* consumer context |

The Silabs row is a design constraint, not a gap. Zephyr was rejected for BG22 on RAM (32 KB
total, CLAUDE.md decision 6), and the absence of a kernel is what makes the BGAPI event-retention
design work: `sl_bt_can_process_event()` holds the vendor event instead of an application RX ring.
That design is only valid without a kernel, so `opendisplay_pipe.c` fails the build if one appears:

```c
#ifdef SL_CATALOG_KERNEL_PRESENT
#error "C13 BGAPI event retention requires the no-kernel sl_bt_step() path"
#endif
```

This is also why `shared/` is plain C with a link-time C API — it has to compile for a superloop
with no kernel at all, not just for two RTOSes (CLAUDE.md § "Architectural decisions" 1).

## Release output

All three targets deliver to one `release/` directory at the repo root, each with its own
`MANIFEST*.txt` because a single manifest would be whichever target ran last.

The released file set, its names, offsets and install flows are defined in
[RELEASE_ARTEFACTS.md](RELEASE_ARTEFACTS.md), and every release carries them as `manifest.json`.
In short:

| Target | Artefacts |
|---|---|
| `esp32-idf` | `opendisplay-<board>-merged.bin` (offset 0, erases config), plus `-{bootloader,partition-table,ota-data,app}.bin` and esp-web-tools manifests for updates that keep NVS; `-app.bin` is the SMP OTA image |
| `nordic-zephyr` | `-merged.hex`, `-ota.bin` (signed), `-dfu.zip` (SMP); nRF52840 adds `-usb-1-sd-erase.zip` and `-usb-2-install.zip` (Adafruit serial DFU) |
| `efr32bg22-slc` | `opendisplay-bg22-ota.gbl`, `-app.s37`, `-app.hex`, `-bootloader.s37` |

The ESP32 chip is not guessable from the filename, so `release/MANIFEST-esp32-idf.txt` and
`manifest.json` record it per image.

## Retired

`s3-e1004` (ESP32-S3, 32 MB + 8 MB) — **board variant retired 2026-08-05**. No fragment exists and
none is wanted. The original blocker (a wrong `bb_epaper` fork) dissolved separately when upstream
merged E1004 support, so the absence is a product decision rather than an unfinished port. Kept
here because the source repo's `platformio.ini` still lists it and its absence otherwise reads as
an oversight.

The legacy nRF52 (`Firmware_NRF`, bare Nordic SDK) is **not** in this matrix and never will be: it
ships, but is maintained in its own repo and never migrates here (CLAUDE.md decision 8). It still
binds the wire contract as the fleet's compatibility floor — no compression, no `0x76`, no PIPE,
no NFC. Do not confuse it with the nRF52840, which is a board on `nordic-zephyr` above.
