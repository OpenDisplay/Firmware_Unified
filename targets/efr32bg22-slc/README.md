# Target: EFR32BG22 (Simplicity SDK + CMake)

Source repo: `Firmware_Silabs` (https://github.com/davelee98/Firmware_Silabs)
Surveyed at commit `d1a866d` (2026-07-25). **Imported 2026-08-05 at `17a8222`** — a
descendant of the surveyed commit, and one commit behind `OpenDisplay/Firmware_Silabs`.

## Status (updated 2026-08-13) — builds; not hardware-verified

Read this before anything below it; several statements further down were written
before the import and are corrected here.

| | |
|---|---|
| Source repo builds on this box | **yes**, clean: `text 236316  data 492  bss 31792` |
| This target builds | **yes**, clean: `text 249892  data 492  bss 31792`, `heap_size 0x2cd0` (`./build-and-flash.sh --no-flash`) |
| Hardware-verified | **no** — nothing here has been flashed to a board |
| Takes from `shared/` | seven tiers it calls — PURE + HAL_CRYPTO + HAL_RADIO + APP_SESSION + APP_INFLATE + APP_XFER + APP_NFC — **plus HAL_LOG compiled capability-off** (`OD_CAP_LOG=0`), which links no logger symbol or state. Declines APP_RXQ, HAL_ADV and HAL_WDT. See [below](#what-this-target-takes-from-shared-today) |
| Blocked by | *(resolved)* `bb_epaper` had no EFR32 IO backend and no `sendPanelInitFull()`; fixed by the target-owned `panel/od_bbep_efr32.cpp` adapter, `third_party/` unedited |
| Also missing | *(resolved)* SEGGER RTT and `app_properties` come from the pinned SDK; neither is vendored |

The rest of this section is the record of how it got there, kept because the failures it
describes are the ones the *next* target import will hit.

The 179/181 figure comes from a scratch probe that temporarily staged the two
un-imported vendored trees; it is evidence about the SDK repoint, **not** a build. The
two failures were:

```
third_party/bb_epaper/src/../esp_idf/esp_generic.inl:9:10:
      fatal error: esp_timer.h: No such file or directory
opendisplay_display.cpp:705:9:
      error: 'class BBEPAPER' has no member named 'sendPanelInitFull'
```

Both are the bb_epaper reconciliation the nRF54 import already recorded, arriving here
for the second time. The first says the shared vendored copy falls through its `#ifdef`
chain to the ESP-IDF backend because no EFR32 arm exists — `silabs_efr32_io.inl` and
`silabs_bbep_busy.inl` live only in `Firmware_NRF54/third_party/bb_epaper/src/`. The
second is the same `sendPanelInitFull()` that commit `5068c29` established was the
nRF54's *local* addition to the vendored class: this target depends on it too, and the
shared copy does not have it. The shape of the fix is already set by the other two
targets — a target-owned `panel/` backend, not an edit to `third_party/`.

## What is imported, and what is deliberately not

| | |
|---|---|
| Imported | 27 flat application sources, `qr/`, `config/`, `cmake_gcc/`, the `.slcp`/`.pintool`, `build-and-flash.sh`, `AGENTS.md` |
| Not imported: the SDK | `simplicity_sdk_2025.12.2/`, 57 MB / 658 files — the documented deviation. Replaced by the pinned install |
| Not imported: `third_party/` | all four trees. `bb_epaper` and `uzlib` are vendored once at repo level; `segger_rtt` and `silabs_app_properties` **have no home yet** |
| Not imported: `include/` | the local protocol-header copies; `shared/protocol/` is the single reference |
| Imported later: the bootloader blob | `bootloader-artifact/opendisplay-bootloader.s37`, 747 KB, byte-identical to Firmware_Silabs (provenance in its README); needed for virgin-board release images |

### The two homeless vendored trees

`third_party/segger_rtt/` (5 files, the RTT log transport) and
`third_party/silabs_app_properties/` (2 files, `app_properties.c` — which the Gecko
Bootloader reads to validate the application, so it is required for `.gbl` OTA, not
optional) exist in the source repo and have **no top-level home in this repo**. The
migration plan does not mention either.

Both placements are defensible — promote to repo-level `third_party/`, or keep them as
a target-owned vendor directory in the shape of `targets/esp32-idf/vendor/fastepd/` —
which is exactly why this is recorded as an open decision rather than settled by
whichever directory was convenient. The build fails on each with an explicit
`FATAL_ERROR` naming this paragraph, not with "cannot find source file".

## autogen/ is not entirely generated — and regenerating it is dangerous

Found 2026-08-05 by regenerating against the installed SDK and diffing. This is not in
any plan document, and it contradicts the expectation (MIGRATION.md § "The Silabs SDK is
not imported as-is", reason 3) that installing a real SDK simply "fixes the regen
hazard". It restores `slc generate` — but three files under `autogen/` carry the
"Automatically-generated file. Do not edit!" banner and **have been hand-edited anyway**,
and the generator silently reverts both edits:

1. **`autogen/linkerfile.ld`.** Checked in: `FLASH ORIGIN = 0x12000, LENGTH = 0x44000`,
   `RAM ORIGIN = 0x20000004`, plus a `BOOTLOADER_RESET_REGION` and a
   `.bootloader_reset_section`. Regenerated: `FLASH ORIGIN = 0x0, LENGTH = 0x56000`, no
   reset region. **A regenerated build links over the Gecko Bootloader and AppLoader** —
   destroying the `.gbl` OTA path, which is the only field-update path any OpenDisplay
   target actually has (MIGRATION.md § "Deployed fleet status", consequence 4).
2. **`autogen/gatt_db.c` and `autogen/gatt_db.h`.** Checked in: "Minimal static GATT: GAP
   (0x1800) + Device Name only", 53 lines. Regenerated from the checked-in
   `config/btconf/gatt_configuration.btconf`: 110 lines with the stock Silabs Device
   Information Service restored, and **`gattdb_device_name` moves from handle 3 to
   handle 11** — wire-visible on shipped hardware.

Two consequences:

- This repo's `.gitignore` already ignored `autogen/` for this target, written before the
  import. That rule is right about 18 of the 21 files and **wrong about these three**,
  which exist nowhere else and would have been lost. They are now committed through
  targeted negations; the other 18 stay ignored.
- `tools/slc_regenerate.sh` runs `slc generate` into a temporary directory, copies back
  only `autogen/`, and then **restores the three protected files from git**, leaving the
  generator's versions in the temp directory for inspection. Do not replace it with a
  plain `slc generate` in place: that also deletes the hand-added display/panel/QR/
  compression sources from `cmake_gcc/opendisplay-bg22.cmake`.

The proper fix is to move both edits somewhere a generator does not shadow them — a
project-owned linker script, and a `.btconf` (or dynamic GATT) that actually describes
the shipped database. Until then the banner is a lie and the protections are what stand
between a regen and a bricked OTA path.

## The protocol header hazard, restated — the source repo is at 2.1

The section near the end of this file describes **two** disagreeing in-tree copies, a
current one at the repo root and a stale 2.1 one in `include/`. At `17a8222` **the root
copy no longer exists** — `refactor/vendored-headers-phase0` deleted it — so `include/`
is the only copy and the source repo compiles against **protocol 2.1**. This target now
compiles against `shared/protocol/`, which is **2.2 plus unreleased additions**.

Measured, the delta is purely additive: `OD_PROTOCOL_VERSION_MINOR` 1u → 2u,
`OD_PROTOCOL_VERSION_STR` "2.1" → "2.2", and six new constants (`OD_BLE_MAX_FRAME`, the
five `OD_LAN_*`). No existing constant changes value, and `opendisplay_structs.h` is
byte-identical. The version string is the one behaviour change, and it is a change: this
firmware will report 2.2 where it used to report 2.1.

The smallest target by a wide margin and the one that constrains `shared/` the most. Read
this before designing anything in `shared/core` — several of the
memory rules in ../../docs/ARCHITECTURE.md exist because of this chip.

## Hardware and budgets

| | |
|---|---|
| Part | `EFR32BG22C222F352GM40` — Cortex-M33, 352 KB flash, **32 KB RAM** |
| App flash region | `0x12000`–`0x56000` = **272 KB** (bootloader + Gecko apploader occupy the first 72 KB) |
| RTOS | **none** — bare-metal superloop (`app_bm.c` + `sl_power_manager`); no kernel component in the `.slcp` |
| Radio | Silabs BLE stack (`sl_bt_*` BGAPI), peripheral role only |
| Log | SEGGER RTT (`od_rtt.c`, J-Link required); no UART console |

**Occupancy at 2026-08-22** (`arm-none-eabi-size` and the link map on
`cmake_gcc/build/base/opendisplay-bg22.out`):

```
text 249892   data 492   bss 31792
```

- Flash: 244.5 KB of 272 KB → **~90 % full, ~27.5 KB headroom.**
- RAM: static ends at `__HeapBase = 0x20005330` → **20.8 KB static, 11.2 KB heap**
  (`heap_size = 0x2cd0`), and the two together are the whole 32 KB. There is no slack.

**`data + bss` IS NOT A BUDGET ON THIS PART.** `.memory_manager_heap` is sized by the linker to
fill whatever RAM is left, so that total is 32,284 B whatever the static footprint does — it has
not moved across several promotions that changed static RAM by hundreds of bytes. The figure that
moves, and the one plans gate on, is `heap_size` (`heap_limit − __HeapBase`).

Largest single RAM consumers in the application's own code, from the current map: `s_cfg` (2064 B,
config storage record), the `od_zlib_stream` state `s` (1676 B), `s_od_global_config` (909 B),
`s_ring` (804 B), `s_buttons` (640 B), `s_nfc` (528 B, the shared NFC chunk assembler). Anything
`shared/` adds competes directly with these.

## Layout of the source repo

Application sources are flat at the repo root; everything else is SDK, generated, or
vendored.

| File | Lines | Notes |
|---|---:|---|
| `opendisplay_ble.c` | 1977 | GATT service, advertising, connection handling, buttons, battery (IADC), NFC endpoint |
| `opendisplay_pipe.c` | 1299 | command dispatch over the PIPE characteristic + session auth (PSA AES-CCM / CMAC) |
| `opendisplay_display.cpp` | 894 | panel driving via `bb_epaper`, streamed inflate, QR — **the only C++ application file** |
| `opendisplay_config_parser.c` | 529 | config TLV parsing |
| `opendisplay_config_storage.c` | 139 | NVM3-backed config blob |
| `opendisplay_led.c` | 485 | LED patterns |
| `opendisplay_epd_map.c` | 75 | 66 panel-id → `bb_epaper` panel constants |
| `shared/core/od_color.c` | — | shared direct-stream color geometry |
| `main.c`, `app.c`, `app_bm.c` | 269 | SDK entry + bare-metal loop |
| `shared/core/od_zlib_inflate.c` | — | canonical bit-serial streaming inflate |
| `third_party/bb_epaper/` | — | vendored, with `silabs_efr32_io.inl` + `silabs_bbep_busy.inl` backends |
| `third_party/segger_rtt/` | — | RTT log transport |
| `third_party/qrcode/qrcode.c` | 529 | canonical QR encoder shared by all targets |

~20.9 k lines total including vendored third-party, of which the OpenDisplay-authored
application is roughly 5.5 k.

## Build

SLC-managed project (`opendisplay-bg22.slcp`) with a **generated** CMake file
(`cmake_gcc/opendisplay-bg22.cmake`, 386 lines) and a hand-written wrapper
(`cmake_gcc/CMakeLists.txt`, 148 lines) that adds the `OD_*` options.

```bash
./build-and-flash.sh --no-flash            # build only
./build-and-flash.sh --no-flash --no-ota-image --no-bootloader --no-artifacts
./build-and-flash.sh                       # build + J-Link flash + .gbl OTA image
```

Project options exposed by the wrapper: `OD_ENABLE_RTT`, `OD_GENERATE_GBL_OTA`,
`OD_APP_VERSION`, `OD_SL_APPLICATION_VERSION`, `OD_TNB132M_BOOT_PROBE`,
`OD_TNB132M_WRITE_NDEF`, `OD_TNB132M_NDEF_TEXT`.

**Application version (app properties).** `SL_APPLICATION_VERSION` is derived from
`OD_FW_VERSION` (else `OD_APP_VERSION`) as `(major << 16) | (minor << 8) | patch` — 3.0.0 is
196608. Firmware_Silabs 2.x used `(major << 8) | minor` (2.3.0 = 515, max 65535), so 3.x is
always above the 2.x fleet. The shipped bootloader (`bootloader-artifact/`) is built with
`BOOTLOADER_ROLLBACK_PROTECTION 0`, so no version is refused on OTA today; the encoding only
keeps that safe if rollback protection is ever enabled. `OD_SL_APPLICATION_VERSION` still
overrides. A release build must pass `OD_FW_VERSION=<tag>` so the number tracks the tag.

**`slc generate` needs to be pointed at a real SDK.** It requires component metadata
(`.slcc`/`.slce`); the source-only `-cp` copy in the source repo at `simplicity_sdk_2025.12.2`
has zero `.slcc` files, so a bare `slc generate` fails there with "no valid sdk could be
loaded", and `cmake_gcc/opendisplay-bg22.cmake` is hand-maintained in that repo as a result.
The SDK it needs *is* installed on this box (below) — the failure is unregistered tooling, not
a missing download. Note the CMake build itself needs neither `slc` nor the SDK metadata, so it
keeps working either way.

> **CORRECTED 2026-08-05, by doing it.** This paragraph used to claim "this target does not
> inherit that hazard", because the pinned install "restores `slc generate` and makes the CMake
> file genuinely generated again". Half of that is now verified and half is now known to be
> wrong.
>
> **Verified:** `slc generate -p opendisplay-bg22.slcp -s <installed SDK> -nocp` succeeds
> against the pinned 2025.12.2 install with no `.slconf` and no registration — it needs only
> `-s`. It emits the full `autogen/` including `gatt_db.c` and `linkerfile.ld`, and its
> `cmake_gcc/opendisplay-bg22.cmake` uses `${SDK_PATH}/...` with SDK_PATH pointing at the
> install, which is what made the repoint in this target a mechanical rewrite rather than a
> guess. All 241 SDK source paths were checked to exist in the install at byte-identical
> relative paths. `tools/slc_regenerate.sh` wraps this.
>
> **Wrong:** the CMake file is *not* safely regenerable, and regenerating is now known to be
> actively dangerous. `slc generate` emits only what the `.slcp` lists, so it drops the
> hand-added display/panel/QR/colour/compression sources and rewrites application paths as
> absolute machine paths — and it reverts three hand-edits inside `autogen/`, one of which
> relocates the image over the bootloader. See § "autogen/ is not entirely generated" above.
> The hand-maintained hazard is deeper than the migration docs record, not resolved by the
> install.

Toolchain locations on this machine are recorded in ../../docs/TOOLCHAINS.md.

**Required: the SDK is a pinned install. The vendored tree is not imported.**
`simplicity_sdk_2025.12.2/` is 57 MB across 658 git-tracked files (~59 % of the 97 MB source
repo), it is a *generated* `slc generate --copy-sources` artifact, and it carries no `.slcc`
metadata so it cannot drive `slc generate`. Every clone of this repo would pay for it, including
people who only touch the ESP32 target. Install instead:

```bash
slt install simplicity-sdk        # pin 2025.12.2 — the version the tree used
slt where simplicity-sdk          # → SDK_PATH for the CMake build
```

The exact pinned version is available — `slt list simplicity-sdk --versions` (checked
2026-07-25) returns 2024.12.1, 2025.6.0, 2025.6.2, 2025.6.3, 2025.12.0, 2025.12.1, **2025.12.2**,
2025.12.3, 2026.6.0 — and `slt` is already the mechanism delivering ARM GCC, CMake, and
Commander, so this is
incremental to an install that was always mandatory, not a new prerequisite. The payoff beyond
size: a real SDK has the component metadata `slc generate` needs, so
`cmake_gcc/opendisplay-bg22.cmake` becomes genuinely generated again rather than hand-maintained
— which matters most during this migration, since adding `shared/` and `third_party/` sources
goes through the `.slcp`.

Rationale and the required install-verify-then-delete sequencing are in ../../docs/MIGRATION.md
§ "The Silabs SDK is not imported as-is". Do not delete the vendored tree in the source repo
until `slc generate` and a clean build are both verified against the installed SDK. If that
verification fails, escalate — falling back to vendoring reverses a recorded decision and needs
an explicit call.

## What this target contributes to `shared/`

- **`opendisplay_config_parser.c` has zero vendor includes today.** It is the only
  application file in any of the four repos that already satisfies the `shared/` rule
  as-is. At 529 lines it is also the smallest of the three config parsers
  (`Firmware` 919 C++, `Firmware_NRF54` 696 C) — the tightest starting point for
  `shared/core`, though it must be checked for missing features before it is treated as the
  donor rather than merely the cleanest.
- **`shared/core/od_zlib_inflate.c` is the reference streaming inflate** for a target with no room for a
  window buffer (see the divergence below).
- **The bare-metal constraint is the useful one.** This target has no threads, no
  scheduler, and no blocking `k_sleep`. Any `shared/core` API that assumes it can block, own
  a task, or be re-entered from another context excludes this target. This is why there is
  **one** execution model rather than a threaded and a single-threaded variant — see
  ../../docs/ARCHITECTURE.md § "One execution model". The rules there are derived from this
  chip; the ESP32 target already satisfies them by accident (it creates no FreeRTOS tasks of
  its own) and must not lose that during the IDF port.

## Divergences to resolve deliberately

Per ../../docs/MIGRATION.md's "silent behavioural divergence" risk — these are the ones
already visible, before any code moves:

1. **Zlib window is 512 B, not 32 KB.** The generated CMake pins
   `OPENDISPLAY_ZLIB_WINDOW_BITS=9`. The workspace-level note that "existing targets pin
   32 KB windows for legacy-client compatibility" is **not** true of this target — a 32 KB
   window is a third of the chip's RAM and cannot exist here. Any stream this device must
   accept has to be encoded with a ≤512 B window. `shared/core/od_zlib_inflate` treats window size
   as a first-class per-target parameter with this as the documented floor, and the encoder
   side (`py-opendisplay`) has to know which devices are 9-bit.
2. **No PIPE sliding-window transfer.** Despite the file name, `opendisplay_pipe.c`
   implements the PIPE *characteristic* (command channel), not the `0x80`–`0x82`
   sliding-window image path in ARCHITECTURE.md's `shared/core` list. Opcodes handled here:
   `AUTHENTICATE`, `CONFIG_READ`/`CONFIG_WRITE`/`CONFIG_CHUNK`, `DIRECT_WRITE_START`/
   `_DATA`/`_END`, `PARTIAL_WRITE_START`, `HEADER_SIZE`, `FIRMWARE_VERSION`, `READ_MSD`,
   `LED_ACTIVATE`/`LED_STOP`, `BUZZER`, `NFC_ENDPOINT`, `DEEP_SLEEP`, `POWER_OFF`,
   `REBOOT`, `ENTER_DFU`. So `shared/core`'s transfer state machines must be selectable —
   a target that does not implement PIPE must not pay for its reorder queue in flash or
   RAM.
3. **`CMD_NFC_ENDPOINT` / TNB132M — decided 2026-07-25, and this is *not* the only
   implementation.** The earlier "Silabs-only" claim here was wrong: `Firmware_NRF54` implements
   the same endpoint (`src/opendisplay_nfc.c`, dispatched at `opendisplay_pipe.c:1325`), so the
   logic is duplicated across two targets today. It lands in `shared/core` behind
   `OD_NFC_ENABLE`; the TNB132M I2C driver stays here in `targets/efr32bg22-slc/`. Note the flag
   does **not** gate config parsing — `0x2A` is canonical schema and every target parses it.
   See ../../docs/SHARED_API_DESIGN.md § "NFC: standard packet, optional support".
4. **`opendisplay_display.cpp` is C++ while everything around it is C**, purely because
   `bb_epaper` is a C++ class. Consistent with `Firmware_NRF54`, and fine — but it means the
   `od_hal_panel` boundary is where C++ has to stop.

## Protocol-header hazard (specific to this repo)

There are **two** in-tree copies and they do not agree:

- `Firmware_Silabs/opendisplay_protocol.h` (root) — the copy the sync tool maintains, and
  the one the compiler actually resolves (quoted include from a root-level source). One
  push behind canonical: missing the unreleased `OD_BLE_MAX_FRAME` addition.
- `Firmware_Silabs/include/opendisplay_protocol.h` — **stale at protocol 2.1** (canonical is
  2.2), missing all of SECTION 9. It is unmaintained by the sync tool, yet it is the path
  the `.slcp` declares for the header, and `../include` is on the include path. Nothing
  compiles it today only because `../.` precedes it and quoted includes hit the source's own
  directory first.

`opendisplay_structs.h` has the same duplication but both copies match canonical.

Do not carry the duplicate into `targets/efr32bg22-slc/`. On import, keep exactly one
reference — `shared/protocol/` — and delete the local copies. (Related open item: the sync
tool's copy map still does not list `Firmware_Unified/shared/protocol/` at all.)

## What must be installed to build this

See ../../docs/TOOLCHAINS.md § EFR32BG22. Short version: ARM GCC 12.2.Rel1, CMake, Ninja,
Simplicity Commander (for `.gbl` + flashing), and **the pinned Simplicity SDK 2025.12.2 via
`slt install simplicity-sdk`** — required, not optional, because this target does not vendor
the SDK. Regenerating the CMake additionally needs `slc-cli` and the bundled Java 21 (present
but **not on `PATH`**; prepend
`~/.silabs/slt/installs/archive/java21-v21.0.5/java21_linux_x86_64/jre/bin` before running
`slc`).

**On this dev box the SDK is already installed** (verified 2026-07-25): full Simplicity SDK
`2025.12.2` at `~/.silabs/slt/installs/conan/p/simpl965e19baece23/p` — 2892 `.slcc` files,
2.4 GB, resolvable via `slt where simplicity-sdk`. It is a real SDK with component metadata,
not a `-cp` copy, so the pinned-install requirement is already satisfied here.

What is **not** yet done is wiring `slc` to it: there is no `.slconf` in the project and
`~/.uc/cli/cli.config` registers no SDK, so `slc generate` must be given
`--sdk-package-path <the path above>` (or a project `.slconf`) to find it. That is a
configuration step, not an install — do not record it as a missing prerequisite.

## What this target takes from `shared/` today

The **PURE tier only** — `core/od_config_asm.c` and `core/od_config_tlv.c` — taken as
`${OD_SHARED_SOURCES_PURE}` from `shared/sources.cmake`, which
[cmake_gcc/opendisplay-bg22.cmake](cmake_gcc/opendisplay-bg22.cmake) includes. Same shape as
`targets/nordic-zephyr`: **compiled, not yet called** *at the time of that step*. Superseded — this
target now calls the shared config, session, transfer, PIPE and NFC machines; see the summary table
above for the tiers it takes today. The measurement below is that step's, not current occupancy.

It cost nothing measurable: identical `text`/`data`/`bss` before and after (240680 / 488 /
31796), because `-ffunction-sections -fdata-sections` + `--gc-sections` drop what nothing calls
and neither TU defines a static instance. **Adopting `od_config_asm` for real is the expensive
step, not this one** — `struct od_config_asm` carries a 4096-byte buffer (decision 12,
`MAX_CONFIG_SIZE` is global) on a 32 KB part, so whoever declares the instance owns that budget;
see [Hardware and budgets](#hardware-and-budgets) before siting it.

The include path takes the whole `OD_SHARED_INCLUDE_DIRS`, not `shared/protocol` alone. The
include-order hazard `shared/sources.cmake` warns about — a target's own subset headers shadowing
the canonical wire contract, silently changing `sizeof`-derived size tables — cannot arise here
because the local `include/` pair was never imported. Re-importing it would reintroduce the
hazard; don't.

## Required before the next tier: the advertising HAL

`shared/core/od_adv_control.c` is in the HAL_ADV tier, deliberately **not** taken above, and
`od_watchdog.c` (HAL_WDT) likewise: this target implements neither
[od_hal_adv.h](../../shared/hal/od_hal_adv.h) nor [od_hal_wdt.h](../../shared/hal/od_hal_wdt.h).
Taking HAL_ADV without supplying `od_hal_adv_{program,start,stop}` fails at link with three
undefined references, which is the intended failure. Add each tier when its HAL lands, not
before.

No stub adapter is committed as a placeholder: a stub that satisfies the linker while doing
nothing would make advertising *look* wired on a target where it isn't. The adapter lands with
the tier.

**This target is the proof that the controller needs no kernel**, which is why it matters more
here than anywhere else. `od_adv_control` is run-to-completion, statically allocated, allocates
nothing and blocks on nothing, so the adapter is:

- record BGAPI facts during the event callback — connection count, advertising ended, stack
  ready — and return;
- run `od_adv_process()` at the tail of the same superloop pass, after the handler returns;
- keep AD-record construction and `sl_bt_legacy_advertiser_set_data` in the adapter.

No RTOS queue, no work item, no synthetic task. If a future change to `shared/` makes that
impossible — a scheduler, a heap allocation, a blocking wait, or a large automatic buffer —
that is a defect in `shared/` to be corrected there, not worked around with a Silabs fork.
