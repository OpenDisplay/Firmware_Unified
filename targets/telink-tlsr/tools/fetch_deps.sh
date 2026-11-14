#!/usr/bin/env bash
# Fetch the pinned Telink BLE SDK and tc32 toolchain into build/deps/telink-tlsr/ (override with
# OD_TELINK_DEPS). Neither is vendored: the SDK is ~16 MB of Telink's tree and the toolchain is a
# host binary. They live under build/ rather than targets/ because tools/check.sh's ratchets scan
# targets/ and skip only build*: the SDK's own CRC-32 table and I2C driver would trip them.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
DEPS="${OD_TELINK_DEPS:-${ROOT}/build/deps/telink-tlsr}"

SDK_URL="https://github.com/telink-semi/tc_ble_single_sdk"
SDK_SHA="78f5e28b85dc6dcdc37f1904364a1203c97b7dfc"

TC32_URL="https://github.com/flyskywhy/tc32"
case "$(uname -s)" in
  Darwin) TC32_BRANCH="macos"; TC32_SHA="fdb1e7eec0a63f88156d99178b0dc64ad7d040e4" ;;
  Linux)  TC32_BRANCH="linux"; TC32_SHA="8ec375bb95ef63b6d0a4b18008913c03e7928710" ;;
  *) echo "unsupported host $(uname -s); set TC32_TOOLCHAIN_DIR yourself" >&2; exit 1 ;;
esac

fetch() {  # url sha dir [branch]
  local url="$1" sha="$2" dir="$3" branch="${4:-}"
  if [ -d "${dir}/.git" ] && [ "$(git -C "${dir}" rev-parse HEAD)" = "${sha}" ]; then
    echo "ok   ${dir##*/} @ ${sha:0:12}"
    return
  fi
  rm -rf "${dir}"
  git clone --quiet ${branch:+--branch "${branch}"} --depth 1 "${url}" "${dir}"
  if [ "$(git -C "${dir}" rev-parse HEAD)" != "${sha}" ]; then
    git -C "${dir}" fetch --quiet --depth 1 origin "${sha}"
    git -C "${dir}" checkout --quiet "${sha}"
  fi
  echo "got  ${dir##*/} @ ${sha:0:12}"
}

mkdir -p "${DEPS}"
fetch "${SDK_URL}" "${SDK_SHA}" "${DEPS}/tc_ble_single_sdk"
fetch "${TC32_URL}" "${TC32_SHA}" "${DEPS}/tc32" "${TC32_BRANCH}"
"${DEPS}/tc32/bin/tc32-elf-gcc" --version | head -1
