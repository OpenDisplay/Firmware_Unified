#!/usr/bin/env bash
# Host tests for this target's portable logic. Run from anywhere:
#
#     targets/esp32-idf/tools/run_host_tests.sh
#
# src/link_owner.cpp, the connection-ownership arbiter, plus the AXP2101 helpers
# (src/sensor_axp2101.h and the register map pinned by tests/host/axp2101_registers_test.c).
# link_owner is target code
# rather than shared/ code, so it is not covered by tests/host/ -- but it is the one file in
# the target whose bugs are genuinely cross-task (BLE claims run on the NimBLE host task, LAN
# claims and every release on the loop task) and therefore unreachable by inspection. It is
# also the clearest shared/ promotion candidate here, so it is worth gating before the
# promotion rather than after.
#
# Two sanitizer builds, matching the reference firmware's: ASan+UBSan, then TSan+UBSan as a
# SEPARATE binary, because TSan and ASan cannot be combined.
#
# The test binary implements the canonical clock seam over its own atomic clock. Both od_hal_time
# and od_log come from shared/, so the real src/link_owner.cpp compiles without target shims.
set -euo pipefail

cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/od-host-tests"
mkdir -p "$OUT"

# The fake is a link-time implementation, not a shadow header. That keeps the test on the same
# declaration every production target uses and prevents include-path order from changing the seam.
CXXFLAGS=(-std=c++17 -Wall -Wextra -Werror -O1 -I ../../shared/hal -I ../../shared/core -I src)
SRCS=(tools/test_link_owner.cpp src/link_owner.cpp)

# ThreadSanitizer aborts at startup with "unexpected memory mapping" under some kernels' ASLR
# layouts -- WSL2 in particular. setarch -R disables ASLR for the child only.
#
# ALWAYS use setarch when it exists. Do not probe.
#
# Two earlier designs failed, and both failure modes are worth knowing:
#
#   * Run the real binary and retry on failure. TSan's abort does not stop the test process, so
#     the first attempt prints a spurious "FAIL ... 1 failures" line before the retry succeeds.
#     The log then shows a failure that did not happen.
#   * Probe with a throwaway binary and use setarch only if the probe fails. This is what was
#     shipped, and it is WRONG: a trivial probe has different memory mappings from the real
#     test binary, so a passing probe does not predict it. Measured 2026-08-03 -- roughly 1 run
#     in 6 had the probe succeed and the real binary then abort, producing the same spurious
#     FAIL line plus an intermittent non-zero exit (66, TSan's default error code) from a suite
#     that is actually clean. An intermittently red gate is worse than no gate: it trains people
#     to re-run until green, which is exactly how a real race would get ignored.
#
# Disabling ASLR for a short single-purpose test child costs nothing and removes the variable
# entirely, so it is not conditional on detecting a problem first.
TSAN_RUN=()
if command -v setarch >/dev/null 2>&1 && setarch -R true >/dev/null 2>&1; then
    TSAN_RUN=(setarch -R)
else
    echo "note: setarch -R unavailable; if TSan aborts with 'unexpected memory mapping' that is" >&2
    echo "      this kernel's ASLR layout, not a test failure" >&2
fi

have_tsan() {
    printf 'int main(void){return 0;}\n' > "$OUT/tsan_probe.c"
    gcc -fsanitize=thread "$OUT/tsan_probe.c" -o "$OUT/tsan_probe" 2>/dev/null
}

echo "==> link_owner: ASan + UBSan"
g++ "${CXXFLAGS[@]}" -fsanitize=undefined,address "${SRCS[@]}" -o "$OUT/test_link_owner" -pthread
"$OUT/test_link_owner"

if have_tsan; then
    echo "==> link_owner: TSan + UBSan"
    g++ "${CXXFLAGS[@]}" -fsanitize=thread,undefined "${SRCS[@]}" \
        -o "$OUT/test_link_owner_tsan" -pthread
    "${TSAN_RUN[@]}" "$OUT/test_link_owner_tsan"
else
    echo "==> link_owner: TSan pass SKIPPED -- no ThreadSanitizer in this toolchain" >&2
fi

echo "==> sensor_axp2101: ASan + UBSan"
g++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=undefined,address -I ../../shared/protocol -I src \
    tools/test_sensor_axp2101.cpp -o "$OUT/test_sensor_axp2101"
"$OUT/test_sensor_axp2101"

echo "==> axp2101 register map"
cc -std=c11 -Wall -Wextra -Werror -O1 ../../tests/host/axp2101_registers_test.c \
    -o "$OUT/axp2101_registers_test"
"$OUT/axp2101_registers_test"

echo "==> od_controller_esp (0xB3 display_controller): ASan + UBSan"
cc -std=c11 -Wall -Wextra -Werror -O1 -fsanitize=undefined,address -DOD_CFG_INDEX_CONTROLLER=1 \
    -I ../../shared/core -I ../../shared/hal -I ../../shared/protocol -I ../../shared \
    -c ../../shared/core/od_cfg_index.c -o "$OUT/od_cfg_index_ctrl.o"
cc -std=c11 -Wall -Wextra -Werror -O1 -fsanitize=undefined,address \
    -I ../../shared/core -I ../../shared/hal -I ../../shared/protocol -I ../../shared \
    -c ../../shared/core/od_config_tlv.c -o "$OUT/od_config_tlv_ctrl.o"
cc -std=c11 -Wall -Wextra -Werror -O1 -fsanitize=undefined,address \
    -I ../../shared/core -I ../../shared/hal -I ../../shared/protocol -I ../../shared \
    -c ../../shared/core/od_config_store.c -o "$OUT/od_config_store_ctrl.o"
g++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=undefined,address -DOD_CFG_INDEX_CONTROLLER=1 \
    -I ../../shared/core -I ../../shared/hal -I ../../shared/protocol -I ../../shared -I src \
    tools/test_controller_esp.cpp src/od_controller_esp.cpp \
    "$OUT/od_cfg_index_ctrl.o" "$OUT/od_config_tlv_ctrl.o" "$OUT/od_config_store_ctrl.o" -o "$OUT/test_controller_esp"
"$OUT/test_controller_esp"

echo "==> all host tests passed"
