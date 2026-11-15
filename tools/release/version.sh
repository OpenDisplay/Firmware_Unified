#!/usr/bin/env bash
# The one place a firmware version is decided for a build.
#
#   tools/release/version.sh [TAG-OR-VERSION]    print X.Y.Z
#   tools/release/version.sh --check             verify the in-source defaults agree
#
# With an argument (a tag such as 3.0.0 or v3.0.0), print it without the leading v after
# checking it is X.Y.Z with every part <= 255: 0x0043 carries major/minor/patch as single
# bytes, and BG22 packs them into SL_APPLICATION_VERSION as (major<<16)|(minor<<8)|patch.
# Without one, print the in-source default (shared/core/od_fw_version.h), which untagged CI
# and dev builds report.
#
# CI passes the result to every target as OD_FW_VERSION. Each target derives everything else
# from that one string: ESP32 PROJECT_VER, the Nordic 0x0043 version, and BG22's 0x0043 version
# plus SL_APPLICATION_VERSION. The in-source defaults below are used only when OD_FW_VERSION
# is unset, and --check (run by tools/check.sh) keeps them from drifting apart.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

default_version() {
  sed -n 's/^#define OD_FW_VERSION_DEFAULT "\([^"]*\)".*/\1/p' "${REPO_ROOT}/shared/core/od_fw_version.h"
}

validate() {
  local v="$1"
  if [[ ! "${v}" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
    echo "version '${v}' is not X.Y.Z" >&2; return 1
  fi
  local p
  for p in "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${BASH_REMATCH[3]}"; do
    if (( 10#${p} > 255 )); then echo "version '${v}': component ${p} exceeds 255" >&2; return 1; fi
  done
}

if [[ "${1:-}" == "--check" ]]; then
  d="$(default_version)"
  validate "${d}"
  IFS=. read -r maj min _ <<< "${d}"
  app_hex="$(printf '0x%02X%02X' "${maj}" "${min}")"
  rc=0
  esp="$(sed -n 's/^ *set(PROJECT_VER "\([^"]*\)").*/\1/p' "${REPO_ROOT}/targets/esp32-idf/CMakeLists.txt" | head -1)"
  [[ "${esp}" == "${d}" ]] || { echo "esp32-idf PROJECT_VER ${esp} != ${d}" >&2; rc=1; }
  nrf="$(sed -n 's/^ *OD_APP_VERSION=\(0x[0-9A-Fa-f]*\).*/\1/p' "${REPO_ROOT}/targets/nordic-zephyr/zephyr/CMakeLists.txt" | head -1)"
  [[ "${nrf^^}" == "${app_hex^^}" ]] || { echo "nordic OD_APP_VERSION ${nrf} != ${app_hex}" >&2; rc=1; }
  sil="$(sed -n 's/^set(OD_APP_VERSION "\(0x[0-9A-Fa-f]*\)".*/\1/p' "${REPO_ROOT}/targets/efr32bg22-slc/cmake_gcc/CMakeLists.txt" | head -1)"
  [[ "${sil^^}" == "${app_hex^^}" ]] || { echo "bg22 OD_APP_VERSION ${sil} != ${app_hex}" >&2; rc=1; }
  [[ "${rc}" == 0 ]] && echo "version defaults agree: ${d} (${app_hex})"
  exit "${rc}"
fi

v="${1:-$(default_version)}"
v="${v#v}"
validate "${v}"
echo "${v}"
