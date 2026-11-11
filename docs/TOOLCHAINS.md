# Toolchain analysis

Which build system and framework each target uses in `Firmware_Unified`.
Scope: **ESP32 (all `platformio.ini` variants), nRF52840, nRF54L15, EFR32BG22.**
`Firmware_NRF` (legacy nRF52 + bare Nordic SDK) is out of scope — it **is** still shipped
(a handful of low-capability units, established 2026-07-25), but it is maintained in place in
its own repo and never migrates here; see MIGRATION.md § "Order and rationale" item 5.

## Decision

**Three toolchains, split by silicon vendor. No PlatformIO and no Arduino anywhere in the repo.**

| Target | Chips | Toolchain | Build | Change from today |
|---|---|---|---|---|
| `targets/esp32-idf/` | ESP32-S3 (N8R8/N16R8/N32R8, ext-UART, E1004), ESP32-C3 (N4/N16), ESP32-C6 (N4), classic ESP32 (N4) | **ESP-IDF** + `idf.py` | CMake + Kconfig | **drops PlatformIO and Arduino** |
| `targets/nordic-zephyr/` | nRF54L15, nRF52840 | Zephyr / nRF Connect SDK + west | CMake + Kconfig | **nRF52840 moves off PlatformIO/Arduino, joins nRF54L15** |
| `targets/efr32bg22-slc/` | EFR32BG22 (`…C222F352GM40`) | **Simplicity SDK** + SLC | CMake (generated) | **none — keep as-is** |

The two Nordic/Espressif targets keep nothing from the current PlatformIO setup; both end up
on **CMake + Kconfig**, which is the property that makes this more than a preference — see
below. The Silabs target is the exception in every dimension: it is already CMake, it is
already vendor-SDK-native, and it is the one target where changing anything is a net loss
(§ EFR32BG22).

## Why this is the right shape (not just a preference)

**`shared/` becomes one CMake library consumed by every target.** This is the strongest
argument for ESP-IDF and it is architectural, not stylistic. Under PlatformIO, `shared/` would
have had to be consumed by PIO's SCons-based library scanner *and* by Zephyr's CMake — two
dependency models, two sets of include-path rules, two ways for a vendor header to sneak in.
With IDF, `shared/` is an `idf_component_register()` component on one side and a
`zephyr_library()`/`target_sources(app ...)` on the other, from the same source list.

**Buffer sizes become Kconfig options, which is what ARCHITECTURE.md already asked for.** Its
memory-sensitivity section requires that buffer sizes be "target-parameterised behind
compile-time constants the target sets, with a documented floor." IDF and Zephyr both have
Kconfig. That turns `-DOPENDISPLAY_ZLIB_WINDOW_BITS=9` and `-DPIPE_SMALL_DRAM_WINDOW` from
ad-hoc `build_flags` strings into declared options with
types, defaults, ranges, and help text — in the same idiom on those two targets.

**But do not design `shared/`'s configuration surface around Kconfig**, because the Silabs
target has none (see § EFR32BG22). `shared/` takes plain preprocessor constants; Kconfig is
merely the mechanism by which two of the three targets set them.

**The platform-fork problem disappears entirely.** `Firmware/platformio.ini` currently depends
on *three* platform sources: `pioarduino/platform-espressif32` (all S3 envs + classic ESP32),
the official `espressif32` registry (C3-N4, C6-N4, C3-N16), and
`maxgerhardt/platform-nordicnrf52` (nRF52840) — two of them community forks. Today the C3/C6
envs and the S3 envs can build against different Arduino cores and different IDF versions
**from the same source tree**, which is a live source of "works on S3, fails on C6" divergence
that has nothing to do with unification. Going to IDF + west removes all three dependencies in
favour of vendor-supported SDKs.

**Eliminating `String` is work that has to happen anyway.** `String` appears at **575 sites**
across `Firmware/src/` — by far the largest Arduino dependency. `shared/` must be plain C
(two targets are C-only), so every `String` on a path destined for `shared/` was going to be
removed regardless. The Arduino removal and the `shared/` extraction are substantially the
same work, done once instead of twice.

## Evidence from the current tree

Findings that drove the analysis, worth re-checking if they change.

### bb_epaper is already an ESP-IDF component upstream

> 3.0: no firmware target compiles bb_epaper (panels run from config drive packets); this and
> the related notes below are the import-time record.

This was the risk I expected to dominate, and it is already solved. The upstream checkout at
`~/bb_epaper` has:

- a top-level `CMakeLists.txt` that is literally
  `idf_component_register(SRCS ... REQUIRES driver esp_timer)`;
- `esp_idf/esp_generic.inl` — an IDF IO backend implementing `digitalWrite`/`pinMode`/SPI over
  `driver/gpio.h`, `driver/spi_master.h`, `esp_timer.h`;
- `esp_idf/esp_main_io.inl`, plus `s3_ulp_io.inl` and `c6_ulp_io.inl` (ULP-coprocessor
  backends);
- working IDF example projects, including a Spectra6 one.

Backend selection happens by `#ifdef` inside `bb_epaper.cpp`, which already dispatches to
`esp_idf/esp_generic.inl` as its non-Arduino, non-Zephyr, non-Silabs default.

**Caveat — no copy is a superset.** `Firmware_NRF54/third_party/bb_epaper/` and
`Firmware_Silabs/third_party/bb_epaper/` have no `esp_idf/` directory; upstream `~/bb_epaper`
(at `2ef09a1`) has no `nrf54_zephyr_io.inl`, `nrf54_bbep_busy.inl`, `silabs_efr32_io.inl`, or
`silabs_bbep_busy.inl`. The single vendored copy has to be assembled from both — see "Where
bb_epaper and uzlib live" below.

**Correction to a plausible assumption:** `esphome_io.inl` is *not* a usable IDF backend
despite ESPHome's IDF association — it includes `<SPI.h>` and calls `pinMode`/`HIGH`, i.e. it
targets ESPHome's Arduino path. The genuine non-Arduino backends are `esp_idf/esp_generic.inl`,
`nrf54_zephyr_io.inl`, and `silabs_efr32_io.inl` — the last two existing only downstream, in the
`Firmware_NRF54` and `Firmware_Silabs` vendored copies.

### Seeed_GFX / TFT_eSPI is a bounded port, not a rewrite

`Firmware/lib/Seeed_GFX/` is vendored in-tree (so it can be modified freely) and its ESP32
processor layer is **already IDF-level**:

```
Processors/TFT_eSPI_ESP32.h      → soc/spi_reg.h, driver/spi_master.h, hal/gpio_ll.h
Processors/TFT_eSPI_ESP32_S3.h   → same, plus OpenDisplay/opendisplay_runtime_pins.h
```

There is already an OpenDisplay-specific hook (`OpenDisplay/opendisplay_runtime_pins.h`), so
the fork is established. What remains Arduino: ~87 call sites in `TFT_eSPI.cpp`
(`Serial`/`String`/`digitalWrite`/`pinMode`/`delay`/`millis`/`SPI.`) and `FS.h`/`SPIFFS.h` for
font loading, which is very likely unused here and can be `#if 0`'d.

Only two S3 envs enable it (`-DOPENDISPLAY_SEEED_GFX`, for `panel_ic_type` 3000/3001), so it is
also isolatable — it does not block the other eight boards.

### The ESP32 application code is already IDF-level

`wifi_service.cpp` includes `esp_wifi.h`, `esp_event.h`, `esp_heap_caps.h`, and raw
`mbedtls/ssl.h`, `mbedtls/ctr_drbg.h`, `mbedtls/entropy.h` alongside `WiFi.h`/`ESPmDNS.h`.
Across `src/`, direct IDF calls (`esp_sleep_*`, `esp_deep_sleep_*`, `esp_restart`,
`esp_wifi_*`, `esp_event_*`) are routine. Arduino is a veneer over code that is already
IDF-shaped.

### Arduino API census — what actually has to be replaced

Call sites across `Firmware/src/` (`*.cpp` + `*.h`), with the IDF and HAL destination:

| Arduino API | Sites | ESP-IDF replacement | HAL destination |
|---|---:|---|---|
| `String` | **575** | `std::string` / fixed `char[]` | must not appear in `shared/` at all |
| `Wire` | 246 | `driver/i2c_master.h` (IDF ≥ 5.2) | `od_hal_i2c` |
| `pinMode` | 102 | `gpio_config()` | `od_hal_gpio` |
| `digitalWrite` | 87 | `gpio_set_level()` | `od_hal_gpio` |
| `delay()` | 77 | `vTaskDelay()` | `od_hal_time` |
| `millis()` | 66 | `esp_timer_get_time() / 1000` | `od_hal_time` |
| `LittleFS` | 23 | `esp_littlefs` component — or NVS, see below | `od_hal_nvs` |
| `delayMicroseconds` | 20 | `esp_rom_delay_us()` | `od_hal_time` |
| `digitalRead` | 13 | `gpio_get_level()` | `od_hal_gpio` |
| `SPI.` | 6 | `driver/spi_master.h` | `od_hal_spi` |
| `attachInterrupt` | 5 | `gpio_isr_handler_add()` | `od_hal_gpio` |
| `Serial.` | **5** | `esp_log` / USB-Serial-JTAG | `od_hal_log` |
| `analogRead` | 1 | `adc_oneshot` | — (fold into battery driver) |

Two things to read out of this table. **`Serial.` is only 5 sites** because logging already
goes through `writeSerial(String message, bool newLine)` in `main.h` — the abstraction exists,
it just has the wrong signature; it becomes `od_hal_log`. And **`String` is 28% of `Wire`+
everything else combined**, concentrated in `config_parser.cpp` (160), `display_service.cpp`
(98), and `wifi_service.cpp` (41) — the first two being exactly the files whose logic is
destined for `shared/core`.

### The nRF54L15 port is a full second implementation of the shared logic

Near-duplicate subsystems, written twice in two languages:

| Subsystem | `Firmware` (Arduino/C++) | `Firmware_NRF54` (Zephyr/C) |
|---|---|---|
| Config parsing | `config_parser.cpp` (919) | `opendisplay_config_parser.c` (696) |
| Transfers / PIPE | `communication.cpp` (754) + part of `display_service.cpp` (3309) | `opendisplay_pipe.c` (1425) |
| BLE | `ble_init.cpp` (321) + `esp32_ble_callbacks.h` (131) | `opendisplay_ble.c` (756) |
| Crypto | `encryption.cpp` (848), 2 `#ifdef` backends | PSA (`CONFIG_PSA_WANT_*`) |
| Config storage | Adafruit_LittleFS / LittleFS | `opendisplay_config_storage.c` (NVS/settings) |

The Zephyr side is already **C**, for everything except `opendisplay_display.cpp` and
`boot_screen.cpp`. Since `shared/` must be C, the nRF54L15 sources are the better donor for
*structure and language*, while `Firmware` remains the reference for *feature coverage*.

### `Firmware_NRF54` already has a nascent HAL

`nrf54_zephyr_compat.h` is four functions — `od_msleep`, `od_uptime_get_32`, `od_busy_wait`,
`od_hwinfo_get_device_id` — plus `nrf54_gpio.c` and `nrf54_zephyr_time.c`. That is `od_hal_time`
and `od_hal_gpio` in embryo, already using the `od_` prefix. Promote these rather than
designing from scratch.

### All three toolchains are installed on this dev box

**Corrected 2026-07-25.** This section previously said only the Silabs toolchain was present
and that there was "no `~/ncs`, no `west`, and no `idf.py`". That was wrong, and the way it was
wrong is worth recording, because it is easy to repeat: **none of the three is on `PATH`**, so
`which idf.py west` returns nothing and the absence looks confirmed. Each needs an activation
step instead.

| Toolchain | Version | Location | Activation |
|---|---|---|---|
| **ESP-IDF** | **v5.5.4** | `~/esp/esp-idf` (tools in `~/.espressif`) | `source ~/esp/esp-idf/export.sh` |
| **nRF Connect SDK** | **v3.3.1**, west **v1.5.0** | `~/ncs/v3.3.1`, toolchain `~/ncs/toolchains/911f4c5c26` | `nrfutil toolchain-manager launch --ncs-version v3.3.1 -- <cmd>` |
| **Simplicity SDK** | 2025.12.2 | see table below | `slt`, plus Java on `PATH` for `slc` |

Verified 2026-07-25 by running `idf.py --version` (→ `ESP-IDF v5.5.4`) and
`west --version` (→ `West version: v1.5.0`) — *version invocations only*. No target
source is imported yet, so nothing here has been built from this repo, and this is not a
claim that any target builds.

**Both installed versions satisfy the floors this document sets, and are the obvious pins:**

- **IDF v5.5.4** clears ≥ 5.1 (C6 support) and ≥ 5.2 (`driver/i2c_master.h`), so the "pin one
  explicit release" open item has a concrete candidate that costs nothing to adopt.
- **NCS v3.3.1** is the *only* version installed, which incidentally settles the
  reproducibility complaint about `Firmware_NRF54/ncs-env.sh` globbing `~/ncs/v3.*` and taking
  the first hit: here there is exactly one hit. Pin it explicitly anyway — the glob is still
  wrong on a machine with two.

The EFR32BG22 toolchain is installed and additionally **verified building headless**
(2026-07-20, Simplicity SDK 2025.12.2) — a stronger claim than the other two, which have only
been run for their version strings:

| Piece | Location |
|---|---|
| ARM GCC 12.2.Rel1 | `~/.silabs/slt/installs/conan/p/gcc-a442105b5c2637/p/bin/` (also the hardcoded fallback in `cmake_gcc/toolchain.cmake`) |
| `slc-cli` 6.0.17 | `~/.silabs/slt/installs/archive/slc-cli-v6.0.17/slc_cli/slc` |
| Simplicity Commander | `~/.silabs/slt/installs/archive/commander/commander` (`.gbl` OTA + J-Link flash) |
| OpenJDK 21 (for `slc`) | `~/.silabs/slt/installs/archive/java21-v21.0.5/java21_linux_x86_64/jre/bin` — **not on `PATH`** |
| Simplicity SDK 2025.12.2 | `~/.silabs/slt/installs/conan/p/simpl965e19baece23/p` — full SDK, 2892 `.slcc`, 2.4 GB (`slt where simplicity-sdk`) |
| `cmake`, `ninja`, `slt` | system-wide / `~/bin/slt` |

The full Simplicity SDK **is installed here** (verified 2026-07-25) — `2025.12.2`, the exact
version the build uses, at `~/.silabs/slt/installs/conan/p/simpl965e19baece23/p`: 2892 `.slcc`
files, 2.4 GB, a real SDK rather than a `-cp` source copy. So the pinned-install requirement
(MIGRATION.md § "The Silabs SDK is not imported as-is") is already met on this machine.

**The remaining gap is registration, not installation.** No `.slconf` exists in the project and
`~/.uc/cli/cli.config` registers no SDK, so `slc generate` still fails unless pointed at the
install with `--sdk-package-path`. Earlier notes — including the workspace-level `CLAUDE.md` —
record this as "no full SDK available"; that is now out of date and should be corrected to "SDK
present, not wired to `slc`". Independently of either, the CMake build needs neither `slc` nor
the SDK metadata, so this remains the one target that builds on this machine today.

Consequences: each target needs a "what must be installed" section, and **CI matters more than
usual** because no single dev box builds everything.

## Per-target detail

### ESP32 (all variants) → ESP-IDF

**Version — done: pinned to `v5.5.4`.** The pin is
[`targets/esp32-idf/.idf_version`](../targets/esp32-idf/.idf_version), a one-line checked-in
file, and `build.sh` **enforces** it: it compares the pin against `idf.py --version` after
activation and refuses to build on a mismatch. Both the pin and the active version are recorded
in `release/MANIFEST.txt`, so an image built off-pin is identifiable after the fact.

```bash
./build.sh                              # enforce (default): mismatch is a hard failure
OD_IDF_VERSION_CHECK=warn ./build.sh    # note the mismatch, build anyway
OD_IDF_VERSION_CHECK=off  ./build.sh    # say nothing
```

Enforced rather than advisory because an IDF minor bump changes generated startup code, the
bootloader, and sdkconfig defaults — all of which land inside a merged image that nobody diffs.
Before this existed, `build.sh` took whatever IDF happened to live at `$IDF_PATH`, which made
"works on my machine" and a CI failure indistinguishable from a source change. **Adopting a new
IDF means editing `.idf_version` in the same commit as whatever the bump requires** — that is
the point of the file, not an obstacle to routing around.

The same hole is still open on the Nordic side: `Firmware_NRF54`'s `ncs-env.sh` globs
`~/ncs/v3.*` then `~/ncs/v2.*` and takes the first hit. Close it the same way when that target
is imported.

Floors the pin clears: **≥ 5.1** for ESP32-C6 support, **≥ 5.2** for the new
`driver/i2c_master.h` API that the 246 `Wire` sites should land on (the legacy `driver/i2c.h`
is deprecated — do not port onto it).

**Board matrix.** The ten PlatformIO envs are the real ergonomic loss — PIO's `extends =
env:...` inheritance (already used by `esp32-s3-E1004`) has no direct IDF equivalent. Replace
with layered sdkconfig fragments, which IDF supports natively:

```
targets/esp32-idf/
  sdkconfig.defaults              # common to all boards
  sdkconfig.defaults.esp32s3      # per-IDF_TARGET, auto-selected by IDF
  sdkconfig.defaults.esp32c3
  sdkconfig.defaults.esp32c6
  sdkconfig.defaults.esp32
  boards/
    s3-n16r8.conf   s3-n32r8-extuart.conf   s3-e1004.conf
    c3-n4.conf      c3-n16.conf   c6-n4.conf   esp32-n4.conf   ...
  partitions/
    default_8MB.csv  default_16MB.csv  default_32MB.csv  huge_app.csv
  build.sh                        # build.sh <board>  — mirrors the Zephyr target's build.sh
```

`build.sh <board>` composes `SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/<board>.conf"` and
sets `IDF_TARGET`. That deliberately mirrors `Firmware_NRF54/build.sh` (and the Silabs target's
`build-and-flash.sh`), so all three targets are
driven the same way — worth the small effort for the symmetry alone.

**Settings translation.** Every current PIO knob has a native equivalent, most of them more
precise:

| PlatformIO | ESP-IDF sdkconfig |
|---|---|
| `board_build.partitions = default_16MB.csv` | `CONFIG_PARTITION_TABLE_CUSTOM` + `..._FILENAME` |
| `board_upload.flash_size = 16MB` | `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` |
| `board_build.flash_mode = qio` / `dio` | `CONFIG_ESPTOOLPY_FLASHMODE_QIO` / `_DIO` |
| `board_build.arduino.memory_type = qio_opi`, `psram_type = qspi_opi` | `CONFIG_SPIRAM_MODE_OCT` + `CONFIG_SPIRAM_SPEED_*` |
| `-DBOARD_HAS_PSRAM` | `CONFIG_SPIRAM=y` |
| `ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1` | `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` |
| `-DOPENDISPLAY_LOG_UART{,_RX,_TX}` (ext-UART boards) | `CONFIG_ESP_CONSOLE_UART_CUSTOM` + TX/RX pin options |
| `-DCONFIG_FREERTOS_WATCHDOG_TIMEOUT_S=120` | `CONFIG_ESP_TASK_WDT_TIMEOUT_S=120` (it was always an IDF option, passed as a raw `-D`) |
| `board_build.filesystem = littlefs` | `esp_littlefs` managed component — or drop, see below |
| `-DPIPE_SMALL_DRAM_WINDOW` (classic ESP32 only) | project Kconfig option, default `y` for `esp32` |
| `-DOPENDISPLAY_ZLIB_WINDOW_BITS`, `..._USE_HEAP_WINDOW` | project Kconfig options with ranges + defaults |
| `extra_scripts = pre:scripts/factory_config_gen.py` | CMake `add_custom_command` — copy the pattern from `Firmware_NRF54/zephyr/CMakeLists.txt`, which already does exactly this |

**This table is necessary and demonstrably not sufficient.** It enumerates the knobs someone
*wrote down* in `platformio.ini`; it cannot enumerate the settings the Arduino core set on the
project's behalf without anyone naming them. The 2026-08-04 audit below found four such
settings — CPU frequency, tick rate, optimisation level, watchdog panic — of which this table
caught two. See § "sdkconfig divergence audit" for what replaced hand-maintenance.

**The one genuinely new piece: BLE.** IDF ships NimBLE natively, but only the **C host API**
(`ble_gap_*`, `ble_gatts_*`). NimBLE-Arduino's C++ wrapper — which `ble_init.h` currently
aliases wholesale (`using BLEDevice = NimBLEDevice;` and nine more) — does not exist in IDF. So
`ble_init.cpp` + `esp32_ble_callbacks.h` (~450 lines) get rewritten against the C API. Two
mitigations: `opendisplay_ble.c` (Zephyr, C, 756 lines) is a structural reference for
"OpenDisplay GATT service written against a C host API", and this is precisely the code that
`od_hal_radio` is supposed to abstract — so the rewrite is the HAL implementation, not throwaway
work.

**Config storage — converge on NVS.** The 23 `LittleFS` sites are all in `config_parser.cpp` /
`main.h`, i.e. config blob persistence and nothing else. The Zephyr side already does this with
NVS (`opendisplay_config_storage.c`). Recommend **NVS on both** rather than adding the
`esp_littlefs` managed component, which collapses `od_hal_nvs` to one semantic on both targets.
Verify first that LittleFS holds nothing else on ESP32 (fonts? boot images?) — if it does, keep
`esp_littlefs` for that and still put config in NVS.

**What is straightforwardly better under IDF:** `menuconfig` instead of `build_flags` strings,
native partition/OTA tooling, `esp_littlefs`/NVS as declared components, no community platform
fork in the dependency chain, and direct access to the IDF APIs the code already calls without
Arduino sitting in between.

### sdkconfig divergence audit — 2026-08-04

The port's premise is that it reproduces the shipped `Firmware` behaviour on a new toolchain.
That premise is only testable against the configuration the shipped build actually runs, so the
target's **effective** sdkconfig was diffed against the precompiled Arduino core sdkconfigs the
reference repo links against.

```bash
# ~/.platformio/packages/framework-arduinoespressif32-libs/<chip>/sdkconfig   (the reference)
#   vs   targets/esp32-idf/build/<board>/sdkconfig                            (ours, effective)
cd targets/esp32-idf && ./build.sh s3-n16r8 c6-n4 c3-n4 esp32-n4
```

Compare with a script that loads both files (treating `# CONFIG_X is not set` as `n`),
intersects the key sets, and classifies each difference as **[DECLARED]** — the symbol appears
in `sdkconfig.defaults*` or `boards/*.sdkconfig`, so somebody chose it — or **[inherited]** —
it does not, so it is an IDF default the project accepted by never naming the symbol. A
behaviour filter keeps the FreeRTOS / watchdog / CPU-freq / BT / LWIP / SPIRAM / heap /
optimisation / console / mbedTLS / WiFi / sleep / bootloader families and drops the rest
(toolpath, component versions, chip-capability `SOC_*`).

Counts at the time of the audit, and on a re-run after the fixes below landed:

| Chip (board) | keys in both | differing | behaviour-relevant | ↳ declared | ↳ inherited |
|---|---:|---:|---:|---:|---:|
| esp32s3 (`s3-n16r8`) | 1960 → **1959** | 191 → **173** | 77 → **63** | **6** | **57** |
| esp32c6 (`c6-n4`) | 1888 → **1887** | 161 → **151** | 59 → **52** | **5** | **47** |
| esp32c3 (`c3-n4`) | 1736 → **1735** | 171 → **161** | 59 → **52** | **5** | **47** |
| esp32 (`esp32-n4`) | 1564 → **1563** | 165 → **153** | 58 → **50** | **2** | **48** |

*First figure in each cell is the audit run (pre-fix); bold is a re-run on 2026-08-04 after
commit `f72187f`. The S3 pre-fix numbers are the ones recorded in that commit message; the
other three chips' pre-fix numbers come from the same audit run and are not re-derivable now
that the fixes have landed. The declared/inherited split is from the re-run.*

**The ratio is the finding, not the totals.** Six of 77 on the S3 — under 8 % — were values
this project had declared. Every other difference existed because nobody wrote the symbol down.
A setting you did not write does not appear in any file you can review, which is exactly why
these survived a code review, a translation table, and ten clean builds.

**The operating rule this establishes.** During a port whose premise is reproducing shipped
behaviour, **an undeclared difference is a defect, not a preference.** It was not weighed
against the alternative; it is the residue of two different tools' defaults. The burden of
proof runs the other way from normal: keeping the IDF default requires a reason, not adopting
the Arduino value. Once a difference is written down with a reason it becomes a decision and
this rule stops applying to it.

#### Resolved (commit `f72187f`)

Four differences were restored to the Arduino build's values, chosen because the reference
firmware's behaviour depends on them. All four are now declared, with the reasoning inline in
`sdkconfig.defaults` / `sdkconfig.defaults.esp32s3`:

| Setting | Was (inherited) | Now | Why |
|---|---|---|---|
| `CONFIG_FREERTOS_HZ` | 100 | **1000** | `portTICK_PERIOD_MS` was 1 upstream, 10 here — which does not coarsen short waits, it deletes them: `pdMS_TO_TICKS(2)` is **zero** ticks and `vTaskDelay(0)` yields without blocking. `src/session_guard.cpp`'s R3a link-down poll is exactly that shape. |
| `CONFIG_COMPILER_OPTIMIZATION_*` | `DEBUG` (`-Og`) | **`SIZE`** (`-Os`) | Nobody chose a debug build; `-Og` is what IDF gives you for saying nothing. |
| `CONFIG_ESP_TASK_WDT_PANIC` | `n` (log only) | **`y`** | Restores the reset backstop, paired with `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n` — also the reference's value. Enabling PANIC while watching an idle task the reference never watched would invent a new reset source rather than restore the old behaviour; the two are one decision. |
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240` | 160 | **240**, **S3 only** | Measured, not assumed: the Arduino core's own sdkconfigs run esp32, C3 and C6 at 160 (and 160 is the *maximum* on C3/C6). Only the S3 is 240. It is the one chip where the port had slowed the silicon by a third. |

Image size fell about 8 % on every chip — which matters most for `c6-n4`, the shipped board
with the least slot headroom:

| Board | before | after | Δ |
|---|---:|---:|---:|
| `s3-n16r8` | 1353712 | 1245040 | −108672 (−8.0 %) |
| `c6-n4` | 916352 | 841072 | −75280 (−8.2 %) |
| `c3-n4` | 810768 | 738448 | −72320 (−8.9 %) |
| `esp32-n4` | 765024 | 702160 | −62864 (−8.2 %) |

**Not hardware-verified.** These four change clock, code generation and reset behaviour on
every path. All ten boards build clean; nothing here has been flashed. A direct consequence:
every throughput and latency figure in `targets/esp32-idf/README.md` § "Verified on hardware"
was measured at 160 MHz with `-Og` and describes no build that now exists — they need
re-measuring, not adjusting.

#### Remaining behaviour-relevant divergences, with verdicts

Values below are from the 2026-08-04 re-run; `arduino → ours`, per chip where they differ.
Everything in this table is **[inherited]** unless marked otherwise.

**Robustness**

| Setting | arduino → ours | Verdict |
|---|---|---|
| `FREERTOS_WATCHPOINT_END_OF_STACK` | `y → n` on **esp32s3 and classic esp32** (the C3/C6 Arduino configs also have it off, so there is no divergence there) | **Adopt.** It costs one debug watchpoint and turns a stack overflow into an immediate, located crash instead of silent corruption. It is also the setting that makes the task-stack row below survivable. |
| `HEAP_POISONING_LIGHT` vs `HEAP_POISONING_DISABLED` | `LIGHT → disabled` (all four chips) | **Keep ours for release.** Poisoning is a per-allocation cost paid forever to catch a class of bug better caught in a debug build. Worth enabling temporarily when chasing heap corruption; not worth shipping. |
| `ESP_SYSTEM_MEMPROT_FEATURE` | `n → y` on **esp32s3**; on C3 both are `y`; the symbol does not exist on C6/classic esp32 | **Keep ours** — ours is the stricter of the two, and this is a divergence in the safe direction. |

**Task stacks — measure, do not copy**

Every one of these is inherited, and most are *smaller* than the Arduino build's:

| Setting | arduino → ours |
|---|---|
| `ESP_TIMER_TASK_STACK_SIZE` | `8192 → 3584` (all four chips) |
| `BT_NIMBLE_HOST_TASK_STACK_SIZE`, `BT_NIMBLE_TASK_STACK_SIZE` | `5120 → 4096` (S3, C6, C3) |
| `BT_LE_CONTROLLER_TASK_STACK_SIZE` | `5120 → 4096` (C6 only) |
| `LWIP_TCPIP_TASK_STACK_SIZE` | `4096 → 3072` (all four) |
| `FREERTOS_TIMER_TASK_STACK_DEPTH` | `3120 → 2048` (S3); `4096 → 2048` (C3, classic esp32); no divergence on C6 |
| `ESP_MAIN_TASK_STACK_SIZE` | `4096 → 3584` (all four) |
| `FREERTOS_ISR_STACKSIZE` | `2096 → 1536` (all four) |

Two go the other way and are called out so the row is not read as "ours is uniformly smaller":
`FREERTOS_IDLE_TASK_STACKSIZE` is `1024 → 1536` on S3/classic esp32 (but `2304 → 1536` on
C3/C6), and `ESP_SYSTEM_EVENT_TASK_STACK_SIZE` is `2048 → 2304` everywhere.

**Verdict: measure before changing anything, then decide per chip.** Copying Arduino's numbers
wholesale would spend several KB of DRAM to buy headroom nobody has shown is needed — and on
C3, C6 and `esp32-n4` there is no PSRAM, so that DRAM is the real and only DRAM. The
measurement is `uxTaskGetStackHighWaterMark()` on each task after a full BLE session plus a
LAN transfer plus a panel refresh; raise only what comes back thin.

**What makes this urgent rather than merely open:** the combination. Smaller stacks *and*
`FREERTOS_WATCHPOINT_END_OF_STACK=n` means an overflow that the reference build would have
trapped at the moment it happened corrupts adjacent memory here and surfaces later as something
unrelated. Adopting the watchpoint (row above) is the cheap half and does not wait on the
measurement.

**Power / wake**

| Setting | arduino → ours | Verdict |
|---|---|---|
| `BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` | `y → n` (all four chips) | **Adopt.** With it off, the bootloader re-validates the app image on *every* deep-sleep wake — pure latency and battery on a device whose duty cycle is wake, draw, sleep. The caveat, stated so the trade is visible: skipping validation is only a security-relevant relaxation if secure boot ever lands here, and it does not today. Revisit if it does. |

**Memory / PSRAM (S3 only)**

| Setting | arduino → ours |
|---|---|
| `SPIRAM_MALLOC_RESERVE_INTERNAL` | `0 → 32768` |
| `SPIRAM_MALLOC_ALWAYSINTERNAL` | `4096 → 16384` |
| `SPIRAM_BOOT_HW_INIT` | `n → y` |
| `SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION` | `n → y` |

**Verdict: do not touch.** These three allocator knobs decide what lands in DRAM versus PSRAM,
and the DRAM-reclaim work merged at `60980f7` (sync to `Firmware` `feat/psram-dram-reclaim`)
was tuned *under our current values*. Changing them invalidates those measurements rather than
improving on them. If they are ever revisited, the order is: re-run the reclaim measurements
first, then change one knob, then re-run again. (Also note the classic ESP32 case, which is not
a divergence to fix: the Arduino `esp32` sdkconfig has `SPIRAM=y`, `esp32-n4` has no PSRAM.)

**LAN / TCP**

| Setting | arduino → ours |
|---|---|
| `LWIP_TCPIP_CORE_LOCKING` | `y → n` |
| `LWIP_TCP_SACK_OUT` | `y → n` |
| `LWIP_TCP_RTO_TIME` | `3000 → 1500` |
| `LWIP_TCP_SYNMAXRTX` | `6 → 12` |
| `LWIP_MAX_SOCKETS` | `16 → 10` |
| `LWIP_TCP_MSS` | `1436 → 1440` |
| `LWIP_SO_RCVBUF` | `y → n` |
| `LWIP_TCP_SND_BUF_DEFAULT` | `5744 → 5760` |
| `LWIP_TCPIP_TASK_AFFINITY` | `CPU0 → no affinity` |

All four chips, all inherited. **Verdict: record only.** Each is individually plausible in both
directions and none has a known victim; the MSS/SND_BUF pair are just the arithmetic
consequence of a different MSS. Revisit as a group **if and only if** LAN throughput is measured
and disappoints — at which point `CORE_LOCKING` and `SACK_OUT` are the two to try first, and
the affinity difference is the one most likely to matter on the dual-core S3.

**The six declared differences — deliberate, and already justified in the tree**

| Setting | arduino → ours | Where the reason lives |
|---|---|---|
| `BT_NIMBLE_ENABLED` (+ `BT_BLUEDROID_ENABLED` off) | `n → y` on classic esp32 | `sdkconfig.defaults` — the source uses NimBLE-Arduino; the C API port keeps the same stack rather than adding a second migration. |
| `BT_NIMBLE_MAX_CONNECTIONS` | `3 → 1` | `sdkconfig.defaults` — one central at a time is the product's connection policy (CONNECTION_POLICY.md). |
| `BT_NIMBLE_LOG_LEVEL_WARNING` | `INFO → WARNING` | `sdkconfig.defaults`, at length: NimBLE's INFO chatter interleaves mid-line with `od_log` on the same UART and corrupts hex dumps. Commit `cce8165`; DIVERGENCE_MATRIX.md 7a. |
| `ESP_TASK_WDT_TIMEOUT_S` | `5 → 60` | `sdkconfig.defaults` — `platformio.ini` asked for 120 via a raw `-D`; IDF caps the Kconfig at [1,60], so 60 is the closest legal value. A translation finding, not a typo. |
| `ESP_CONSOLE_USB_CDC` / `ESP_CONSOLE_USB_SERIAL_JTAG` / `ESP_CONSOLE_UART_DEFAULT` | varies by chip | `sdkconfig.defaults.esp32s3` (CDC), `.esp32c3` / `.esp32c6` (USB-Serial-JTAG — the C3/C6 have no CDC), `.esp32` and `boards/_extuart.sdkconfig` (UART). This is the `ARDUINO_USB_MODE`/`ARDUINO_USB_CDC_ON_BOOT` row of the translation table above. |
| `SPIRAM_MODE_OCT` | `QUAD → OCT` | `sdkconfig.defaults.esp32s3` — the `board_build.arduino.memory_type = qio_opi` row of the translation table. The R8 modules are octal PSRAM; the Arduino core's generic sdkconfig is not. |

#### The honest caveat

**The Arduino core ships against an older ESP-IDF than the `v5.5.4` this target pins**, so some
part of the 50-63 remaining differences per chip is version drift — a default Espressif changed
between releases — rather than a decision anyone made on either side. That is a reason to treat
the long tail as "record and revisit", not as a defect list to burn down. It is **not** a reason
to dismiss the four already fixed: CPU frequency, tick rate, optimisation level and watchdog
panic are all settings the Arduino core sets *explicitly* in its own sdkconfig, and the
divergence is against that explicit value, not against a moved default.

Which of the remaining rows are version drift has not been established. Doing so means diffing
the Arduino core's IDF version against `v5.5.4`'s Kconfig defaults, which nobody has done.

#### The gate that replaces hand-maintenance

- [`targets/esp32-idf/sdkconfig.baselines/`](../targets/esp32-idf/sdkconfig.baselines) — the
  **whole** effective sdkconfig of all 11 boards, checked in, header block stripped (it carries
  the IDF version and would otherwise conflict on every toolchain bump for a non-configuration
  reason).
- [`targets/esp32-idf/tools/sdkconfig_baseline.sh`](../targets/esp32-idf/tools/sdkconfig_baseline.sh)
  — diffs the live build against the baseline; `--update` re-records after an approved change.
  With no arguments it checks every board that has been built.
- [`.github/workflows/esp32-sdkconfig-baseline.yml`](../.github/workflows/esp32-sdkconfig-baseline.yml)
  — one board per chip (`s3-n16r8`, `c3-n4`, `c6-n4`, `esp32-n4`) on `espressif/idf:v5.5.4`,
  the same release `.idf_version` pins. This is the first CI job in the repo that builds
  anything. Its deliberate coverage gap is recorded in the workflow: a board-fragment change
  affecting only one of the other six boards is caught locally, not by CI.

**Why the whole config and not a curated list.** State it plainly: the hand-maintained
translation table in this document existed for exactly this purpose and caught **two of the
four**. A curated list can only catch drift in settings someone already thought of, and all
four were missed precisely because nobody thought of them. The cost is a noisy diff on an IDF
upgrade — and that noise *is* the review, because an IDF bump moves defaults underneath the
project and this is the only place that becomes visible before hardware makes it visible
instead. Same argument `tools/check.sh`'s arduino-free check makes about Arduino
primitives, same answer: a mechanical gate rather than good intentions.

#### The `build.sh` footgun found doing this

**IDF applies `sdkconfig.defaults` only when it *creates* `build/<board>/sdkconfig`.** Once
that file exists it is authoritative: edits to the defaults are silently ignored, the build
succeeds, warns about nothing, and produces a binary carrying the old configuration. That cost
a full ten-board build to notice while restoring the four values above — they appeared to have
no effect whatsoever, including an `-Os` change that did not move the image size by a byte.

`build.sh` now deletes the generated `build/<board>/sdkconfig` when any input that produces it
(`sdkconfig.defaults`, `sdkconfig.defaults.<chip>`, `boards/<board>.sdkconfig`,
`boards/_*.sdkconfig`) is newer, and says so on stderr. Deleting it is safe here specifically
because this target never runs `menuconfig` — the generated file holds nothing a human authored.
A target that did edit its sdkconfig interactively would need a different fix.

### nRF52840 → Zephyr / nRF Connect SDK

The case for this is **stronger** once Arduino leaves the ESP32 side: keeping nRF52840 on
PlatformIO/Arduino would leave exactly one Arduino toolchain, for one chip, wrapping a
third-party fork (`maxgerhardt/platform-nordicnrf52`) of a platform wrapping the Adafruit nRF52
core wrapping the deprecated SoftDevice model.

**Why it fits.** The nRF52840 env's own `lib_ignore = Seeed_GFX, NimBLE-Arduino` says its scope
is *bb_epaper + BLE + config + transfers* — precisely the nRF54L15's scope, already implemented
in Zephyr. Moving it:

- leaves Zephyr's BT host as the single Nordic BLE stack (no Bluefruit);
- replaces `Adafruit_LittleFS`/`InternalFileSystem` and `Adafruit_nRFCrypto` +
  `nrf_cc310/crys_aesccm.h` (raw CC310 headers, visible in `encryption.cpp`) with Zephyr
  NVS/`settings` + PSA Crypto — the same APIs nRF54L15 uses;
- deletes an `#ifdef` arm from `encryption.cpp` (the other, mbedTLS + `esp_random`, becomes the
  IDF path);
- **makes nRF52840 a board, not a target.** `board_nrf54.c` already switches on
  `NRF54_BOARD_LM20` vs `NRF54_BOARD_L15`, and `zephyr/CMakeLists.txt` already selects conf
  files via `BOARD MATCHES`. A third Nordic board extends an existing pattern.

**What it costs.** A board port, but a real one: devicetree/overlay and pinctrl for the custom
nRF52840 board (replacing `variants/nrf52840custom`), a `prj.conf` for the nRF52 series,
re-verifying deep sleep and the `NRF_ADV_BOOST_*` advertising-interval logic against Zephyr's
BT API, replacing the UF2 packaging step (`scripts/nrf_uf2_post.py`) with MCUboot/`west sign`
or equivalent, and moving factory provisioning to the CMake path. **Flash-layout and DFU
compatibility with already-deployed nRF52840 units is the item most likely to bite** and needs
settling before the port starts.

**Verify** (I could not check these — no NCS on this box): that the pinned NCS version still
supports the nRF52 series (it should; nRF52 remains supported in NCS v3.x — the deprecated one
is the old nRF5 SDK), that PSA Crypto on nRF52840 provides AES-CCM and CMAC via `nrf_cc3xx`,
and that nRF52840 + Zephyr BT + the 512-byte MTU settings in `prj.conf` leave enough RAM
alongside the panel and inflate buffers.

### nRF54L15 → Zephyr / nRF Connect SDK

**No decision to make.** nRF54L is supported only by nRF Connect SDK — no Arduino core, no
PlatformIO platform. Keep `west` plus the existing `build.sh`/`flash.sh`/`ncs-env.sh` wrapper,
which already handles profile selection (`battery` | `uart`), board-conditional conf files,
factory-config generation, and NCS auto-detection.

Two things to fix rather than carry forward: **pin the NCS version** (`ncs-env.sh` globs
`~/ncs/v3.*` then `~/ncs/v2.*` and takes the first hit — fine for one dev, not reproducible
across machines or CI), and keep the RTT-vs-UART profile split, since the battery build has no
USB UART.

### EFR32BG22 → stay on Simplicity SDK + SLC

**Keep it. This is the one target where the current toolchain is already the right answer**,
and the only one that adds a third build system to the repo. Full target detail — file
inventory, budgets, build invocation, divergences — is in
[../targets/efr32bg22-slc/README.md](../targets/efr32bg22-slc/README.md); this section
covers only the toolchain choice.

The alternative worth naming is folding it into `targets/nordic-zephyr/` as a Silabs board —
Zephyr does support Series 2 SoCs, and recent Zephyr carries an HCI driver that wraps the
Silabs link-layer library. **Reject it, on RAM.** The chip has 32 KB, and the shipping
build already ends with static RAM at `__HeapBase = 0x200056b0` (~22 KB) and a 10.3 KB
heap — the entire 32 KB, accounted for. Zephyr's kernel plus the BT host does not fit in
what is left, and the current firmware is not merely bare-metal by preference: it has **no
kernel component at all** (`app_bm.c`, `sl_power_manager`, a superloop). Porting would mean
re-earning the memory budget from scratch to gain nothing the build system currently costs
us. Flash is nearly as tight: 236.8 KB of a 272 KB app region, ~87 % full.

What this costs the repo, stated plainly:

- **A third build system**, against ARCHITECTURE.md's "three toolchains remain three
  toolchains" non-goal — which this satisfies, but it is worth being honest that the
  vendor-split rationale that justifies two here justifies three.
- **`shared/` gets no Kconfig on this target.** The ESP-IDF/Zephyr argument that buffer
  sizes become declared Kconfig options with types and ranges does not extend to SLC.
  Here they are `target_compile_definitions` in `cmake_gcc/CMakeLists.txt`
  (`OD_ENABLE_RTT`, `OD_APP_VERSION`, …) and generated `-D`s. So `shared/` must keep taking
  its sizing from **plain preprocessor constants**, with Kconfig on the other two targets
  merely being how those constants get set. Do not design the `shared/` configuration
  surface around Kconfig.
- **A pinned SDK install is a hard prerequisite** — `slc generate` needs a full Simplicity SDK
  with `.slcc`/`.slce` metadata, which the source-only `-cp` tree does not carry. The unified
  repo therefore **requires `slt install simplicity-sdk` at the pinned version** and does not
  vendor the SDK (MIGRATION.md § "The Silabs SDK is not imported as-is"). This is the same
  class of prerequisite as IDF and NCS below, and the same answer applies: CI has to build it.

What the repo gets in return, and it is more than it looks:

- **`opendisplay_config_parser.c` (529 lines) already has zero vendor includes** — the only
  application file across the four repos that satisfies the `shared/` rule today, and the
  smallest of the three config parsers (`Firmware` 919 C++, `Firmware_NRF54` 696 C).
- **It is the constraint that keeps `shared/` honest.** Bare-metal, no threads, no blocking
  sleep, 32 KB, no Kconfig. Any `shared/core` API that assumes a scheduler, a heap it can
  lean on, or a 32 KB window buffer is excluded by this target before it ships — which is
  exactly what MIGRATION.md wants from putting Silabs third rather than last.

**The zlib window divergence is the concrete instance of that**, and it contradicts a note
carried at the workspace level. This target pins `OPENDISPLAY_ZLIB_WINDOW_BITS=9` — a
**512-byte** window, not the 32 KB that "existing targets pin 32 KB windows for legacy-client
compatibility" implies. 32 KB is a third of the chip's RAM; it can never exist here. So
window size is a genuine per-target parameter in `shared/core/od_zlib_inflate`, 512 B is its documented
floor, and the encoder side (`py-opendisplay`) must know which devices are 9-bit. Resolve
this in writing before the shared inflater is designed, not after.

**Verify at import time** (I could not check these here): that the panel set actually
overlaps — `opendisplay_epd_map.c` maps 66 panel ids to `bb_epaper` constants and whether
that is a subset or a superset of the other targets' maps decides how much of the map is
shareable; and that the PSA Crypto usage in `opendisplay_pipe.c` (AES-CCM + CMAC via
`psa_mac_compute`/`psa_import_key`) is API-identical to nRF54L15's, which would make
`od_hal_crypto` one implementation for two targets.

**Two mechanical traps** for whoever wires `shared/` into this target. First, adding `shared/`
and `third_party/` sources must go through the `.slcp`, never by hand-editing the generated
`cmake_gcc/opendisplay-bg22.cmake` — the next regen overwrites it. With the SDK installed at a
pinned version this stops being theoretical: regen goes from ~never to routine, so a hand edit
that skips the `.slcp` will be silently deleted rather than surviving indefinitely. Second, the
bundled OpenJDK 21 is not on `PATH`, so bare `slc` reports
`java: command not found`; that reads as a missing runtime but is only a PATH miss.
`bb_epaper` already has a `silabs_efr32_io.inl` backend, so the panel stack needs no new work.

## Where bb_epaper and uzlib live

Both are cross-target but they are **not** the same kind of dependency:

- **`third_party/bb_epaper/`** (top level, *outside* `shared/`). Its IO backends are selected by
  `#ifdef` inside `bb_epaper.cpp` and each includes vendor headers — so the file transitively
  references `driver/gpio.h`, `zephyr/*`, or `sl_*` depending on target. It can never satisfy
  the `shared/` rule and must not live there. Today `Firmware` pulls it from git via `lib_deps`
  while `Firmware_NRF54` and `Firmware_Silabs` each vendor a copy — one vendored copy resolves
  that split.

  **Correction, now that Silabs is in scope: there is no superset copy to vendor.** Upstream
  `~/bb_epaper` has `esp_idf/` but **no** `nrf54_zephyr_io.inl`, `nrf54_bbep_busy.inl`,
  `silabs_efr32_io.inl`, or `silabs_bbep_busy.inl`. Those four backends exist only in the
  `Firmware_NRF54` and `Firmware_Silabs` vendored copies, which in turn lack `esp_idf/`.
  Neither side is complete, so the vendored copy has to be **assembled**: upstream as the base
  plus the four downstream backend `.inl`s and their `#ifdef` arms in `bb_epaper.cpp`. Worth
  upstreaming them so this stays a checkout rather than a fork — check first whether upstream
  has since taken them.
- **`shared/core/od_zlib_inflate.{c,h}`**. Pure C, no vendor headers or heap dependency. C14
  collapsed the byte-identical target copies and made this the one canonical portable engine.

The CI boundary check in `.github/workflows/shared-boundary.yml` greps `shared/` only, so
`third_party/` is already outside its scope — but add a comment there recording *why*
`third_party/bb_epaper` is exempt, or someone will eventually "fix" it by moving it in.

## Consequences for the scaffold — applied

The scaffold has been updated to match this analysis. Recorded here so the reasoning stays
attached to the change:

1. **`targets/` layout** — `esp32-nrf52840-pio/` → `esp32-idf/`, `nrf54l15-zephyr/` →
   `nordic-zephyr/` (both now multi-board), `nrf52-sdk/` marked out-of-scope/EOL. *(Superseded
   2026-07-25: `targets/nrf52-sdk/` is now deleted outright, not marked — the legacy fleet is
   shipped and stays in `Firmware_NRF`. See MIGRATION.md § "Order and rationale" item 5.)*
2. **`README.md`** — target table rebuilt around the vendor split; `third_party/` added to the
   layout; "no PlatformIO, no Arduino" stated up front.
3. **`docs/ARCHITECTURE.md`** — the "one build system" non-goal reworded (this lands on *three*
   build systems chosen per vendor, not one); `shared/` documented as plain C with no `String`;
   the forbidden-header list extended with the IDF families (`driver/*`, `soc/*`, `hal/*`,
   `freertos/*`); the memory-sensitivity section pointed at the existing HAL embryos.
4. **`docs/MIGRATION.md`** — order changed to **ESP32 (IDF) → nRF54L15 (Zephyr) → Silabs →
   nRF52840 as a board on the Zephyr target**, with `nrf52-sdk` dropped. Silabs stays third
   rather than last for the reason given in § EFR32BG22: it is the constraint that keeps
   `shared/` honest, so it must bite before three targets bake in assumptions it cannot meet.
   nRF52840 goes last because it is the only step that is pure toolchain migration with no new
   capability — the one to defer if priorities shift. Also added: the three-phase ESP32 import
   (unchanged → `arduino_compat.h` shim → shim demolition) added, because "import unchanged, fix
   only build files" does not survive a framework change; concurrent-development-on-`Firmware`
   promoted to the top risk.
5. **`CLAUDE.md`** — toolchain section added; `third_party/` exemption explained so it does not
   get "fixed" into `shared/`.

One ARCHITECTURE.md non-goal is worth re-reading as confirmation rather than contradiction:
"rewriting working drivers" still holds — the ESP32 drivers keep their logic and change only
their IO calls, and the nRF52840 port reuses the nRF54L15 drivers.

## Risks

- **This lands the largest single piece of work on the most active codebase.** `Firmware` is
  under active development; a multi-week IDF port will accumulate brutal rebase conflicts
  against concurrent feature work. Either freeze feature work on `Firmware` for the duration,
  or accept that the port will be re-done against a moving target. Decide deliberately, up
  front — this is the risk most likely to derail the migration, and it is a scheduling problem,
  not a technical one. *(Decided 2026-07-25: frozen for the duration of the port —
  MIGRATION.md § "Risks to watch".)*
- **`String` removal touches 575 sites**, concentrated in the files whose logic is going to
  `shared/` anyway. High volume, low per-site difficulty, but it is where the hours go.
- **BLE is a genuine rewrite** (~450 lines, NimBLE C API). Everything else is mechanical or
  already IDF-level.
- ~~**This dev box can build exactly one of the three targets** (EFR32BG22). Install IDF and
  NCS~~ — **stale, corrected 2026-07-25: all three toolchains are installed** (§ "All three
  toolchains are installed on this dev box"). The risk does not disappear, it changes shape:
  local builds are now possible, so a broken target can be caught before pushing — but "it
  builds on Dave's box" is one machine with one set of versions, and the CI matrix
  (per-board `idf.py build` + a west build + the BG22 CMake build) is still what makes that
  reproducible for anyone else. Stand it up early regardless; the `shared/` boundary grep is
  necessary and nowhere near sufficient.
- **EFR32BG22 has no headroom for `shared/` to be careless.** ~35 KB of 272 KB flash free and a
  32 KB RAM budget already fully accounted for (~22 KB static + 10.3 KB heap). A shared
  implementation that costs a few KB more than the target's hand-written one is a build failure
  here, not a regression to tune later — so measure `.bss`/`.text` on BG22 after every subsystem
  swap, not at the end.
- **Seeed_GFX is only used by two S3 boards.** If the port stalls, ship the other eight and come
  back to it — do not let it block.

## Open questions

- **Is nRF52840 still in the product line, or is nRF54L15 its replacement?** Decides whether
  the Zephyr port is worth doing at all. *Answered 2026-07-25: shipped, yes — MIGRATION.md
  § "Deployed fleet status"; the port stays step 4.*
- **Are there deployed nRF52840 units that must accept OTA from the current UF2/Bluefruit flash
  layout?** If so, this constrains or blocks the Zephyr move. *Largely answered 2026-07-25:
  deployed units run the Adafruit UF2 bootloader with a working BLE DFU path, and moving them
  to Zephyr almost certainly means replacing the bootloader — physical access — see
  targets/nordic-zephyr/README.md § "Deployed nRF52840". Still open is the product call
  recorded there: migrate deployed units at all, or leave them on Arduino/Bluefruit while new
  production ships Zephyr + MCUboot.*
- **Does ESP32 LittleFS hold anything besides the config blob?** Decides whether
  `esp_littlefs` is needed at all or whether NVS covers it. *Largely mooted 2026-07-25 by the
  flash-and-reconfigure decision (MIGRATION.md § "Deployed fleet status"): stored data is not
  preserved across the transition, so nothing needs migrating out of LittleFS either way. What
  survives of the question is only whether the new IDF build wants a filesystem component at
  all — answer that at import.*
- **Is `Firmware_NRF` (legacy nRF52, bare Nordic SDK) still shipped?** If not, delete it from
  the plan rather than carrying it as a fourth target. *Answered 2026-07-25: yes — a handful of
  low-capability units, maintained in place in `Firmware_NRF` and never migrated here; they set
  `py-opendisplay`'s backward-compatibility floor. MIGRATION.md § "Order and rationale"
  item 5.*
- **Can `Firmware` feature work pause during the IDF port?** See the first risk. *Decided
  2026-07-25: frozen for the duration of the port — MIGRATION.md § "Risks to watch".*
- **What zlib window does the host encoder target per device?** Settled on the firmware side:
  the default is **9 bits / 512 B on every current target** (`od_zlib_inflate.h`), and targets
  *reject* streams declaring a larger window. The former `esp32-s3-E1004` exception is retired.
  The "32 KB for legacy-client compatibility" note was simply wrong and is corrected in the workspace
  `CLAUDE.md`; `opendisplay-protocol/docs/AUDIT_2026-07-19_SUMMARY.md:113` reached the same
  conclusion independently. *The contract half has since resolved too (2026-07-25): the host
  already encodes `windowBits = 9` unconditionally and rejects its own output otherwise
  (DESIGN_REVIEW_2026-07-25.md F5). What remains open is narrower — whether anyone ever wants
  >9 bits and where the
  per-device capability value lives, which is the capability-discovery problem, not a
  compression one. `shared/core/od_zlib_inflate` exposes the window as a per-target parameter with
  512 B as the floor (MEMORY_CONSTRAINTS.md).*
- ~~**Is `CMD_NFC_ENDPOINT` (TNB132M) going to any other target?**~~ **ANSWERED 2026-07-25 —
  it is already on two targets**, and the premise here ("only EFR32BG22 implements it") was
  wrong: `Firmware_NRF54/src/opendisplay_nfc.c` implements it as well, dispatched at
  `opendisplay_pipe.c:1325`. Placement decided with it: the `0x83` endpoint goes to
  `shared/core` behind `OD_NFC_ENABLE`, the IC drivers stay target-local, and the `0x2A` config
  packet is parsed unconditionally on every target because it is canonical schema. Full split
  in SHARED_API_DESIGN.md § "NFC: standard packet, optional support".
- **Who owns regenerating `cmake_gcc/opendisplay-bg22.cmake`?** Largely answered: the full
  Simplicity SDK 2025.12.2 **is** installed on the primary dev box, so regen is possible here
  once `slc` is pointed at it (`--sdk-package-path`, or a project `.slconf`). What is still open
  is whether that wiring is committed to the repo — until it is, `.slcp` changes keep carrying
  unverifiable hand-edits.
