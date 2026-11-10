#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/target-registry.sh
source "${SCRIPT_DIR}/scripts/target-registry.sh"
od_target_load nrf52840

BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/${OD_TARGET_BUILD_DIR}}"
CONFIG="${BUILD_DIR}/zephyr/zephyr/.config"
UF2="${BUILD_DIR}/zephyr/zephyr/zephyr.uf2"

if [[ ! -f "${CONFIG}" ]] || ! grep -q '^CONFIG_SOC_NRF52840=y$' "${CONFIG}"; then
  echo "Refusing to flash: ${BUILD_DIR} is not a verified nRF52840 build." >&2
  exit 1
fi
if grep -q '^CONFIG_BOOTLOADER_MCUBOOT=y$' "${CONFIG}"; then
  # The default (MCUboot) layout has no UF2 path: EN04/EN05 expose serial DFU only, and the
  # first install must erase the SoftDevice. This script does not write flash for it.
  echo "${BUILD_DIR} is the MCUboot layout. Install it over USB serial DFU:"
  echo "  tools/release/nrf52840_usb_packages.sh    (builds the two packages into release/)"
  echo "  adafruit-nrfutil dfu serial --package release/opendisplay-xiao_nrf52840-usb-1-sd-erase.zip -p <port> -b 115200 --singlebank"
  echo "  adafruit-nrfutil dfu serial --package release/opendisplay-xiao_nrf52840-usb-2-install.zip -p <port> -b 115200 --singlebank"
  echo "Step 1 only while the SoftDevice is still present. Updates afterwards: SMP with"
  echo "release/opendisplay-xiao_nrf52840-ota.bin. Procedure and caveats: docs/UPGRADING.md."
  exit 0
fi
if [[ ! -f "${UF2}" ]]; then
  echo "Missing ${UF2}; run ./build-nrf52840.sh first." >&2
  exit 1
fi

UF2_MOUNT="${1:-${UF2_MOUNT:-}}"
if [[ -z "${UF2_MOUNT}" ]]; then
  echo "UF2 ready: ${UF2}"
  echo "Put the board in the Adafruit bootloader, then run:"
  echo "  ./flash-nrf52840.sh /path/to/BOOTLOADER_VOLUME"
  exit 0
fi
if [[ ! -d "${UF2_MOUNT}" ]]; then
  echo "UF2 mount does not exist: ${UF2_MOUNT}" >&2
  exit 1
fi

cp "${UF2}" "${UF2_MOUNT}/opendisplay-nrf52840.uf2"
echo "Copied nRF52840 UF2 to ${UF2_MOUNT}/opendisplay-nrf52840.uf2"
