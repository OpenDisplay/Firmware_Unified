#!/usr/bin/env bash
# Join the per-board ESP32 build results into the one release directory a sequential
# targets/esp32-idf/build.sh run would have produced.
#
#     tools/release/esp32_join.sh <release-dir> '<json array of board names>'
#
# build.yml builds every board in its own job and each delivers its files plus
# MANIFEST-esp32-idf.<board>.part. This checks that EVERY listed board delivered a merged
# image (a board whose job silently produced nothing must not vanish from a release), then
# writes MANIFEST-esp32-idf.txt: the header of the first part and the board row of each.
set -euo pipefail

DIR="${1:?release directory}"
BOARDS_JSON="${2:?json array of boards}"

mapfile -t BOARDS < <(python3 -c 'import json,sys; print("\n".join(json.loads(sys.argv[1])))' "$BOARDS_JSON")
[ "${#BOARDS[@]}" -gt 0 ] || { echo "::error::empty board list" >&2; exit 1; }

missing=()
for b in "${BOARDS[@]}"; do
    [ -f "$DIR/opendisplay-$b-merged.bin" ] && [ -f "$DIR/MANIFEST-esp32-idf.$b.part" ] || missing+=("$b")
done
if [ "${#missing[@]}" -gt 0 ]; then
    echo "::error::boards without a delivered image: ${missing[*]}" >&2
    exit 1
fi

OUT="$DIR/MANIFEST-esp32-idf.txt"
first="$DIR/MANIFEST-esp32-idf.${BOARDS[0]}.part"
# Header = everything up to and including the column-title row.
sed -n '1,/^BOARD /p' "$first" > "$OUT"
for b in "${BOARDS[@]}"; do
    part="$DIR/MANIFEST-esp32-idf.$b.part"
    rows="$(sed '1,/^BOARD /d' "$part")"
    [ "$(printf '%s\n' "$rows" | grep -c .)" = 1 ] \
        || { echo "::error::$part does not hold exactly one board row" >&2; exit 1; }
    printf '%s\n' "$rows" >> "$OUT"
    rm -f "$part"
done

stray=("$DIR"/MANIFEST-esp32-idf.*.part)
[ ! -e "${stray[0]}" ] || { echo "::error::results for boards not in the list: ${stray[*]}" >&2; exit 1; }

cat "$OUT"
echo "joined ${#BOARDS[@]} boards"
