# Vendored third-party code

Origin, revision, licence, and pruning for every tree under `third_party/`.

README.md § License requires this: *"Vendored third-party code keeps its own licence and its
own notice; do not relicense it on the way in. Record the origin and licence of each vendored
tree."* MIGRATION.md § "Risks to watch" adds the rule that decided the pruning below: *"Decide
what a blob's pruned form is before its first commit, not after it has been bumped twice."*

`third_party/` is deliberately **outside `shared/`** and is not scanned by the boundary check.
`bb_epaper` selects its IO backend by `#ifdef` and every backend includes vendor headers, so it
can never satisfy the `shared/` rule — but it is still *one* vendored copy for all targets, not
a per-target fork. Do not "fix" this by moving it into `shared/`.

---

## bb_epaper

> **RESOLVED 2026-08-04 -- bb_epaper needs NO Arduino surface on this target.** The open
> decision below ("replacing `esp_generic.inl` with an in-project backend as the other two
> targets already did") was taken: `targets/esp32-idf/panel/od_bbep.cpp` replaces the vendored
> `bb_epaper.cpp` and supplies our own IDF backend, `panel/od_bbep_idf_io.inl`. Consequences
> worth stating, because earlier notes across this repo assumed otherwise:
>
> * **Zero edits to vendored files.** The glue TU is replaced rather than its `#ifdef` chain
>   patched, so there is no fifth `OD-PATCH` and nothing new joins the re-verify list.
> * `arduino_io.inl` and `esphome_io.inl` are **never compiled**, and `bb_epaper.h`'s
>   `#include <Arduino.h>` sits behind `#ifdef ARDUINO`, which this build does not define.
> * Dropping the unused `BBEPAPER` C++ class removed the only consumer of `pinMode()` (13
>   calls) and `millis()` (6), which is why the backend contract is nine functions, not eleven.
>
> **So the FastEPD vendor adapter is FastEPD's alone.** Any text claiming the permanent adapter
> exists for both libraries, or that it must own `delay()`/`delayMicroseconds()`/`millis()`/
> `ledc_compat.h`, predates this and is wrong.
>
> **`third_party/bb_epaper/src` is no longer on the component include path** (2026-08-04).
> `main/CMakeLists.txt` grants it per-source; an unlisted file including `<bb_epaper.h>` fails
> to compile. Same for `third_party/FastEPD/src`. This is containment, not abstraction -- the
> destination is `od_hal_panel` / `od_panel_ops`.

> **Backend analysis:** [docs/BBEPAPER_IO_BACKENDS.md](../docs/BBEPAPER_IO_BACKENDS.md) covers how
> bb_epaper selects an IO backend, the five-function contract a backend must satisfy, a
> function-by-function comparison of `esp_generic.inl` against `arduino_io.inl` (the backend the
> shipped fleet runs), the full defect inventory for `esp_generic.inl`, and the open decision on
> replacing it with an in-project backend as the other two targets already did. Read it before
> adding a fifth patch below.

| | |
|---|---|
| Author | Larry Bank (BitBank Software, Inc.) |
| Upstream | https://github.com/bitbank2/bb_epaper |
| Vendored from | https://github.com/davelee98-creator/bb_epaper — a fork, **now fast-forwarded to upstream and carrying no unique commits** (verified: `git log upstream/main..main` is empty) |
| Revision | `5dccfbb` ("Added support for the Seeed reTerminal E1004 and its 13.3 Spectra6 1200x1600 panel") |
| Licence | **GPL-3.0-or-later** |
| Vendored | 2026-07-25 at the ESP32 phase-B import; **re-vendored 2026-08-05** (was `2ef09a1`, "Fixed 4.26 again") |

**Pruned on the way in — 6.6 MB → 492 KB.** Kept `src/`, the four `esp_idf/*.inl` backends,
`CMakeLists.txt`, `LICENSE`, `README.md`. Dropped the example projects under `esp_idf/`
(`Spectra6`, `mini_epaper_s3`, `hello_world_c`, `hello_world_cpp`), `Fonts/`, `examples/`,
`fontconvert/`, `imageconvert/`, and the `ch32v`/`macos`/`rpi`/`rpi_pico` ports. None is
reachable from a firmware build.

### Re-vendor 2026-08-05 — what the bump changed

Taken for `s3-e1004`: upstream added `EPD_SEEED_E1004` / `EP133_SPECTRA_1200x1600`, the 13.3"
Spectra6 panel that board needs and that no earlier revision had. 76 panels → 78.

**IT WAS NOT A DROP-IN. The backend contract changed twice**, and both breaks were compile
errors rather than silent behaviour changes, which is the only reason this was cheap:

1. **`iCS1Pin` was REMOVED** from `BBEPDISP`. `iCSPin` *is* CS1 now, and dual-controller panels
   are addressed through a new `cs_mode` field (`CMD_CS1` / `CMD_CS2` / `CMD_CS1_CS2`) instead
   of mutating `iCSPin` around each write. Both target backends set `iCS1Pin` and the Zephyr
   composite helpers restored CS through it; all of that is gone.
2. **`bbepWriteCmdData()` joined the contract** — `bb_ep.inl` calls it directly, so a backend
   without it no longer links. Both backends now implement it, modelled on `arduino_io.inl`:
   command byte with DC low then payload with DC high, all inside ONE CS assertion.

`cs_mode` gating went in at each backend's single CS seam rather than at the eight sites
`arduino_io.inl` repeats it across. Both treat `cs_mode == 0` as `CMD_CS1`, because a
memset-zeroed `BBEPDISP` — which is how both targets create theirs — would otherwise assert no
CS line at all and the panel would sit silent.

### Local patches — re-verified at the 2026-08-05 bump

**One of the three retired: upstream fixed it.** `bb_ep.inl` no longer defines
`epd42yr_init_full` twice, and the panel table references the surviving definition correctly.
The `OD-PATCH` is deleted rather than carried, and the re-verify list shrinks to two.

The other two are still unfixed upstream and are re-applied:

The tree does not compile as vendored. Two defects, each fixed with a one-line change tagged
`OD-PATCH` so a future bump can find them:

1. ~~`src/bb_ep.inl` defined `epd42yr_init_full` twice~~ — **FIXED UPSTREAM at `5dccfbb`; patch
   retired 2026-08-05.**
2. **`src/bb_epaper.h` declared `void delay(int)` while `esp_idf/esp_generic.inl` defines
   `void delay(long)`.** Two different overloads of the library's own function, so any
   `delay(uint32_t)` call inside the library was ambiguous. The declaration now says `long`.

Both are bugs in the library itself, not incompatibilities with this repo — worth reporting
upstream, and worth checking against `bitbank2/bb_epaper` to see whether the fork introduced
them. Re-apply or re-verify on every bump.

### `esp_idf/esp_generic.inl` is UNPATCHED and UNUSED (reverted 2026-08-03)

This file is byte-identical to upstream `~/bb_epaper/esp_idf/esp_generic.inl` (295 lines) and
**is not compiled.** Do not patch it. Do not read it to understand how this target drives a
panel — it does not.

The ESP32 target uses its own backend, `targets/esp32-idf/panel/od_bbep_idf_io.inl`, selected by
`targets/esp32-idf/panel/od_bbep.cpp` replacing `bb_epaper.cpp`'s 52 lines of glue rather than
by patching its `#ifdef` chain. That is why nothing here needs an `OD-PATCH` any more, and why
`bb_epaper.cpp` is excluded in `main/CMakeLists.txt`.

**It carried four `OD-PATCH` sites and they are all gone.** Kept for the record, because the
history is the argument for owning the backend rather than patching upstream's:

1. **`bbepInitIO()` was not re-entrant** — it called `spi_bus_initialize()` on every cold panel
   bring-up while the file contained no `spi_bus_free()` anywhere, so bring-up #2 could only
   `assert()`. `epdSessionAcquire()` re-acquires the panel per transfer, making the panic
   guaranteed on the first image upload after the boot screen. Invisible under Arduino, which
   used `src/arduino_io.inl` and shared the application's global `SPI` object, so
   `SPI.begin()`/`SPI.end()` happened to pair across the library boundary.
2. **The first fix for (1) was wrong**, and instructively so: guarding the block behind an
   "already initialised" flag stranded SCLK/MOSI, because project code revokes the pad routing
   between bring-ups (`configureDisplayPinsLowPower()` → `pinMode()` → `gpio_config()`) and
   `spi_bus_initialize()` is the only call that restores it. The peripheral stayed healthy and
   connected to nothing: every transfer returned `ESP_OK` and the screen never changed.
3. Two supporting edits — an `esp_log.h` include so the file could warn at all, and `#if 0`
   around its Arduino-core/libc reimplementation (`delay`, `millis`, `mymemset`, `i2str`, …)
   which collided with `compat/arduino_compat.h`.

None of that was the cause of the 16–21 s cold bring-up it was chased for; that was BUSY-wait
expiry inside `bb_ep.inl`, measured at 5 s a time. See
[docs/BBEPAPER_IO_BACKENDS.md](../docs/BBEPAPER_IO_BACKENDS.md) § 8.

**Why the file is reverted rather than deleted.** A re-vendor would restore it, so deleting it
makes the vendored tree diverge from upstream by omission — harder to verify than a file that
matches. Pristine and unused is a state a `diff` against upstream confirms in one command;
absent is not.

## FastEPD

| | |
|---|---|
| Author | Larry Bank |
| Upstream | https://github.com/bitbank2/FastEPD |
| Revision | `770e168d695e693688053560d9a8d296a0dbc967` ("changed to taskYIELD") |
| Licence | **Apache-2.0** |
| Vendored | 2026-07-25, at the ESP32 phase-B import |
| Scope | **PSRAM-mandatory** parallel e-paper: the ESP32-S3 LCD peripheral on the S3 boards, and the classic-ESP32 path on `esp32-wrover-e-n4r8` (esp32-resync branch) |

**Pruned on the way in — 13 MB → 380 KB.** Kept `src/`, `LICENSE`, `README.md`,
`library.properties`. Dropped `examples/` (8.2 MB), `Fonts/` (1.6 MB), `Linux/`, `fontconvert/`,
`imageconvert/`.

Apache-2.0 code vendored inside a GPL-3.0 work is fine; the combined work is GPL-3.0 and this
tree keeps its own licence and notice. Do not relicense it.

### ~~The vendored bb_epaper cannot build `s3-e1004` — wrong fork~~ — RETIRED 2026-08-05

**The E1004 board variant is retired. There is no `boards/s3-e1004.cmake` and none is wanted.**

This section described a real blocker that no longer has a subject. Kept as a short record
rather than deleted, because "why is there no fragment for this board" is a question the
repository will be asked again.

Both halves of the blocker dissolved on the same day, from opposite directions:

- **The fork requirement went away.** `bitbank2/bb_epaper` merged E1004 support upstream
  (`5dccfbb`, "Added support for the Seeed reTerminal E1004 and its 13.3 Spectra6 1200x1600
  panel"), so the `limengdu` fork this section pointed at is obsolete. The re-vendor at
  `5dccfbb` brought `EPD_SEEED_E1004` and `EP133_SPECTRA_1200x1600` into the tree, and the
  panel is handled by the core: the table entry carries `BBEP_7COLOR | BBEP_SPLIT_BUFFER` and
  `bb_ep.inl` drives the dual-CS refresh itself through `cs_mode = CMD_CS1_CS2`.
- **`BBEP_T133A01` and `e1004_write_stream_bytes()` no longer exist.** Upstream replaced that
  whole `#ifdef`-guarded stream with the `cs_mode` model, so the second half of the blocker --
  `SPI.writeBytes()` with nothing calling `SPI.begin()` -- has no code left to fix.

**And the board variant itself is retired**, so even the remaining work is moot. For the record,
had it not been: the source env `esp32-s3-E1004` differed from `esp32-s3-N32R8-extuart` by
exactly one build flag, `OPENDISPLAY_ZLIB_WINDOW_BITS=15`. Everything panel-specific -- pins,
controller type, and the second chip-select -- comes from the runtime config blob's
`DisplayConfig`, which carries `cs_pin_2` precisely for "panels split over 2 drivers
(dual-controller Spectra)". A fragment would have been three lines, not a driver.

**The panel support stays in the tree** and is not dead weight: it arrived with an ordinary
upstream re-vendor, not as E1004-specific work, and any future dual-controller Spectra panel
configured through `DisplayConfig` uses the same core path.

### FastEPD has no ESP-IDF IO backend — OD-PATCH applied

FastEPD picks its IO backend with `#ifdef ARDUINO` / `#elif defined(__LINUX__)`. There is no
third arm, and under ESP-IDF **neither macro is defined**. That is not a compile error: every
`it8951*` transport function in `FastEPD.inl` simply collapsed to an *empty body between two
`gpio_set_level()` calls*.

The consequences were invisible to the build and to the boot log:

- `it8951WriteCmdCode` / `it8951WriteData` / `it8951WriteNData` transmitted nothing
- `it8951ReadData` / `it8951ReadNData` returned 0, so the controller probe read all zeros
- the GPIO reset/busy/power handshake still ran, so the panel *looked* alive

`bbepInitIT8951` therefore "succeeded" against a panel that had never received a byte. Only an
IT8951-over-SPI panel is affected; the parallel ED103 path drives the S3 LCD peripheral and
never went through this code.

**The patch.** All 15 guarded SPI blocks become
`#if defined(ARDUINO) || defined(OD_FASTEPD_IDF_SPI)`, and
`targets/esp32-idf/main/CMakeLists.txt` sets `OD_FASTEPD_IDF_SPI=1` on `FastEPD.cpp` alone.
The Arduino branch is reused verbatim because `targets/esp32-idf/vendor/fastepd/SPI.h` provides exactly
that API over IDF's `spi_master`. Patched sites are marked `OD-PATCH` and all point at the
explanatory note on `it8951WriteData()`.

> **This section previously claimed "every affected guard" when only six were patched**, and
> `targets/esp32-idf/README.md` § "Known defects" was right to call the claim wrong. The six
> covered the command/data/read path — `it8951WriteCmdCode`, `it8951WriteData`,
> `it8951WriteNData`, `it8951ReadData`, `it8951ReadNData`, and `SPI.begin` in
> `bbepInitIT8951`. The **nine** in `it8951WriteFramebuffer{1,2,4}Bit` — three each: open
> transaction, `SPI.writeBytes(d, iPitch)`, close transaction — were missed, which is exactly
> the set that carries pixels. The result was a panel that accepted every command, performed a
> real refresh, reported success, and displayed whatever was already in its RAM.
>
> Completed 2026-08-04; the count above is now the measured total, not a summary word.
> Verified in preprocessed output rather than by reading the source: all three writers emit
> live `SPI.writeBytes` calls where they previously preprocessed to an empty row loop.

**Why not just define `ARDUINO`.** Tried, and rejected: it also selects FastEPD's Arduino
font/`Print`/`PROGMEM` surface (`pgm_read_*`, `memcpy_P`, `ledcAttach`, `TwoWire::setTimeout`)
— an entire Arduino core rather than an IO backend — and `bb_epaper` reads the same macro to
choose *its* backend and must keep the `esp_idf` one.

**Retiring it.** The upstream-shaped fix is an `esp_idf_io.inl` in FastEPD selected by
`ESP_PLATFORM`. When that exists, delete both the patch and the CMake define. Until then, note
that a vendored library's Arduino dependency does not end with the application port. `compat/`
was deleted on 2026-08-16; what FastEPD needs moved into `targets/esp32-idf/vendor/fastepd/` and
what bb_epaper needs into `targets/esp32-idf/panel/od_bbep_idf_io.inl`, each owned by the
backend that wants it.

---

## The Group5 collision — the same code, twice, under two licences

`bb_epaper` and `FastEPD` **both ship `Group5.cpp`, `Group5.h`, `bb_ep_gfx.inl`, `g5dec.inl`,
`g5enc.inl` and `arduino_io.inl`.** Same author, overlapping history — and the copies are *not*
identical. The licence headers differ:

```
bb_epaper/src/Group5.cpp:  SPDX-License-Identifier: GPL-3.0-or-later
FastEPD/src/Group5.cpp:    (no SPDX header; FastEPD as a whole is Apache-2.0)
```

`Group5.cpp` is the only overlapping file that is *compiled* (the `.inl` files are includes), so
it is the only one that produces duplicate symbols at link time.

**The source repo's answer was `-Wl,--allow-multiple-definition`**, with the comment *"FastEPD
and bb_epaper both ship bb_ep_gfx/Group5; we only use FastEPD buffer+update APIs."* That flag
tells the linker to silently keep whichever definition it saw first — which means the effective
G5 codec depends on link order, and a divergence between the two copies becomes a bug nobody can
see in the source.

**This repo does not carry that flag, and the collision no longer arises.** The ESP32 firmware
does not link bb_epaper: SPI panels are driven from the config's drive packets
(`shared/core/od_display.c`), and FastEPD is compiled only on FastEPD boards. So exactly one copy
of `Group5.cpp` and `bb_ep_gfx.inl` is ever built, FastEPD's, and `FastEPD.cpp` includes its
`bb_ep_gfx.inl` exactly as upstream does.

That matters beyond tidiness: `FastEPD.inl` declares `bbepSetPixel2Clr` and
`bbepSetPixelFast2Clr` extern and installs them as its 1-bpp pixel setters. While FastEPD's
drawing layer had internal linkage beside a linked bb_epaper, those references resolved to
bb_epaper's copies, which read the pointer as a `BBEPDISP`. They now resolve to FastEPD's own.
FastEPD's 1-bpp mode is not exercised by this firmware's upload paths and has not been run on
hardware either way.

bb_epaper stays vendored under `third_party/bb_epaper/`; whether any other target still needs it
is that target's question, not this one's.

---

## espressif/mdns — vendored under `targets/esp32-idf/components/mdns/`

**Not under `third_party/`, and that is deliberate.** It is an ESP-IDF *component*: it must sit
in a directory the IDF build system scans (`targets/esp32-idf/components/`) with its own
`CMakeLists.txt` and `Kconfig`, and it is ESP32-only by construction. It is recorded here
because this file is where vendored code is accounted for, not because of where it lives.

| | |
|---|---|
| Author | Espressif Systems |
| Upstream | https://github.com/espressif/esp-protocols — `components/mdns` |
| Registry | https://components.espressif.com/components/espressif/mdns |
| Version | **1.11.3** |
| Upstream commit | `1a6e7191044b2faed61ece3dc6b06112e838fa8d` (from the component's own `idf_component.yml`) |
| Archive | `espressif__mdns-v1.11.3.zip`, sha256 `4c2759bf02adbb1b97c81f39ef9c482dd60a4a5f155fd82586146e308535b2c0` |
| Licence | **Apache-2.0** (see `LICENSE` in the tree) |
| Vendored | 2026-08-04, at phase C step 9b-iv |
| Requires | ESP-IDF `>= 5.0`; this repo pins v5.5.4 |

**Why vendored rather than fetched.** ESP-IDF dropped `mdns` from its core components in v5.0,
so it exists only in the Espressif component registry. Taking it via `main/idf_component.yml`
would make the component manager — and therefore network access to the registry — a requirement
of every clean build and every CI run, and this build has no managed components at all today.
Vendoring keeps builds offline and byte-reproducible, at the cost of a copy to bump by hand.
Decided 2026-08-04.

**Pruned on the way in — 1.3 MB → 640 KB.** Kept the sources, `include/`, `private_include/`,
`CMakeLists.txt`, `Kconfig`, `idf_component.yml` (the build reads it for `COMPONENT_VERSION`,
which becomes `ESP_MDNS_VERSION_NUMBER`), `LICENSE`, `README.md`, `CHANGELOG.md`. Dropped
`examples/`, `tests/` (host tests, unit tests, test apps — ~600 KB), the upstream refactoring
notes (`refactor_2025.md`, `refactoring_details.md`, `mdns_diagram.md`), `.cz.yaml`, and
`mem_prefix_script.py`. None is reachable from a firmware build.

**No local patches.** The tree is byte-for-byte upstream 1.11.3 minus the pruning above. Keep it
that way: configuration belongs in `sdkconfig.defaults`, not in edits here.

**This device ADVERTISES ONLY — it never resolves.** There is no upstream build flag for that
(the querier and browser compile in regardless), so it is expressed in configuration: see
`sdkconfig.defaults` for the responder-only settings and what each one is worth.

---

## qrcode

| | |
|---|---|
| Upstream | https://github.com/ricmoo/QRCode (Richard Moore) |
| Licence | **MIT** (stated in `qrcode.h`: "Minimal QR header (MIT) derived from ricmoo/QRCode") |
| Form | A trimmed encoder: byte mode, ECC level M, versions 1-10 only (`qrcode.c` lookup tables) |
| Arrived | `1f30d36` ("feat: unify boot screen rendering", 2026-08-18) |
| Used by | the shared boot screen's QR code (`shared/core/od_boot_screen.c`), on every target |

**Open item: the MIT licence text and copyright line are not in the tree.** MIT requires both
to accompany copies. Neither `qrcode.c` nor `qrcode.h` carries them, and no upstream revision
is recorded. Before a release: add upstream's `LICENSE.txt` (Copyright (c) 2017 Richard Moore)
as `third_party/qrcode/LICENSE` and record the revision here. Not done here because the
revision this was derived from is not known; see docs/RELEASE_CHECKLIST_3.0.md.

## boot_logo

| | |
|---|---|
| Content | `od_logo.svg` (the OpenDisplay logo), `convert_logo.py` (SVG to packed 1-bit C header), `logo_bitmap.h` (its generated output) |
| Origin | OpenDisplay project artwork and tooling, not third-party code |
| Licence | **Not stated in the tree.** The code is presumably GPL-3.0 with the rest of the repo. The logo is a project mark, and its terms of use are the maintainer's to state (maintainer question) |
| Arrived | `1f30d36`, 2026-08-18 |

`logo_bitmap.h` is generated and must not be hand-edited. Regenerate it with
`convert_logo.py` (needs Pillow plus cairosvg or ImageMagick).

## uzlib / tinf (inflate)

`shared/core/od_zlib_inflate.c` is "based on uzlib/tinf" (its header). uzlib (Paul Sokolovsky)
and tinf (Joergen Ibsen) are under the **zlib licence**. That licence asks that altered source
be plainly marked as altered and that the notice not be removed. The file says it is based on
uzlib/tinf but carries **no copyright notice or licence text** (open item, listed in the
release checklist). No other uzlib copy is in the tree.

## Linked from toolchains, not vendored here

Release images contain code that is not in this repository but is linked from the pinned
SDKs. It keeps its own licence:

| Component | In which images | Licence |
|---|---|---|
| MCUboot (via nRF Connect SDK v3.3.1) | every Nordic `-merged.hex` and the nRF52840 `usb-2-install.zip` | Apache-2.0 |
| Zephyr RTOS, nRF Connect SDK (SoftDevice Controller, nrfx, Oberon PSA crypto) | every Nordic image | Apache-2.0 and the LicenseRef-Nordic-5-Clause terms of their sources |
| ESP-IDF (bootloader, NimBLE, mbedTLS, FreeRTOS) | every ESP32 image | Apache-2.0 (FreeRTOS: MIT) |
| Simplicity SDK (Bluetooth stack, Gecko bootloader, emlib, SEGGER RTT when enabled) | every BG22 image and `opendisplay-bg22-bootloader.s37` | Silicon Labs MSLA / Zlib / SEGGER RTT BSD-style, per file |
| Adafruit nRF52 Bootloader | not shipped. It is the bootloader already on the device; the nRF52840 USB packages are written through it | MIT |

A release ships `LICENSE` and this file. Notices for the SDK components are in the SDK sources
at the pinned versions (docs/TOOLCHAINS.md).
