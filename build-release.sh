#!/usr/bin/env bash
# Build every target into release/.
#
#   ./build-release.sh                    # all three targets
#   ./build-release.sh nordic esp32       # a subset
#   PROFILE=debug ./build-release.sh      # nordic honours it; the others have no equivalent
#   ./build-release.sh --list             # show targets and what each builds
#
# THIS DRIVES THE PER-TARGET SCRIPTS, it does not reimplement them. Each target owns its
# toolchain activation (none of the three is on PATH), its board list, its artefact naming
# and its own release/MANIFEST-<target>.txt. Those manifests stay separate on purpose: a
# single shared one would only ever describe whichever target built last.
#
# EVERY TARGET RUNS EVEN IF AN EARLIER ONE FAILS, and the exit status is nonzero if any did.
# Stopping at the first failure would leave the other targets' artefacts in release/ from
# some previous run, with nothing on screen saying they are stale -- which is the failure
# mode a release directory is least able to survive.
#
# All three build headless in CI (.github/workflows/build.yml), which also runs
# tools/release/collect.py on the result; this script does not. See docs/RELEASE_ARTEFACTS.md.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RELEASE_DIR="${SCRIPT_DIR}/release"
SUMMARY="${RELEASE_DIR}/MANIFEST.txt"
PROFILE="${PROFILE:-}"

# name : directory : command...  (the command is the target's own front door)
od_target_dir() {
  case "$1" in
    esp32)  echo "targets/esp32-idf" ;;
    nordic) echo "targets/nordic-zephyr" ;;
    silabs) echo "targets/efr32bg22-slc" ;;
    *)      return 1 ;;
  esac
}

od_target_cmd() {
  case "$1" in
    # No arguments: builds every board in boards/*.cmake.
    esp32)  echo "./build.sh" ;;
    # --all is required here; the default is single-board because flash.sh pairs with one
    # BUILD_DIR and the edit/flash loop is per board.
    nordic) echo "./build.sh --all" ;;
    # Build only. The OTA/bootloader/artefact steps need Simplicity Commander and a board.
    silabs) echo "./build-and-flash.sh --no-flash" ;;
    *)      return 1 ;;
  esac
}

ALL_TARGETS=(esp32 nordic silabs)

if [[ "${1:-}" == "--list" ]]; then
  printf '%-8s %-24s %s\n' TARGET DIRECTORY COMMAND
  for t in "${ALL_TARGETS[@]}"; do
    printf '%-8s %-24s %s\n' "$t" "$(od_target_dir "$t")" "$(od_target_cmd "$t")"
  done
  exit 0
fi
if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  sed -n '2,20p' "$0" | sed 's/^# \?//'
  exit 0
fi

TARGETS=("$@")
if [[ ${#TARGETS[@]} -eq 0 ]]; then
  TARGETS=("${ALL_TARGETS[@]}")
fi

# Validate before building: failing on the third name after twenty minutes is the one
# outcome worth spending a loop to avoid.
for t in "${TARGETS[@]}"; do
  if ! od_target_dir "$t" >/dev/null; then
    echo "unknown target '$t' (known: ${ALL_TARGETS[*]})" >&2
    exit 2
  fi
done

mkdir -p "${RELEASE_DIR}"
LOG_DIR="${RELEASE_DIR}/logs"
mkdir -p "${LOG_DIR}"

# Not declare -A: macOS ships bash 3.2 (GPLv2-frozen), which predates associative arrays.
# STATUS/ELAPSED are plain indexed arrays kept parallel to TARGETS by position instead.
STATUS=()
ELAPSED=()
rc=0

i=0
for t in "${TARGETS[@]}"; do
  dir="$(od_target_dir "$t")"
  cmd="$(od_target_cmd "$t")"
  log="${LOG_DIR}/build-${t}.log"

  echo "=============================================================="
  echo "  ${t}   (${dir}: ${cmd})"
  echo "=============================================================="

  start=$SECONDS
  # Each target script sources its own toolchain, so run it from its own directory in a
  # subshell -- PROFILE is exported for the one target that reads it.
  if ( cd "${SCRIPT_DIR}/${dir}" && PROFILE="${PROFILE}" bash -c "${cmd}" ) 2>&1 | tee "${log}"; then
    STATUS[$i]=ok
  else
    STATUS[$i]=FAILED
    rc=1
  fi
  ELAPSED[$i]=$((SECONDS - start))
  echo "--- ${t}: ${STATUS[$i]} (${ELAPSED[$i]}s, log: ${log#${SCRIPT_DIR}/})"
  i=$((i+1))
done

# The summary records THIS RUN. Artefacts in release/ can outlive the build that made them,
# so a target that failed here may still have a file and a per-target manifest on disk from
# an earlier run; the status column is the only thing that distinguishes them.
{
  echo "OpenDisplay release build"
  echo "built    $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
  commit="$(git -C "${SCRIPT_DIR}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  dirty=""
  git -C "${SCRIPT_DIR}" diff --quiet HEAD 2>/dev/null || dirty=" (working tree DIRTY)"
  echo "commit   ${commit}${dirty}"
  [[ -n "${PROFILE}" ]] && echo "profile  ${PROFILE} (nordic-zephyr only)"
  echo
  printf '%-8s %-8s %8s  %s\n' TARGET STATUS SECONDS MANIFEST
  i=0
  for t in "${TARGETS[@]}"; do
    m="MANIFEST-$(basename "$(od_target_dir "$t")").txt"
    [[ -f "${RELEASE_DIR}/${m}" ]] || m="(none)"
    printf '%-8s %-8s %8s  %s\n' "$t" "${STATUS[$i]}" "${ELAPSED[$i]}" "$m"
    i=$((i+1))
  done
} > "${SUMMARY}"

echo
cat "${SUMMARY}"
echo
echo "release -> ${RELEASE_DIR}"
exit "${rc}"
