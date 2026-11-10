#!/usr/bin/env bash
# ci_check.sh -- run tools/check.sh in CI and decide what its exit status means.
#
# check.sh exits 2 when ANY check was skipped, deliberately: "a skip is not a pass". In CI that
# status needs splitting, because one skip is legitimate and every other kind is not. The three
# target builds are skipped here because they run in their OWN jobs with their own toolchains --
# putting ESP-IDF, the nRF toolchain and the Simplicity SDK in one image to avoid that would be
# a far worse trade. A skip for any other reason (no clang, no Python that can install
# py-opendisplay) means the gate silently stopped covering something, and must be red.
#
# So: failures are always fatal, the listed skips are tolerated, and ANY OTHER SKIP IS FATAL.
# The allow-list is spelled out rather than pattern-matched loosely, so a newly-skipping check
# cannot slip in under an existing entry.
#
#   tools/ci_check.sh [extra check.sh args...]
#
# Exit 0 only when nothing failed and nothing outside the allow-list was skipped.

set -u -o pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

# Substrings of check names whose skip is covered by a sibling CI job.
ALLOWED_SKIPS=(
    "esp32: all fragments + sdkconfig baseline"
    "nordic: all three boards"
    "silabs: BG22 headless build"
)

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

tools/check.sh "$@" 2>&1 | tee "$LOG"
rc="${PIPESTATUS[0]}"

echo
echo "===== ci_check: interpreting check.sh exit ${rc} ====="

if [ "$rc" -eq 1 ]; then
    echo "::error::check.sh reported failures"
    exit 1
fi

if [ "$rc" -eq 0 ]; then
    echo "ci_check: nothing failed, nothing skipped"
    exit 0
fi

if [ "$rc" -ne 2 ]; then
    echo "::error::check.sh exited ${rc}, which is neither pass (0), fail (1) nor skip (2)"
    exit 1
fi

# Exit 2: pull the reprinted "not checked:" block out of the summary and judge each line.
# sed from the marker to EOF; the block is the last thing check.sh prints. Colour codes are
# stripped first: check.sh prints the marker in yellow, and the escape sits before it.
mapfile -t skipped < <(sed 's/\x1b\[[0-9;]*m//g' "$LOG" | sed -n '/^not checked:/,$p' \
    | tail -n +2 | sed 's/^ *//' | grep -v '^$')

if [ "${#skipped[@]}" -eq 0 ]; then
    echo "::error::check.sh exited 2 but printed no 'not checked:' block -- cannot verify what was skipped"
    exit 1
fi

unexpected=0
for s in "${skipped[@]}"; do
    ok=0
    for a in "${ALLOWED_SKIPS[@]}"; do
        case "$s" in *"$a"*) ok=1; break ;; esac
    done
    if [ "$ok" = 1 ]; then
        echo "  tolerated (covered by its own CI job): $s"
    else
        echo "::error::unexpected skip, nothing else covers this: $s"
        unexpected=1
    fi
done

[ "$unexpected" = 0 ] || exit 1
echo "ci_check: no failures; only sibling-job target builds were skipped"
exit 0
