#!/usr/bin/env bash
# check.sh -- every gate this repo has, run locally.
#
# There is no CI. The checks that used to live in .github/workflows/ are here instead, which
# means NOTHING RUNS UNLESS SOMEONE RUNS IT. Run this before you push.
#
#   tools/check.sh                 boundary + host suites + sanitizers + fuzz + arduino-free
#   tools/check.sh --targets       also builds ESP32, Nordic, and Silabs targets. Required before
#                                  merge.
#   tools/check.sh --esp32         ESP32 only, as --targets used to be
#   tools/check.sh --fuzz-time 300 longer fuzz budget per target (default 60 s)
#   tools/check.sh --latest        also replay the corpus against the NEWEST py-opendisplay
#   tools/check.sh --list          print the checks and exit
#
# A SKIP IS NOT A PASS. Several checks need a toolchain that may be absent -- clang, ESP-IDF,
# a Python that can install py-opendisplay. Those SKIP rather than fail, but they are counted
# and reprinted in the summary, and the exit status is 2 when anything was skipped. Silent
# skipping is how a suite comes to mean nothing: "all green" has to distinguish "checked" from
# "could not check".

set -u -o pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

FUZZ_TIME=60
DO_ESP32=0
DO_NORDIC=0
DO_SILABS=0
DO_LATEST=0

while [ $# -gt 0 ]; do
    case "$1" in
        --esp32)      DO_ESP32=1 ;;
        --nordic)     DO_NORDIC=1 ;;
        --silabs)     DO_SILABS=1 ;;
        --targets)    DO_ESP32=1; DO_NORDIC=1; DO_SILABS=1 ;;
        --latest)     DO_LATEST=1 ;;
        --fuzz-time)  FUZZ_TIME="${2:?--fuzz-time needs a value}"; shift ;;
        --list)
            grep -oP '^\s*check "\K[^"]+' "$0" | sed 's/^/  /'
            exit 0 ;;
        -h|--help)    sed -n '2,20p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *)            echo "unknown argument: $1" >&2; exit 64 ;;
    esac
    shift
done

PASS=0; FAIL=0; SKIP=0
FAILED_NAMES=(); SKIPPED_NAMES=()
BUILD_ROOT="$REPO/build/check"
mkdir -p "$BUILD_ROOT"

say()  { printf '\n\033[1m== %s\033[0m\n' "$1"; }
ok()   { printf '\033[32mPASS\033[0m  %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '\033[31mFAIL\033[0m  %s\n' "$1"; FAIL=$((FAIL+1)); FAILED_NAMES+=("$1"); }
skip() { printf '\033[33mSKIP\033[0m  %s -- %s\n' "$1" "$2"; SKIP=$((SKIP+1)); SKIPPED_NAMES+=("$1 -- $2"); }

# check <name> <command...>  -- runs the command, capturing output, reporting only on failure.
check() {
    local name="$1"; shift
    say "$name"
    local log="$BUILD_ROOT/$(echo "$name" | tr ' /' '__').log"
    if "$@" >"$log" 2>&1; then
        ok "$name"
    else
        bad "$name"
        echo "--- output ($log) ---"
        tail -40 "$log"
    fi
}

# ==================================================================================== boundary ==
# Ported verbatim from .github/workflows/shared-boundary.yml. A GREP ONLY: it sees #include
# lines and literal tokens, and cannot see an extern-declared vendor symbol, a macro leaked in
# through a target config header, or a libc call absent on a freestanding target. Compiling
# shared/ for the host under both compilers is the load-bearing check; this is the fast one.
boundary_includes() {
    # Header patterns that pin a file to one SDK. Extend as targets are imported. Note the
    # ESP-IDF families beyond esp_*: driver/, soc/, hal/, rom/ and freertos/ are all IDF-only.
    # miniz.h is the ROM tinfl header -- the portable inflate engine belongs in shared/core, the ROM
    # binding does not. Panel libraries are target-layer: shared/ reaches the panel only through
    # od_hal_panel.
    local pattern='#[[:space:]]*include[[:space:]]*[<"]([[:space:]]*)?(esp_|esp32|driver/|soc/|hal/|rom/|miniz|freertos/|nrf_|nrfx|nrf\.h|sl_|sli_|em_|zephyr/|Arduino\.h|WiFi\.h|ESPmDNS|bluefruit\.h|Adafruit|NimBLE|bb_epaper|FastEPD|TFT_eSPI|Seeed)'
    # shared/hal's own headers are od_hal_*.h and may legitimately be included as
    # "hal/od_hal_panel.h", which the IDF `hal/` family above would otherwise flag. Anchor the
    # exemption on the INCLUDE TARGET, not the line: a bare `grep -v od_hal_` would excuse
    # `#include <esp_wifi.h> /* od_hal_ */`.
    local exempt='#[[:space:]]*include[[:space:]]*[<"][[:space:]]*hal/od_hal_'
    # third_party/ is deliberately NOT scanned: bb_epaper selects its IO backend by #ifdef and
    # every backend includes vendor headers, so it can never satisfy this rule -- but it is one
    # vendored copy shared by all targets, not a per-target fork. Do not "fix" it by moving it
    # under shared/.
    local hits rc
    # Preserve grep status; an unreadable tree is unproven.
    hits=$(grep -rInE "$pattern" shared/)
    rc=$?
    if [ "$rc" -gt 1 ]; then
        echo "grep could not scan shared/ (status $rc); the vendor-include absence is unproven"
        return 1
    fi
    hits=$(printf '%s\n' "$hits" | grep -vE "$exempt" || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo
        echo "vendor/framework include found under shared/."
        echo "shared/ must use only the C standard library and shared/hal interfaces."
        echo "Move the offending file to targets/<target>/, or route the dependency through a"
        echo "shared/hal interface. See docs/ARCHITECTURE.md."
        return 1
    fi
}

boundary_c_only() {
    # shared/ must be plain C: two targets are C-only, and the Zephyr sources that are the best
    # donor for shared/core are already C.
    if find shared/ \( -name '*.cpp' -o -name '*.cc' -o -name '*.hpp' \) | grep -q . ; then
        echo "C++ source found under shared/. shared/ is plain C."
        find shared/ \( -name '*.cpp' -o -name '*.cc' -o -name '*.hpp' \)
        return 1
    fi
    # Scoped to C sources on purpose: unscoped, this fires on prose in any shared/**/README.md
    # ("a length+ptr pair rather than a String"), and a check that cries wolf on documentation
    # is a check someone eventually deletes.
    if grep -rInE --include='*.c' --include='*.h' '\bString\b' shared/ ; then
        echo "Arduino String found under shared/. Use char[] or a length+ptr pair."
        return 1
    fi
}

boundary_protocol() {
    # shared/protocol/ is a byte-for-byte copy of the canonical opendisplay-protocol source: pure
    # constants and config structs, no behaviour. Change the canonical source and re-run
    # tools/sync_protocol_header.py --push instead of editing these.
    if grep -rInE '^[[:space:]]*(void|int|bool|uint[0-9]+_t|static)[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(' shared/protocol/ ; then
        echo
        echo "function declaration found in shared/protocol/ -- these files are synced, not written."
        return 1
    fi
}

check "shared boundary: no vendor includes"   boundary_includes
check "shared boundary: C only, no String"    boundary_c_only
check "shared boundary: protocol headers"     boundary_protocol

# od_session.c compares attacker-supplied bytes -- an auth proof and a session id -- and must do
# it in od_ct_equal, whose fixed-iteration volatile loop is the whole defence. A HOST TEST CANNOT
# CATCH THIS: replacing od_ct_equal with memcmp passes the entire suite, verified by mutation.
# It is exactly the regression AUDIT_NORDIC_ZEPHYR_2026-08-14.md:259 records shipping unnoticed on
# another target, so the enforcement has to be structural.
session_constant_time() {
    local hits
    hits=$(grep -nE '\bmemcmp[[:space:]]*\(' shared/core/od_session.c || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo
        echo "memcmp in od_session.c: comparisons of attacker-supplied bytes belong in"
        echo "od_ct_equal(), which is fixed-iteration with a volatile accumulator. If this is a"
        echo "comparison of non-secret data, move it behind a helper with a name that says so."
        return 1
    fi
}
check "shared boundary: od_session uses no memcmp"  session_constant_time

# od_hal_time is the two-function ambient/busy-wait seam. Millisecond sleep stays private until
# ESP32's unsigned round-up contract is reconciled with Nordic's signed k_msleep contract, and the
# BG22 wrapper must convert the SDK's extended tick count before narrowing to uint32_t milliseconds.
time_hal_structure() {
    local rc=0 hits

    hits=$(find targets -name od_hal_time.h \
           ! -path '*/build/*' ! -path '*/build-*/*' -print 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "target-local od_hal_time.h shadows shared/hal/od_hal_time.h"
        rc=1
    fi

    hits=$(grep -rInE '\b(od_uptime_get_32|od_busy_wait)[[:space:]]*\(' \
           --exclude-dir='build*' targets/nordic-zephyr 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "retired Nordic time wrapper returned"
        rc=1
    fi

    if grep -qE '^[[:space:]]*void[[:space:]]+od_hal_delay_ms[[:space:]]*\(' \
            shared/hal/od_hal_time.h; then
        echo "bounded millisecond sleep entered the shared time HAL without its contract decision"
        rc=1
    fi
    if ! grep -qE '^uint32_t od_hal_uptime_ms\(void\);' shared/hal/od_hal_time.h ||
       ! grep -qE '^void od_hal_delay_us\(uint32_t us\);' shared/hal/od_hal_time.h; then
        echo "shared time HAL declaration drifted"
        rc=1
    fi
    if ! grep -qE '\bod_hal_delay_ms[[:space:]]*\(' targets/esp32-idf/hal/od_hal_sleep.h; then
        echo "ESP32 private millisecond-sleep declaration is missing"
        rc=1
    fi

    if ! grep -q 'sl_sleeptimer_get_tick_count64()' targets/efr32bg22-slc/od_hal_time.c ||
       ! grep -q 'sl_sleeptimer_tick64_to_ms' targets/efr32bg22-slc/od_hal_time.c; then
        echo "BG22 uptime must convert the 64-bit tick count before narrowing"
        rc=1
    fi
    if grep -qE 'sl_sleeptimer_tick_to_ms|sl_sleeptimer_get_tick_count\(' \
            targets/efr32bg22-slc/od_hal_time.c; then
        echo "BG22 time HAL reintroduced the 32-bit tick-domain clock"
        rc=1
    fi
    # Uptime converted from the 32-bit tick count wraps at the tick rollover (~36 h at
    # 32.768 kHz), not at 2^32 ms, so now-then arithmetic across it breaks. A tick DELTA
    # converted afterwards (tick_to_ms(get_tick_count() - t0)) is wrap-safe and allowed.
    hits=$(grep -rnE 'sl_sleeptimer_tick_to_ms\(sl_sleeptimer_get_tick_count\(\)\)' \
           --include='*.c' --include='*.cpp' --exclude-dir='cmake_gcc' \
           targets/efr32bg22-slc 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "BG22 uptime read from the 32-bit tick domain; use od_hal_uptime_ms()"
        rc=1
    fi

    return "$rc"
}
check "structure: shared time HAL" time_hal_structure

# ARCHITECTURE, RATCHETED BY SYMBOL. Each name below is one whose ABSENCE is the property: a
# second opcode map, an implicit frame context, an exported session singleton. Grepping for them
# is crude but it is what stops a well-meaning "just for now" reintroduction, and unlike a line
# count it says what is actually wrong when it fires.
#
# Scoped to targets/*/src and targets/*/hal so a build directory's .map files cannot trip it.
c11_structure() {
    local rc=0 hits
    scan() {
        local what="$1" pattern="$2" why="$3"
        hits=$(grep -rInE "$pattern" targets/*/src targets/*/hal shared/ \
               targets/efr32bg22-slc/*.[ch] targets/efr32bg22-slc/*.cpp 2>/dev/null || true)
        if [ -n "$hits" ]; then
            echo "$hits"; echo; echo "$what: $why"; echo
            rc=1
        fi
    }
    # The opcode map is od_dispatch.c's. A target definition of this is a second map, and two maps
    # is how one target answers an opcode the other treats as unknown.
    scan "od_cmd_dispatch" '\bod_cmd_dispatch[[:space:]]*\(' \
         "the per-command seam is od_cmd_app.h; the opcode map is shared."
    # A frame context stored in a global outlives its frame: any nested or later path can read it
    # after the caller has moved on, and no compiler can catch that. Both ESP32 ingresses build an
    # od_reply_t and pass it.
    scan "implicit frame context" '\b(g_commandOrigin|g_commandInstance|commandOrigin|imageDataWritten)\b' \
         "build an od_reply_t at the ingress and pass it to od_dispatch_app_frame()."
    # The session object is private to its od_session_app translation unit. A name reachable from
    # elsewhere is one a caller can memset -- and memset is not teardown here: the key lives in an
    # od_hal_crypto slot, so zeroing the struct strands a prepared key in a finite pool and loses
    # the slot index with it. od_session_clear() is the only teardown.
    scan "exported session singleton" '\b(g_session|od_pipe_session|od_pipe_device_id)\b' \
         "reach the session through od_session_app_state()."
    # Confidentiality is chosen at the CALL SITE. Inferring it from response bytes is how this
    # firmware once sealed its own rejection frames, which the host then validated as ACKs.
    scan "byte-inferred sealing" 'response\[(0|2)\][[:space:]]*==[[:space:]]*(RESP_NACK|0xFF)' \
         "call od_reply_plain() explicitly; never inspect the frame to decide."
    return $rc
}
check "structure: ownership ratchets"  c11_structure

xfer_direct_dispatch() {
    local rc=0 hits
    local rows=shared/core/od_dispatch_ops.h

    for row in \
        'X(CMD_DIRECT_WRITE_START,  od_xfer_direct_start,       1u)' \
        'X(CMD_DIRECT_WRITE_DATA,   od_xfer_data,               2u)' \
        'X(CMD_DIRECT_WRITE_END,    od_xfer_end,                2u)' \
        'X(CMD_PARTIAL_WRITE_START, od_xfer_partial_start,      1u)'; do
        if ! grep -Fq "$row" "$rows"; then
            echo "shared transfer dispatch row or reservation budget drifted: $row"
            rc=1
        fi
    done

    hits=$(grep -RInE '\bod_cmd_app_(direct_start|direct_data|direct_end|partial_start)\b' \
        shared/core targets --include='*.c' --include='*.cpp' --include='*.h' \
        --exclude-dir='build*' 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "legacy direct/partial target hook returned; dispatch must route straight to od_xfer"
        rc=1
    fi
    if [ -e targets/nordic-zephyr/src/od_cmd_direct.c ]; then
        echo "Nordic's retired direct/partial bridge translation unit returned"
        rc=1
    fi
    return $rc
}
check "structure: direct transfer dispatch ownership" xfer_direct_dispatch

pipe_dispatch_ownership() {
    local rc=0 hits row count
    local rows=shared/core/od_dispatch_ops.h

    for row in \
        'X(CMD_PIPE_WRITE_START,    od_pipe_start,              1u)' \
        'X(CMD_PIPE_WRITE_DATA,     od_pipe_data,               (OD_CAP_PIPE ? 3u : 1u))' \
        'X(CMD_PIPE_WRITE_END,      od_pipe_end,                (OD_CAP_PIPE ? 3u : 1u))'; do
        if ! grep -Fq "$row" "$rows"; then
            echo "shared PIPE dispatch row or reservation budget drifted: $row"
            rc=1
        fi
    done

    hits=$(grep -RInE '\bod_cmd_app_pipe_(start|data|end)\b' shared/core targets \
        --include='*.c' --include='*.cpp' --include='*.h' --exclude-dir='build*' \
        2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "target PIPE hook returned; dispatch must route straight to shared od_pipe"
        rc=1
    fi
    hits=$(grep -nE '\b(od_xfer_app_|od_zlib_pump_)' shared/core/od_pipe.c || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "od_pipe must use reply-free od_xfer internal operations"
        rc=1
    fi
    count=$(grep -Ec '^[[:space:]]*"\$\{CMAKE_CURRENT_LIST_DIR\}/core/od_pipe\.c"' \
        shared/sources.cmake)
    if [ "$count" -ne 1 ]; then
        echo "od_pipe.c must be registered exactly once in shared/sources.cmake (found $count)"
        rc=1
    fi
    return $rc
}
check "structure: PIPE dispatch ownership" pipe_dispatch_ownership

# Absence checks share this: grep's STATUS is the proof, not the emptiness of its output. An
# unreadable tree or a mistyped path must not read as clean -- a check that passes because it read
# nothing is worse than no check.
#   0 = matched (the thing came back)   1 = clean   >1 = could not be evaluated
absent_or_fail() {
    local what=$1 pattern=$2 hits rc
    shift 2
    # 'build*', not 'build': Nordic's trees are build-xiao_ble, build-nrf54l15, ... so a bare
    # 'build' left every generated Zephyr header in scope, where a pattern can match code no one
    # here wrote.
    hits=$(grep -RInE "$pattern" "$@" --include='*.c' --include='*.cpp' --include='*.h' \
           --exclude-dir='build*')
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "$hits"
        echo "$what"
        return 1
    fi
    if [ "$rc" -gt 1 ]; then
        echo "grep could not scan (status $rc); the absence of '$what' is unproven"
        return 1
    fi
}

# PERMANENT. One rule over every target, mirroring the PIPE and NFC absence ratchets: a per-target
# check cannot see a target that arrives without one.
xfer_target_parser_absent() {
    absent_or_fail "target-local direct/partial command policy returned; shared od_xfer owns it" \
        '\b(handleDirectWrite(Start|Data|End)|handlePartialWriteStart|opendisplay_display_direct_write_(start|data|end|end_prepare|end_refresh))[[:space:]]*\(' \
        targets
}
check "transfer: no target transfer parser" xfer_target_parser_absent

# The parser, pitch policy, repeat machine and cap are shared/core/od_buzzer.c's. Target adapters
# may own PWM/tone timers and config lookup, but not a second interpretation of the melody bytes.
buzzer_target_machine_absent() {
    absent_or_fail "target-local buzzer melody policy returned; shared od_buzzer owns it" \
        '\b(buzzer_(index_to_(hz|centihz)|fold_index|run(_step)?)|kBuzzer(CentiHzTable|MaxTotalMs|InterPatternGapMs|DurationUnitMs)|BUZZER_(MAX_TOTAL_MS|INTER_PATTERN_GAP_MS|DURATION_UNIT_MS)|BZ_(PHASE_(STEP|GAP)|PATTERN_ENTER|PATTERN_END|STEP))\b' \
        targets
}
check "buzzer: one melody policy" buzzer_target_machine_absent

# PERMANENT, and a product decision rather than a current-state observation: the EFR32BG22 will
# never carry a buzzer. It compiles OD_CONFIG_WITH_BUZZER=0, takes no APP_BUZZER tier, and answers
# 0x0077 with a capability refusal from its own command hook.
#
# The rule is worth having because the failure is silent: adding ${OD_SHARED_SOURCES_APP_BUZZER}
# to that target builds and links cleanly, and costs a 256-entry pitch table plus runner state on
# a part with 480 B of RAM headroom. Nothing else would notice.
bg22_has_no_buzzer_runner() {
    local img
    local nm=~/.silabs/slt/installs/conan/p/gcc-a442105b5c2637/p/bin/arm-none-eabi-nm

    if grep -q 'OD_SHARED_SOURCES_APP_BUZZER' \
        targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake; then
        echo "BG22 took the shared buzzer tier; this target has no buzzer by product decision"
        return 1
    fi
    if ! grep -qE '^[[:space:]]*OD_CONFIG_WITH_BUZZER=0$' shared/profiles.cmake; then
        echo "the BG22 profile must state OD_CONFIG_WITH_BUZZER=0 explicitly"
        return 1
    fi

    # When an image exists, the map is the proof a source grep cannot give: a shared symbol can
    # arrive through a tier the .cmake does not name.
    img=targets/efr32bg22-slc/cmake_gcc/build/base/opendisplay-bg22.out
    if [ -x "$nm" ] && [ -f "$img" ]; then
        if "$nm" "$img" | grep -q 'od_buzzer'; then
            "$nm" "$img" | grep 'od_buzzer'
            echo "BG22 image links a shared buzzer symbol"
            return 1
        fi
    fi
}
check "silabs: BG22 has no buzzer runner" bg22_has_no_buzzer_runner
# PERMANENT. One entry point for "advertise sooner, something happened".
#
# THE FIRST VERSION OF THIS RULE LISTED TWO RETIRED NAMES AND PASSED WHILE A THIRD SURVIVED --
# BG22's static od_boost_advertising(), which no one had noticed because the ESP32 comment it was
# checked against said the fast-advertising window was "nRF-only". A ratchet that enumerates the
# names you already know about proves nothing about the one you do not.
#
# So this matches the SHAPE instead: any identifier combining boost with advertising, in call or
# definition syntax, that is not the seam. Nordic's and BG22's legitimate window internals
# (s_adv_boost_until_ms, adv_boost_active, OD_ADV_BOOST_MS, od_advertising_boost_tick) are state
# and policy below the seam, not entry points. BG22's loop reconciler was renamed to
# od_advertising_interval_tick() rather than exempted, because it reconciles an interval and the
# old name made a policy function look like a second way to ask for a boost.
#
# NO LEADING \b, DELIBERATELY. The name that escaped was od_boost_advertising, where 'boost' is
# preceded by a word character -- so \bboost cannot match it. This repo has made that exact
# mistake before (od_nfc_i2c_start vs \bi2c_start). A prefix anchor on a rule about identifiers
# that carry prefixes is a rule that reads as coverage and is not.
adv_boost_one_entry_point() {
    absent_or_fail "a private advertising-boost entry point returned; od_adv_app_boost() is the seam" \
        '(boost_?[Aa]dvertis|[Aa]dvertis[a-z]*_?[Bb]oost)[a-zA-Z_]*[[:space:]]*\(' \
        targets shared
}
check "advertising: one boost entry point" adv_boost_one_entry_point

# PERMANENT, AND IT CATCHES A TRANSCRIPTION, NOT A REIMPLEMENTATION. Derived from targets/ rather
# than a fixed file list so a fourth target is in scope from the day it arrives -- which matters,
# because this driver reached two ports by being copied, and the copies then disagreed about the
# over-count wedge and the read framings.
#
# What it actually detects is the constants and helper names a copy carries over. A determined
# reimplementation using lowercase 0x814e, computed register addresses, renamed helpers or a bare
# `status & 0x80` passes. Do not read this as proof that only one parser exists; it is a tripwire
# on the cheapest way to grow a second one. od_touch_app.c is the seam and holds none of it.
touch_one_gt911_driver() {
    absent_or_fail "target-side GT911 register policy returned; shared od_touch_gt911 owns it" \
        '(0[xX]814[EeFf]|GT911_REG_|GT911_MAX_CONTACTS|gt911_hw_reset|apply_touch_map)' \
        targets $(find shared -name '*.c' -o -name '*.h' | grep -v 'od_touch_gt911')
}
check "touch: one GT911 driver" touch_one_gt911_driver

# PERMANENT. A promoted opcode answers from shared code; a target assembling its own reply bytes
# for one is invisible until a wire capture disagrees with the corpus.
#
# Two patterns, because the opcodes are not named alike: direct and NFC have RESP_* constants,
# PIPE (0x80-0x82) and partial (0x76) have none. Raw replies may use RESP_* or bare status bytes.
# Cover every promoted opcode while matching initializer shape, so unrelated opcode constants do
# not trigger the rule.
promoted_response_literal_absent() {
    local rc=0
    absent_or_fail "target-side response constant for a promoted opcode returned" \
        '\bRESP_(NFC_ENDPOINT|DIRECT_WRITE_(START|DATA|END)_ACK|DIRECT_WRITE_REFRESH_(SUCCESS|TIMEOUT))\b' \
        targets || rc=1
    absent_or_fail "target-side raw response frame for a promoted opcode returned" \
        '\{[[:space:]]*(RESP_ACK|RESP_NACK|0x00u?|0xFFu?)[[:space:]]*,[[:space:]]*0x(7[0126]|8[012])u?' \
        targets || rc=1
    return $rc
}
check "transfer: no target response literal for a promoted opcode" promoted_response_literal_absent

# PERMANENT. `securityConfig` is a C++ reference to globalConfig.security (targets/esp32-idf/src/
# main.h). Re-declaring it as an object compiles and LINKS -- a variable's mangled name carries no
# type -- and the consumer then reads the reference's own 4-byte pointer word as if it were the
# 64-byte struct. The boot screen did exactly that: encryption_enabled came back as a byte of an
# address, so a device with encryption off printed "KEY1: hidden" and shipped garbage in the QR.
# encryption_state.h holds the one correct declaration; every consumer includes it.
security_config_is_a_reference() {
    absent_or_fail "securityConfig re-declared as an object; include encryption_state.h instead" \
        'extern[[:space:]]+struct[[:space:]]+SecurityConfig[[:space:]]+securityConfig\b' \
        targets
}
check "esp32: securityConfig declared only as a reference" security_config_is_a_reference

# PERMANENT. od_core_reset() is the shared half of a teardown; a target hand-rolling the list is
# what od_core.h exists to prevent.
#
# WHAT IT PROVES IS THAT A CALL IS PRESENT, not that it executes: this is a grep, so a commented-out
# call still satisfies it. It catches deletion and a target arriving without one. Anything stronger
# needs the teardown driven, and core_reset_test.c is where that lives.
core_reset_is_the_teardown() {
    local rc=0 d n src_file found=0

    # Discover targets dynamically; each target must contain an od_core_reset() caller. Which file
    # holds it is the target's business.
    for d in targets/*/; do
        [ -d "$d" ] || continue
        # Branch switches can leave only ignored build/cache files behind.
        src_file=$(find "$d" -type d -name 'build*' -prune -o \
            -type f \( -name '*.c' -o -name '*.cpp' \) -print -quit) || {
            echo "could not scan ${d} for sources; the teardown rule is unproven"
            return 1
        }
        [ -n "$src_file" ] || continue
        found=1
        n=$(grep -RIlE '\bod_core_reset[[:space:]]*\(' "$d" \
            --include='*.c' --include='*.cpp' --exclude-dir='build*' | wc -l)
        if [ "$n" -eq 0 ]; then
            echo "${d} has no od_core_reset() caller; its teardown does not reach the shared half"
            rc=1
        fi
    done
    if [ "$found" -eq 0 ]; then
        echo "no targets with sources found; the teardown rule is unproven"
        rc=1
    fi
    return $rc
}
check "reset: every target teardown uses od_core_reset" core_reset_is_the_teardown

transfer_single_pump_owner() {
    local hits rc
    hits=$(grep -RInE '\bod_zlib_pump_(reset|push)[[:space:]]*\(' shared/core targets \
        --include='*.c' --include='*.cpp' --include='*.h' --exclude='od_zlib_pump.c' \
        --exclude='od_zlib_pump.h' --exclude-dir='build*')
    rc=$?
    if [ "$rc" -gt 1 ]; then
        echo "grep could not scan for pump callers (status $rc); the ownership is unproven"
        return 1
    fi
    hits=$(printf '%s\n' "$hits" | grep -v '^shared/core/od_xfer\.c:' || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "production zlib pump caller returned outside shared/core/od_xfer.c"
        return 1
    fi
}
check "transfer: single pump owner" transfer_single_pump_owner

pipe_target_machine_absent() {
    absent_or_fail "target-local PIPE state machine returned" \
        '\b(PipeWriteState|PipeReorderSlot|pipe_build_ack_payload|pipeBuildAckPayload|send_pipe_(ack|nack)|sendPipe(Ack|Nack)|handlePipeWrite(Start|Data|End)|pipe_refused_on_lan|sessionOrigin)\b' \
        targets
}
check "transfer: no target PIPE machine" pipe_target_machine_absent

transfer_logging_target_strings_absent() {
    absent_or_fail "retired target-owned transfer diagnostic returned" \
        'Shared transfer timeout - aborting session|dw init begin|Refresh timed out' \
        targets/esp32-idf/src/display_service.cpp \
        targets/nordic-zephyr/src/opendisplay_display.cpp
}
check "transfer: retired target logging strings absent" transfer_logging_target_strings_absent

# The 0x0083 machine is shared/core/od_nfc.c's. A target that grows its own assembler, its own
# record-type table or its own hook is the divergence Phase 4 closed, so the symbols that carried
# it on each port are named here rather than the behaviour, which no grep can see.
#
# opendisplay_nfc.c's NDEF encoder and the TNB132M I2C work are deliberately OUT OF SCOPE: they are
# controller adaptation reached through od_nfc_app, and they stay.
nfc_target_machine_absent() {
    absent_or_fail "target-local NFC assembler or hook returned" \
        '\b(od_nfc_write_chunk_t|s_nfc_write_chunk|s_nfc_data|s_nfc_rsp_buf|nfc_rec_type_valid|nfc_type_valid|od_cmd_nfc_reset|od_cmd_app_nfc)\b' \
        targets
}
check "transfer: no target NFC assembler" nfc_target_machine_absent

core_reset_owns_transfer() {
    local rc=0 body xfer_line txq_line hits
    body=$(sed -n '/^void od_core_reset(void)/,/^}/p' shared/core/od_core.c)
    xfer_line=$(grep -n '\bod_xfer_reset[[:space:]]*(' <<<"$body" | head -n 1 | cut -d: -f1)
    txq_line=$(grep -n '\bod_txq_reset[[:space:]]*(' <<<"$body" | head -n 1 | cut -d: -f1)
    if [ -z "$xfer_line" ] || [ -z "$txq_line" ] || [ "$xfer_line" -ge "$txq_line" ]; then
        echo "od_core_reset must reset transfer state before the egress queue"
        rc=1
    fi
    absent_or_fail "target teardown bypassed od_core_reset transfer ownership" \
        '\bod_xfer_reset[[:space:]]*\(' targets || rc=1
    return $rc
}
check "reset: od_core owns transfer teardown" core_reset_owns_transfer

command_context_fixtures() {
    local rc=0 hits statics constructors
    hits=$(grep -RInE '\bod_cmd_ctx_t[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*(;|=)' \
        tests/host --include='*.c' --include='*.h' --exclude='od_cmd_test_ctx.h' \
        2>/dev/null \
        | grep -vE 'tests/host/nordic_cmd_device_test\.c:.*static od_cmd_ctx_t CTX;' \
        | grep -v 'od_test_cmd_ctx[[:space:]]*(' || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "host command contexts must be initialized by od_test_cmd_ctx()"
        rc=1
    fi

    statics=$(grep -RInE '\bstatic[[:space:]]+od_cmd_ctx_t\b' tests/host \
        --include='*.c' --include='*.h' --exclude='od_cmd_test_ctx.h' 2>/dev/null \
        | grep -vE '^tests/host/nordic_cmd_device_test\.c:.*static od_cmd_ctx_t CTX;' || true)
    if [ -n "$statics" ]; then
        echo "$statics"
        echo "untracked static command-context fixture returned"
        rc=1
    fi
    constructors=$(grep -RInE \
        '^[[:space:]]*(static[[:space:]]+)?od_cmd_ctx_t[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(' \
        tests/host --include='*.c' --include='*.h' --exclude='od_cmd_test_ctx.h' \
        2>/dev/null || true)
    if [ -n "$constructors" ]; then
        echo "$constructors"
        echo "file-local command-context constructor returned; use od_test_cmd_ctx()"
        rc=1
    fi
    if ! sed -n '/^static void reset_all(void)/,/^}/p' tests/host/nordic_cmd_device_test.c \
         | grep -q 'CTX = od_test_cmd_ctx[[:space:]]*('; then
        echo "Nordic command fixture must initialize CTX through od_test_cmd_ctx() in reset_all()"
        rc=1
    fi
    return $rc
}
check "command context: explicit host fixtures" command_context_fixtures

od_color_structure() {
    local rc=0 hits count
    for path in \
        targets/nordic-zephyr/src/opendisplay_display_color.c \
        targets/nordic-zephyr/src/opendisplay_display_color.h \
        targets/efr32bg22-slc/opendisplay_display_color.c \
        targets/efr32bg22-slc/opendisplay_display_color.h; do
        if [ -e "$path" ]; then
            echo "retired target color helper returned: $path"
            rc=1
        fi
    done

    hits=$(grep -rInE '\bopendisplay_color_[A-Za-z0-9_]*\b|\bcalc_controller_plane_bytes\b|\b(getBitsPerPixel|getplane)[[:space:]]*\(' \
           targets/*/src targets/*/panel targets/efr32bg22-slc/*.[ch] \
           targets/efr32bg22-slc/*.cpp 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "legacy target-local direct-stream color geometry returned; use shared/core/od_color.h"
        rc=1
    fi

    # NAME THE FILE, NEVER THE DIRECTORY. cmake_gcc/ contains build/, whose generated ninja
    # files, compile_commands.json and linker map all mention this source -- so a recursive grep
    # counts 6 and this check fails IFF BG22 has been built, which --targets does in the same run.
    count=$(grep -RFl 'core/od_color.c' shared/sources.cmake targets/*/CMakeLists.txt \
            targets/*/*/CMakeLists.txt targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake \
            2>/dev/null | wc -l)
    if [ "$count" -ne 1 ] || ! grep -q 'core/od_color.c' shared/sources.cmake; then
        echo "od_color.c must be registered exactly once, in shared/sources.cmake (found $count)"
        rc=1
    fi
    return $rc
}
check "structure: od_color is the direct geometry authority"  od_color_structure

inflate_pump_structure() {
    local rc=0 hits count
    local target_sources=(
        targets/esp32-idf/src
        targets/nordic-zephyr/src
        targets/efr32bg22-slc/*.c
        targets/efr32bg22-slc/*.cpp
    )

    # Targets reach an inflater only through their selected od_inflate_app adapter. Direct engine
    # calls would restore a target-local push/poll loop and bypass the shared final/accounting
    # policy.
    hits=$(grep -rInE '\bod_zlib_stream_(reset|push|poll|error|output_count)[[:space:]]*\(' \
           "${target_sources[@]}" 2>/dev/null \
           | grep -vE '/od_inflate_app\.(c|cpp):' || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "portable inflater bypassed od_inflate_app; target transfer code must call od_zlib_pump"
        rc=1
    fi

    hits=$(grep -rInE '\bod_inflate_tinfl_(reset|push|poll|error|output_count)[[:space:]]*\(' \
           "${target_sources[@]}" 2>/dev/null \
           | grep -vE '/od_inflate_(app|tinfl)\.(cpp|h):' || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "tinfl bypassed od_inflate_app; target transfer code must call od_zlib_pump"
        rc=1
    fi

    hits=$(grep -rInE '\bod_inflate_app_(reset|push|poll|error|output_count)[[:space:]]*\(' \
           "${target_sources[@]}" 2>/dev/null \
           | grep -vE '/od_inflate_app\.(c|cpp):' || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "target transfer code bypassed od_zlib_pump through the backend seam"
        rc=1
    fi

    count=$(grep -RFl 'core/od_zlib_pump.c' shared/sources.cmake targets/*/CMakeLists.txt \
            targets/*/*/CMakeLists.txt targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake \
            2>/dev/null | wc -l)
    if [ "$count" -ne 1 ] || ! grep -q 'core/od_zlib_pump.c' shared/sources.cmake; then
        echo "od_zlib_pump.c must be registered exactly once, in shared/sources.cmake (found $count)"
        rc=1
    fi
    return $rc
}
check "structure: od_zlib_pump owns inflater progression"  inflate_pump_structure

# The declared-window bound is a wire contract with the host encoder, not a buffer size. An engine
# that applies its own limit accepts streams the rest of the fleet refuses, and no host can
# interrogate the difference -- so the comparison lives in shared/core/od_zlib_header.h and every
# decoder reaches it. Engines are found by what they do (drive tinfl, or parse the zlib header
# themselves), so a new one cannot arrive without an answer here.
zlib_header_single_rule() {
    local hits rc engines tu

    if ! grep -q 'OPENDISPLAY_ZLIB_WINDOW_BITS' shared/core/od_zlib_header.h; then
        echo "shared/core/od_zlib_header.h no longer holds the window bound"
        return 1
    fi
    hits=$(grep -RIn --include='*.c' --include='*.cpp' --include='*.h' \
           --exclude='od_zlib_header.h' --exclude-dir='build*' \
           -e '>[[:space:]]*OPENDISPLAY_ZLIB_WINDOW_BITS' shared targets)
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "$hits"
        echo "the zlib window bound is compared outside od_zlib_header.h"
        return 1
    fi
    if [ "$rc" -gt 1 ]; then
        echo "grep could not scan for the window bound (status $rc); single-rule is unproven"
        return 1
    fi

    engines=$(grep -RIl --include='*.c' --include='*.cpp' --exclude-dir='build*' \
              -e 'tinfl_decompress[[:space:]]*(' -e 'ST_ZLIB_FLG' shared targets)
    rc=$?
    if [ "$rc" -ne 0 ] || [ -z "$engines" ]; then
        echo "found no inflate engine to check (grep status $rc); single-rule is unproven"
        return 1
    fi
    for tu in $engines; do
        # A CALL, not a mention: the rule is named in prose in more than one comment.
        if ! grep -qE 'od_zlib_header_(check|observe)[[:space:]]*\(' "$tu"; then
            echo "$tu decodes a zlib stream without calling od_zlib_header_check/observe"
            return 1
        fi
    done
}
check "transfer: one zlib header rule" zlib_header_single_rule


silabs_c13_config() {
    grep -q '^#define PSA_WANT_ALG_CCM 1$' \
        targets/efr32bg22-slc/config/od_psa_config_autogen.h || {
        echo "Silabs tracked PSA CCM selection missing"; return 1; }
    grep -A2 -q 'id: psa_crypto_ccm' targets/efr32bg22-slc/opendisplay-bg22.slcp || {
        echo "Silabs SLC project no longer selects PSA CCM"; return 1; }
    grep -q '^#define SL_PSA_KEY_USER_SLOT_COUNT[[:space:]]\+1$' \
        targets/efr32bg22-slc/config/psa_crypto_config.h || {
        echo "Silabs application PSA key-slot reserve is not one"; return 1; }
    grep -A1 -q "name: SL_PSA_KEY_USER_SLOT_COUNT[[:space:]]*$" \
        targets/efr32bg22-slc/opendisplay-bg22.slcp || {
        echo "Silabs SLC project lost its PSA key-slot setting"; return 1; }
    grep -A1 "name: SL_PSA_KEY_USER_SLOT_COUNT[[:space:]]*$" \
        targets/efr32bg22-slc/opendisplay-bg22.slcp | grep -q "value: '1'" || {
        echo "Silabs SLC project would regenerate the PSA key-slot reserve incorrectly"; return 1; }
    grep -q '^#define SL_BGAPI_MAX_PAYLOAD_SIZE[[:space:]]\+(263)$' \
        targets/efr32bg22-slc/config/sl_bgapi_config.h || {
        echo "Silabs BGAPI payload no longer supports ATT MTU 256"; return 1; }
    grep -A2 -q 'id: bluetooth_feature_resource_report' \
        targets/efr32bg22-slc/opendisplay-bg22.slcp || {
        echo "Silabs TX completion reporting component missing from tracked SLC input"; return 1; }
    if grep -q '^#define SL_CATALOG_KERNEL_PRESENT' \
        targets/efr32bg22-slc/autogen/sl_component_catalog.h; then
        echo "Silabs kernel component invalidates the BGAPI event-retention design"; return 1
    fi
}
check "silabs: C13 config ratchets" silabs_c13_config

# PERMANENT. shared/core/od_config_store.c is the only implementation of the stored config
# record -- its magic, its 16-byte header, its version field and its CRC-32. A target owns the
# MEDIUM (shared/hal/od_hal_nvs.h) and nothing above it. All three carried their own copy of this
# framing before the promotion, and they agreed only by maintenance.
config_store_target_framing_absent() {
    absent_or_fail "target-local config record framing returned; shared od_config_store owns it" \
        '\b(CONFIG_STORAGE_MAGIC|CONFIG_STORAGE_VERSION|calculateConfigCRC|config_header_t|od_config_header_t|opendisplay_config_storage_t|OD_CONFIG_STORE_MAGIC|OD_CONFIG_STORE_VERSION|od_config_store_crc32)\b|0[xX][dD][eE][aA][dD][bB][eE][eE][fF]' \
        targets
}
check "config storage: one record framing" config_store_target_framing_absent

# The CRC-32 itself, by polynomial rather than by name: a second copy under a different function
# name is the same defect. Both spellings, because a table-driven version reverses the constant.
# Checked in two directions -- absent from targets/, and in exactly ONE file under shared/ -- so
# that neither "a target grew one back" nor "shared grew a second" can pass.
# opendisplay_protocol.h names the polynomial in prose (the CMD_FLASH_LIST slot digest) and is a
# byte-for-byte canonical copy, so it is listed rather than edited.
CONFIG_STORE_CRC_FILES=$'shared/core/od_config_store.c\nshared/core/od_config_store.h\nshared/protocol/opendisplay_protocol.h'
config_store_single_crc32() {
    local files rc
    absent_or_fail "a second CRC-32 returned to a target; od_config_store_crc32 is the one" \
        '0[xX](EDB88320|edb88320|04C11DB7|04c11db7)' \
        targets || return 1

    files=$(grep -RIlE '0[xX](EDB88320|edb88320|04C11DB7|04c11db7)' shared \
            --include='*.c' --include='*.h' --exclude-dir='build*')
    rc=$?
    if [ "$rc" -gt 1 ]; then
        echo "grep could not scan shared/ (status $rc); the CRC-32 location is unproven"
        return 1
    fi
    files=$(printf '%s\n' "$files" | LC_ALL=C sort | grep -v '^$')
    # An exact set, not a count: "every match is in the right file" also passes when there are NO
    # matches, which would let the canonical implementation be deleted or renamed silently. The
    # header is listed because it names the polynomial in prose.
    if [ "$files" != "$CONFIG_STORE_CRC_FILES" ]; then
        echo "expected the CRC-32 polynomial in exactly:"
        printf '  %s\n' $CONFIG_STORE_CRC_FILES
        echo "found:"
        printf '  %s\n' ${files:-"(nothing)"}
        echo "shared/core/od_config_store is the only home for the record's CRC-32"
        return 1
    fi
    return 0
}
check "config storage: one CRC-32" config_store_single_crc32

# PERMANENT. ONE I2C ENGINE PER TARGET, and the statement is only true because BG22's NFC
# transport is inside it: excluding that would have made the rule read as full coverage while a
# fourth bit-banger sat in opendisplay_ble.c (PLAN_SENSOR_SEAM_2026-08-23.md T6).
#
# The sanctioned implementations are one per silicon vendor. Anything else in targets/ that
# clocks bits or drives the IDF master directly is a second engine.
I2C_ENGINE_FILES='targets/esp32-idf/hal/od_hal_i2c.c targets/nordic-zephyr/src/opendisplay_i2c.c targets/efr32bg22-slc/hal/od_hal_i2c.c'
i2c_single_engine_per_target() {
    local hits rc missing=0 f

    # No leading \b on the primitive names: every engine this rule has had to catch was
    # PREFIXED -- od_nfc_i2c_start, t_i2c_write_byte -- and a word boundary before "i2c_"
    # does not match after an underscore, so the first version of this rule missed the very
    # bit-banger step 9 removed.
    #
    # The named implementations must exist: a rule that only forbids duplicates passes happily
    # when the original is deleted or renamed.
    for f in $I2C_ENGINE_FILES; do
        if [ ! -f "$f" ]; then
            echo "missing sanctioned I2C engine: $f"
            missing=1
        fi
    done
    [ "$missing" -eq 0 ] || return 1

    hits=$(grep -RInE \
        'i2c_(start|stop|write_bit|read_bit|write_byte|read_byte)\b|\bI2C_HALF_BIT_US\b|\bi2c_master_(transmit|receive|probe|bus_add_device|transmit_receive)\b' \
        targets --include='*.c' --include='*.cpp' --include='*.h' --exclude-dir='build*')
    rc=$?
    if [ "$rc" -gt 1 ]; then
        echo "grep could not scan targets/ (status $rc); the absence of a second I2C engine is unproven"
        return 1
    fi
    for f in $I2C_ENGINE_FILES; do
        hits=$(printf '%s\n' "$hits" | grep -v "^${f}:")
    done
    hits=$(printf '%s\n' "$hits" | grep -v '^$')
    if [ -n "$hits" ]; then
        printf '%s\n' "$hits"
        echo "a second I2C engine returned; one per target, and BG22's NFC transport is inside the rule"
        return 1
    fi
    return 0
}
check "i2c: one engine per target" i2c_single_engine_per_target

silabs_advertising_ownership() {
    local count close_calls

    if grep -q '\bopendisplay_ble_restart_advertising[[:space:]]*(' \
        targets/efr32bg22-slc/app.c targets/efr32bg22-slc/opendisplay_ble.h; then
        echo "Silabs advertising restart escaped the BLE event-layer owner"
        grep -n '\bopendisplay_ble_restart_advertising[[:space:]]*(' \
            targets/efr32bg22-slc/app.c targets/efr32bg22-slc/opendisplay_ble.h
        return 1
    fi
    count=$(grep -c '\bopendisplay_ble_restart_advertising[[:space:]]*(' \
        targets/efr32bg22-slc/opendisplay_ble.c)
    if [ "$count" -ne 2 ]; then
        echo "Silabs restart must have one private definition and one close-event call; found $count"
        return 1
    fi
    close_calls=$(sed -n '/case sl_bt_evt_connection_closed_id:/,/^[[:space:]]*break;/p' \
        targets/efr32bg22-slc/opendisplay_ble.c \
        | grep -c '\bopendisplay_ble_restart_advertising[[:space:]]*(')
    if [ "$close_calls" -ne 1 ]; then
        echo "Silabs close-event handler must restart advertising exactly once; found $close_calls"
        return 1
    fi
}
check "silabs: advertising lifecycle ownership" silabs_advertising_ownership

# The BG22 compile-time profile decides the ABI of shared structs (OD_CONFIG_WITH_* gate members
# of struct od_config, OD_CONFIG_MAX_SIZE sizes the assembler). A host archive built from a
# different set validates a layout the firmware never runs -- and passes while doing it. So
# shared/profiles.cmake is the only definer, and every silabs consumer reads it from there.
silabs_profile_single_definer() {
    local names name block hits rc
    local bg22=targets/efr32bg22-slc/cmake_gcc/opendisplay-bg22.cmake

    if ! grep -q 'include(\${CMAKE_CURRENT_LIST_DIR}/profiles.cmake)' shared/sources.cmake; then
        echo "shared/sources.cmake no longer brings in the per-target profiles"
        return 1
    fi
    names=$(sed -n '/^set(OD_PROFILE_SILABS/,/)$/p' shared/profiles.cmake \
            | grep -oE '\bOD_[A-Z0-9_]+=' | tr -d '=')
    if [ -z "$names" ]; then
        echo "shared/profiles.cmake defines no OD_PROFILE_SILABS macros; the rule cannot be proven"
        return 1
    fi
    if ! grep -q '\${OD_PROFILE_SILABS}' "$bg22"; then
        echo "$bg22 no longer consumes OD_PROFILE_SILABS"
        return 1
    fi

    # Only the two silabs archives are in scope: unrelated executables set capability macros of
    # their own, and those are not this profile.
    block=$(sed -n '/target_compile_definitions(od_shared_silabs PUBLIC/,/)$/p;
                    /target_compile_definitions(od_shared_dispatch_fixture_silabs PUBLIC/,/)$/p' \
            tests/host/CMakeLists.txt)
    if [ "$(printf '%s\n' "$block" | grep -c '\${OD_PROFILE_SILABS}')" -ne 2 ]; then
        echo "both host silabs archives must take their profile from shared/profiles.cmake"
        return 1
    fi

    # grep's status is the proof: 0 restated, 1 clean, >1 unreadable and therefore unproven.
    for name in $names; do
        hits=$(grep -nE "\b${name}=" "$bg22")
        rc=$?
        if [ "$rc" -eq 0 ]; then
            echo "$hits"
            echo "$bg22 restates $name, which shared/profiles.cmake owns"
            return 1
        fi
        if [ "$rc" -gt 1 ]; then
            echo "could not scan $bg22 (status $rc); single-definer is unproven"
            return 1
        fi
        hits=$(printf '%s\n' "$block" | grep -nE "\b${name}=")
        rc=$?
        if [ "$rc" -eq 0 ]; then
            echo "$hits"
            echo "a host silabs archive restates $name, which shared/profiles.cmake owns"
            return 1
        fi
        if [ "$rc" -gt 1 ]; then
            echo "could not scan the host silabs archives (status $rc); single-definer is unproven"
            return 1
        fi
    done
}
check "silabs: one profile definer" silabs_profile_single_definer

log_hal_structure() {
    local rc=0 hits count grep_rc
    local esp32_converged_files=(
        targets/esp32-idf/hal/od_hal_adc.c
        targets/esp32-idf/hal/od_hal_crypto.c
        targets/esp32-idf/hal/od_hal_crypto_random.c
        targets/esp32-idf/hal/od_hal_nvs.c
        targets/esp32-idf/hal/od_hal_nvs_secure.c
        targets/esp32-idf/src/ble_transport_esp32.cpp
    )

    hits=$(find targets \( -name od_log.c -o -name od_log.cpp -o -name od_log.h \
             -o -name od_hal_log.h \) \
           ! -path '*/build/*' ! -path '*/build-*/*' -print 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "target-local logger API or implementation shadows shared ownership"
        rc=1
    fi

    hits=$(grep -nHE '\bESP_LOG[EWIDV][[:space:]]*\(' "${esp32_converged_files[@]}" 2>&1)
    grep_rc=$?
    if [ "$grep_rc" -eq 0 ]; then
        echo "$hits"
        echo "ESP32 application diagnostics returned to the raw ESP_LOG transport"
        rc=1
    elif [ "$grep_rc" -gt 1 ]; then
        echo "$hits"
        echo "could not scan every converged ESP32 logging file"
        rc=1
    fi

    count=$(grep -c '^void _od_log(' shared/core/od_log.c || true)
    if [ "$count" -ne 1 ]; then
        echo "shared od_log.c must contain exactly one _od_log definition (found $count)"
        rc=1
    fi
    count=$(grep -c '^void od_log_hex_line(' shared/core/od_log.c || true)
    if [ "$count" -ne 1 ]; then
        echo "shared od_log.c must contain exactly one od_log_hex_line definition (found $count)"
        rc=1
    fi

    hits=$(grep -rInE '\b(od_hal_log_room|od_log_set_ready_hook|od_log_set_loop_task)[[:space:]]*\(|\[DROP:' \
           shared targets/esp32-idf/src targets/esp32-idf/hal targets/esp32-idf/panel \
           targets/nordic-zephyr/src targets/nordic-zephyr/panel \
           targets/efr32bg22-slc --include='*.c' --include='*.cpp' --include='*.h' \
           --include='*.inl' --exclude-dir='build*' 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "retired logging backpressure or loop-task machinery returned"
        rc=1
    fi

    # BG22 states this through shared/profiles.cmake; "silabs: one profile definer" proves the
    # target consumes that list and restates none of it.
    if ! grep -qE '^[[:space:]]*OD_CAP_LOG=0$' shared/profiles.cmake; then
        echo "the BG22 profile must state logging capability-off explicitly"
        rc=1
    fi
    hits=$(grep -rInE '^([A-Za-z_][A-Za-z0-9_]*[[:space:]]+)+od_hal_log_[A-Za-z0-9_]+[[:space:]]*\(' \
           targets/efr32bg22-slc --include='*.c' --include='*.cpp' --exclude-dir='build*' \
           2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "BG22 capability-off target must not implement the log HAL"
        rc=1
    fi

    hits=$(grep -rInE '\bod_hal_uptime_ms[[:space:]]*\(' shared/core --include='*.c' \
           | grep -v '/od_log.c:' || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "shared policy replaced an explicit clock with the ambient time HAL"
        rc=1
    fi

    # Both submission paths hand Zephyr a whole mutable NUL-terminated buffer as "%s": the log
    # package ignores width and precision for strings, and a const cast would instead identify
    # retained read-only storage.
    if grep -q '%\.\*s' targets/nordic-zephyr/src/od_hal_log.c ||
       grep -qE '\(const[[:space:]]+char[[:space:]]*\*\)' targets/nordic-zephyr/src/od_hal_log.c; then
        echo "Nordic logger must submit whole mutable buffers as LOG_PRINTK/LOG_RAW(\"%s\", ...)"
        rc=1
    fi
    # A complete record cannot go out with its CR still on it. Zephyr writes a CR ahead of every
    # LF it forwards -- LOG_RAW's raw-string marker does not survive source resolution, so it
    # converts too -- and CR CR LF makes terminals overprint the previous line.
    if ! grep -q 'LOG_PRINTK' targets/nordic-zephyr/src/od_hal_log.c; then
        echo "Nordic logger must submit complete records through LOG_PRINTK with the CR removed"
        rc=1
    fi

    # STRING LITERALS ARE STRIPPED FIRST, and that is not a nicety. The rule is about ARGUMENTS,
    # but a message whose own text reads "disabled (%s)" or "(check wiring, pull-ups)" puts an
    # identifier immediately before a '(' inside the quotes -- so the unstripped form flags correct
    # code for the wording of its message, and the only way to pass is to write worse messages.
    # awk removes double-quoted spans per line before matching, and keeps file:line so a real hit
    # still points at itself.
    hits=$(find shared/core -name '*.c' ! -name 'od_log.c' -print0 \
           | xargs -0 -r awk '{ line=$0; gsub(/"[^"]*"/, "", line);
                             if (line ~ /od_log_(error|warn|info|debug|raw)[[:space:]]*\([^;]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(/)
                                 print FILENAME ":" FNR ": " $0 }' 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "shared log argument contains a nested function call; capability-off must not hide side effects"
        rc=1
    fi

    return $rc
}
check "structure: shared logging ownership" log_hal_structure

# The compile-time log level has one mapping from a profile name to OD_LOG_LEVEL. Its refusals
# are the half worth proving: a selector that returns nothing for an unknown profile hands the
# build od_log.h's implicit INFO default and looks like it worked.
log_profile_selector() {
    if ! grep -q '^function(od_select_log_profile' shared/profiles.cmake; then
        echo "shared/profiles.cmake no longer defines od_select_log_profile"
        return 1
    fi
    tests/cmake/run_log_profile_test.sh
}
check "log profile: one compile-time selector" log_profile_selector

nordic_epd_spi_ownership() {
    local rc=0 hits count
    local adapter=targets/nordic-zephyr/panel/od_bbep_zephyr_io.inl
    local backend=targets/nordic-zephyr/src/od_epd_spi_nrfx.c
    local fallback=targets/nordic-zephyr/src/od_epd_spi_bitbang.c

    hits=$(grep -nE '\bbb_spi_bitbang\b|od_gpio_write[[:space:]]*\([[:space:]]*pBBEP->i(CLK|MOSI)' \
           "$adapter" || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "bb_epaper adapter regained a per-edge generic GPIO loop"
        rc=1
    fi

    hits=$(grep -nE '\bod_(pin_decode|gpio_write)[[:space:]]*\(|\bgpio_pin_(set|configure)[[:space:]]*\(' \
           "$fallback" || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "EPD fallback inner path must use pre-decoded nRF GPIO registers"
        rc=1
    fi

    for selection in 'NRF_SPIM2' 'NRF_SPIM00' 'NRF_SPIM23'; do
        count=$(grep -c "$selection" "$backend" || true)
        if [ "$count" -lt 1 ]; then
            echo "missing Nordic EPD SPIM selection: $selection"
            rc=1
        fi
    done

    count=$(grep -c '^CONFIG_NRFX_SPIM=y$' targets/nordic-zephyr/zephyr/prj.conf || true)
    if [ "$count" -ne 1 ]; then
        echo "common Nordic config must enable the nrfx SPIM family exactly once (found $count)"
        rc=1
    fi

    if ! sed -n '/^&spi2 {/,/^};/p' \
          targets/nordic-zephyr/zephyr/boards/xiao_ble_nrf52840.overlay \
          | grep -q 'status = "disabled";'; then
        echo "xiao_nrf52840: application-owned SPIM2 must remain disabled in devicetree"
        rc=1
    fi
    if ! sed -n '/^&spi00 {/,/^};/p' \
          targets/nordic-zephyr/zephyr/boards/xiao_nrf54l15_nrf54l15_cpuapp.overlay \
          | grep -q 'status = "disabled";'; then
        echo "xiao_nrf54l15: application-owned SPIM00 must remain disabled in devicetree"
        rc=1
    fi
    for node in spi23 spi00; do
        if ! sed -n "/^&${node} {/,/^};/p" \
              targets/nordic-zephyr/zephyr/boards/xiao_nrf54lm20a_nrf54lm20a_cpuapp.overlay \
              | grep -q 'status = "disabled";'; then
            echo "xiao_nrf54lm20a: ${node} must remain disabled in devicetree"
            rc=1
        fi
    done

    if ! grep -q 'if(NOT OD_EPD_SPI_REQUIRE_SPIM)' targets/nordic-zephyr/zephyr/CMakeLists.txt ||
       ! grep -q -- '-DOD_EPD_SPI_REQUIRE_SPIM=ON' targets/nordic-zephyr/build.sh ||
       ! grep -q 'zephyr_get(OD_EPD_SPI_REQUIRE_SPIM)' targets/nordic-zephyr/zephyr/CMakeLists.txt; then
        echo "OD_EPD_SPI_REQUIRE_SPIM must both omit fallback code and cross the sysbuild boundary"
        rc=1
    fi

    return $rc
}
check "nordic: EPD SPI ownership" nordic_epd_spi_ownership

# ================================================================================== host suites ==
# The real boundary enforcement: shared/ compiled for the host at -std=c99 -Wall -Wextra -Werror
# under BOTH compilers. A GNU-ism gcc accepts is a failure discovered later on a target instead.
host_suite() {
    local cc="$1" dir="$BUILD_ROOT/host-$cc"
    command -v "$cc" >/dev/null 2>&1 || return 127
    cmake -S tests/host -B "$dir" -DCMAKE_C_COMPILER="$cc" >/dev/null \
        && cmake --build "$dir" -j"$(nproc)" \
        && ctest --test-dir "$dir" --output-on-failure --no-tests=error
}

for cc in gcc clang; do
    if command -v "$cc" >/dev/null 2>&1; then
        check "host suite ($cc)" host_suite "$cc"
    else
        skip "host suite ($cc)" "$cc not installed"
    fi
done

log_off_link_proof() {
    local binary="$BUILD_ROOT/host-gcc/od_log_off_test" hits

    [ -x "$binary" ] || { echo "capability-off logging fixture was not built"; return 1; }
    hits=$(nm "$binary" | grep -E 'od_hal_log_|s_armed|s_level_chars' || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "OD_CAP_LOG=0 retained log-HAL linkage or logger state at -O0"
        return 1
    fi
}
check "host: logging capability-off link proof" log_off_link_proof

# The _? in these patterns tolerates Mach-O's leading underscore on every C symbol (nm reports
# _od_pipe_start, not od_pipe_start) so the ratchet means the same thing on macOS as on Linux.
pipe_off_link_proof() {
    local binary="$BUILD_ROOT/host-gcc/od_pipe_off_test" hits entry_count obj obj_count refs
    local xfer_objs

    [ -x "$binary" ] || { echo "capability-off PIPE fixture was not built"; return 1; }
    hits=$(nm -a "$binary" | grep -E '\b_?(s_pipe|s_reorder|od_pipe_reorder_slot_t)\b' || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "OD_CAP_PIPE=0 retained PIPE sequencing or reorder storage"
        return 1
    fi
    entry_count=$(nm -g "$binary" | grep -Ec '\b_?od_pipe_(start|data|end|log_suffix)$' || true)
    if [ "$entry_count" -ne 4 ]; then
        echo "OD_CAP_PIPE=0 must retain its three dispatch entries and empty suffix formatter (found $entry_count)"
        return 1
    fi
    obj=$(find "$BUILD_ROOT/host-gcc/CMakeFiles/od_shared_silabs.dir" -name 'od_pipe.c.o' \
          2>/dev/null)
    obj_count=$(printf '%s\n' "$obj" | grep -c . || true)
    if [ "$obj_count" -ne 1 ] || [ ! -r "$obj" ]; then
        echo "expected exactly one readable capability-off od_pipe.c.o, found $obj_count"
        return 1
    fi
    refs=$(nm -u "$obj" | grep -Ec '_od_log' || true)
    if [ "$refs" -ne 0 ]; then
        echo "OD_CAP_PIPE=0 still references the logger ($refs undefined log symbols)"
        return 1
    fi
    xfer_objs=$(find "$BUILD_ROOT/host-gcc/CMakeFiles/od_shared_silabs.dir" \
        \( -name 'od_xfer.c.o' -o -name 'od_xfer_direct.c.o' -o -name 'od_xfer_partial.c.o' \) \
        2>/dev/null)
    obj_count=$(printf '%s\n' "$xfer_objs" | grep -c . || true)
    if [ "$obj_count" -ne 3 ]; then
        echo "expected three readable capability-off transfer objects, found $obj_count"
        return 1
    fi
    refs=$(nm -u $xfer_objs | grep -Ec '_od_log' || true)
    if [ "$refs" -ne 0 ]; then
        echo "OD_CAP_LOG=0 transfer objects still reference the logger ($refs symbols)"
        return 1
    fi
    hits=$(nm -a $xfer_objs "$obj" | grep -E '\b_?(s_peer_warning_budget|s_xfer_diag|s_pipe_log)\b' || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "OD_CAP_LOG=0 retained transfer diagnostic state"
        return 1
    fi
    hits=$(strings $xfer_objs "$obj" | grep -E 'DW (complete|failed|ended)|PIPE started|Transfer frame refused' || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "OD_CAP_LOG=0 retained transfer logging format strings"
        return 1
    fi
}
check "host: PIPE capability-off link proof" pipe_off_link_proof

# The NFC capability-off claim is an ABSENCE, and nfc_off_test proves only the behavioural half.
# This is the structural half: the assembler and both seam references must be gone, while the
# frame/reset entry points and lifecycle reporter remain.
#
# The seam symbols are DEFINED in that binary on purpose and must not be REFERENCED by od_nfc.o --
# checking for their absence entirely would pass by link failure rather than by behaviour.
nfc_off_link_proof() {
    local binary="$BUILD_ROOT/host-gcc/od_nfc_off_test" hits entry_count refs log_refs obj obj_count

    [ -x "$binary" ] || { echo "capability-off NFC fixture was not built"; return 1; }
    hits=$(nm -a "$binary" | grep -E "\b_?s_nfc\b" || true)
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "OD_CAP_NFC=0 retained the chunk assembler"
        return 1
    fi
    # RESOLVE THE OBJECT FIRST, and insist on exactly one. Piping an unresolved path straight into
    # nm made this check fail OPEN: no match meant an empty argument, nm's error went to /dev/null,
    # and refs came back 0 -- reporting "no seam references" from a file that was never read.
    obj=$(find "$BUILD_ROOT/host-gcc" -name 'od_nfc.c.o' -path '*od_nfc_off*' 2>/dev/null)
    obj_count=$(printf '%s\n' "$obj" | grep -c . || true)
    if [ "$obj_count" -ne 1 ] || [ ! -r "$obj" ]; then
        echo "expected exactly one readable capability-off od_nfc.c.o, found $obj_count"
        return 1
    fi
    refs=$(nm -u "$obj" | grep -Ec "od_nfc_app_(read|write)" || true)
    if [ "$refs" -ne 0 ]; then
        echo "OD_CAP_NFC=0 still references the tag seam ($refs undefined seam symbols)"
        return 1
    fi
    log_refs=$(nm -u "$obj" | grep -Ec "_od_log" || true)
    if [ "$log_refs" -ne 0 ]; then
        echo "OD_CAP_NFC=0 still references the logger ($log_refs undefined log symbols)"
        return 1
    fi
    entry_count=$(nm -g "$binary" | grep -Ec "\b_?od_nfc_(frame|reset|log_event)$" || true)
    if [ "$entry_count" -ne 3 ]; then
        echo "OD_CAP_NFC=0 must retain its three public entry points (found $entry_count)"
        return 1
    fi
}
check "host: NFC capability-off link proof" nfc_off_link_proof

# ASan + UBSan over every host test at once -- including the two PRE-AUTH parsers, od_config_tlv
# and od_session, whose inputs an unauthenticated peer controls. halt_on_error is not decoration:
# UBSan's default is to print a diagnostic and carry on with status 0.
host_sanitizers() {
    local dir="$BUILD_ROOT/host-asan"
    cmake -S tests/host -B "$dir" \
          -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
          -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" >/dev/null \
        && cmake --build "$dir" -j"$(nproc)" \
        && UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
           ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1}" \
           ctest --test-dir "$dir" --output-on-failure --no-tests=error
}
check "host suite (ASan + UBSan)" host_sanitizers

# ======================================================================================== fuzz ==
# MIGRATION.md Gate 1 requires fuzz coverage for anything reachable pre-authentication. Clang
# only: -fsanitize=fuzzer is a Clang feature and gcc has no libFuzzer. The corpora under
# tests/fuzz/corpus/ are committed, so each run starts from the coverage the last one found.
#
# A crash writes its input to ./crash-<sha1>. Commit it to the corpus AND pin it as a numbered
# case in the corresponding ordinary host test, so the regression fails without fuzzing too.
FUZZ_TARGETS=(session_open_raw session_open_sealed session_auth pipe_start pipe_data)

fuzz_all() {
    local dir="$BUILD_ROOT/fuzz" t
    cmake -S tests/host -B "$dir" -DCMAKE_C_COMPILER=clang >/dev/null \
        && cmake --build "$dir" -j"$(nproc)" || return 1
    # If tests/fuzz ever stops being registered -- a renamed guard, a moved directory -- the loop
    # below would pass with nothing to run. Fail loudly instead.
    for t in "${FUZZ_TARGETS[@]}"; do
        if [ ! -x "$dir/fuzz/od_fuzz_$t" ]; then
            echo "od_fuzz_$t was not built -- the pre-auth surface is UNFUZZED"
            return 1
        fi
    done
    for t in "${FUZZ_TARGETS[@]}"; do
        echo "--- $t (${FUZZ_TIME}s)"
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            "$dir/fuzz/od_fuzz_$t" -max_total_time="$FUZZ_TIME" -runs=2000000 \
            -print_final_stats=1 "tests/fuzz/corpus/$t" || return 1
    done
}

if command -v clang >/dev/null 2>&1; then
    check "fuzz: pre-auth surface (${FUZZ_TIME}s x ${#FUZZ_TARGETS[@]})" fuzz_all
else
    skip "fuzz: pre-auth surface" "clang not installed; libFuzzer is Clang-only"
fi

# ============================================================================== wire corpus ==
# The cross-repo wire contract: the same vectors decoded by this repo's C core and by the
# py-opendisplay release Home_Assistant_Integration pins. py-opendisplay 7.14.0 needs Python
# >= 3.13, which the system Python may not be -- uv fetches one rather than failing on install.
replay_pinned() {
    if command -v uv >/dev/null 2>&1; then
        uv run --python 3.13 --with 'py-opendisplay==7.14.0' \
            python tests/host/replay_vectors.py tests/vectors
    else
        python3 -c 'import opendisplay' 2>/dev/null || return 127
        python3 tests/host/replay_vectors.py tests/vectors
    fi
}
if command -v uv >/dev/null 2>&1 || python3 -c 'import opendisplay' 2>/dev/null; then
    # Guarded above rather than relying on the 127 return, which check() would report as a FAIL:
    # a missing toolchain is a skip everywhere else in this file and must be here too.
    check "wire corpus vs py-opendisplay 7.14.0 (pinned)" replay_pinned
else
    skip "wire corpus vs py-opendisplay 7.14.0 (pinned)" "needs uv, or py-opendisplay importable"
fi

# The nightly advisory check that used to catch upstream drift within 24 h. WITHOUT CI THIS ONLY
# RUNS WHEN ASKED, so a py-opendisplay release that breaks the wire contract is now found
# whenever someone next passes --latest, not the next morning. That is the real cost of having
# no scheduled runner; run it before any release.
replay_latest() {
    command -v uv >/dev/null 2>&1 || return 127
    uv run --python 3.13 --with py-opendisplay python tests/host/replay_vectors.py tests/vectors
}
if [ "$DO_LATEST" = 1 ]; then
    if command -v uv >/dev/null 2>&1; then
        check "wire corpus vs py-opendisplay latest (advisory)" replay_latest
    else
        skip "wire corpus vs py-opendisplay latest (advisory)" "needs uv"
    fi
fi

# ======================================================================================= esp32 ==
# THE SHIM IS GONE, and this is what keeps it gone. It replaces compat/ratchet.sh, whose metric
# was "how many files INCLUDE a shim header" -- a count that reached 0 while three call sites
# were still reaching shim primitives, because `delay(long)` and `millis()` are declared by an
# OD-PATCH in third_party/bb_epaper/src/bb_epaper.h, not by any header the ratchet grepped for.
# So this checks CALLS, which is the property that actually matters.
#
# panel/ and vendor/ are exempt BY DESIGN, not by oversight: od_bbep_idf_io.inl and
# fastepd_adapter.cpp exist precisely to supply these primitives to the two vendored libraries
# that demand them, and third_party/ is exempt from the one rule (CLAUDE.md, decision 13).
arduino_free() {
    local rc=0

    # SPI.h is deliberately NOT in this list: the only one in the tree is the PERMANENT FastEPD
    # adapter, vendor/fastepd/SPI.h, and counting it would make this check fail forever on a
    # file that is meant to include it. The guard against a shim SPI.h reappearing is the next
    # block, which pins where SPI.h may live -- compat/ used to hold that risk and is now gone.
    local hdrs='(arduino_compat|Arduino|Wire|WiFi|ledc_compat|esp32-hal-gpio|HardwareSerial)\.h'
    local hits
    hits=$(grep -rlE "#[[:space:]]*include[[:space:]]*[<\"][[:space:]]*$hdrs" \
             targets/esp32-idf --include='*.c' --include='*.cpp' --include='*.h' \
             --include='*.hpp' --include='*.inl' 2>/dev/null || true)
    if [ -n "$hits" ]; then
        echo "::error::Arduino header included under targets/esp32-idf/:"
        echo "$hits" | sed 's/^/    /'
        rc=1
    fi

    hits=$(find targets/esp32-idf -name 'SPI.h' -not -path '*/build/*' \
           | grep -v '^targets/esp32-idf/vendor/fastepd/SPI\.h$' || true)
    if [ -n "$hits" ]; then
        echo "::error::an SPI.h outside the FastEPD vendor adapter -- the header pattern above"
        echo "         does not count <SPI.h>, so files including this one would be invisible:"
        echo "$hits" | sed 's/^/    /'
        rc=1
    fi

    # Calls to Arduino primitives in APP code. panel/ and vendor/ are exempt BY DESIGN:
    # od_bbep_idf_io.inl and fastepd_adapter.cpp exist precisely to supply these to the two
    # vendored libraries that demand them, and third_party/ is exempt from the one rule
    # (CLAUDE.md, decision 13).
    #
    # The awk pass strips comments before matching. Without it the check fires on prose --
    # "millis()-poll from loop()" in a header comment is not a call, and a gate that cannot
    # tell those apart gets disabled rather than obeyed.
    #
    # `[(]` rather than `\(`: the pattern crosses shell -> awk -v -> dynamic regex, and a
    # backslash does not survive that trip. It arrived as a bare `(`, which made the regex
    # invalid, and awk died on the first file -- while the check still reported OK, because the
    # `|| true` that was here swallowed the status. A gate that cannot fail is worse than none,
    # so the scan's own failure is now a failure.
    local prims='(pinMode|digitalWrite|digitalRead|analogRead|analogWrite|analogReadResolution|analogSetPinAttenuation|delay|delayMicroseconds|millis|micros|temperatureRead|yield)'
    hits=$(find targets/esp32-idf/src targets/esp32-idf/hal targets/esp32-idf/ble \
                targets/esp32-idf/main -type f \
                \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.inl' \) 2>/dev/null \
           | xargs awk -v pat="(^|[^_[:alnum:]>.])$prims[[:space:]]*[(]" '
               FNR == 1 { inblock = 0 }
               { line = $0
                 if (inblock) { if (sub(/.*\*\//, "", line)) inblock = 0; else next }
                 gsub(/\/\*[^*]*\*\//, "", line)
                 if (sub(/\/\*.*/, "", line)) inblock = 1
                 sub(/\/\/.*/, "", line)
                 if (line ~ pat) printf "%s:%d:%s\n", FILENAME, FNR, $0
               }') || { echo "::error::the Arduino-primitive scan failed to run"; return 1; }
    if [ -n "$hits" ]; then
        echo "::error::Arduino primitive called from app code (use od_hal_*):"
        echo "$hits" | sed 's/^/    /'
        rc=1
    fi

    [ $rc -eq 0 ] && echo "OK: no Arduino header or primitive in targets/esp32-idf/ app code."
    return $rc
}
check "esp32: arduino-free app code" arduino_free

# ALL 11 FRAGMENTS, not a sampled subset. The c3/c6/classic parts differ in PSRAM,
# LAN and panel backends, which is precisely where a shared-core change breaks one target and not
# the others. `build.sh` with no arguments builds every fragment; `sdkconfig_baseline.sh` with no
# arguments then checks every board that has a build, so the two stay in step automatically
# instead of through a hand-maintained list that silently narrows coverage.
#
# Opt-in only because it needs ESP-IDF (never on PATH; build.sh sources it) and several minutes.
esp32_all_and_baseline() {
    ( cd targets/esp32-idf && ./build.sh ) || return 1
    targets/esp32-idf/tools/sdkconfig_baseline.sh
}
if [ "$DO_ESP32" = 1 ]; then
    check "esp32: all fragments + sdkconfig baseline" esp32_all_and_baseline
else
    skip "esp32: all fragments + sdkconfig baseline" "needs --esp32/--targets (builds 10 fragments)"
fi

# ====================================================================================== nordic ==
# Nordic has no cheap check at all -- no ratchet, no baseline -- so without this the crypto HAL and
# the session adapter on this target are covered by nothing but somebody remembering to build. It
# is opt-in only because it needs the nRF toolchain and several minutes, NOT because it is
# optional before merge.
nordic_all() {
    ( cd targets/nordic-zephyr && ./build.sh --all )
}
if [ "$DO_NORDIC" = 1 ]; then
    check "nordic: all three boards" nordic_all
else
    skip "nordic: all three boards" "needs --nordic/--targets (nRF toolchain, several minutes)"
fi

# ===================================================================================== silabs ==
silabs_bg22() {
    targets/efr32bg22-slc/build-and-flash.sh --clean --no-flash
}
if [ "$DO_SILABS" = 1 ]; then
    check "silabs: BG22 headless build" silabs_bg22
else
    skip "silabs: BG22 headless build" "needs --silabs/--targets (Simplicity SDK 2025.12.2)"
fi

# ===================================================================================== summary ==
printf '\n\033[1m======== summary ========\033[0m\n'
printf '%d passed, %d failed, %d skipped\n' "$PASS" "$FAIL" "$SKIP"
if [ "$FAIL" -gt 0 ]; then
    printf '\n\033[31mfailed:\033[0m\n'
    printf '  %s\n' "${FAILED_NAMES[@]}"
fi
if [ "$SKIP" -gt 0 ]; then
    # Reprinted so a skipped check cannot be mistaken for a passing one when the summary is the
    # only thing read.
    printf '\n\033[33mnot checked:\033[0m\n'
    printf '  %s\n' "${SKIPPED_NAMES[@]}"
fi
[ "$FAIL" -gt 0 ] && exit 1
[ "$SKIP" -gt 0 ] && exit 2
exit 0
