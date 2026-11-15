#!/usr/bin/env bash

# Shell target registry shared by the thin build/flash front doors. Call
# od_target_load with one of: nrf52840, nrf54l15, nrf54lm20.
od_target_load() {
  case "${1:-}" in
    nrf52840)
      OD_TARGET_NAME=nrf52840
      OD_TARGET_BOARD=xiao_ble/nrf52840
      OD_TARGET_BUILD_DIR=build-nrf52840
      # Default: MCUboot chained behind the Adafruit bootloader (OD_NRF52840_MCUBOOT=0 for
      # the Adafruit-only layout, whose artefact is a UF2).
      OD_TARGET_BOOTLOADER=adafruit+mcuboot
      OD_TARGET_ARTIFACT=signed
      ;;
    nrf54l15|l15)
      OD_TARGET_NAME=nrf54l15
      OD_TARGET_BOARD=xiao_nrf54l15/nrf54l15/cpuapp
      OD_TARGET_BUILD_DIR=build-nrf54l15
      OD_TARGET_BOOTLOADER=mcuboot
      OD_TARGET_ARTIFACT=signed
      ;;
    nrf54lm20|lm20)
      OD_TARGET_NAME=nrf54lm20
      OD_TARGET_BOARD=xiao_nrf54lm20a/nrf54lm20a/cpuapp
      OD_TARGET_BUILD_DIR=build-nrf54lm20
      OD_TARGET_BOOTLOADER=mcuboot
      OD_TARGET_ARTIFACT=signed
      ;;
    *)
      echo "Unknown Nordic target '${1:-}'. Use nrf52840, nrf54l15, or nrf54lm20." >&2
      return 2
      ;;
  esac
  export OD_TARGET_NAME OD_TARGET_BOARD OD_TARGET_BUILD_DIR
  export OD_TARGET_BOOTLOADER OD_TARGET_ARTIFACT
}
