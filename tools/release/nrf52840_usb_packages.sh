#!/usr/bin/env bash
# Build the two nRF52840 USB serial-DFU packages that install the 3.x layout from the Adafruit
# bootloader (docs/RELEASE_ARTEFACTS.md § 4.3):
#
#   release/opendisplay-xiao_nrf52840-usb-1-sd-erase.zip   one-shot app at 0x27000 that erases
#                                                           the s140 SoftDevice and resets
#   release/opendisplay-xiao_nrf52840-usb-2-install.zip    MCUboot + signed app (merged.hex),
#                                                           which the bootloader then writes at 0x1000
#
# Run after targets/nordic-zephyr/build.sh has put opendisplay-xiao_nrf52840-merged.hex in
# release/. Needs west (sourced from targets/nordic-zephyr/ncs-env.sh unless already on PATH)
# and adafruit-nrfutil (ADAFRUIT_NRFUTIL, else PATH).
#
# adafruit-nrfutil genpkg turns the hex into a flat binary starting at its lowest address, and
# the bootloader writes that binary at its own application start (0x27000 with a SoftDevice,
# 0x1000 without). So each hex must START exactly where the bootloader will put it; both are
# checked here rather than trusted.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RELEASE_DIR="${1:-${REPO_ROOT}/release}"
NORDIC_DIR="${REPO_ROOT}/targets/nordic-zephyr"
APP_DIR="${REPO_ROOT}/tools/nrf52840_sd_erase"
BUILD_DIR="${SD_ERASE_BUILD_DIR:-${NORDIC_DIR}/build-nrf52840-sd-erase}"
MERGED="${RELEASE_DIR}/opendisplay-xiao_nrf52840-merged.hex"
NRFUTIL="${ADAFRUIT_NRFUTIL:-$(command -v adafruit-nrfutil || true)}"

[[ -f "${MERGED}" ]] || { echo "missing ${MERGED}; run targets/nordic-zephyr/build.sh first" >&2; exit 1; }
[[ -n "${NRFUTIL}" && -x "${NRFUTIL}" ]] || { echo "adafruit-nrfutil not found (set ADAFRUIT_NRFUTIL)" >&2; exit 1; }

hex_low_address() {
  python3 - "$1" <<'PY'
import sys
base, low = 0, None
for line in open(sys.argv[1]):
    if not line.startswith(":"):
        continue
    t = int(line[7:9], 16)
    if t == 4:
        base = int(line[9:13], 16) << 16
    elif t == 2:
        base = int(line[9:13], 16) << 4
    elif t == 0:
        a = base + int(line[3:7], 16)
        low = a if low is None else min(low, a)
print(hex(low if low is not None else -1))
PY
}

expect_low() {
  local hex="$1" want="$2" got
  got="$(hex_low_address "${hex}")"
  if [[ "${got}" != "${want}" ]]; then
    echo "${hex} starts at ${got}, expected ${want}; the bootloader would write it to the wrong place" >&2
    exit 1
  fi
}

if ! command -v west >/dev/null 2>&1; then
  # shellcheck source=/dev/null
  source "${NORDIC_DIR}/ncs-env.sh"
fi

rm -rf "${BUILD_DIR}"
west build -p always -b xiao_ble/nrf52840 -d "${BUILD_DIR}" "${APP_DIR}" -- \
  -DSB_CONF_FILE="${APP_DIR}/sysbuild.conf"

SD_HEX=""
for c in "${BUILD_DIR}/nrf52840_sd_erase/zephyr/zephyr.hex" "${BUILD_DIR}"/*/zephyr/zephyr.hex \
         "${BUILD_DIR}/zephyr/zephyr.hex"; do
  [[ -f "${c}" ]] && { SD_HEX="${c}"; break; }
done
[[ -n "${SD_HEX}" ]] || { echo "sd_erase build produced no zephyr.hex under ${BUILD_DIR}" >&2; exit 1; }

expect_low "${SD_HEX}" 0x27000
expect_low "${MERGED}" 0x1000

S1="${RELEASE_DIR}/opendisplay-xiao_nrf52840-usb-1-sd-erase.zip"
S2="${RELEASE_DIR}/opendisplay-xiao_nrf52840-usb-2-install.zip"
# The NCS toolchain environment exports PYTHONHOME/PYTHONPATH for its own interpreter; a
# separately installed adafruit-nrfutil must not inherit them.
nrfutil() { env -u PYTHONHOME -u PYTHONPATH "${NRFUTIL}" "$@"; }
nrfutil dfu genpkg --dev-type 0x0052 --application "${SD_HEX}" "${S1}"
nrfutil dfu genpkg --dev-type 0x0052 --application "${MERGED}" "${S2}"

MANIFEST="${RELEASE_DIR}/MANIFEST-nordic-zephyr.txt"
if [[ -f "${MANIFEST}" ]]; then
  {
    echo
    echo "nRF52840 one-time USB install (adafruit-nrfutil serial DFU, --dev-type 0x0052):"
    printf '  %-48s %8s  %s\n' "$(basename "${S1}")" "$(stat -c %s "${S1}")" "step 1: erase SoftDevice (from ${SD_HEX#"${REPO_ROOT}/"})"
    printf '  %-48s %8s  %s\n' "$(basename "${S2}")" "$(stat -c %s "${S2}")" "step 2: MCUboot + app at 0x1000"
  } >> "${MANIFEST}"
fi
echo "USB packages -> ${S1}, ${S2}"
