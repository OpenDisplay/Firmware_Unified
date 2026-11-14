# CLAUDE.md

Firmware_Unified: one repo for all OpenDisplay firmware targets, replacing four repos
(`Firmware`, `Firmware_NRF54`, `Firmware_Silabs`, `Firmware_NRF`) that each reimplemented the
same wire protocol, config parsing, transfer state machines, compression and session encryption.
`../CLAUDE.md` covers the wider workspace.

**ALL SIX SOURCE REPOS ARE CHECKED OUT AS SIBLINGS AND ARE READABLE — USE THEM.** `../Firmware`,
`../Firmware_NRF54`, `../Firmware_Silabs`, `../Firmware_NRF`, plus `../opendisplay-protocol` (the
canonical wire contract) and `../py-opendisplay` (the host, and the answer to "does the client
actually do X?"). `targets/*/` here are import *snapshots* that drift; the siblings are live. Diff
before you port — see § "Migration constraints".

**SIBLING REPOSITORIES ARE READ-ONLY REFERENCES.** Read them freely — that is the whole point of
the paragraph above. Do not create or switch branches, edit or regenerate files, commit, or push in
a sibling while working here. Keep every implementation change, artifact, test and branch in this
repository, and record a needed sibling change as external follow-up work — `FOLLOWUPS.md`, or the
plan that wants it — unless the user explicitly overrides this in a later request. A defect found
upstream is reported, not fixed from here.

## Reading budget

Code first. Headers, build files and tests are ground truth; `docs/` explains *why* and is never a
prerequisite. This file already distils it — don't open a doc to re-confirm a rule stated here, or
to answer what a grep answers. Dated files (`*_REVIEW_<date>`, `FINDINGS*`, NEXT_STEPS.md) are
archive. One doc per task, at most; past that, write the code and flag the uncertainty.

## Comments

Say what the code does and why it is the way it is. Do not narrate history — no "this used to",
no "before the promotion", no recounting the bug that prompted the change. Git and the commit
message hold that. A comment longer than the code it explains is almost always history in
disguise. Keep the non-obvious constraint (a wire contract, a RAM ceiling, an ordering that
looks arbitrary and is not); delete the story.

## Status

This section is a terse summary, not the record. Row-level hardware evidence — which board, which
opcode, which date, plaintext vs encrypted, raw vs compressed, happy-path vs negative case — belongs
solely in `docs/HARDWARE_VERIFICATION_CHECKLIST.md`; a bullet here should point at that file's
section rather than restate its detail. If you're about to write more than one sentence of
test-scope narrative in this file, it belongs in the checklist instead.

- **Two broadly hardware-verified targets**: `esp32-idf` (11 board configs, verified on an
  ESP32-S3) and `nordic-zephyr`'s `xiao_nrf52840` board. Everything else — other boards, other
  opcodes, other subsystems — is scoped or open. `docs/HARDWARE_VERIFICATION_CHECKLIST.md` is the
  itemized per-target/per-opcode record; update it alongside this section whenever a hardware
  test runs, and read it (not this file) for row-level evidence.
- **No CI.** `tools/check.sh` (repo root) is the only gate, and nothing runs it but you. Plain
  invocation covers host tests, sanitizers, fuzz targets and the wire corpus; `--targets` adds all
  three target families and is required before merge. A skip is not a pass — read the summary,
  which exits 2 on any skip. Codex sandbox failures involving the Nordic Zephyr cache or
  LeakSanitizer under `ptrace`, and their exact rerun procedure, are recorded in
  `codex/known_issues.md`.
- `targets/esp32-idf/build.sh` builds every board fragment (sources ESP-IDF itself; never on
  `PATH`). `targets/esp32-idf/tools/run_host_tests.sh` runs host tests without ESP-IDF.
- `./build-release.sh` (repo root) drives every target's own build entry point, writing
  `release/MANIFEST-<target>.txt` + one top-level summary and a log per target. `--list` prints
  the target table; every target still runs even if an earlier one fails. It does not activate
  any toolchain — export what each target's script expects first (docs/TOOLCHAINS.md).
- **`shared/{core,hal}` implements the whole protocol stack** — dispatch, config, transfer
  (pump/direct/partial/PIPE/NFC), session/crypto, logging, watchdog, advertising, GT911 touch,
  buzzer, config storage — composed per target in named tiers from `shared/sources.cmake` (never
  globbed; that file is the ground truth for which target takes which tier). No promoted
  subsystem has a target-side reimplementation — enforced by the absence ratchets in
  `tools/check.sh` and "The one rule" below. Layering and the `od_hal_*` interface contracts are
  `docs/SHARED_API_DESIGN.md`.
- **Command/dispatch/session/rxq/watchdog/crypto path is shared on ESP32 and Nordic**, both
  hardware-verified for PIPE upload, config read/write and (ESP32 only) `CMD_PARTIAL_WRITE`,
  plaintext and encrypted — checklist has the per-opcode rows. `docs/OD_SESSION.md` is the
  session subsystem reference (wire shapes, KDF, replay window, verification state). Two known
  open risks worth carrying in your head rather than looking up each time: `FOLLOWUPS.md` § 5,
  **bidirectional nonce reuse** (both directions share one session id and start both counters at
  0 — needs a protocol revision, not a firmware fix), and the `OD-S1` PIPE-silence-on-replay path
  is still unproven on any board (checklist § Nordic `xiao_nrf52840`).
- **Transfer plane (pump, direct, partial, PIPE, NFC) is fully shared** across all three targets
  — no target owns wire parsing, chunk assembly, SACK construction or transfer ownership; see
  `docs/DIVERGENCE_MATRIX.md` §§ 3-5, 10 for the wire-visible decisions made while promoting it.
  Per-phase/per-target hardware qualification is itemized and mostly open in the checklist
  (§§ Transfer Phase 1-5). **NFC's hardware gates were waived by project direction**, not passed —
  no board in this fleet has an NFC antenna or a TNB132M fitted, so every `0x0083` row is release
  debt against hardware that doesn't exist here, a stronger form of "open" than the other phases.
- **Shared HALs beyond crypto/session**: time (`od_hal_time`), logging (`od_hal_log`), watchdog,
  advertising, GT911 touch, config storage (`od_config_store` + `od_hal_nvs`), buzzer. All
  implemented on their respective target sets; hardware qualification is itemized per-HAL in the
  checklist and mostly open — touch and buzzer especially, since **no board in this fleet has a
  touch controller fitted** (checklist § Shared GT911 touch driver).
- **Boot-screen key policy** (`od_boot_key_state`) fixed two separable defects — write-ups in
  `docs/DIVERGENCE_MATRIX.md` §§ 26-27 — hardware-verified on ESP32-S3 only, and only for the
  panel key-state lines (checklist § Boot-screen key policy has the exact coverage and gaps,
  including the undecoded QR payload).
- **nRF52840 idle power:** USB is enabled only while VBUS is present (PR #90). Battery idle current
  measured at 40 µA, down from ~2 mA (`../Firmware`: ~55 µA), and the USB console still enumerates
  (checklist § nRF52840 USB power gating has the remaining open rows).
- **`telink-tlsr` (fork addition): running on four ATC boards** (Hanshow 2.66" BWR, 2.0" BWY, 3.5" BWY; a 9.7" dual-controller BWR) — BLE install
  from ATC firmware, config write/read, boot screen, correct direct image uploads, battery and panel
  temperature in the MSD, and wireless updates of its own (gated by ENTER_DFU). tc32 must not build at `-Os` (broken jump tables; the build checks).
  Checklist § `telink-tlsr` has the rows.
- **Never hardware-verified:** the WiFi/LAN transport, and the F4/F7 correctness fixes.
- Arduino shim fully removed from `esp32-idf` (docs/ARCHIVE_esp32_arduino_shim.md);
  `targets/esp32-idf/vendor/fastepd/` is its permanent (non-shim) FastEPD adapter.
- **The wire corpus (`tests/vectors/dispatch.json`) is checked from both ends**:
  `tests/host/corpus_runner.c` drives it through firmware's own `od_dispatch_frame()` (previously
  only py-opendisplay's side was checked), across three target-composition executables with
  different semantic guarantees — see that file's header comments for what each profile does and
  doesn't prove before trusting a pass.
- Live plan: `plans/PLAN_TRANSFER_PHASE45_2026-08-20.md` — complete; its hardware rows are the
  outstanding work, not its steps. Earlier transfer-sequence plans and `docs/NEXT_STEPS.md` are
  superseded/historical.

## The one rule

`shared/` compiles for **every** target: C standard library and `shared/hal` only. **No vendor or
framework header** — `esp_*`, `driver/*`, `soc/*`, `hal/*`, `freertos/*`, `nrf_*`, `nrfx`, `sl_*`,
`em_*`, `zephyr/*`, `Arduino.h`, `bluefruit.h`, `NimBLE*`, `bb_epaper`, `TFT_eSPI`. A file needing
one belongs in `targets/<target>/`. One slip and the repo is four codebases in a directory.
Enforced by the three `shared boundary:` checks in [tools/check.sh](tools/check.sh) — run them
before proposing anything under `shared/`; extend the pattern as targets are imported.

## Architectural decisions

1. **`shared/` is plain C.** Every interface across the boundary is a link-time `extern` C
   function the target implements. No C++ classes, no Arduino `String`.
   Re-argued 2026-08-13 and upheld — but not because C++ is unavailable (all three toolchains
   already compile target `.cpp`). It is that the boundary must be a C API for Zephyr's C
   drivers and the Silabs superloop regardless, and the code being ported is already C in
   `.cpp` files (~20k lines of `targets/esp32-idf/src/` hold 32 `new`, 9 `String`, one class).
   So C++ buys implementation ergonomics only, priced in a doubled host gate, an
   `-fno-exceptions -fno-rtti -fno-threadsafe-statics` contract spread across three build
   systems `shared/` does not own, and an enforcement grep that libstdc++'s transitive
   includes defeat. Three C-side rules buy back most of what RAII and strong types offered:
   - **`_Static_assert` every wire size.** The `sizeof`-derived size table can go silently
     wrong via include order (`shared/sources.cmake`, closing note) — a wrong *value*, not a
     build error. Assert the sizes so it is a build error. Zero cost, C99, unused today.
   - **Non-owning views are `od_span_t`** (`shared/core/od_span.h`), not ptr+len arguments.
     `od_span_split()` is the checked cut and the one place bounds arithmetic is written;
     `take`/`drop` saturate and are only for lengths already known to fit. The whole config
     parse path takes spans; new shared code does too.
   - **Single-exit + `goto cleanup`** in anything holding a resource across a fallible step.
   Revisit only at `od_session.c` / `od_xfer_partial.c`: nested resource
   lifetimes with several failure exits per function are the one shape where manual cleanup
   reliably loses. That is also the last point where switching is cheap.
2. **One vtable, deliberately** — `od_panel_ops` (`targets/esp32-idf/hal/od_hal_panel.h`), for the
   one target with 2-3 panel backends. Keep it the only one.
3. **Three toolchains stay three** (ESP-IDF, west/Zephyr, SLC), all CMake. Unification is about
   shared *source*.
4. **Grouped by silicon vendor, not repo of origin.** nRF52840 is a *board* on `nordic-zephyr`,
   sharing its BT host, PSA Crypto, NVS and panel stack.
5. **No PlatformIO, no Arduino** — no `platformio.ini`, `lib_deps`, Arduino APIs or `build_flags`
   idioms, from any source repo.
6. **BG22 stays on Simplicity SDK.** Zephyr rejected on RAM: 32 KB total, no kernel at all
   (superloop + `sl_power_manager`).
7. **No Kconfig in `shared/`'s config surface** — Silabs lacks it. Plain preprocessor constants;
   Kconfig is only how two targets set them.
8. **No `targets/nrf52-sdk/`.** Legacy nRF52 stays in `Firmware_NRF` and sets the host's compat
   floor (no compression, no `0x76`, no PIPE, no NFC). A dir under `targets/` means "a target this
   repo builds".
9. **No lowest-common-denominator features.** Differences (PSRAM, ROM inflate, panel families,
   PIPE — absent on Silabs) go behind config and `#if`; a target must not pay for a feature it
   lacks.
10. **Import working drivers as-is**; only shared logic is refactored.
11. **First `shared/` source: `shared/core/od_adv_control.c`** (portable BLE advertising/
    lifecycle). `od_config.c` is the first *protocol* subsystem promoted.
12. **`OD_CONFIG_MAX_SIZE` is 4096 — except BG22, which is 2048** (C13, 2026-08-17). It was a
    global cap for exactly the right reason: a per-target limit is a wire divergence a host
    **cannot interrogate**, and `MAX_CONFIG_SIZE` was dropped from the capability bytes *because*
    the value had become uniform. That reason has not gone away; it was outweighed on a 32 KB
    part, where 4096 costs +2 KB of NVM3 record against a ~10.5 KB heap. So the constant is now
    `#ifndef`-guarded in `od_config_asm.h` and set per target — 4096 on ESP32 and Nordic, 2048 on
    BG22 via `target_compile_definitions`.
    **Two things make that safe, and neither is optional.** `od_config_asm_start()` bounds the
    declared total against `OD_CONFIG_MAX_SIZE` as well as the 4,000-byte transferable ceiling —
    without it any cap below 4,000 is a remotely triggerable buffer overflow from a two-byte
    length field, which is what the pre-C13 code was one constant away from. And BG22 **refuses,
    never truncates**: an oversize declaration is NACKed at the start frame with nothing stored.
    Silent truncation is the exact failure the fleet-wide rule existed to prevent, and it is
    invisible to the host.
    `struct od_config_asm` changes size with this macro, so any host test or target build mixing
    caps across translation units is an ABI mismatch — `tests/host/` compiles a separate
    `od_shared_silabs` for this reason.
13. **`third_party/` is exempt from the one rule** — `bb_epaper` picks its IO backend by `#ifdef`.
    Still one vendored copy for all targets; do not move it into `shared/`.
14. **Headers beat design docs.** Where `targets/esp32-idf/hal/*.h` and docs/SHARED_API_DESIGN.md
    disagree, fix the doc.
15. **`OD_COLOR_SCHEME_GRAY8` is a reserved identity, not an implemented scheme** (C-color,
    2026-08-18). `shared/core/od_color.h` squats value 9 behind an `#ifndef` so a future canonical
    assignment syncs down without renumbering, and `od_color_direct_geometry()` returns
    `OD_COLOR_UNSUPPORTED` for it. Do not add 3-bpp packing, a 4-bpp substitute, FastEPD
    expansion, Inkplate waveform selection, target admission, or a boot fallback — every
    operational display path rejects it explicitly, and `tests/host/color_test.c` static-asserts
    the number.
    When `../opendisplay-protocol` eventually reserves value 9 and the vendored header is synced,
    remove the `#ifndef` fallback and update `epaper-dithering`'s parity exemptions as external
    follow-up work. **That enum reservation alone does not authorize wire behavior:**
    `py-opendisplay` must keep rejecting uploads, and firmware must keep rejecting operational
    use, until a separate device-qualified protocol/backend plan defines and tests the encoding.
    See `plans/PLAN_OD_COLOR_2026-08-18.md` § 2.1.

## Layout

`shared/{protocol,core,compress,hal}` — wire contract / dispatch + config + transfer + auth /
inflate engines / interfaces targets implement.
`targets/{esp32-idf,nordic-zephyr,efr32bg22-slc}` — chip drivers + build system + HAL impl.
`third_party/` vendored cross-target libs (bb_epaper); `tools/`; `docs/`.

## Toolchains

Installed, but **none on `PATH`** — `which idf.py west` returning nothing ≠ absent.

| Toolchain | Version | Activate with |
|---|---|---|
| ESP-IDF | v5.5.4 | `source ~/esp/esp-idf/export.sh` |
| nRF Connect SDK / west | v3.3.1 / west v1.5.0 | `nrfutil toolchain-manager launch --ncs-version v3.3.1 -- <cmd>` |
| Simplicity SDK | 2025.12.2 | `slt`; `slc` also needs the bundled Java on `PATH` |

Pin one ESP-IDF release; floors ≥ 5.1 (C6), ≥ 5.2 (`driver/i2c_master.h` — not the deprecated
`driver/i2c.h`). Only ESP-IDF has built anything here. **Do not claim a build passes without
running it.** docs/TOOLCHAINS.md has the version pins and the Arduino-API replacement census.

## Protocol header — do not hand-edit

`shared/protocol/opendisplay_protocol.h` and `opendisplay_structs.h` are byte-for-byte copies of
`../opendisplay-protocol`. Edit the canonical file there, never these.

## Migration constraints

Rationale in [docs/MIGRATION.md](docs/MIGRATION.md) / [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
— open only if a change contradicts a rule here or adds a target.

- **One target at a time:** `esp32-idf` → `nordic-zephyr` (nRF54L15) → `efr32bg22-slc` →
  `nordic-zephyr` (nRF52840). ESP32 first as the reference for `shared/core`; Silabs third so its
  32 KB / no-kernel / no-Kconfig limits bite before other targets bake in assumptions.
- **Import unchanged in one commit**, then fix only include paths and build files; record source
  repo + SHA. Exceptions: the ESP32 framework change, and Silabs' 57 MB vendored SDK.
- **One subsystem per swap**, build + flash + hardware verify between each, independently
  revertable.
- **Delete nothing from the original repos** until the unified target is hardware-verified.
- **Resolve divergence deliberately and write it down** — not by whichever repo was copied first.
  **`Firmware` is the authority over `Firmware_NRF54`** when their algorithms disagree: it is the
  field-proven original and what the host tooling was validated against, and the NRF54 port
  re-derived several of them. So port the `esp32-idf` behaviour and make the Zephyr difference
  justify itself — in a differential test, the Firmware form is the reference. `esp32-idf` is
  C++ and `shared/` is plain C, so this usually means a C port, not a file move. A default, not
  a licence to skip the write-up.
- **THE AUTHORITY IS `../Firmware/`, THE SIBLING REPO — NOT `targets/esp32-idf/src/`.** That
  directory is a *snapshot* taken at import, and upstream keeps moving. Before porting or
  transcribing any algorithm, diff the two; when this repo and upstream disagree, upstream wins
  unless the difference is a deliberate Firmware_Unified adaptation (Arduino removal, `od_hal_*`,
  `od_log`, `struct od_config`), which the import is full of — so separate *drift* from
  *adaptation* rather than blanket-copying either way. `../Firmware/tools/` also carries host
  tests worth porting with the code they cover.
  Learned the hard way on `od_session` (2026-08-15): the replay window had been extracted upstream
  into `src/nonce_window.h` with an 816-line host test, and three rounds of design were built on
  the stale ring instead — two of them wrong in security-relevant ways (a forward window cap that
  strands a session, and counting nonce failures as integrity strikes so packet loss tears one
  down). A 30-second diff would have caught all of it.
  The same applies to the other three source repos for their targets.

## Memory sensitivity

EFR32BG22 32 KB, nRF52840 256 KB, ESP32-S3 512 KB + PSRAM. `shared/` must avoid buffers sized for
the biggest target, unbounded heap, and assuming a heap exists. Sizes go behind compile-time
constants the target sets, with a documented floor — `OD_CONFIG_MAX_SIZE` included since C13
(decision 12), so there is no longer an exception to this rule. Its floor is a 400-byte policy
assert in `od_config_asm.h`, and a cap a host cannot query has to be **refused loudly** rather
than silently truncated (docs/MEMORY_CONSTRAINTS.md item 3, `DIVERGENCE_MATRIX` 2.7).
