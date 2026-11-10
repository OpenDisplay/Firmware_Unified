# Firmware_Unified

One repository for **all OpenDisplay firmware targets**, replacing four independently
versioned repos that had drifted apart while implementing the same wire protocol.

> **Status: all three targets build; two are hardware-verified** (updated 2026-08-17).
> `targets/esp32-idf/` builds **11 boards** (9 shipping + 2 debug) and is verified on an
> ESP32-S3. `targets/nordic-zephyr/` builds three boards and is verified on `xiao_nrf52840`
> only — its other two boards build clean but have never been flashed.
> `targets/efr32bg22-slc/` builds headless and **has never been flashed**.
> [docs/HARDWARE_MATRIX.md](docs/HARDWARE_MATRIX.md) is the board list, with the release/debug
> variants and the kernel each runs on;
> [docs/HARDWARE_VERIFICATION_CHECKLIST.md](docs/HARDWARE_VERIFICATION_CHECKLIST.md) is what has
> actually run on silicon. A board building is not a board working.
>
> Two things that sound like omissions but are deliberate:
>
> - **The Arduino shim is gone** (2026-08-16). `targets/esp32-idf/compat/` went 22 files to 0 and
>   was deleted. What survives is the permanent FastEPD adapter under
>   `targets/esp32-idf/vendor/fastepd/`, which is not a shim and does not die; the record of the
>   demolition is [docs/ARCHIVE_esp32_arduino_shim.md](docs/ARCHIVE_esp32_arduino_shim.md).
> - **`efr32bg22-slc` runs no RTOS at all** — a bare-metal superloop, not an unfinished port.
>   Zephyr was rejected for it on RAM (32 KB total), and its BGAPI event-retention design is only
>   valid without a kernel, so the build fails if one appears.
>
> The ESP32 target is **not** correctness-signed-off. Ten findings from
> [docs/CORRECTNESS_REVIEW_2026-08-04.md](docs/CORRECTNESS_REVIEW_2026-08-04.md) are open or
> partly closed; the WiFi/LAN transport in particular has never been exercised on hardware.

## Why unify

The four firmware repos each implement the same BLE/LAN wire protocol, config-packet
parsing, chunked-transfer state machines, compression, and session encryption — separately.
That has real costs:

- **Fixes land once, per repo.** A bug in the config parser or a transfer state machine has
  to be found and fixed up to four times.
- **The protocol header is copied byte-for-byte into every repo**, kept in sync only by a
  script and a CI check.
- **Behaviour silently diverges.** Targets differ in which transmission modes, refresh
  paths, and error responses they actually implement, with no single place to compare.

Unifying puts the target-agnostic ~80% in `shared/` and leaves each target only its
chip-specific drivers and build system.

## Layout

```
shared/          target-agnostic. MUST NOT include any vendor SDK header.
  protocol/      canonical wire contract (see "Protocol header" below)
  core/          command dispatch, config parsing, transfer state machines, session auth
  compress/      inflate engines (uzlib bit-serial, ROM tinfl adapter) behind one API
  hal/           abstract interfaces that targets implement (SPI, GPIO, timers, NVS, radio)
targets/         one directory per target: chip drivers + build system + HAL implementation
third_party/     vendored cross-target libraries whose IO layer is per-target (bb_epaper)
tools/           protocol-header sync, provisioning, device CLI
docs/            architecture and migration notes
```

`third_party/` exists because `bb_epaper` selects its IO backend by `#ifdef` and each backend
includes vendor headers, so it can never satisfy the `shared/` rule — but it is still one
vendored copy shared by every target, not a per-target fork.

## Targets

| Directory | Chips | Boards | Kernel | Build system | From |
|---|---|---|---|---|---|
| `targets/esp32-idf/` | ESP32-S3 / C3 / C6 / classic | 11 | FreeRTOS | **ESP-IDF** (CMake + Kconfig) | `Firmware` |
| `targets/nordic-zephyr/` | nRF54L15, nRF54LM20A, nRF52840 | 3 | Zephyr | Zephyr / nRF Connect SDK + west | `Firmware_NRF54`, `Firmware` |
| `targets/efr32bg22-slc/` | EFR32BG22 | 1 | **none** (superloop) | Simplicity SDK + SLC (CMake) | `Firmware_Silabs` |

Board-by-board, including which variants are debug builds and how each target produces one:
[docs/HARDWARE_MATRIX.md](docs/HARDWARE_MATRIX.md).

**Three targets, not four.** The legacy nRF52 (bare Nordic SDK, `Firmware_NRF` — *not* the
nRF52840, which is a board of `targets/nordic-zephyr/` above) is deliberately absent: it **is
shipped** — a handful of low-capability devices — but it is maintained in place in its own repo
and never migrates here, so this repo carries no directory for it. It still
binds the wire contract, and is the strictest one in the fleet: no compression, no `0x76`, no
PIPE, no NFC, no `LED_STOP`, no `CONFIG_CLEAR`, no `READ_MSD`. That is the
backward-compatibility floor `py-opendisplay` must keep meeting, because those units cannot be
updated in practice. See [docs/MIGRATION.md](docs/MIGRATION.md) § "Order and rationale" item 5.

Targets are grouped **by silicon vendor, not by repo of origin**. That is why nRF52840 sits with
nRF54L15 rather than with the ESP32s it currently shares a repo with: the two Nordic parts share
a BLE stack, crypto API, storage API, and panel stack, so nRF52840 is a *board* on the Zephyr
target, not a target of its own.

**No PlatformIO and no Arduino anywhere in this repo.** All three targets build with CMake,
which is what lets `shared/` be one library consumed by all of them. Two of the three add
Kconfig; the Silabs target does not, so `shared/` takes plain preprocessor constants and Kconfig
is only how the other two set them. The full analysis — including the Arduino-API replacement
census and the PlatformIO-knob → sdkconfig translation table — is in
[docs/TOOLCHAINS.md](docs/TOOLCHAINS.md).

Three build systems stay three build systems: unification is about **shared source**, not a
single build system. Do not attempt to make one build system drive all three.

## The `shared/` rule

`shared/` compiles for every target, so it may use only the C standard library and
`shared/hal` interfaces. It is **plain C** — two targets are C-only, and CI rejects
`.cpp`/`.hpp` and Arduino `String` under `shared/`. **No `esp_*`, `nrf_*`, `sl_*`, `zephyr/*`,
or `Arduino.h` includes.**
Anything needing a peripheral goes through `shared/hal`, implemented per target.

This is the invariant that makes the repo worth having. Code that reaches for a vendor
header belongs in `targets/`.

## Protocol header

`shared/protocol/` holds the canonical wire contract, copied from the
[`opendisplay-protocol`](../opendisplay-protocol) repo. It is **pure `#define` constants and
config structs** — no functions, no per-target types.

**Never hand-edit these files.** Change the canonical source, then propagate:

```bash
cd ../opendisplay-protocol
tools/sync_protocol_header.py --push     # canonical -> all copies
tools/sync_protocol_header.py --check    # fail if any copy drifted (CI/pre-commit)
```

> **Required follow-up:** the sync tool's copy map does not know about this repo yet, so the
> headers here are currently a one-time copy and `--check` will not police them. Add
> `Firmware_Unified/shared/protocol/` as a destination in
> `opendisplay-protocol/tools/sync_protocol_header.py` before relying on them.

## Versioning and releases

**Semantic versioning, one version line for the whole repository.** Decided 2026-07-25,
resolving docs/DESIGN_REVIEW_2026-07-25.md § F6. Tags are `MAJOR.MINOR.PATCH` (`release.yml`
also accepts a leading `v`; see docs/RELEASE_ARTEFACTS.md § 1); the four
per-repo schemes this repo replaces (ESP32 `BUILD_VERSION "1.0.0"`, nRF54 `OD_APP_VERSION`
`0x0100`, Silabs `0x0019`) do not carry over.

For firmware, the semver components mean:

| Bump | When |
|---|---|
| **MAJOR** | a breaking wire or stored-config change, or anything that requires a coordinated `py-opendisplay` release to keep working |
| **MINOR** | a new backward-compatible capability — a new opcode handler, a new config packet, a new panel or board |
| **PATCH** | a fix with no observable wire change |

### One line for all targets, not one per target

The alternative — per-target tags like `esp32-idf/v2.3.0` — is what the four separate version
schemes suggest, and it is **not available today**, for a hard reason rather than a stylistic
one: *nothing on the wire identifies which target a device is.* `CMD_FIRMWARE_VERSION` (`0x43`)
returns version bytes and a commit SHA but no hardware type, and `MsdAdvertisement` carries none
either (DESIGN_REVIEW § F1.4). A host reading `1.4.2` from an unknown device would have no way
to know *whose* 1.4.2 it is, so per-target numbering would be uninterpretable by the only
consumer that matters. A single line is unambiguous without target identity.

Revisit this if target identity ever lands on the wire — that is the one change that makes
per-target versioning workable, and it is currently blocked by the header freeze.

The cost of a single line, stated plainly: **a release bumps the version for targets that did
not change.** That is accepted. The version identifies the *source tree* a binary was built
from, which is what a host actually needs in order to reason about behaviour; the commit SHA in
the same response pins the exact build.

### It maps onto the frozen wire exactly

No protocol change is needed, which is why this could be decided while the headers are frozen.
The `0x43` response is already:

```
[0x00][0x43][major:1][minor:1][shaLen:1][sha:shaLen][patch:1]?
```

Three consequences. Each component is **one byte, so 0-255** — not a practical limit, but a real
one. Older firmware **omits** the trailing patch byte and a host must read a missing byte as
patch `0`, which the canonical header already mandates. And `major`/`minor` stay at fixed
offsets ahead of the SHA, so an old host parsing a new device still gets a correct major.minor.

### The first release is `3.0.0`

The original plan was `v2.0.0`, so that "major ≥ 2" would mean "built from
`Firmware_Unified`". That no longer works: `Firmware` reached 2.27 and `Firmware_Silabs` 2.3
while this repo was being built. 3.0.0 is the first number above every shipped line (ESP32
2.27.x, nRF54 1.1.x, BG22 2.3.x). It is also the MAJOR bump that the config-driven drive,
the ESP32 NVS move and the nRF52840 bootloader change call for anyway (CHANGELOG.md).

Release mechanics:

- the artefact contract is [docs/RELEASE_ARTEFACTS.md](docs/RELEASE_ARTEFACTS.md);
- the go/no-go list is [docs/RELEASE_CHECKLIST_3.0.md](docs/RELEASE_CHECKLIST_3.0.md);
- the per-target steps are [docs/UPGRADING.md](docs/UPGRADING.md).

A tag builds every target with that version stamped in and creates a **draft** release; a
maintainer publishes it. 3.x is published only from this repository, under names no 2.x
updater matches.

A release records **which targets were built and hardware-verified at that tag**. Targets are
verified separately by construction, and a device that was not updated keeps reporting its
older version. That is expected, not a defect.

`git tag` holds five tags (`0.0.1-design`, `0.1.0`, `0.1.1`, `0.2.0`, `nrf51822-v0.1.0`). They came with
imported history and are not releases of this repo.

> **Do not couple this to `OD_PROTOCOL_VERSION`.** They are
> unrelated numbers on different schedules: the protocol version (`2.2`) is the wire contract
> owned by `opendisplay-protocol`, and firmware `3.0.0` is this repo's build. A firmware patch
> release does not touch the protocol version, and a protocol MINOR bump does not require a
> firmware MAJOR.

## Getting started

The ESP32 target builds every board with one command, which sources ESP-IDF itself:

```bash
cd targets/esp32-idf
./build.sh                 # every board -> release/, with a MANIFEST
./build.sh --release       # the shipping boards; excludes the -debug ones
./build.sh s3-n16r8        # or just one; ./build.sh --list to see them
tools/run_host_tests.sh    # host tests under ASan+UBSan and TSan+UBSan
tools/sdkconfig_baseline.sh
```

The last two are gates a change must not break, and `tools/check.sh --targets` at the repo root
builds all three target families. Per-target details are in each `targets/*/README.md`; the full
board list, its release/debug variants and the kernel each board runs on are in
[docs/HARDWARE_MATRIX.md](docs/HARDWARE_MATRIX.md).

All three toolchains are installed on the primary dev box (ESP-IDF v5.5.4, nRF Connect SDK
v3.3.1 with west v1.5.0, Simplicity SDK 2025.12.2), though **none is on `PATH`** — each needs
an activation step, listed in [docs/TOOLCHAINS.md](docs/TOOLCHAINS.md). All three build both
here and in CI (`.github/workflows/build.yml`, pinned containers and SDKs). CI is the reference:
one machine with one set of versions is not a build matrix.

Host-runnable tests for `shared/` need none of that — see [tests/](tests/README.md):

```bash
cmake -S tests/host -B build -G Ninja && cmake --build build && ctest --test-dir build
```

- [docs/HARDWARE_MATRIX.md](docs/HARDWARE_MATRIX.md) — every board, its chip, its kernel, and how
  its release and debug variants are built. "Debug" is a different mechanism on each target
- [docs/HARDWARE_VERIFICATION_CHECKLIST.md](docs/HARDWARE_VERIFICATION_CHECKLIST.md) — what has
  been observed on real silicon, per board and per behaviour
- [docs/TOOLCHAINS.md](docs/TOOLCHAINS.md) — which toolchain each target uses and why
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — the `shared`/`hal` boundary
- [docs/MIGRATION.md](docs/MIGRATION.md) — migration order and per-target procedure
- [docs/NEXT_STEPS.md](docs/NEXT_STEPS.md) — **historical.** The migration sequence as of
  2026-07-26; items 1-4 are closed and its forward-looking half is superseded. Read it for the
  reasoning it records, not to decide what to do next
- [docs/FOLLOWUPS.md](docs/FOLLOWUPS.md) — known defects and open items awaiting action,
  including host-side bugs the wire corpus found
- [docs/CORRECTNESS_REVIEW_2026-08-04.md](docs/CORRECTNESS_REVIEW_2026-08-04.md) — ten findings
  against the ESP-IDF target; F1 and F2 are fixed, the other eight are open
- [docs/F4_PORTABLE_BLE_LIFECYCLE_PLAN.md](docs/F4_PORTABLE_BLE_LIFECYCLE_PLAN.md) — proposed
  design for finding F4; no implementation is implied
- [docs/BBEPAPER_IO_BACKENDS.md](docs/BBEPAPER_IO_BACKENDS.md) — how `bb_epaper` is ported to a
  platform, which backend each target uses, and the open decision on owning the ESP-IDF one

## License

**GPL-3.0** — see [LICENSE](LICENSE). Byte-identical to the licence carried by every source
repo this one consolidates (`Firmware`, `Firmware_NRF54`, `Firmware_Silabs`, all GPLv3,
29 June 2007), which is what makes importing their sources here lawful. Settled 2026-07-25,
before the first import.

Vendored third-party code keeps its own licence and its own notice; do not relicense it on the
way in. That applies to `bb_epaper` and `FastEPD` (Larry Bank), `uzlib`, SEGGER RTT, and any SDK
sources a target carries. Record the origin and licence of each vendored tree in the target
README that pulls it in.
