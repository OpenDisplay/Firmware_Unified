#!/usr/bin/env bash
# Exercise tools/release/collect.py on a synthetic release directory: one board per target,
# fragment -> merge -> check, plus the refusals the contract depends on (2.x asset names,
# an unlisted file, a tampered artefact). No toolchain needed; tools/check.sh runs it.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COLLECT="${HERE}/collect.py"
PY="${PYTHON:-python3}"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

fail() { echo "selftest FAIL: $*" >&2; exit 1; }
blob() { head -c "$2" /dev/urandom > "$1"; }

mk_esp32() {
  local d="$1" b=c3-n4
  for k in bootloader partition-table ota-data app merged; do blob "$d/opendisplay-$b-$k.bin" 64; done
  blob "$d/opendisplay-s3-n16r8-extuart-debug-merged.bin" 16
  cat > "$d/opendisplay-$b-manifest.json" <<EOF
{"name":"OpenDisplay Firmware","version":"3.0.0","home_assistant_domain":"open_display",
 "new_install_prompt_erase":false,"new_install_improv_wait_time":0,
 "builds":[{"chipFamily":"ESP32-C3","parts":[
  {"path":"opendisplay-$b-bootloader.bin","offset":0},
  {"path":"opendisplay-$b-partition-table.bin","offset":32768},
  {"path":"opendisplay-$b-ota-data.bin","offset":57344},
  {"path":"opendisplay-$b-app.bin","offset":65536}]}]}
EOF
  echo "esp32 manifest" > "$d/MANIFEST-esp32-idf.txt"
}

mk_nordic() {
  local d="$1"
  for b in xiao_nrf52840 xiao_nrf54l15; do
    blob "$d/opendisplay-$b-merged.hex" 64; blob "$d/opendisplay-$b-ota.bin" 64
    blob "$d/opendisplay-$b-dfu.zip" 64
  done
  blob "$d/opendisplay-xiao_nrf52840-usb-1-sd-erase.zip" 32
  blob "$d/opendisplay-xiao_nrf52840-usb-2-install.zip" 64
  blob "$d/opendisplay-xiao_nrf52840-adafruit.uf2" 16
  printf 'SIGNING: MCUboot DEBUG key (published; anyone can sign images)\n' \
    > "$d/MANIFEST-nordic-zephyr.txt"
}

mk_bg22() {
  local d="$1"
  for e in gbl s37 hex out; do blob "$d/opendisplay-bg22.$e" 64; done
  echo "bg22 manifest" > "$d/MANIFEST-efr32bg22-slc.txt"
}

# --- happy path: three jobs, then one merge ---------------------------------------------
for t in esp32 nordic bg22; do mkdir -p "${WORK}/$t"; "mk_$t" "${WORK}/$t"; done
"$PY" "$COLLECT" fragment --target esp32-idf "${WORK}/esp32" >/dev/null
"$PY" "$COLLECT" fragment --target nordic-zephyr "${WORK}/nordic" >/dev/null
"$PY" "$COLLECT" fragment --target efr32bg22-slc "${WORK}/bg22" >/dev/null
OUT="${WORK}/out"; mkdir -p "${OUT}"
cp "${WORK}"/{esp32,nordic,bg22}/* "${OUT}/"
"$PY" "$COLLECT" merge --version v3.0.0 --tag v3.0.0 --commit deadbeef "${OUT}" >/dev/null
"$PY" "$COLLECT" check "${OUT}" >/dev/null

[[ -f "${OUT}/opendisplay-bg22-ota.gbl" && ! -e "${OUT}/opendisplay-bg22.gbl" ]] \
  || fail "BG22 gbl not renamed to the contract name"
[[ ! -e "${OUT}/opendisplay-bg22.out" ]] || fail "ELF shipped"
[[ ! -e "${OUT}/opendisplay-s3-n16r8-extuart-debug-merged.bin" ]] || fail "debug board shipped"
[[ ! -e "${OUT}/opendisplay-xiao_nrf52840-adafruit.uf2" ]] || fail "Adafruit-only image shipped"
[[ -f "${OUT}/opendisplay-c3-n4-install-manifest.json" ]] || fail "no ESP32 install manifest"
(cd "${OUT}" && sha256sum -c --quiet SHA256SUMS) || fail "SHA256SUMS does not verify"
"$PY" - "${OUT}/manifest.json" <<'PY' || fail "manifest content"
import json, sys
m = json.load(open(sys.argv[1]))
assert m["version"] == "3.0.0" and m["tag"] == "v3.0.0", m["version"]
assert m["signing"] == {"esp32": "unsigned", "mcuboot": "debug-key", "bg22": "unsigned"}, m["signing"]
b = {x["board"]: x for x in m["boards"]}
assert set(b) == {"c3-n4", "xiao_nrf52840", "xiao_nrf54l15", "bg22"}, set(b)
assert b["c3-n4"]["flows"]["usb-update"]["erases_config"] is False
assert b["c3-n4"]["flows"]["usb-install"]["erases_config"] is True
assert b["xiao_nrf52840"]["flows"]["usb-install"]["files"][0].endswith("usb-1-sd-erase.zip")
assert "ble-ota-from-2x" in b["bg22"]["flows"]
a = {x["file"]: x for x in m["artefacts"]}
assert a["opendisplay-c3-n4-app.bin"]["offset"] == 65536
PY

# --- refusals ------------------------------------------------------------------------------
cp "${OUT}/opendisplay-bg22-ota.gbl" "${OUT}/opendisplay-bg22-v3.0.0.gbl"
if "$PY" "$COLLECT" check "${OUT}" >/dev/null 2>&1; then fail "2.x asset name accepted"; fi
rm "${OUT}/opendisplay-bg22-v3.0.0.gbl"

touch "${OUT}/stray.bin"
if "$PY" "$COLLECT" check "${OUT}" >/dev/null 2>&1; then fail "unlisted file accepted"; fi
rm "${OUT}/stray.bin"

cp "${OUT}/opendisplay-c3-n4-app.bin" "${WORK}/app.keep"
printf 'xx[SEC2-TEST] claim CODE' >> "${OUT}/opendisplay-c3-n4-app.bin"
"$PY" - "${OUT}" <<'PY' || fail "re-sum for the trace case"
import hashlib, json, pathlib, sys
d = pathlib.Path(sys.argv[1]); m = json.loads((d / "manifest.json").read_text())
for a in m["artefacts"]:
    if a["file"] == "opendisplay-c3-n4-app.bin":
        b = (d / a["file"]).read_bytes(); a["size"] = len(b); a["sha256"] = hashlib.sha256(b).hexdigest()
(d / "manifest.json").write_text(json.dumps(m, indent=2) + "\n")
PY
if "$PY" "$COLLECT" check "${OUT}" >/dev/null 2>&1; then fail "SEC2 test-trace image accepted"; fi
cp "${WORK}/app.keep" "${OUT}/opendisplay-c3-n4-app.bin"

printf 'x' >> "${OUT}/opendisplay-c3-n4-app.bin"
if "$PY" "$COLLECT" check "${OUT}" >/dev/null 2>&1; then fail "tampered artefact accepted"; fi

mkdir -p "${WORK}/n2"; mk_nordic "${WORK}/n2"; rm "${WORK}/n2/opendisplay-xiao_nrf52840-usb-1-sd-erase.zip"
if "$PY" "$COLLECT" fragment --target nordic-zephyr "${WORK}/n2" >/dev/null 2>&1; then
  fail "nRF52840 without its USB install packages accepted"
fi

echo "release collect selftest: OK"
