#!/usr/bin/env bash
# Build OpenDisplay nRF54 firmware with NCS/west.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="${APP_DIR:-${SCRIPT_DIR}/zephyr}"
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/build}"
BOARD="${BOARD:-xiao_nrf54l15/nrf54l15/cpuapp}"

# Every board this target supports. `./build.sh --all` builds each and delivers all of their
# artefacts to <repo>/release/, which is what the ESP32 target's build.sh does by default. This
# one keeps single-board as the DEFAULT because flash.sh pairs with one BUILD_DIR and the
# edit/flash loop is per board; --all is for producing a release set.
OD_ALL_BOARDS=(
  "xiao_nrf54l15/nrf54l15/cpuapp"
  "xiao_nrf54lm20a/nrf54lm20a/cpuapp"
  "xiao_ble/nrf52840"
)
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
od_write_manifest_header() {
  local commit dirty=""
  commit="$(git -C "${REPO_ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  git -C "${REPO_ROOT}" diff --quiet HEAD 2>/dev/null || dirty=" (working tree DIRTY)"
  echo "OpenDisplay Nordic/Zephyr firmware"
  echo "built    $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
  echo "commit   ${commit}${dirty}"
  echo "ncs      ${OD_NCS_VERSION:-v3.3.1}"
  echo
  echo "BOOTLOADER: every board uses MCUboot and emits -ota.bin / -dfu.zip (SMP)."
  echo "xiao_nrf52840 (Zephyr board xiao_ble) is dual-slot MCUboot chained behind the"
  echo "Adafruit bootloader with the SoftDevice ERASED. Its first install is a one-time USB"
  echo "serial-DFU visit (tools/release/nrf52840_usb_packages.sh builds the two packages;"
  echo "docs/UPGRADING.md has the procedure). EN04/EN05 have NO UF2 drive."
  echo "OD_NRF52840_MCUBOOT=0 builds the old Adafruit-only layout instead; its artefacts carry"
  echo "an -adafruit suffix so the two can never be confused."
  echo
  if [[ -n "${OD_MCUBOOT_KEY_DIR:-}" ]]; then
    echo "SIGNING: project key from OD_MCUBOOT_KEY_DIR (ecdsa-p256.pem / ed25519.pem)"
  else
    echo "SIGNING: MCUboot DEBUG key (published; anyone can sign images)"
  fi
  echo
  printf '%-22s %10s  %s\n' BOARD BYTES MERGED-IMAGE
}
RELEASE_DIR="${REPO_ROOT}/release"
MANIFEST="${RELEASE_DIR}/MANIFEST-nordic-zephyr.txt"
PROFILE="${PROFILE:-battery}"   # battery | uart | debug | quiet
PURGE="${PURGE:-always}"        # always | never | auto

case "${PROFILE}" in
  battery|uart|debug|quiet) ;;
  *)
    echo "Unknown PROFILE '${PROFILE}'. Use battery, uart, debug, or quiet." >&2
    exit 2
    ;;
esac

PROFILE_SUFFIX=""
if [[ "${PROFILE}" == debug ]]; then
  PROFILE_SUFFIX="-debug"
fi

export PROFILE

# shellcheck source=ncs-env.sh
if [[ "${1:-}" == "--all" ]]; then
  shift
  mkdir -p "${RELEASE_DIR}"
  : > "${MANIFEST}.tmp"          # fresh per --all run; single-board builds append instead
  rc=0
  for b in "${OD_ALL_BOARDS[@]}"; do
    tag="${b%%/*}"
    echo "=============================================================="
    echo "  ${b}"
    echo "=============================================================="
    # Separate BUILD_DIR per board: sharing one would make each build a full rebuild and would
    # leave whichever ran last as the only thing flash.sh could find.
    if BOARD="${b}" BUILD_DIR="${SCRIPT_DIR}/build-${tag}${PROFILE_SUFFIX}" OD_MANIFEST_APPEND="${MANIFEST}.tmp" \
         "${BASH_SOURCE[0]}" "$@"; then :; else rc=1; echo "!! ${b} FAILED" >&2; fi
  done
  od_write_manifest_header > "${MANIFEST}" 2>/dev/null || true
  cat "${MANIFEST}.tmp" >> "${MANIFEST}"; rm -f "${MANIFEST}.tmp"
  echo; echo "release -> ${RELEASE_DIR}"; cat "${MANIFEST}"
  exit "${rc}"
fi

source "${SCRIPT_DIR}/ncs-env.sh"

CMAKE_ARGS=(-DBOARD_ROOT="${SCRIPT_DIR}")

# Which bootloader composition this board builds. Every board uses MCUboot unless nRF52840 is
# built with OD_NRF52840_MCUBOOT=0 (Adafruit-only); every
# other board here uses MCUboot + Partition Manager. Pristine handling and sysbuild Kconfig
# selection derive from the same predicate so their board scopes cannot diverge.
OD_BOARD_TAG="${BOARD%%/*}"
OD_USES_MCUBOOT=1
# nRF52840 DEFAULT (3.0): dual-slot MCUboot chained BEHIND the Adafruit bootloader with the
# SoftDevice erased: Adafruit (0xF4000) -> MCUboot (0x1000) -> application (0xD200). This is
# the composition every 2026-11 EN04 hardware row ran on (BLE OTA, rollback, image store,
# 16-colour). It needs a one-time USB serial-DFU install that erases the SoftDevice; see
# pm_static_nrf52840_mcuboot.yml and docs/UPGRADING.md.
#
# OD_NRF52840_MCUBOOT=0 opts back into the Adafruit-only layout (application at 0x27000, no
# BLE OTA). Its artefacts are suffixed -adafruit so a release directory cannot hold two
# different files under one name.
OD_NRF52840_MCUBOOT="${OD_NRF52840_MCUBOOT:-1}"
if [[ "${OD_BOARD_TAG}" == xiao_ble* && "${OD_NRF52840_MCUBOOT}" != 1 ]]; then
  OD_USES_MCUBOOT=0
fi

# MCUboot SIGNING KEY: one input, OD_MCUBOOT_KEY_DIR, a directory holding one PEM per
# signature type. NCS picks the type per SoC (ECDSA-P256 on nRF52840, Ed25519 on nRF54L), so a
# single file cannot serve every board; the directory can:
#   ${OD_MCUBOOT_KEY_DIR}/ecdsa-p256.pem   nRF52840
#   ${OD_MCUBOOT_KEY_DIR}/ed25519.pem      nRF54L15 / nRF54LM20A
# Unset, the build uses MCUboot's PUBLISHED DEBUG KEYS (the NCS default), which is the
# accepted 3.0 limitation: anyone can sign an image these bootloaders will accept. Changing
# the key later strands every unit already in the field on the old key until a USB reflash,
# and for nRF54 it also breaks SMP from Firmware_NRF54 1.1.x (same default key today).
if [[ -n "${OD_MCUBOOT_KEY_DIR:-}" ]]; then
  case "${OD_BOARD_TAG}" in
    xiao_ble*) od_key="${OD_MCUBOOT_KEY_DIR}/ecdsa-p256.pem" ;;
    *)         od_key="${OD_MCUBOOT_KEY_DIR}/ed25519.pem" ;;
  esac
  if [[ ! -f "${od_key}" ]]; then
    echo "OD_MCUBOOT_KEY_DIR is set but ${od_key} does not exist." >&2
    echo "Refusing to fall back to the debug key silently." >&2
    exit 2
  fi
  od_key="$(cd "$(dirname "${od_key}")" && pwd)/$(basename "${od_key}")"
fi

# `west build -p always` does not fully discard the outer sysbuild state for an MCUboot /
# Partition Manager composition. After an application CMakeLists change it can reconfigure that
# tree with an unexpanded PM placeholder, and signing then dies on
# `--slot-size @PM_MCUBOOT_PRIMARY_SIZE@`. The default/gate contract says "always" means a clean
# build, so enforce that contract here. Only directories this script names are removed; an
# externally supplied BUILD_DIR is left to west.
if [[ "${PURGE}" == "always" && "${OD_USES_MCUBOOT}" == 1 && -d "${BUILD_DIR}" ]]; then
  case "${BUILD_DIR}" in
    "${SCRIPT_DIR}/build"|"${SCRIPT_DIR}/build-${OD_BOARD_TAG}"|"${SCRIPT_DIR}/build-${OD_BOARD_TAG}-debug")
      rm -rf -- "${BUILD_DIR}"
      ;;
    *)
      echo "Not manually removing external MCUboot build directory; west will handle pristine: ${BUILD_DIR}" >&2
      ;;
  esac
fi

# Adafruit-only nRF52840 (opt-in) and chained-MCUboot nRF52840 (default) each take their own
# sysbuild file; the nRF54 boards use sysbuild.conf. The
# choice lives in a sysbuild Kconfig file, which cannot be overridden from CMake or -D, so the
# right file is selected here. Without this an xiao_ble build fails at configure with
# "required nodelabel not found: slot0_partition" -- upstream xiao_ble ships pm_static.yml for
# the Adafruit layout and has no MCUboot partitions.
OD_EXTRA_CONFS=()
if [[ "${OD_USES_MCUBOOT}" == 0 ]]; then
  CMAKE_ARGS+=(-DSB_CONF_FILE="${SCRIPT_DIR}/zephyr/sysbuild_adafruit.conf")
elif [[ "${OD_BOARD_TAG}" == xiao_ble* ]]; then
  # Chained MCUboot: its own sysbuild file, plus an app overlay that undoes the three
  # disables in boards/xiao_ble_nrf52840.conf. The static partition map that pins MCUboot to
  # 0x27000 is picked up by Partition Manager from pm_static_xiao_ble_nrf52840.yml.
  CMAKE_ARGS+=(-DSB_CONF_FILE="${SCRIPT_DIR}/zephyr/sysbuild_adafruit_mcuboot.conf")
  OD_EXTRA_CONFS+=("${APP_DIR}/prj_nrf52840_mcuboot.conf")
  # Passed explicitly rather than relying on Partition Manager's board-name discovery: a file
  # called pm_static_xiao_ble_nrf52840.yml would also be applied to the default Adafruit-only
  # build, which then fails looking for an MCUboot image it never builds.
  CMAKE_ARGS+=(-DPM_STATIC_YML_FILE="${APP_DIR}/pm_static_nrf52840_mcuboot.yml")
  # MCUboot's CMake needs slot0_partition as a DEVICETREE nodelabel during its own configure,
  # before Partition Manager has placed anything, and the xiao_ble DTS has the Adafruit/UF2
  # layout with no slots. Both images take the same overlay so the two maps cannot disagree.
  CMAKE_ARGS+=(-DEXTRA_DTC_OVERLAY_FILE="${APP_DIR}/nrf52840_mcuboot_partitions.overlay")
  CMAKE_ARGS+=(-Dmcuboot_EXTRA_DTC_OVERLAY_FILE="${APP_DIR}/nrf52840_mcuboot_partitions.overlay")
fi
if [[ "${OD_USES_MCUBOOT}" == 1 && -n "${od_key:-}" ]]; then
  # A sysbuild Kconfig string, so the value carries its own quotes.
  CMAKE_ARGS+=("-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"${od_key}\"")
fi
if [[ "${PROFILE}" == "uart" ]]; then
  OD_EXTRA_CONFS+=("${APP_DIR}/prj_uart.conf")
fi
# One -DEXTRA_CONF_FILE carrying a semicolon-separated list: two separate -D flags would not
# accumulate, the last would win, and a uart build with chained MCUboot would silently drop
# one of the two overlays.
if [[ ${#OD_EXTRA_CONFS[@]} -gt 0 ]]; then
  OD_EXTRA_CONF_LIST="$(IFS=';'; echo "${OD_EXTRA_CONFS[*]}")"
  CMAKE_ARGS+=(-DEXTRA_CONF_FILE="${OD_EXTRA_CONF_LIST}")
fi
# PROFILE=quiet is picked up from the environment in CMakeLists.txt
# (prj_quiet.conf + OD_LOW_POWER_QUIET).
FW_VER="${OD_FW_VERSION:-${BUILD_VERSION:-}}"
if [[ -n "${FW_VER}" ]]; then
  # Export for app CMakeLists (sysbuild-safe). Also pass cmake arg for completeness.
  export OD_FW_VERSION="${FW_VER}"
  export BUILD_VERSION="${FW_VER}"
  CMAKE_ARGS+=(-Dzephyr_OD_FW_VERSION="${FW_VER}")
fi
if [[ -n "${SHA:-}" ]]; then
  export SHA
  CMAKE_ARGS+=(-Dzephyr_GIT_SHA="${SHA}")
fi
if [[ -n "${FACTORY_CONFIG_HEX:-}" ]]; then
  CMAKE_ARGS+=(-DFACTORY_CONFIG_HEX="${FACTORY_CONFIG_HEX}")
fi
if [[ "${OPENDISPLAY_FACTORY_CLEAR_CONFIG:-}" =~ ^(1|true|yes|on)$ ]]; then
  CMAKE_ARGS+=(-DFACTORY_CLEAR_CONFIG_ON_BOOT=ON)
fi
if [[ "${OD_EPD_SPI_REQUIRE_SPIM:-}" =~ ^(1|true|yes|on)$ ]]; then
  CMAKE_ARGS+=(-DOD_EPD_SPI_REQUIRE_SPIM=ON)
fi

west build -p "${PURGE}" -d "${BUILD_DIR}" -b "${BOARD}" "${APP_DIR}" -- "${CMAKE_ARGS[@]}"

HEX="${BUILD_DIR}/merged.hex"
if [[ ! -f "${HEX}" ]]; then
  HEX="${BUILD_DIR}/zephyr/zephyr/zephyr.hex"
fi
if [[ ! -f "${HEX}" ]]; then
  HEX="${BUILD_DIR}/opendisplay_nordic/zephyr/zephyr.hex"
fi

UPDATE_BIN=""
for candidate in \
  "${BUILD_DIR}/zephyr/zephyr/zephyr.signed.bin" \
  "${BUILD_DIR}/zephyr/app_update.bin" \
  "${BUILD_DIR}/app_update.bin"; do
  if [[ -f "${candidate}" ]]; then
    UPDATE_BIN="${candidate}"
    break
  fi
done
DFU_ZIP=""
for candidate in \
  "${BUILD_DIR}/dfu_application.zip" \
  "${BUILD_DIR}/zephyr/dfu_application.zip"; do
  if [[ -f "${candidate}" ]]; then
    DFU_ZIP="${candidate}"
    break
  fi
done

CONF="${BUILD_DIR}/zephyr/zephyr/.config"
echo
echo "Built: ${HEX}"
if [[ -n "${UPDATE_BIN}" ]]; then
  echo "OTA:   ${UPDATE_BIN}"
fi
if [[ -n "${DFU_ZIP}" ]]; then
  echo "DFU:   ${DFU_ZIP}"
fi
# ---------------------------------------------------------------- release delivery ---
# Same contract as the ESP32 target: build artefacts land in <repo>/release/, which is
# gitignored. Names carry the board tag because three boards share one directory and
# "merged.hex" alone says nothing about which part it is for -- flashing an nRF54 image to an
# nRF52840 is a soft brick that reads as a bad cable.
# Artefact tag, NOT the Zephyr board name. Upstream calls the nRF52840 board "xiao_ble", which
# says nothing about which part is inside -- and the whole point of a release filename here is
# that flashing an nRF54 image to an nRF52840 is a soft brick that reads as a bad cable. The
# board name stays xiao_ble everywhere Zephyr needs it; only the artefact is renamed.
BOARD_TAG="${BOARD%%/*}"
case "${BOARD_TAG}" in
  xiao_ble) BOARD_TAG="xiao_nrf52840" ;;
esac
# The opt-in Adafruit-only nRF52840 layout gets its own name: its merged.hex starts at 0x27000
# and needs the SoftDevice, the default one starts at 0x1000 and needs it erased.
if [[ "${OD_USES_MCUBOOT}" == 0 ]]; then
  BOARD_TAG="${BOARD_TAG}-adafruit"
fi
mkdir -p "${RELEASE_DIR}"
REL_LINE=""
if [[ -f "${HEX}" ]]; then
  cp "${HEX}" "${RELEASE_DIR}/opendisplay-${BOARD_TAG}${PROFILE_SUFFIX}-merged.hex"
  REL_LINE="$(printf '%-22s %10s  opendisplay-%s%s-merged.hex' \
      "${BOARD_TAG}${PROFILE_SUFFIX}" "$(stat -c %s "${HEX}" 2>/dev/null || stat -f %z "${HEX}")" "${BOARD_TAG}" "${PROFILE_SUFFIX}")"
fi
if [[ -n "${UPDATE_BIN}" ]]; then
  cp "${UPDATE_BIN}" "${RELEASE_DIR}/opendisplay-${BOARD_TAG}${PROFILE_SUFFIX}-ota.bin"
fi
if [[ -n "${DFU_ZIP}" ]]; then
  cp "${DFU_ZIP}" "${RELEASE_DIR}/opendisplay-${BOARD_TAG}${PROFILE_SUFFIX}-dfu.zip"
fi
# UF2 only for the Adafruit-only layout, where the application sits at 0x27000 exactly where
# the bootloader's UF2 volume writes it. That volume exists on the stock Seeed XIAO bootloader
# and NOT on EN04/EN05 (serial DFU only). In the MCUboot layout Zephyr's zephyr.uf2 is the
# unsigned application at its slot address, which nothing should flash, so it is not shipped.
UF2=""
if [[ "${OD_USES_MCUBOOT}" == 0 ]]; then
  for candidate in \
    "${BUILD_DIR}/zephyr/zephyr/zephyr.uf2" \
    "${BUILD_DIR}/zephyr/zephyr.uf2"; do
    if [[ -f "${candidate}" ]]; then UF2="${candidate}"; break; fi
  done
fi
if [[ -n "${UF2}" ]]; then
  cp "${UF2}" "${RELEASE_DIR}/opendisplay-${BOARD_TAG}${PROFILE_SUFFIX}.uf2"
fi
if [[ -n "${REL_LINE}" ]]; then
  if [[ -n "${OD_MANIFEST_APPEND:-}" ]]; then
    echo "${REL_LINE}" >> "${OD_MANIFEST_APPEND}"      # --all collects, writes header once
  else
    od_write_manifest_header > "${MANIFEST}"
    echo "${REL_LINE}" >> "${MANIFEST}"
  fi
fi

echo "Release: ${RELEASE_DIR}/opendisplay-${BOARD_TAG}${PROFILE_SUFFIX}-merged.hex"
if [[ "${BOARD}" == xiao_ble* ]]; then
  if [[ "${PROFILE}" == debug ]]; then
    echo "Flash: ./flash-nrf52840-debug.sh"
  else
    echo "Flash: ./flash-nrf52840.sh"
  fi
else
  # Per-board front doors: the board is in the script name, not an argument, so a
  # wrong-image flash is a visible typo rather than a positional mistake.
  if [[ "${BOARD}" == *lm20* ]]; then
    od_flash_script="./flash-nrf54lm20.sh"
  else
    od_flash_script="./flash-nrf54l15.sh"
  fi
  if [[ "${PROFILE}" == debug ]]; then
    od_flash_script="${od_flash_script%.sh}-debug.sh"
  fi
  echo "Flash: ${od_flash_script}"
fi
if [[ -f "${CONF}" ]]; then
  echo "Profile: ${PROFILE}  serial=$(grep -E '^CONFIG_SERIAL=|^# CONFIG_SERIAL is not set' "${CONF}" | head -1)"
  if [[ "${PROFILE}" == "debug" ]]; then
    echo "Debug build: application DEBUG plus Zephyr subsystem INFO"
  elif [[ "${PROFILE}" == "uart" ]]; then
    echo "Serial monitor: 115200 baud on the board USB port"
  elif [[ "${PROFILE}" == "quiet" ]]; then
    echo "Quiet build: LOG off, no heartbeat prints (for advertising-current tests)"
  elif [[ "${BOARD}" == xiao_ble* ]]; then
    echo "Logs: USB CDC ACM"
  else
    echo "Logs: SEGGER RTT (battery build has no USB UART)"
  fi
fi
