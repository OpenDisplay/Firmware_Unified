#!/usr/bin/env bash
# Build the Telink TLSR825x target: targets/telink-tlsr/build/opendisplay_tlsr.bin, and the
# bring-up probe opendisplay_tlsr_probe.bin (src/probe/od_tlsr_probe.c)
#
#   TC32_DIR=<toolchain root, holding bin/tc32-elf-gcc> \
#   TELINK_SDK_DIR=<tc_ble_single_sdk root, holding proj_lib/> \
#   targets/telink-tlsr/build.sh [extra cmake -D options]
#
# Neither the toolchain nor the SDK is vendored or on PATH (README.md says where they come
# from). OD_FW_VERSION / OD_BUILD_ID pass through to CMake like the other targets'.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${TC32_DIR:?set TC32_DIR to the tc32 toolchain root (holding bin/tc32-elf-gcc)}"
: "${TELINK_SDK_DIR:?set TELINK_SDK_DIR to the tc_ble_single_sdk root (holding proj_lib/)}"
BUILD="${BUILD_DIR:-${HERE}/build}"
args=(-DTC32_DIR="${TC32_DIR}" -DTELINK_SDK_DIR="${TELINK_SDK_DIR}")
[ -n "${OD_FW_VERSION:-}" ] && args+=(-DOD_FW_VERSION="${OD_FW_VERSION}")
[ -n "${OD_BUILD_ID:-}" ] && args+=(-DOD_BUILD_ID="${OD_BUILD_ID}")
cmake -S "${HERE}" -B "${BUILD}" -G "Unix Makefiles" "${args[@]}" "$@" >/dev/null
cmake --build "${BUILD}" -j"$(nproc)"
python3 -I "${HERE}/tools/tlsr_image.py" check "${BUILD}/opendisplay_tlsr.bin" "${BUILD}/opendisplay_tlsr_probe.bin"
