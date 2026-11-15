# tests

Host-runnable tests for `shared/`. No hardware, no target toolchain.

Ownership, rationale, and the full mechanism are in [docs/TEST_OWNERSHIP.md](../docs/TEST_OWNERSHIP.md);
this file is just how to run things.

```bash
cmake -S tests/host -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Add `-DCMAKE_C_COMPILER=clang` for the second compiler. Neither compiler is required
locally — CI runs both, and gcc alone is enough to work here.

## Layout

```
vectors/     the wire-vector corpus (JSON, one file per subsystem), drive_panels.inc and the
             od_epd_cfg golden bus traces (vectors/epd_golden/)
host/        host build of shared/ + CTest wiring (tests/host/CMakeLists.txt, drive_coverage.cmake):
             one *_test.c per subsystem, fakes per target under fake_*/, the C corpus runner
             (dispatch_corpus) and replay_vectors.py (the py-opendisplay side)
fuzz/        libFuzzer harnesses over the pre-auth surface (session open/auth, PIPE, config_drive)
             with their committed seed corpora under fuzz/corpus/<target>/
panels/      test_panels.py: drive presets (panels/generated/) replayed through od_epd vs bb_epaper
cmake/       shared CMake helpers
```

The full gate is `tools/check.sh` (`--list` names every step). Two host tests,
`bench_dispatch_gate` and `nfc_read_tool`, need PyYAML + cryptography in the Python CMake picks
(tools/requirements-host.txt); check.sh reports them as one SKIP when those are missing, while a
bare `ctest` reports them as failures.

## Why this exists before any target code

`shared/` is plain C with no vendor headers, so it compiles for the host **by
construction** — the boundary rule and host testability are the same property seen twice.
That makes this the cheapest real enforcement available, and the reason it is standing up
first: every later import lands as a test-first change into a build that already enforces
the contract, instead of having to stand the contract up afterwards.

It is deliberately green against an empty `shared/`. Nothing here is waiting on a target.

## The two runners

The corpus is data. It is replayed by **two** implementations of the wire protocol — the C
core (once `shared/` has one) and `py-opendisplay`. A vector both sides agree on is a
contract test; a vector they disagree on is exactly the host/firmware divergence this repo
exists to eliminate, caught in CI instead of on a bench.

Both sides exist: `dispatch_corpus` replays `vectors/dispatch.json` through the production
`od_dispatch_frame()`, and `replay_vectors` drives the same corpus through py-opendisplay
(registered only when py-opendisplay is importable; tools/check.sh runs it via uv).

`replay_vectors.py` calls py-opendisplay's **public API only** and must never reimplement
encode logic — a vector both runners derive from the same local helper proves nothing.
Vectors it cannot express are reported as `SKIP` with a reason, never quietly passed.

## The compiler is the boundary check

The `shared boundary:` steps of [tools/check.sh](../tools/check.sh) (run by
[.github/workflows/ci.yml](../.github/workflows/ci.yml)) grep for
vendor includes. A grep sees `#include` lines and literal tokens; it cannot see an
`extern`-declared vendor symbol, a macro leaked through a target config header, or a libc
call that does not exist on a freestanding target. `-std=c99 -Wall -Wextra -Werror` with
`CMAKE_C_EXTENSIONS OFF` does. Treat the grep as the fast pre-filter and this as the real
gate.

## Not here yet

- **HAL test doubles as one set** -- the fakes exist per target (`host/fake_*`), not one per
  shared interface.
- **ESP32 SMP/OTA fuzzing** -- `od_smp.cpp` needs ESP-IDF OTA/partition/timer/FreeRTOS fakes, or
  its frame parser split into a vendor-free `.c` that a fuzzer can link.
- **Captured wire vectors.** The corpus is authored. Captures from real sessions have their own
  deadline: once `shared/core` replaces a target's logic, there is no longer an untouched
  reference to capture from. See TEST_OWNERSHIP § "Capture is time-sensitive".
