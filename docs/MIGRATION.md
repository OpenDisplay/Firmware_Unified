# Migration plan

Incremental, one target at a time, keeping the original repos buildable throughout. Nothing
is deleted from the source repos until the unified target has been flashed and verified on
hardware.

## Order and rationale

1. **`esp32-idf`** (from `Firmware`) — first, because it is the most actively developed and its
   protocol implementation is the most complete, so it becomes the reference for what
   `shared/core` must expose. It is also the largest piece of work by a wide margin, because it
   changes framework: PlatformIO + Arduino → ESP-IDF. See "The ESP32 import is different" below.
2. **`nordic-zephyr`, nRF54L15 boards** (from `Firmware_NRF54`) — second: Zephyr's build system
   is the most opinionated, so it stresses the `shared/` boundary hardest. If `shared/` survives
   being consumed by west/CMake with no vendor includes, the boundary is real.
3. **`efr32bg22-slc`** (from `Firmware_Silabs`) — third, and deliberately **not** last. Its
   32 KB of RAM, absence of any kernel, and absence of Kconfig are what keep `shared/` honest:
   any `shared/core` API that assumes a scheduler, a heap to lean on, or a 32 KB window buffer is
   excluded by this target before it can ship. Leaving it until the end means discovering those
   assumptions after three targets have baked them in. Its toolchain does not change; note that
   `slc generate` needs a full Simplicity SDK, which is not available on every dev box. Its
   import has one mandatory deviation from step 1 — see "The Silabs SDK is not imported as-is"
   below.
4. **`nordic-zephyr`, nRF52840 board** (from `Firmware`) — fourth, and a **port rather than an
   import**: it currently builds with PlatformIO + Arduino/Bluefruit and arrives as a third
   board on the target step 2 creates. It therefore cannot start until step 2 lands. Settle
   deployed-unit OTA/flash-layout compatibility before starting. It is last because it is the
   only step that is pure toolchain migration with no new capability — if priorities shift, this
   is the one to defer.
5. **`nrf52-sdk`** (from `Firmware_NRF`) — **not migrated, but not gone either.** Legacy bare-C
   Nordic SDK. **It is shipped** — a handful of low-capability devices (established 2026-07-25)
   — so the earlier "confirm it is no longer shipped, then delete" instruction is answered, and
   answered the other way. Lower priority for development, not absent from the fleet.

   **Resolved 2026-07-25: it stays in `Firmware_NRF`, in maintenance only, and never becomes a
   target here.** `targets/nrf52-sdk/` has been removed from this repo so the layout does not
   imply a migration that will not happen. The removal is *scope*, not abandonment — this entry
   is the record that the deployed units are served from the original repo, so the missing
   directory does not later read as "that fleet was forgotten." Anyone looking for the nRF52
   legacy firmware should look in `Firmware_NRF`, not here; do not re-create the directory as a
   marker, because a directory under `targets/` means "a target this repo builds."

   **It still binds the wire contract.** Those units cannot be updated in practice, so the host
   must keep working with them, and DIVERGENCE_MATRIX records what that means: no compression,
   no `0x76`, no PIPE, no NFC, no `LED_STOP`, no `CONFIG_CLEAR`, no `READ_MSD` handler. That is
   the **backward-compatibility floor for `py-opendisplay`**, and a hard one — a host change that
   assumes any of those features is present breaks a shipped device with no remedy. Being
   low-priority for development does not make it low-priority for compatibility.

Note what steps 1 and 4 have in common: the `Firmware` repo is the source for **two** different
targets in this plan, because its ESP32 and nRF52840 halves go to different toolchains. It is
not retired until step 4 completes.

## Deployed fleet status — what the migration may and may not break

Established 2026-07-25. This is the constraint that prices every step below, so it comes before
the procedure.

| Chip | Shipped? | Bootloader today | OTA-capable partitions? | Consequence |
|---|---|---|---|---|
| **ESP32-S3** | **yes** | IDF 2nd-stage | **yes** — `default_8/16/32MB.csv` carry `app0` + `app1` | BLE OTA over mcumgr SMP with app rollback since `7372072` (verified on E1003, plaintext only; checklist). Needs the rollback-enabled 3.x bootloader from one USB flash |
| **ESP32-C6** | **yes** | IDF 2nd-stage | **no** — `huge_app.csv` has `app0` only | Cannot OTA without repartitioning, and repartitioning *is* a physical reflash. Field units are bench-update-only |
| **nRF52840** | **yes** | Adafruit UF2 | n/a | Moving to MCUboot replaces the bootloader → physical reflash of every deployed unit |
| **nRF54L15** | **no** | — | — | Greenfield. No compatibility burden of any kind |
| **EFR32BG22** | **yes** (established 2026-07-25) | Gecko Bootloader + AppLoader | n/a — `.gbl` OTA works | **The only shipped target that can actually be updated in the field today**, via `.gbl` through the AppLoader — the sole OTA extra HA pins. Deployed units are reachable without a bench visit, which makes it the cheapest fleet to change and the only one where a firmware fix reaches customers on the host's schedule |
| **nRF52 (legacy, `Firmware_NRF`)** | **yes** — a handful of low-capability devices | Nordic SDK | not assessed | Not migrated; maintained in place. Sets the host's backward-compatibility floor (strict feature subset — see "Order and rationale" item 5) |

**The BG22 row was missing when this table was first written and is now filled in: it is
shipped.** That matters more than a table cell, because this target is simultaneously the one
paying the full cost of the 4096 config decision, the one carrying three known security defects,
and the *only* one whose fleet can be reached over the air. Consequence 4 below.

Four consequences that change decisions already recorded:

1. **ESP32-S3 is the cheapest place to gain OTA.** The partition layout already has two app
   slots, so `esp_ota` needs no layout change and no physical access. If the answer to
   ARCHITECTURE.md's "can OTA extend to more targets" is to be *yes* anywhere, it is here.
2. **ESP32-C6 sets a hard ceiling on Phase B.** A shipped 4 MB fleet on a single-slot layout
   cannot be updated in the field at all. Any Phase B change to partitioning, config storage
   (the LittleFS→NVS question), or bootloader behaviour is a bench visit per C6 unit — so
   either keep those unchanged for C6, or accept touching every one.
3. **nRF54L15 carries no field risk whatsoever.** It is the only target where bootloader,
   partitioning, storage format, and wire behaviour can all change freely. Combined with it
   being the better structural donor for `shared/` (already C, already HAL-shaped), this is a
   real argument for reconsidering migration order — see the risk of the same name below.
4. **EFR32BG22 is shipped *and* field-updatable — which weakens the security-hotfix deferral
   specifically for this target.** The deferral (§ "Risks to watch") rests on the exposure being
   bounded and the fix being expensive to deliver early. The second half does not hold here:
   `.gbl` through the AppLoader is the one update path HA actually ships, so a Silabs fix
   reaches deployed units without touching them. Two of the three deferred defects are Silabs
   defects — the `<31 byte` plaintext auth bypass and the session surviving a key change — and
   they sit on shipped hardware until migration **step 3**, months away, on the one fleet that
   could have been patched in a release. Nothing here re-decides the deferral; it removes the
   "expensive to fix now" leg of it, which is reason to revisit rather than inherit it.

   The same fact carries the 4096 config decision's rollout: deployed BG22 units keep
   truncating at 2048 until updated, and a host cannot tell an updated device from a stale one
   — but the window closes over the air rather than at a bench, which is why this is a rollout
   ordering problem and not a permanent hazard.

**ESP32 deployed-unit migration, decided 2026-07-25: flash and reconfigure.** No
LittleFS→NVS config migration path is built. A deployed S3 or C6 unit is physically reflashed
with the IDF firmware and then reconfigured from the host; stored config is not preserved
across the transition. This removes the Phase B storage-migration work entirely and frees the
partition layout, since every unit is being touched anyway.

Two things follow, and the second is the valuable one:

- **The cost is operational, not engineering** — one physical visit plus one reconfigure per
  deployed unit. That cost is now known and bounded, rather than being a migration hazard.
- **It is the one chance to fix C6's partitioning, and the layout is decided: `default.csv`.**
  ESP32-C6 ships on `huge_app.csv` — a single 3 MB app slot, so it can never be updated over
  the air. While each unit is on the bench, reflash it to **`default.csv`** (dual-slot), so it
  never needs a bench visit again:

  | Layout | app0 | app1 | filesystem |
  |---|---|---|---|
  | `huge_app.csv` (today) | 3 MB | — none — | 896 KB |
  | **`default.csv` (decided 2026-07-25)** | **1.25 MB** | **1.25 MB** | 1.4 MB |
  | `min_spiffs.csv` (fallback) | 1.875 MB | 1.875 MB | 128 KB |

  **The condition this decision rests on: the IDF image must fit in 1.25 MB.** That is the
  tighter of the two dual-slot options — chosen for the 1.4 MB filesystem — and it is less
  headroom than the app has today by a wide margin. Measure the C6 IDF binary before the
  rollout, not during it. If it exceeds 1.25 MB, `min_spiffs.csv` buys another 625 KB per slot
  at the cost of the filesystem; if it exceeds 1.875 MB, C6 is bench-only permanently and that
  should be recorded as a fact rather than rediscovered.

  Reflashing C6 back onto `huge_app.csv` is the one outcome to avoid — it forecloses OTA for
  that fleet forever, by choice rather than by constraint.

**Bootloaders, decided 2026-07-25:** MCUboot for the NCS targets going forward (nRF54L15 now,
nRF52840 when it lands) — the configuration already exists in `Firmware_NRF54` and is adopted
target-wide; see `targets/nordic-zephyr/README.md` § Bootloader. ESP32 keeps the IDF
second-stage bootloader and EFR32BG22 keeps the Gecko Bootloader + AppLoader; on both, changing
the bootloader would mean physically touching shipped units to gain capability the existing one
already provides.

The catch to carry forward: **MCUboot OTA is firmware-complete on nRF54L15 and undriveable** —
`py-opendisplay` has no SMP/mcumgr client, only legacy Nordic DFU and Silabs `.gbl`. One new
host backend covers both Nordic boards, and it is unbudgeted.

## Per-target procedure

1. Import the target's sources **unchanged** into `targets/<t>/` in one commit, so the diff
   afterwards is reviewable and blame stays meaningful.
2. Build it from the new location without touching logic. Fix only include paths and build
   files. Flash and verify on hardware. Commit.
3. Identify duplicated logic against `shared/` (or, for the first target, promote its logic
   *into* `shared/` behind HAL interfaces).
4. Replace the target's copy with the shared implementation **one subsystem at a time** —
   config parsing, then dispatch, then each transfer path. Build + flash + verify after each.

   > **Promotion order, amended 2026-08-05.** The *first* source in `shared/` is
   > `shared/core/od_adv_control.c`, not the config parser — see
   > [NEXT_STEPS_2026-08-05.md](NEXT_STEPS_2026-08-05.md) D1 and
   > [F4_PORTABLE_BLE_LIFECYCLE_PLAN.md](F4_PORTABLE_BLE_LIFECYCLE_PLAN.md). It is a narrow
   > infrastructure exception taken on architectural grounds: the advertising controller is a
   > self-contained plain-C state machine with no wire surface, so it establishes the
   > shared-source build and test pattern without moving protocol state.
   >
   > **The subsystem order above is unchanged.** Config parsing remains the first subsystem
   > that parses, stores, or alters wire behaviour, and the pre-auth-attack-surface reasoning
   > for putting it first still holds. The amendment inserts one non-protocol component ahead
   > of the sequence; it does not reorder the sequence.
5. Only then delete the corresponding code from the original repo, and point its README at
   this one.

Do not batch step 4. Each subsystem swap is independently revertable; a combined swap is not.

### The ESP32 import is different

Steps 1–2 above assume the framework stays constant. It does not for `esp32-idf`: removing
Arduino is not an "include paths and build files" fix, so an unchanged import cannot build.
Substitute three phases for steps 1–2, keeping steps 3–5 as written:

- **Phase A — import unchanged, non-building.** Bring `Firmware`'s `src/`, `lib/`, `tools/`,
  `scripts/` across in one commit that is *expected* not to compile. The point is provenance and
  reviewable blame; record the source repo and commit SHA in the message.
- **Phase B — shim to a booting build.** Add the IDF CMake/sdkconfig skeleton plus
  `targets/esp32-idf/compat/arduino_compat.h`, a deliberately temporary shim mapping the Arduino
  surface onto IDF (`String` → a minimal `std::string` wrapper; `pinMode`, `digitalWrite`,
  `delay`, `millis`, `delayMicroseconds` → their IDF equivalents — the full census is in
  TOOLCHAINS.md). The goal is to **link and boot on hardware early**, so every later step is
  bisectable against a known-good baseline. Two pieces cannot be shimmed and must be written
  here: the NimBLE C-API port of `ble_init.cpp` + `esp32_ble_callbacks.h` (~450 lines), and the
  IDF backends for `bb_epaper` (already upstream) and Seeed_GFX/TFT_eSPI.
- **Phase C — demolish the shim.** Delete `arduino_compat.h` usage subsystem by subsystem as
  steps 3–4 pull logic into `shared/`, then delete the file.

**The shim is the biggest risk in this plan.** Temporary compatibility layers become permanent
ones. Give it a mechanical ratchet rather than good intentions: a CI check that the number of
files including `arduino_compat.h` only ever *decreases*. The shim is scaffolding with a
scheduled demolition, not a portability layer — if it is still present when the last subsystem
lands, the port is not done.

> **CORRECTED TWICE.** 2026-08-05, from doing it: this paragraph said to "remove the check
> together with the file when it reaches zero", and the answer then was that zero was not
> reachable — the last five files were counted only for `TARGET_NRF` arms that do not compile
> on the ESP32 target.
>
> **RESOLVED 2026-08-16.** Those arms were deleted (they had been unreachable since the Nordic
> port took step 4), the count reached 0, and `compat/` was deleted. The check was replaced
> rather than retired: `tools/check.sh`'s "esp32: arduino-free app code" checks CALLS, because
> the ratchet's include-count reached 0 while three call sites still reached shim primitives.
> Record: [ARCHIVE_esp32_arduino_shim.md](ARCHIVE_esp32_arduino_shim.md).
>
> Separately, **FastEPD needs a *permanent* compatibility layer** — a vendored Arduino library
> whose IT8951 transport is written against the Arduino `SPI` object. That surface lives in
> `targets/esp32-idf/vendor/fastepd/`, outside `compat/` on purpose and excluded from the
> count, so that "delete `compat/`" stays an unambiguous instruction rather than a judgement
> call. It is deliberately **not** called a shim: in this repo "shim" means scheduled
> demolition, and using the word for something permanent poisons it for the thing the ratchet
> actually polices.

### The Silabs SDK is not imported as-is

`Firmware_Silabs` vendors `simplicity_sdk_2025.12.2/` — **57 MB across 658 git-tracked files,
~59 % of the 97 MB repo.** It is the source-only `-cp` copy (`slc generate --copy-sources`
output) that feeds the CMake build; it carries no `.slcc`/`.slce` component metadata, so it does
**not** enable `slc generate` and does not solve the hand-maintained-CMake hazard. Whatever the
import does, it must not silently carry 57 MB into the repo where every clone — including people
who only touch the ESP32 target — pays for it and every future SDK bump adds another permanent
copy to git history. Step 1's "import unchanged" is right about application sources and wrong
about a vendored SDK; this target gets one deviation, and the decision must be made *before the
first commit*, because a binary blob committed once is carried forever.

**Required: the SDK is installed at a pinned version, not vendored.** This is a decision, not a
preference — the vendored tree does not come across at import. It makes the rule uniform: *no
target vendors its SDK; every target requires one installed at a pinned version*, already true
of `esp32-idf` (IDF) and `nordic-zephyr` (NCS). The reasons:

1. **"Builds with nothing installed" is not a property this target actually has.** The build
   already needs `slt`-delivered ARM GCC 12.2, CMake, Ninja, and Simplicity Commander. The
   install is already mandatory; vendoring only makes *one component of an already-required
   install* checked-in instead of fetched.
2. **The exact version is installable — verified.** `slt list simplicity-sdk --versions`
   (checked 2026-07-25) returns `2024.12.1, 2025.6.0, 2025.6.2, 2025.6.3, 2025.12.0, 2025.12.1,
   2025.12.2, 2025.12.3, 2026.6.0` — including **2025.12.2**, the version in the tree today. A
   pinned `slt install` therefore reproduces the current build inputs exactly, the same way the
   other two targets pin IDF and NCS. This is the premise the requirement rests on; it has been
   confirmed rather than assumed.
3. **It fixes the regen hazard.** A real installed SDK has the `.slcc`/`.slce` metadata
   `slc generate` needs, so `cmake_gcc/opendisplay-bg22.cmake` becomes genuinely generated
   again rather than hand-maintained. That matters most *during* this migration, because adding
   `shared/`/`third_party/` sources goes through the `.slcp` — regen goes from ~never to routine.

Mechanically small: the generated CMake already reaches the SDK via `${COPIED_SDK_PATH}` /
`SDK_PATH`, so it is a variable repoint. Sequence it so the in-tree copy stays the safety net
until the replacement is proven: (1) install the pinned SDK and register it with `slc`;
(2) verify `slc generate` reproduces the CMake; (3) verify a clean `./build-and-flash.sh
--no-flash` against it; (4) only then import without the vendored tree, recording the pinned
version and install command in `targets/efr32bg22-slc/README.md`.

**Step 1 is already half-done on the primary dev box** (verified 2026-07-25): full Simplicity
SDK `2025.12.2` — the exact pinned version — is installed at
`~/.silabs/slt/installs/conan/p/simpl965e19baece23/p` with 2892 `.slcc` files. What remains is
registration: no `.slconf` exists in the project and `~/.uc/cli/cli.config` lists no SDK, so
`slc generate` must be handed `--sdk-package-path`. Treat step 1 as a configuration task, not a
download.

If step 2 or 3 fails, that is a **blocker to escalate, not a licence to fall back to
vendoring.** Reverting to a vendored SDK reverses a decision recorded here and needs an explicit
call; do not make it silently because an install was awkward on one box.

> **Contingency only — if the requirement is ever formally reversed.** Should vendoring come
> back, do not re-import the tree as-is: drop what the GCC build provably never links. The
> toolchain is pinned to ARM GCC 12.2 (hardcoded fallback in `cmake_gcc/toolchain.cmake`), so
> IAR/LLVM build trees and non-GCC RAIL libraries are dead weight — measured 2026-07-25 at
> **17.6 MB** (`*/build/iar/` + `*/build/llvm/`) plus **3.8 MB** (non-GCC RAIL) = **21.4 MB
> removable, ~36 MB remaining**. `grep -ciE '/iar/|/llvm/' cmake_gcc/opendisplay-bg22.cmake`
> returns `0`, and all 22 linked archives resolve under `build/gcc/cortex-m33/` or
> `build/gcc/xg22/`.
>
> Note that "the tree is already a `-cp` subset dominated by genuinely-linked archives" and
> "21.4 MB of it is removable" are both true and not in conflict: the large subtrees
> (`bluetooth_le_host` ~20 MB, `bluetooth_le_controller` ~15 MB, `rail_library` ~7.5 MB) each
> ship GCC, IAR **and** LLVM builds of the same libraries, and only the GCC ones are linked. An
> earlier note treated these as contradictory; they are not.

## Verification bar per subsystem

Two gates, in order. **Host tests come first** — they are cheap, parallel, and exercise the
error paths that hardware testing never reaches. Do not promote a subsystem into `shared/core`
without them, because the promotion is exactly when its error handling gets rewritten.

**Gate 1 — host, no hardware** (ARCHITECTURE.md § "Everything on the wire is testable without
hardware"; owned by this repo under `tests/` — see TEST_OWNERSHIP.md):

- unit tests for every function in the promoted subsystem, run in CI on every push,
- fuzz coverage for anything reachable pre-authentication (config TLV parse, frame dispatch),
- stress coverage for the transfer state machines: reordered, duplicated, truncated, and
  interleaved chunks,
- shared wire vectors passing against both the C core and `py-opendisplay`, and
- the subsystem compiling under every `OD_*_ENABLE` permutation it participates in.

**Gate 2 — real hardware for that target**, unchanged:

- a full image push renders correctly (compressed and uncompressed),
- a config read/write round-trips,
- the encrypted/authenticated path works if the target supports it, and
- an interrupted transfer recovers (disconnect mid-stream, then retry).

Gate 1 does not replace Gate 2 — a passing host suite says the logic is right, not that it runs
on the chip. But a subsystem that fails Gate 1 should never reach a board.

## Risks to watch

- **Concurrent development on `Firmware` during the ESP32 port.** ~~Decide up front.~~
  **DECIDED 2026-07-25: features are frozen on `Firmware` for the duration of the port.** This
  was the risk most likely to derail the migration — a multi-week IDF port against the most
  actively developed repo, which put +2007 lines in on 2026-07-25 alone (`#124`). The freeze
  removes it, and unblocks Phase A.

  What the freeze does *not* cover, and should be stated so it is not assumed: security
  hotfixes (deferred separately — see below), build breakages, and changes to targets the port
  does not touch. If an exception is needed, land it on `Firmware` and rebase the port
  deliberately rather than letting the port drift silently.

  The freeze has a cost that starts accruing now: every week of it is a week `Firmware` does not
  ship features. That argues for keeping Phase A→B short and for not letting the port stall on
  decisions that could have been made before it started.
- **Migration order was set before the fleet status was known.** Step 1 (`esp32-idf`) is a
  multi-week framework change against **two shipped product lines**, one of which (ESP32-C6)
  cannot be field-updated at all. Step 2 (`nRF54L15`) is **not shipped** — zero field risk, and
  the docs' own preferred structural donor for `shared/` (already C, already HAL-shaped). The
  ordering rationale in "Order and rationale" is about feature completeness and boundary stress,
  both of which predate knowing this.

  The case for swapping steps 1 and 2: promote `shared/core` first on the target where a mistake
  cannot reach a customer, then bring the reference implementation to a boundary that has
  already survived one consumer. The case against: `Firmware`/ESP32 defines the feature set, so
  `shared/core` shaped by NRF54 first may need widening later — and features are frozen on
  `Firmware` *now*, so a long NRF54 detour spends that freeze without reducing the ESP32 work.
  **Not re-decided here; priced so it can be.**
- **Security bugs found during the survey — hotfix DEFERRED (decided 2026-07-25).** Three
  defects were found in shipping firmware while surveying for this migration. The decision is
  **not** to hotfix them in the source repos now; they are fixed when their subsystem is
  promoted to `shared/core`, on the shared implementation. Recorded here so the exposure is
  deliberate and reviewable rather than forgotten:

  | Defect | Where | Exposure while deferred |
  |---|---|---|
  | Plaintext bypass for frames < 31 bytes | `Firmware_Silabs/opendisplay_pipe.c:1236` | With `sec_enabled()`, short commands (REBOOT, DEEP_SLEEP) execute unauthenticated from any BLE peer — no session required |
  | Session survives a key change | Silabs — no `clear_session()` on any config-save path | An old session keeps working after the encryption key is rotated |
  | `diff == 0` replay | session/nonce handling | A replayed frame at the current counter is accepted |

  These are BLE-proximity attacks on display devices, not remote ones, which is the reason the
  deferral is defensible. Two consequences to accept along with it: the exposure lasts until the
  session/dispatch subsystems are promoted — months, not weeks, and *after* Silabs is step 3 —
  and the shared implementations must land with these closed, so they are Gate 1 test cases
  (DIVERGENCE_MATRIX §1.5a, §2.4), not TODOs. If the timeline slips materially, revisit this
  decision rather than inheriting it by default.

  **Revisit trigger reached 2026-07-25, before the timeline even started to slip.** EFR32BG22
  was confirmed shipped, and it is the one target with a working field-update path (§ "Deployed
  fleet status", consequence 4). The first two defects in the table are Silabs defects on that
  fleet. So the deferral now trades a real, months-long exposure on deployed hardware against a
  fix that could ship over the air — which is not the trade that was priced when the deferral
  was taken, because BG22's fleet status was not in the table at the time. The decision stands
  until it is explicitly re-taken; this note exists so that re-taking it is a choice and not an
  archaeology exercise.
- **Silent behavioural divergence.** The repos do not implement identical semantics today. When
  promoting logic to `shared/`, differences must be resolved deliberately and written down — not
  settled by whichever repo was copied first. Note the two donors pull in different directions
  and both are right about something: `Firmware` has the most complete **feature coverage**,
  while `Firmware_NRF54` has the better **structure and language** for `shared/` (it is already
  C, which `shared/` must be). Take coverage from the first and shape from the second.
- **The Arduino shim outliving its purpose.** See "The ESP32 import is different" above.
- **Protocol header drift.** Until the sync tool's copy map includes
  `Firmware_Unified/shared/protocol/`, the headers here are unpoliced. Fix that early. Two
  further facts, verified with `sync_protocol_header.py --check` / `git show` (see
  DIVERGENCE_MATRIX.md §8): canonical is 2.2 + an unreleased `OD_BLE_MAX_FRAME`, but **the true
  ESP32 import source, `upstream/main` of `Firmware`, is still at protocol 2.1** — missing all
  of SECTION 9 (LAN) — so the first import lands a stale header that must be re-synced from
  canonical, not trusted as-is. And `upstream/main` PR #120 made a comment-only Seeed_GFX→FastEPD
  edit to the *vendored* `opendisplay_structs.h` that canonical does not yet carry; apply that
  wording to the canonical structs header *before* any `--push`, or a push will revert it.

  **Half of this risk is now void (2026-07-25, measured at the Phase A import).** The
  "import lands a stale 2.1 header" limb was based on a wrong reading: `upstream/main` is at
  **2.2**, because `#124` *is* the SECTION 9 LAN feature. Its header differs from canonical by
  ten doc-only lines with no wire difference, and Phase A did not import it at all — the ESP32
  target uses `shared/protocol/` directly, so there is no tenth copy to drift. What remains
  live is the second limb: the copy-map gap, and PR #120's structs wording, which must be
  applied to canonical *before* any `--push`.
- ~~**Scaffold docs disagree on the nRF52840 step number.**~~ **Fixed 2026-07-25.**
  `targets/nordic-zephyr/README.md` said "step 3"; the order in this file is authoritative and
  makes the nRF52840 port **step 4** (Silabs is step 3, because it must bite third to keep
  `shared/` honest). The target README now says step 4, and additionally distinguishes the
  nRF52840 — a supported board of `nordic-zephyr` — from the legacy nRF52 of item 5, which is
  not migrated here at all.
- **Memory regressions on small targets.** Shared code sized for the ESP32 will not fit the
  EFR32BG22's 32 KB. Keep buffer sizes target-parameterised as plain preprocessor constants —
  *not* as Kconfig options, since the Silabs target has no Kconfig. See ARCHITECTURE.md.
- **Designing `shared/` against the wrong target's assumptions.** The EFR32BG22 has no kernel,
  no threads, no blocking sleep, and 32 KB of RAM. An API shaped by the ESP32's FreeRTOS and
  PSRAM will not retrofit onto it. This is why Silabs is step 3, not step 4.
- **No dev box builds every target.** ~~Neither `idf.py` nor `west` is installed on the primary
  machine today~~ — **corrected 2026-07-25: all three are installed** (ESP-IDF v5.5.4, NCS
  v3.3.1 with west v1.5.0, Simplicity SDK 2025.12.2 — TOOLCHAINS.md § "All three toolchains are
  installed on this dev box"). None is on `PATH`, which is how the earlier claim survived: each
  needs an activation step, so `which` reports nothing.

  This downgrades the risk rather than closing it. One machine with one set of versions is not
  a build matrix, and nothing here has actually been built yet — only version strings were run.
  Stand up the CI matrix early anyway; the `shared/` boundary grep is necessary and nowhere
  near sufficient. What *does* change is the migration's inner loop: a target import can now be
  compiled locally before it is pushed, so the "verify on hardware" gate is no longer the first
  time anyone learns whether the thing links.
- **OTA shipped through HA exists for exactly one target, and the migration assumes more.**
  Field firmware update through the shipped HA path works only on EFR32BG22 (`.gbl` via the
  Silabs AppLoader, the sole OTA extra pinned in `Home_Assistant_Integration`'s
  `manifest.json`). ESP32 has none — `ENTER_DFU` merely reboots. Deployed nRF52840 units *do*
  have a working BLE DFU path — the Adafruit bootloader plus in-app `bledfu`, driven by
  `py-opendisplay`'s `perform_nrf_dfu` (targets/nordic-zephyr/README.md § "Deployed nRF52840")
  — but HA does not pin the `nrf-ota` extra, so it is library capability rather than shipped
  product behaviour.

  The consequence is concrete: **a target with no field-update path cannot have its flash
  layout or bootloader changed without a bench reflash of every deployed unit.** That is a hard
  constraint on the ESP32 framework change (step 1) and on the nRF52840 port (step 4), whose
  own entry already says "settle deployed-unit OTA/flash-layout compatibility before starting"
  — this is why. **Open item for investigation: can OTA be extended to more targets?** Resolve
  it before Phase B commits to a partition layout, and record the answer here. It may
  legitimately conclude "no, and these units are bench-updated only" — but that must be a
  decision, not a discovery made after the layout changed.

  *(Partially resolved 2026-07-25 — § "Deployed fleet status" above: the transition is
  flash-and-reconfigure, so the partition layout no longer waits on this (S3 stays dual-slot,
  C6 moves to `default.csv` on the bench). What remains of the open item is whether the update
  implementations get built: `esp_ota` on S3, and the unbudgeted SMP/mcumgr host backend the
  Nordic MCUboot path needs.)*
- **Vendored binaries bloating history permanently.** The Silabs SDK is the large case (see "The
  Silabs SDK is not imported as-is"), but the rule is general: a binary committed once is
  carried forever, and every target here vendors *something*. Decide what a blob's pruned form
  is before its first commit, not after it has been bumped twice.
- **Losing history.** Prefer imports that preserve provenance where practical; if a plain
  copy is used, record the source repo and commit SHA in the import commit message.
