#!/usr/bin/env bash
# Build targets/telink-tlsr into build/telink-tlsr/. Fetches the pinned SDK/toolchain first.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
OUT="${OUT:-${ROOT}/build/telink-tlsr}"

"${HERE}/tools/fetch_deps.sh"
cmake -S "${HERE}" -B "${OUT}" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="${HERE}/cmake/tc32-toolchain.cmake"
cmake --build "${OUT}"
echo "image: ${OUT}/opendisplay_tlsr.bin"
