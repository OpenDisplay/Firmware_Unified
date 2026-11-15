#!/usr/bin/env python3
"""Turn build output into the release asset set that docs/RELEASE_ARTEFACTS.md defines.

    collect.py fragment --target <esp32-idf|nordic-zephyr|efr32bg22-slc> [DIR]
        Run in each build job after the target's build script. Normalises names in DIR
        (default <repo>/release), drops what is not a release asset (debug boards, ELF),
        generates the ESP32 install manifests, and writes DIR/fragment-<target>.json with the
        per-board metadata only the build job knows (offsets, chip).

    collect.py merge --version X.Y.Z --tag TAG --commit SHA [DIR]
        Run once over the union of the fragments' directories. Writes manifest.json and
        SHA256SUMS, deletes the fragments, and fails on anything the contract forbids.

    collect.py check [DIR]
        Validate an already merged directory against manifest.json.

Standard library only: this runs in three different CI containers.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import fnmatch
import hashlib
import json
import re
import sys
from pathlib import Path

SCHEMA = "opendisplay-release/1"
REPOSITORY = "OpenDisplay/Firmware_Unified"
REPO_ROOT = Path(__file__).resolve().parents[2]

# 2.x asset names that old updaters match exactly (HA BG22 updater, website ESP32/nRF flashers,
# nRF54 sync). A 3.x file under one of these would be pushed onto 2.x devices whose configs
# have no drive packets, leaving the panel blank. Never publish one.
LEGACY_NAME_PATTERNS = [
    "*_full.bin", "NRF52840.*", "nrf54l15-*", "nrf54lm20a-*", "opendisplay-bg22-v*",
    "bootloader-apploader-v*", "*app_update.bin", "EPD-nRF52-*", "esp32-*.bin",
]

RELEASE_LEVEL = {"manifest.json", "SHA256SUMS", "LICENSE", "NOTICE.md"}

# Website/2.x board stem for each 3.x board, so a sync can map one-to-one.
LEGACY_NAMES = {
    "s3-n16r8": ["esp32-s3-N16R8"],
    "s3-n16r8-extuart": ["esp32-s3-N16R8-extuart"],
    "s3-n32r8": ["esp32-s3-N32R8"],
    "s3-n32r8-extuart": ["esp32-s3-N32R8-extuart"],
    "s3-n8r8": ["esp32-s3-N8R8"],
    "s3-n4r2": ["esp32-s3-N4R2"],
    "c3-n4": ["esp32-c3-N4"],
    "c3-n16": ["esp32-c3-N16"],
    "c6-n4": ["esp32-c6-N4"],
    "esp32-n4": ["esp32-N4"],
    "esp32-wrover-e-n4r8": ["esp32-wrover-e-N4R8"],
    "xiao_nrf52840": ["NRF52840"],
    "xiao_nrf54l15": ["nrf54l15"],
    "xiao_nrf54lm20a": ["nrf54lm20a"],
    "bg22": ["opendisplay-bg22"],
}

ESP_FAMILY_TO_CHIP = {
    "ESP32": "esp32", "ESP32-S2": "esp32s2", "ESP32-S3": "esp32s3",
    "ESP32-C3": "esp32c3", "ESP32-C6": "esp32c6",
}

ESP_PART_TYPE = {
    "bootloader": "bootloader", "partition-table": "partition-table",
    "ota-data": "ota-data", "app": "app",
}

NORDIC_CHIP = {
    "xiao_nrf52840": "nrf52840", "xiao_nrf54l15": "nrf54l15", "xiao_nrf54lm20a": "nrf54lm20a",
}

# BG22 build output name -> contract name.
BG22_RENAMES = {
    "opendisplay-bg22.gbl": "opendisplay-bg22-ota.gbl",
    "opendisplay-bg22.s37": "opendisplay-bg22-app.s37",
    "opendisplay-bg22.hex": "opendisplay-bg22-app.hex",
    "bootloader-apploader.s37": "opendisplay-bg22-bootloader.s37",
}


class ContractError(Exception):
    pass


def _fail(msg: str) -> None:
    raise ContractError(msg)


def _legacy_hits(names) -> list[str]:
    return sorted(n for n in names
                  if any(fnmatch.fnmatchcase(n, p) for p in LEGACY_NAME_PATTERNS))


def _sha256(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


# shared/core/od_sec2_trace.h: every line of the TEST-ONLY SEC2 trace build carries this tag, and
# that build prints the claim code on the serial console. No released file may contain it.
TEST_TRACE_TAG = b"[SEC2-TEST]"


def _has_test_trace(p: Path) -> bool:
    tail = b""
    with p.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            if TEST_TRACE_TAG in tail + block:
                return True
            tail = block[-(len(TEST_TRACE_TAG) - 1):]
    return False


def _artefact(d: Path, name: str, board: str, typ: str, **extra) -> dict:
    a = {"file": name, "board": board, "type": typ, "offset": None}
    a.update(extra)
    a["size"] = (d / name).stat().st_size
    return a


def _drop(d: Path, name: str, why: str) -> None:
    (d / name).unlink()
    print(f"  drop {name} ({why})")


# ---------------------------------------------------------------- fragments ---

def fragment_esp32(d: Path) -> dict:
    boards, artefacts = [], []
    for name in sorted(p.name for p in d.iterdir()):
        if name.startswith("opendisplay-") and "-debug" in name:
            _drop(d, name, "debug board")
    for upd in sorted(d.glob("opendisplay-*-manifest.json")):
        if upd.name.endswith("-install-manifest.json"):
            continue
        board = upd.name[len("opendisplay-"):-len("-manifest.json")]
        m = json.loads(upd.read_text(encoding="utf-8"))
        build = m["builds"][0]
        family = build["chipFamily"]
        chip = ESP_FAMILY_TO_CHIP.get(family)
        if chip is None:
            _fail(f"{upd.name}: unknown chipFamily {family!r}")
        merged = f"opendisplay-{board}-merged.bin"
        if not (d / merged).is_file():
            _fail(f"{board}: update manifest without {merged}")

        install = {
            "name": m.get("name", "OpenDisplay Firmware"),
            "version": m.get("version"),
            "home_assistant_domain": m.get("home_assistant_domain", "open_display"),
            # A fresh install overwrites NVS anyway (the merged image pads it with 0xFF), so
            # erasing first costs nothing and removes stale data beyond the image.
            "new_install_prompt_erase": True,
            "new_install_improv_wait_time": 0,
            "builds": [{"chipFamily": family, "parts": [{"path": merged, "offset": 0}]}],
        }
        install_name = f"opendisplay-{board}-install-manifest.json"
        (d / install_name).write_text(json.dumps(install, indent=2) + "\n", encoding="utf-8")

        parts = []
        for part in build["parts"]:
            fname = part["path"]
            kind = fname[len(f"opendisplay-{board}-"):-len(".bin")]
            typ = ESP_PART_TYPE.get(kind)
            if typ is None or not (d / fname).is_file():
                _fail(f"{upd.name}: part {fname!r} missing or of unknown kind")
            parts.append(fname)
            artefacts.append(_artefact(d, fname, board, typ, offset=int(part["offset"])))
        app = f"opendisplay-{board}-app.bin"
        if app not in parts:
            _fail(f"{board}: update manifest has no app part")
        artefacts += [
            _artefact(d, merged, board, "merged", offset=0),
            _artefact(d, upd.name, board, "web-manifest-update"),
            _artefact(d, install_name, board, "web-manifest-install"),
        ]
        boards.append({
            "board": board, "target": "esp32-idf", "chip": chip,
            "legacy_names": LEGACY_NAMES.get(board, []),
            "flows": {
                "usb-install": {"files": [merged], "web_manifest": install_name,
                                "erases_config": True},
                "usb-update": {"files": parts, "web_manifest": upd.name,
                               "erases_config": False, "requires": "device runs 3.x"},
                "ble-ota": {"files": [app], "protocol": "mcumgr-smp",
                            "erases_config": False, "requires": "device runs 3.x"},
            },
        })
    if not boards:
        _fail("esp32-idf: no opendisplay-<board>-manifest.json in the release directory")
    return {"boards": boards, "artefacts": artefacts, "signing": {"esp32": "unsigned"}}


def _signing_from_nordic_manifest(d: Path) -> str:
    txt = d / "MANIFEST-nordic-zephyr.txt"
    if txt.is_file():
        for line in txt.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("SIGNING:"):
                return "project-key" if "project key" in line else "debug-key"
    return "debug-key"


def fragment_nordic(d: Path) -> dict:
    for name in sorted(p.name for p in d.iterdir()):
        if name.startswith("opendisplay-") and ("-debug" in name or "-adafruit" in name):
            _drop(d, name, "debug or opt-in Adafruit-only build, not a release asset")
        elif name.startswith("opendisplay-xiao_nrf52840") and name.endswith(".uf2"):
            _drop(d, name, "no UF2 for the MCUboot layout")
    boards, artefacts = [], []
    for board, chip in NORDIC_CHIP.items():
        hexf = f"opendisplay-{board}-merged.hex"
        if not (d / hexf).is_file():
            continue
        ota, dfu = f"opendisplay-{board}-ota.bin", f"opendisplay-{board}-dfu.zip"
        for f in (ota, dfu):
            if not (d / f).is_file():
                _fail(f"{board}: {hexf} without {f} -- every 3.x Nordic board is MCUboot")
        artefacts += [
            _artefact(d, hexf, board, "merged-hex"),
            _artefact(d, ota, board, "ota"),
            _artefact(d, dfu, board, "smp-dfu-zip"),
        ]
        flows = {
            "ble-ota": {"files": [ota], "protocol": "mcumgr-smp", "erases_config": False,
                        "requires": "device runs the 3.x MCUboot layout"},
        }
        if board == "xiao_nrf52840":
            s1 = "opendisplay-xiao_nrf52840-usb-1-sd-erase.zip"
            s2 = "opendisplay-xiao_nrf52840-usb-2-install.zip"
            for f in (s1, s2):
                if not (d / f).is_file():
                    _fail(f"{board}: missing {f} (tools/release/nrf52840_usb_packages.sh)")
            artefacts += [
                _artefact(d, s1, board, "nrfutil-dfu-zip", load_address=0x27000),
                _artefact(d, s2, board, "nrfutil-dfu-zip", load_address=0x1000),
            ]
            flows["usb-install"] = {
                "files": [s1, s2], "protocol": "adafruit-serial-dfu", "erases_config": True,
                "requires": "Adafruit nRF52 bootloader with serial DFU; step 1 only while "
                            "the s140 SoftDevice is present",
            }
            # The bootloader preserves only the top 40 kB below itself (0xEA000-0xF4000);
            # settings start one page lower, at 0xE9000. Whether the config survives a serial
            # DFU of step 2 has not been measured, so the manifest says unknown.
            flows["usb-update"] = {
                "files": [s2], "protocol": "adafruit-serial-dfu", "erases_config": None,
                "requires": "device already on the 3.x layout (SoftDevice erased); config "
                            "survival UNTESTED",
            }
        else:
            flows["usb-install"] = {"files": [hexf], "protocol": "swd", "erases_config": True}
            flows["ble-ota-from-2x"] = {
                "files": [ota], "protocol": "mcumgr-smp", "erases_config": False,
                "requires": "Firmware_NRF54 1.1.x (same partition map and MCUboot key); "
                            "UNTESTED on hardware",
            }
        boards.append({"board": board, "target": "nordic-zephyr", "chip": chip,
                       "legacy_names": LEGACY_NAMES.get(board, []), "flows": flows})
    if not boards:
        _fail("nordic-zephyr: no opendisplay-<board>-merged.hex in the release directory")
    return {"boards": boards, "artefacts": artefacts,
            "signing": {"mcuboot": _signing_from_nordic_manifest(d)}}


def fragment_bg22(d: Path) -> dict:
    for old, new in BG22_RENAMES.items():
        if (d / old).is_file():
            (d / old).replace(d / new)
            print(f"  rename {old} -> {new}")
    if (d / "opendisplay-bg22.out").is_file():
        _drop(d, "opendisplay-bg22.out", "ELF, debug only")
    gbl, s37, hexf = ("opendisplay-bg22-ota.gbl", "opendisplay-bg22-app.s37",
                      "opendisplay-bg22-app.hex")
    for f in (gbl, s37):
        if not (d / f).is_file():
            _fail(f"bg22: missing {f}")
    artefacts = [_artefact(d, gbl, "bg22", "gbl"), _artefact(d, s37, "bg22", "s37")]
    usb = [s37]
    if (d / hexf).is_file():
        artefacts.append(_artefact(d, hexf, "bg22", "hex"))
    bl = "opendisplay-bg22-bootloader.s37"
    if (d / bl).is_file():
        artefacts.append(_artefact(d, bl, "bg22", "bootloader-s37"))
        usb = [bl, s37]
    flows = {
        "usb-install": {"files": usb, "protocol": "swd", "erases_config": True,
                        "requires": "Gecko bootloader + AppLoader already on the part"
                                    if bl not in usb else "none (virgin part)"},
        "ble-ota": {"files": [gbl], "protocol": "silabs-apploader", "erases_config": False,
                    "requires": "device runs 3.x"},
        "ble-ota-from-2x": {
            "files": [gbl], "protocol": "silabs-apploader", "erases_config": False,
            "requires": "MANDATORY config migration (0xAE-0xB1 drive packets) written right "
                        "after the OTA; without it the panel stays blank",
        },
    }
    return {"boards": [{"board": "bg22", "target": "efr32bg22-slc", "chip": "efr32bg22",
                        "legacy_names": LEGACY_NAMES["bg22"], "flows": flows}],
            "artefacts": artefacts, "signing": {"bg22": "unsigned"}}


FRAGMENTS = {"esp32-idf": fragment_esp32, "nordic-zephyr": fragment_nordic,
             "efr32bg22-slc": fragment_bg22}


def cmd_fragment(target: str, d: Path) -> None:
    print(f"collect fragment {target} in {d}")
    frag = FRAGMENTS[target](d)
    frag["target"] = target
    hits = _legacy_hits(a["file"] for a in frag["artefacts"])
    if hits:
        _fail(f"2.x asset names in a 3.x release: {hits}")
    (d / f"fragment-{target}.json").write_text(json.dumps(frag, indent=2) + "\n",
                                               encoding="utf-8")
    print(f"  {len(frag['boards'])} board(s), {len(frag['artefacts'])} artefact(s)")


# -------------------------------------------------------------------- merge ---

def _validate(d: Path, manifest: dict) -> None:
    if manifest.get("schema") != SCHEMA:
        _fail(f"manifest schema is {manifest.get('schema')!r}, expected {SCHEMA!r}")
    if not re.fullmatch(r"\d+\.\d+\.\d+", manifest.get("version", "")):
        _fail(f"version {manifest.get('version')!r} is not X.Y.Z")
    on_disk = {p.name for p in d.iterdir() if p.is_file()}
    hits = _legacy_hits(on_disk)
    if hits:
        _fail(f"2.x asset names in a 3.x release: {hits}")
    listed = {a["file"] for a in manifest["artefacts"]}
    seen_boards = {b["board"] for b in manifest["boards"]}
    for a in manifest["artefacts"]:
        p = d / a["file"]
        if not p.is_file():
            _fail(f"manifest lists {a['file']} but it is not in {d}")
        if a["board"] not in seen_boards:
            _fail(f"{a['file']}: board {a['board']} has no boards[] entry")
        if p.stat().st_size != a["size"] or _sha256(p) != a["sha256"]:
            _fail(f"{a['file']}: size/sha256 differ from manifest.json")
        if not a["file"].startswith(f"opendisplay-{a['board']}-"):
            _fail(f"{a['file']}: name does not follow opendisplay-<board>-<kind>")
        if _has_test_trace(p):
            _fail(f"{a['file']}: a SEC2 TEST-TRACE build (prints the claim code) in a release")
    for b in manifest["boards"]:
        for flow, spec in b["flows"].items():
            for f in spec["files"] + ([spec["web_manifest"]] if "web_manifest" in spec else []):
                if f not in listed:
                    _fail(f"{b['board']} flow {flow}: {f} is not a listed artefact")
    stray = sorted(n for n in on_disk - listed - RELEASE_LEVEL
                   if not fnmatch.fnmatchcase(n, "MANIFEST-*.txt"))
    if stray:
        _fail(f"files not described by manifest.json: {stray}")


def cmd_merge(d: Path, version: str, tag: str, commit: str) -> None:
    v = version[1:] if version.startswith("v") else version
    frags = sorted(d.glob("fragment-*.json"))
    if not frags:
        _fail(f"no fragment-*.json in {d}")
    manifest = {
        "schema": SCHEMA, "repository": REPOSITORY, "version": v, "tag": tag,
        "commit": commit,
        "built_at": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "signing": {}, "boards": [], "artefacts": [],
    }
    for f in frags:
        frag = json.loads(f.read_text(encoding="utf-8"))
        manifest["signing"].update(frag["signing"])
        manifest["boards"] += frag["boards"]
        manifest["artefacts"] += frag["artefacts"]
        f.unlink()
    names = [a["file"] for a in manifest["artefacts"]]
    dupes = sorted({n for n in names if names.count(n) > 1})
    if dupes:
        _fail(f"artefact listed twice: {dupes}")
    for a in manifest["artefacts"]:
        a["sha256"] = _sha256(d / a["file"])
    _validate(d, manifest)
    (d / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    sums = [f"{_sha256(p)}  {p.name}" for p in sorted(d.iterdir())
            if p.is_file() and p.name != "SHA256SUMS"]
    (d / "SHA256SUMS").write_text("\n".join(sums) + "\n", encoding="utf-8")
    print(f"manifest.json: {len(manifest['boards'])} boards, "
          f"{len(manifest['artefacts'])} artefacts, signing {manifest['signing']}")


def cmd_check(d: Path) -> None:
    _validate(d, json.loads((d / "manifest.json").read_text(encoding="utf-8")))
    print(f"{d}: manifest.json consistent")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fragment")
    f.add_argument("--target", required=True, choices=sorted(FRAGMENTS))
    m = sub.add_parser("merge")
    m.add_argument("--version", required=True)
    m.add_argument("--tag", required=True)
    m.add_argument("--commit", required=True)
    c = sub.add_parser("check")
    for p in (f, m, c):
        p.add_argument("dir", nargs="?", default=str(REPO_ROOT / "release"))
    args = ap.parse_args(argv)
    d = Path(args.dir)
    try:
        if args.cmd == "fragment":
            cmd_fragment(args.target, d)
        elif args.cmd == "merge":
            cmd_merge(d, args.version, args.tag, args.commit)
        else:
            cmd_check(d)
    except ContractError as e:
        print(f"collect.py: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
