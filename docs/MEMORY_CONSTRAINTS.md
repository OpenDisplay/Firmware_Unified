# Memory, portability, and dependency policy

What in `shared/core` risks not fitting the EFR32BG22, with the numbers, and the dependency
rules for the two Arduino panel libraries (bb_epaper, FastEPD) under ESP-IDF. Companion to
SHARED_API_DESIGN.md and DIVERGENCE_MATRIX.md; everything here is measured from the checked-in
builds, not estimated.

## The BG22 budget (measured)

From `arm-none-eabi-size` on the shipped `cmake_gcc/build/base/opendisplay-bg22.out` and the
`.map`:

```
text 236316   data 492   bss 31792     (bss as reported includes the NOLOAD heap)
```

- **RAM: 32 KB total, ~97 % committed at link.** Real static footprint ≈ 21.2 KB
  (`.stack` 2752 + `.bss` + `.data` + `.noinit`), heap = **10 576 B** (`heap_size = 0x2950`),
  and `heap_size` is literally *whatever RAM is left* (`linkerfile.ld`: `RAM_end − __HeapBase`).
  **Every static byte `shared/core` adds comes straight out of the 10.3 KB heap**, which the BT
  stack already draws 3150 B from (`SL_BT_CONFIG_BUFFER_SIZE`).
- **Flash: 236.8 KB of 272 KB, ~87 %, ~35 KB headroom** at the time of this measurement, which
  is against the 272 KB slot and ignores the 24 KB NVM3 region inside it. **The real ceiling is
  253,952 B, and at `6df2d6e` the image is 247,104 B: 6,848 B of headroom.** Measure `.text`
  after each subsystem swap, against 253,952 and not against the build script's figure.
- App code owns **~11.6 KB of the 21.2 KB static** (61 %). That is the negotiating room.

### The app-owned statics that `shared/core` competes with (BG22, measured via `nm`)

| Bytes | Symbol | What |
|---:|---|---|
| 2064 | `s_cfg_rec` | NVM3 config record (16 hdr + 2048 data) |
| 1676 | `od_zlib_stream` state `s` | inflate: 512 B window + 608 B literal tree + 320 B lengths |
| 844 | `s_od_global_config` | parsed `struct GlobalConfig` |
| 640 | `s_session` | `EncryptionSession` — **512 B of it is `replay_window[64]`** |
| 640 | `s_buttons[32]` | button state |
| 520 | `s_nfc_write_chunk` | incl. `data[512]` |
| 513+512 | `s_crypto_payload_buf` + `s_plain_buf` | CCM staging |
| 256 | `s_decompression_chunk` | inflate output staging |

Realistic recoverable RAM if `shared/core` is careful: **~6 KB** — unify the config staging
buffer with the CCM plaintext buffer, and shrink the replay window (below).

## What in `shared/core` risks not fitting — concretely

1. **The PIPE reorder queue — the single biggest risk.** Firmware's `pipeReorder[33]` at
   4 + 248 B/slot is **~8.3 KB `.bss`** (4.3 KB in the `PIPE_SMALL_DRAM_WINDOW` 17-slot form).
   That is a quarter of the BG22's entire RAM for a subsystem the chip does not implement and
   the spec says it must not. **`OD_PIPE_ENABLE=0` must `#if` the array out of existence**, not
   merely skip the handlers — a linked-but-unused 8.3 KB array is an instant link failure here.

2. **The zlib window — a wire contract, not a knob.** BG22 pins
   `OPENDISPLAY_ZLIB_WINDOW_BITS=9` → **512 B**, statically. A 32 KB
   window (bits 15) is a third of the chip's RAM and can never exist. The inflater *rejects* any
   stream whose CMF header declares a window larger than its compile limit
   (`shared/core/od_zlib_inflate.c`), so this is not a local sizing choice — **the host encoder
   (`py-opendisplay`) must cap `windowBits ≤ 9` for any stream a 9-bit device will receive.**
   Correction to the workspace-level note "existing targets pin 32 KB windows for legacy-client
   compatibility": that was false for all but one now-retired board. The current reality —

   | Target / board | `WINDOW_BITS` | Window | Alloc |
   |---|---:|---:|---|
   | EFR32BG22 | 9 | 512 B | static |
   | nRF54L15 | 9 | 512 B | static |
   | ESP32 (all current boards) | 9 | 512 B | static or tinfl backend |

   So `shared/core/od_zlib_inflate` treats the window as a per-target macro with **512 B as the
   documented floor**. The host side already complies unconditionally — `py-opendisplay` encodes
   `window_bits = 9` for every device and rejects its own output if the header advertises more
   (DESIGN_REVIEW_2026-07-25.md F5) — so the wire contract holds today and the E1004's 15-bit
   build no longer exists. What is left open is narrower; see below.

3. **`MAX_CONFIG_SIZE` — DECIDED 2026-07-25: 4096 fleet-wide, BG22 included.** It is a single
   product-wide value, not a per-target macro: the host may send up to 4096 bytes of config to
   any device. This ends the silent-truncation class and makes `py-opendisplay` correct as it
   stands (it already hardcodes 4096 — DESIGN_REVIEW F5), but **the cost lands entirely on the
   BG22 and it is the largest single RAM ask in this document.** Before the Silabs swap
   (migration step 3), measure it:

   | Item | Today | At 4096 | Delta |
   |---|---:|---:|---:|
   | `s_cfg_rec` NVM3 record (16 hdr + data) | 2064 B | 4112 B | **+2048 B** |
   | `opendisplay_config_buf()` read scratch (`MAX_CONFIG_SIZE`-shaped) | 2048 B | 4096 B | **+2048 B** |

   Against a **10 576 B heap** and a realistic recoverable budget of ~6 KB (above), +4 KB is
   more than the whole negotiating margin. Two mitigations are already identified and now become
   load-bearing rather than optional: unify the read scratch with the CCM plaintext buffer (2.5
   says one shared scratch, which removes the second row), and shrink the 512 B replay window to
   a 64-bit bitmap (§ below, ~8 B). That converts the ask from ~4 KB to ~2 KB, which the ~6 KB
   budget absorbs.

   **Also verify on the NVM3 side, not just the RAM side:** a 4112-byte object must fit the
   configured NVM3 max-object-size and the NVM3 instance's flash capacity. Neither is a given —
   check both before committing the Silabs build, because this is the one target where the
   decision can fail to *fit* rather than merely cost.

   If it does not fit after the two mitigations, that is a finding to escalate — the decision is
   fleet-wide 4096, and reverting BG22 to 2048 re-opens the divergence deliberately rather than
   by discovery. Historical context follows.

   **4096 is the *absolute* ceiling — storable and transferable (decided 2026-07-25).** The
   chunked `CONFIG_WRITE` path caps at 4000 today (`MAX_CONFIG_CHUNKS` 20 × 200 B, enforced on
   all three targets), so the bound moves to match the size: **`MAX_CONFIG_CHUNKS` becomes 21**,
   which is exactly `ceil(4096/200)`. That is a canonical-header change and the headers are
   frozen, so it is queued. Two things follow while it waits: hosts must not build a config
   above 4000, and deployed units enforce 20 until reflashed regardless of the header. Full
   detail and the fleet consequence in FOLLOWUPS.md § 3.1.

   BG22 stored 2048 (NVM3 record 2064 B) while nRF/ESP32 stored 4096, so a config that fit nRF
   but not BG22 was silently truncated on BG22 (DIVERGENCE_MATRIX 2.7). The decision above
   removes that split rather than making it discoverable: with one fleet-wide value there is no
   per-device limit for a host to interrogate, which is why `MAX_CONFIG_SIZE` also drops out of
   the capability-reporting bytes in ARCHITECTURE.md § "The gap, and a proposed fix".

4. **Any assumption of heap.** The core must run on caller/static buffers only. No `malloc`
   exists in BG22 app code today; the one `malloc` in the inflater is compiled out
   (`USE_HEAP_WINDOW=0`). A `shared/core` that mallocs anything excludes this target.

5. **Any assumption of a scheduler or blocking.** No kernel, no `k_msleep`, no work queue.
   Handlers run inline in the BGAPI callback. This is why SHARED_API_DESIGN.md makes refresh a
   non-blocking `refresh_start` + `refresh_busy` pump instead of a blocking
   `wait_refresh(timeout)` — the Silabs 60 s in-callback block already drops the BLE link
   mid-refresh, and a shared core must not standardize that.

6. ~~**The replay window is 8× oversized.**~~ **CLOSED 2026-08-15 on `esp32-idf` and
   `nordic-zephyr`** by the `od_session` promotion; still open on `efr32bg22-slc`, which has not
   swapped. Measured, not estimated — the whole session object, since more than the window
   shrank:

   | Target | before | after | returned |
   |---|---:|---:|---:|
   | `esp32-idf` (`s3-n8r8`) | 632 B (`0x278`) | 112 B (`0x70`) | **520 B** |
   | `nordic-zephyr` (`xiao_nrf52840`) | 640 B (`0x280`) | 112 B (`0x70`) | **528 B** |

   The window itself is a **32-byte** bitmap, not the 8-16 B guessed here: upstream `Firmware`
   had already replaced the 512 B ring with a 256-bit backward bitmap (`src/nonce_window.h`,
   ported verbatim to `shared/core/od_nonce_window.h`), and a wider backward window than the
   ±32 assumed above is worth the extra 24 B under PIPE reordering. The rest of the saving is
   not the window at all: `session_key[16]` moved into an `od_hal_crypto` slot, and
   `client_nonce`/`server_nonce` became handshake-locals. Holding no key material in the struct
   is a security property as much as a RAM one — a memory dump of a live session now yields none.

   The security-sensitivity warning was right and was honoured: the `diff == 0` replay hole
   closed in the same change (`DIVERGENCE_MATRIX` § 6.6), along with the window-advance ordering
   (§ 6.7). **Hardware-verified on `nordic-zephyr`/`xiao_nrf52840` (2026-08-15, Gate 2); still
   unverified on `esp32-idf`.** The bitmap carried real encrypted traffic on the nRF52840, but
   not under deliberately induced reordering, so the backward window's depth is not yet exercised
   by anything but the host sweep.

### RAM sizing rules for `shared/core`

- Every buffer sized by a target macro with a documented floor; nothing sized for the biggest
  target. `MAX_CONFIG_SIZE`, `OPENDISPLAY_ZLIB_WINDOW_BITS`, `OD_RX_QUEUE_DEPTH`,
  `OD_PIPE_*` are the knobs.
- These are **plain preprocessor constants** — do not build the config surface around Kconfig
  (BG22 has none; it sets them via `target_compile_definitions`). Kconfig on ESP-IDF/Zephyr is
  merely *how those two set* the constants.
- Measure `.bss`/`.text` on BG22 after every subsystem swap (MIGRATION.md verification bar), not
  at the end — a few KB over the hand-written original is a link failure, not a tuning task.

## Dependency policy: bb_epaper and FastEPD under ESP-IDF

> 3.0: only FastEPD is still compiled (ESP32 IT8951 / parallel boards). No target links
> bb_epaper; SPI panels run from config drive packets through `shared/core/od_epd.c`, and the
> Group5 collision below no longer arises. The bb_epaper half is the import-time record.

Both are Arduino-origin panel libraries; both must work under ESP-IDF (the ESP32 target drops
Arduino). Findings verified against the clones at `/home/davelee/opendisplay/{bb_epaper,FastEPD}`.

### Both are already ESP-IDF-capable — this is not a port

- **bb_epaper** selects its backend by `#ifdef` in `bb_epaper.cpp:40-50`; the non-Arduino
  default is `esp_idf/esp_generic.inl` (`driver/gpio.h` + `driver/spi_master.h` + `esp_timer.h`,
  with Arduino-compat shims). Root `CMakeLists.txt` is already
  `idf_component_register(... REQUIRES driver esp_timer)`. Backend `.inl` inventory in the clone:
  `arduino_io`, `esphome_io`, `mem_io`, `rpi_io`, `esp_idf/{esp_generic,esp_main_io,s3_ulp_io,
  c6_ulp_io}`, `ch32v`, `rpi_pico`. **`nrf54_zephyr_io.inl` and `silabs_efr32_io.inl` do NOT
  exist upstream** — they live only in the NRF54/Silabs vendored copies. So the single vendored
  copy under `third_party/bb_epaper/` must be **assembled** from upstream (for `esp_idf/`) plus
  the two downstream backends. Worth upstreaming so it stays a checkout, not a fork.
- **FastEPD** is *primarily* an ESP-IDF library; `arduino_io.inl` is a misnomer for its ESP32
  backend, which has a full `#ifndef ARDUINO` branch (`driver/gpio.h`, `esp_timer.h`,
  `driver/i2c_master.h`). `<Arduino.h>`/`<SPI.h>`/`<Wire.h>` are all `#ifdef ARDUINO`-guarded.
  The panel bus is pure ESP-IDF (`esp_lcd_panel_io`, `esp_lcd_new_i80_bus`, `parlio_tx` for
  C5). Root `CMakeLists.txt`: `idf_component_register(... REQUIRES driver esp_timer esp_lcd)`.
  Working **C** IDF examples exist (`examples/esp_idf/papers3_demo/main/papers3_demo.c`).

**Neither ships an `idf_component.yml`**, so neither is on the ESP Component Registry — the IDF
build must vendor them or add them via `EXTRA_COMPONENT_DIRS`/submodule. Given the "assemble one
bb_epaper copy" requirement above, **vendor both under `third_party/`**, consistent with the
scaffold.

### The duplicate-symbol collision is real, follows to ESP-IDF, and has no opt-out

Both libraries ship `Group5.cpp` and `bb_ep_gfx.inl`, registered *unconditionally* in both
`CMakeLists.txt` files. The genuinely colliding symbols (identical C++ mangled name):

- **Group5, 5 symbols**: `G5DECODER::init`, `G5DECODER::decodeLine`, `G5ENCODER::init`,
  `G5ENCODER::encodeLine`, `G5ENCODER::size` (`bb_epaper/src/Group5.cpp` vs
  `FastEPD/src/Group5.cpp` — bodies identical, only license header differs).
- **bb_ep_gfx, 4 symbols**: `bbepUnicodeString`, `bbepStretchAndSmooth`, `RotateCharBox`,
  `bbepUnicodeTo1252` (identical signatures; the rest are spared only by C++ overloading on the
  differing state-struct pointer type `BBEPDISP*` vs `FASTEPDSTATE*`).

Neither library offers a `NO_GROUP5`/`BB_EP_NO_GFX` opt-out. The shared include guards are
useless (separate translation units). So the PR-#120 workaround `-Wl,--allow-multiple-definition`
reproduces under ESP-IDF's CMake exactly as under PlatformIO — the link picks the first
definition and drops the rest.

**Policy recommendation.** `-Wl,--allow-multiple-definition` is a blunt instrument: it silences
*all* duplicate-symbol errors, including future accidental ones, and which copy wins is
link-order-dependent. Since both `Group5` copies are byte-identical in behaviour and only one
library's drawing API is actually used per board (upstream comment: "we only use FastEPD
buffer+update APIs"), prefer in priority order:

1. **Compile Group5.cpp / bb_ep_gfx.inl from exactly one library.** In the IDF component for the
   *unused*-gfx library, drop those two files from its `SRCS` list (a one-line `CMakeLists.txt`
   edit in the vendored copy — allowed, it is our fork under `third_party/`). This removes the
   collision at the source instead of papering over it, and keeps the linker strict for real
   duplicate bugs. Verify the two `Group5.cpp` are behaviourally identical first (they are:
   only the license header differs) so dropping one is safe.
2. If (1) is impractical because both drawing APIs are needed on the same board, keep
   `--allow-multiple-definition` but **scope it** and add a build-time check that no *new*
   duplicate symbols appear beyond the known Group5/bb_ep_gfx set.

Also flagged: `g5enc.inl` declares `g5_encode_*` `static` in bb_epaper but `extern` in FastEPD —
three more collisions appear if bb_epaper ever drops `static`. Pin versions (below) so that
can't surprise a build.

### Version pinning — fix the asymmetry

Current `Firmware` `upstream/main`:

- **FastEPD** = `bitbank2/FastEPD@^2.2.0` — a floating caret range (clone HEAD is `2.2.0-9`,
  9 commits past the tag).
- **bb_epaper** = a bare git URL with **no ref** on the default `[env]` (clone HEAD is
  `2.1.11-4`, 4 commits past its tag), except `env:esp32-s3-E1004` which pins a specific
  `limengdu` fork commit.

**Pin both to exact commits in this repo.** They are vendored under `third_party/`, so pin by
the committed tree, and record the upstream commit SHA + tag in a provenance header
(`bb_epaper 71f6e70 / v2.1.11+4`, `FastEPD 770e168 / v2.2.0+9`). A caret range and a bare git URL
are both non-reproducible, and the Group5 `static`/`extern` asymmetry above means an unpinned
bump can introduce link collisions silently.

### FastEPD is ESP32-only and PSRAM-mandatory — scope it accordingly

FastEPD's parallel path uses the ESP32 i80/parlio LCD peripheral (`library.properties:
architectures=esp32`) and **hard-`#error`s without PSRAM** (`FastEPD.inl:29-31`), allocating its
framebuffers from `MALLOC_CAP_SPIRAM` (~259 KB + ~130 KB for 960×540). It cannot be a universal
panel layer — it is one backend for the two IT8951 panels (`OD_PANEL_IC_ED103TC2_*`, ids
3000/3001) on PSRAM-equipped S3 boards. Its flat C API (`bbepInitPanel(FASTEPDSTATE*, …)`,
exercised from `app_main` in the IDF examples) means it sits behind `od_hal_panel` with **no C++
shim** — but only as the `needs_framebuffer=true` ops table, selected by `panel_ic_type`, never
as the default. bb_epaper remains the streaming (`needs_framebuffer=false`) default.

## Open questions needing a human decision

- **The zlib window — the contract itself is no longer open** (corrected 2026-07-25). The host
  encodes `window_bits = 9` unconditionally and rejects its own output if the header says
  otherwise (`py-opendisplay` `encoding/compression.py:14`, `device.py:389, 1842` —
  DESIGN_REVIEW_2026-07-25.md F5), so everything on the wire is 9 bits and the E1004's 15-bit
  build is host-unreachable dead capability. What still needs a human is narrower: does anyone
  ever want >9 bits, and where does the per-device "max windowBits" value live — which is the
  capability-discovery problem (ARCHITECTURE.md § "Capabilities are discovered by
  interrogation"), not a compression one.
- ~~**`MAX_CONFIG_SIZE` 2048 vs 4096.**~~ **DECIDED 2026-07-25 — 4096, product-wide, a global
  cap rather than a per-target macro.** The host may send up to 4096 bytes to any device,
  BG22 included. See item 3 above for the BG22 cost (~+4 KB naive, ~+2 KB after the two
  required mitigations) and the NVM3 checks that must precede the Silabs swap.
- ~~**`CMD_NFC_ENDPOINT` placement.**~~ **DECIDED 2026-07-25 — core behind `OD_NFC_ENABLE`**,
  which is what SHARED_API_DESIGN.md assumed. Two targets implement it (NRF54 and Silabs), so
  the endpoint logic is duplicated today and promoting it is the ordinary case, not a special
  one. The flag gates the `0x83` endpoint and the IC drivers only: the `0x2A` config packet is
  canonical schema (`OD_PKT_NFC`) and is parsed unconditionally on every target, ESP32
  included. See SHARED_API_DESIGN.md § "NFC: standard packet, optional support".
- **Group5 de-duplication vs `--allow-multiple-definition`.** Recommendation is to compile the
  duplicated files from one library only; confirm both drawing APIs are not simultaneously
  required on any single board first.
- **Enforce the outer config CRC16?** It is advisory-only (logged, never rejected) on all three
  targets today. Starting to enforce it in `shared/core` is a behaviour change that could reject
  configs the host currently gets away with sending.
