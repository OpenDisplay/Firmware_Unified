#!/usr/bin/env bash
#
# Build ESP32 boards and deliver merged, ready-to-flash images to <repo>/release/.
#
#   ./build.sh                     # every board in boards/, including -debug ones
#   ./build.sh --release           # every board EXCEPT -debug ones -- what actually ships
#   ./build.sh s3-n16r8 c6-n4      # just these (bypasses --release filtering)
#   ./build.sh --list              # what boards exist
#   ./build.sh --clean             # wipe each board's build dir first
#
# -debug boards (OD_LOG_LEVEL=OD_LOG_DEBUG compiled in, per their boards/*-debug.cmake) are
# bench builds, not shipping ones -- see boards/*-debug.cmake. They are ordinary board
# fragments, so the default (no args) build already includes them; --release is the one that
# has to opt OUT, by name suffix, since nothing else marks a fragment as debug-only.
#
# Each board produces ONE file, release/opendisplay-<board>-merged.bin, flashed at offset 0:
#
#   python -m esptool --chip <chip> write_flash 0x0 release/opendisplay-<board>-merged.bin
#
# Alongside it, for updates rather than first flashes (built by CMakeLists.txt, see
# tools/web_flash_parts.py):
#   release/opendisplay-<board>-{bootloader,partition-table,ota-data,app}.bin
#   release/opendisplay-<board>-manifest.json   esp-web-tools manifest over those parts. They
#       skip NVS (0x9000-0xDFFF), so a browser update keeps the device config; the merged
#       image does not.
#   release/opendisplay-<board>-app.bin is also the BLE (SMP) OTA image.
#
# The chip is not guessable from the filename, so release/MANIFEST.txt records it per image
# along with the size and the source commit. Do not infer it -- flashing an esp32c3 image to
# an esp32c6 is a soft brick that reads as a bad cable.
#
# WHY MERGED IMAGES. The four-image form (bootloader / partition table / OTA data / app) has
# per-chip offsets: the classic ESP32 puts its bootloader at 0x1000, every other variant here
# at 0x0. That is the single most common flashing mistake, and it fails as "boots to a
# bootloop" rather than as an error. The merge is done by CMakeLists.txt on every build from
# IDF's own flash_args, so the offsets are never transcribed by hand and the image can never
# be stale relative to the build.
#
# This script does NOT flash. Attaching a board is a separate, deliberate act; see the
# per-image command in the manifest.
set -euo pipefail

cd "$(dirname "$0")"
TARGET_DIR="$PWD"
REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || echo "$TARGET_DIR/../..")"
RELEASE_DIR="$REPO_ROOT/release"

# --- board list ---------------------------------------------------------------------------
# boards/*.cmake is the registry. *.panel.cmake is excluded: those are panel fragments
# included BY a board fragment, not boards -- building one would configure a nonexistent
# target, and the glob catching them is an easy mistake to make here.
all_boards() {
    local f b
    for f in boards/*.cmake; do
        b="$(basename "$f" .cmake)"
        [[ "$b" == *.panel ]] && continue
        echo "$b"
    done
}

CLEAN=0
RELEASE_ONLY=0
BOARDS=()
for arg in "$@"; do
    case "$arg" in
        --list)  all_boards; exit 0 ;;
        --clean) CLEAN=1 ;;
        --release) RELEASE_ONLY=1 ;;
        -h|--help) sed -n '2,32p' "$0" | sed 's/^# \?//'; exit 0 ;;
        -*) echo "unknown option: $arg" >&2; exit 2 ;;
        *)  BOARDS+=("$arg") ;;
    esac
done
if [ ${#BOARDS[@]} -eq 0 ]; then
    # Not mapfile: macOS ships bash 3.2 (GPLv2-frozen), which predates it.
    BOARDS=()
    while IFS= read -r b; do BOARDS+=("$b"); done < <(all_boards)
    if [ "$RELEASE_ONLY" = 1 ]; then
        # Filter by name, not a separate registry: -debug is the only marker a board fragment
        # carries today (see boards/*-debug.cmake), so this is where "what ships" is decided.
        release=()
        for b in "${BOARDS[@]}"; do
            [[ "$b" == *-debug ]] && continue
            release+=("$b")
        done
        BOARDS=("${release[@]}")
    fi
elif [ "$RELEASE_ONLY" = 1 ]; then
    echo "--release has no effect with explicit board names -- drop it or the board list" >&2
    exit 2
fi

# Validate up front rather than failing on board 7 of 10 after ten minutes of building.
for b in "${BOARDS[@]}"; do
    if [ ! -f "boards/$b.cmake" ]; then
        echo "no such board: $b" >&2
        echo "known boards:" >&2
        all_boards | sed 's/^/  /' >&2
        exit 2
    fi
done

# --- toolchain ------------------------------------------------------------------------------
# ESP-IDF is installed but never on PATH (CLAUDE.md § Toolchains), so `which idf.py` finding
# nothing is the normal state, not a missing install. Source the export script unless the
# caller already did.
if ! command -v idf.py >/dev/null 2>&1; then
    IDF_EXPORT="${IDF_PATH:-$HOME/esp/esp-idf}/export.sh"
    if [ ! -f "$IDF_EXPORT" ]; then
        echo "ESP-IDF not found. Expected $IDF_EXPORT" >&2
        echo "Set IDF_PATH, or run: source ~/esp/esp-idf/export.sh" >&2
        exit 1
    fi
    # shellcheck disable=SC1090
    source "$IDF_EXPORT" >/dev/null
fi
IDF_ACTUAL="$(idf.py --version 2>&1 | head -1 | sed 's/^ESP-IDF //')"
echo "ESP-IDF: $IDF_ACTUAL"

# --- version pin ----------------------------------------------------------------------------
# .idf_version is the checked-in pin. Without it build.sh takes whatever IDF happens to live at
# $IDF_PATH, so "works on my machine" and a CI failure are indistinguishable from a code change
# -- the same reproducibility hole as globbing for an NCS version. The pin is enforced, not
# advisory: an IDF minor bump changes generated startup code, the bootloader, and sdkconfig
# defaults, and those land in a merged image nobody diffs.
#
# Override deliberately (testing a new IDF, bisecting a toolchain regression):
#   OD_IDF_VERSION_CHECK=warn ./build.sh     # note the mismatch, build anyway
#   OD_IDF_VERSION_CHECK=off  ./build.sh     # say nothing
# If a new IDF is adopted, edit .idf_version in the same commit as whatever the bump requires.
IDF_PIN_FILE="$TARGET_DIR/.idf_version"
IDF_PIN=""
# Guarded: under `set -e` + pipefail a missing pin file would abort the script on sed's exit
# status instead of reaching the "no pin" warning below -- i.e. deleting the pin would look
# like a broken build script rather than an unpinned build.
if [ -f "$IDF_PIN_FILE" ]; then
    IDF_PIN="$(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$IDF_PIN_FILE" | head -1 | tr -d '[:space:]')"
fi
case "${OD_IDF_VERSION_CHECK:-enforce}" in
    off) ;;
    *)
        if [ -z "$IDF_PIN" ]; then
            echo "!! no version pin found at $IDF_PIN_FILE -- build is not reproducible" >&2
        elif [ "$IDF_PIN" != "$IDF_ACTUAL" ]; then
            echo "ESP-IDF version mismatch: pinned $IDF_PIN, active $IDF_ACTUAL" >&2
            if [ "${OD_IDF_VERSION_CHECK:-enforce}" = "warn" ]; then
                echo "   continuing anyway (OD_IDF_VERSION_CHECK=warn)" >&2
            else
                echo "   Pin lives in $IDF_PIN_FILE." >&2
                echo "   Build with the pinned IDF, or OD_IDF_VERSION_CHECK=warn ./build.sh" >&2
                exit 1
            fi
        fi
        ;;
esac

mkdir -p "$RELEASE_DIR"

# --- build ------------------------------------------------------------------------------
# -DSDKCONFIG per board is NOT optional. Without it every board writes the project-root
# sdkconfig, and the second board fails with "Target 'esp32s3' in sdkconfig does not match
# currently selected IDF_TARGET 'esp32c3'" -- a confusing way to discover that the builds
# share state. Keeping it inside the build dir also makes --clean actually clean.
FAILED=()
BUILT=()
for b in "${BOARDS[@]}"; do
    echo
    echo "=============================================================="
    echo "  $b"
    echo "=============================================================="
    [ "$CLEAN" = 1 ] && rm -rf "build/$b"

    # Drop the generated sdkconfig when any input that produces it is newer.
    #
    # IDF applies sdkconfig.defaults ONLY when creating sdkconfig. Once the file exists it is
    # authoritative, and edits to the defaults are silently ignored -- the build succeeds,
    # reports no warning, and produces a binary with the OLD configuration. That cost a full
    # ten-board build to notice on 2026-08-04 (the four restored values appeared to have no
    # effect whatsoever, including an -Os change that did not move the image size by a single
    # byte), and it would have been far worse to discover on hardware.
    #
    # Deleting is right rather than heavy-handed: this project has no interactive menuconfig
    # workflow, so build/<board>/sdkconfig holds nothing a human authored. Its inputs -- the
    # common defaults, the per-chip defaults, and the board fragment -- are the source of truth,
    # and regenerating from them is exactly what should happen when one of them changes.
    cfg="build/$b/sdkconfig"
    if [ -f "$cfg" ]; then
        for src in sdkconfig.defaults sdkconfig.defaults.* "boards/$b.sdkconfig" boards/_*.sdkconfig; do
            if [ -f "$src" ] && [ "$src" -nt "$cfg" ]; then
                echo "note: $src is newer than $cfg -- regenerating config" >&2
                rm -f "$cfg"
                break
            fi
        done
    fi

    # OD_FW_VERSION, as on the other two targets: a release tag stamps the same version on all.
    if idf.py -B "build/$b" -DSDKCONFIG="build/$b/sdkconfig" -DOD_BOARD="$b" \
        ${OD_FW_VERSION:+-DPROJECT_VER="${OD_FW_VERSION}"} build; then
        BUILT+=("$b")
    else
        FAILED+=("$b")
        echo "!! $b FAILED" >&2
    fi
done

# --- deliver ------------------------------------------------------------------------------
# Only boards that actually built. Copying whatever happens to be on disk would silently
# ship the previous run's image for a board that just failed, which is worse than shipping
# nothing for it.
echo
echo "=============================================================="
echo "  release -> $RELEASE_DIR"
echo "=============================================================="
COMMIT="$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
DIRTY=""
git -C "$REPO_ROOT" diff --quiet HEAD 2>/dev/null || DIRTY=" (working tree DIRTY)"

# Per-target manifest name. Three targets now deliver into one release/ directory, so a bare
# MANIFEST.txt would be whichever target happened to build last -- and the ESP32 one would look
# authoritative for images it says nothing about.
MANIFEST="$RELEASE_DIR/MANIFEST-esp32-idf.txt"
{
    echo "OpenDisplay ESP32 firmware -- merged images, flash at offset 0x0"
    echo "built    $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "commit   ${COMMIT}${DIRTY}"
    echo "idf      $(idf.py --version 2>&1 | head -1)"
    # Both, not just the active one: under OD_IDF_VERSION_CHECK=warn these can differ, and an
    # image built off-pin has to be identifiable as such after the fact.
    echo "idf pin  ${IDF_PIN:-(none)}"
    echo
    printf '%-28s %-10s %10s  %s\n' BOARD CHIP BYTES FLASH-WITH
} > "$MANIFEST"

for b in "${BUILT[@]}"; do
    src="build/$b/opendisplay-$b-merged.bin"
    if [ ! -f "$src" ]; then
        echo "!! $b built but produced no merged image ($src)" >&2
        FAILED+=("$b (no image)")
        continue
    fi
    cp "$src" "$RELEASE_DIR/"
    if ! ls "build/$b/web/opendisplay-$b-"*.bin "build/$b/web/opendisplay-$b-manifest.json" >/dev/null 2>&1; then
        echo "!! $b built but produced no web-flasher parts (build/$b/web/)" >&2
        FAILED+=("$b (no web parts)")
        continue
    fi
    cp "build/$b/web/opendisplay-$b-"* "$RELEASE_DIR/"
    # The chip comes from the build's own sdkconfig, not from a table here: a second mapping
    # of board -> chip is a thing that can disagree with the board fragment.
    chip="$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"$/\1/p' "build/$b/sdkconfig" | head -1)"
    size="$(stat -c %s "$src" 2>/dev/null || stat -f %z "$src")"
    printf '%-28s %-10s %10s  python -m esptool --chip %s write_flash 0x0 opendisplay-%s-merged.bin\n' \
        "$b" "$chip" "$size" "$chip" "$b" >> "$MANIFEST"
    printf '  %-28s %-10s %10s bytes\n' "$b" "$chip" "$size"
done

echo
cat "$MANIFEST"

if [ ${#FAILED[@]} -gt 0 ]; then
    echo
    echo "FAILED: ${FAILED[*]}" >&2
    exit 1
fi
echo
echo "OK: ${#BUILT[@]} board(s) delivered to $RELEASE_DIR"
