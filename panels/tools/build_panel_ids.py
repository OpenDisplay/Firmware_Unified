#!/usr/bin/env python3
"""Rebuild panels/panel_ids.json from the sibling repos (read-only, via `git show`).

    legacy panel_ic_type -> bb_epaper enum : Firmware origin/main src/display_service.cpp mapEpd()
    legacy colour scheme / geometry         : website presets (opendisplay.org_2 origin/main)
    BG22 2.x id set                          : Firmware_Silabs origin/main opendisplay_epd_map.c
    Nordic unified map                       : Firmware_Unified targets/nordic-zephyr/src/opendisplay_epd_map.c

Only needed when an id is added upstream; the committed JSON is what the generator reads.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import pdlib  # noqa: E402

WS = Path(os.environ.get("OD_WORKSPACE", str(pdlib.REPO.parent)))
if not (WS / "Firmware").exists():
    WS = pdlib.REPO.parents[1]


def show(repo: str, ref: str, path: str) -> str:
    return subprocess.run(["git", "-C", str(WS / repo), "show", f"{ref}:{path}"],
                          check=True, capture_output=True, text=True).stdout


def main() -> int:
    ds = show("Firmware", "origin/main", "src/display_service.cpp")
    body = ds[ds.index("int mapEpd(int id)"):]
    body = body[:body.index("default:")]
    fw = {}
    for m in re.finditer(r"case\s+(0x[0-9A-Fa-f]+|OD_PANEL_IC_\w+):\s*return\s+(\w+);", body):
        key, enum = m.group(1), m.group(2)
        if key.startswith("OD_PANEL_IC_"):
            num = {"OD_PANEL_IC_EP133A_SPECTRA_1200X1600": 0x42,
                   "OD_PANEL_IC_JD79668_420_BWRY": 1004}[key]
        else:
            num = int(key, 16)
        if enum != "EP_PANEL_UNDEFINED":
            fw[num] = enum

    presets = json.loads(show("opendisplay.org_2", "origin/main",
                              "httpdocs/firmware/toolbox/simple-config-presets.json"))
    by_ic: dict[int, list] = {}
    for d in presets["displays"]:
        by_ic.setdefault(int(str(d["panelIcType"]), 0), []).append(d)

    sil = show("Firmware_Silabs", "origin/main", "opendisplay_epd_map.c")
    sil_ids = {int(x, 16) for x in re.findall(r"case\s+(0x[0-9A-Fa-f]{4})\s*:", sil)}
    try:
        nrd = (pdlib.REPO / "targets/nordic-zephyr/src/opendisplay_epd_map.c").read_text()
        nordic = {int(a, 16): b for a, b in re.findall(r"case\s+(0x[0-9A-Fa-f]{4})\s*:\s*return\s+(\w+)", nrd)}
    except FileNotFoundError:
        nordic = {}

    out = {}
    for num in sorted(fw):
        enum = fw[num]
        alias_of = None
        if num >= 1000:
            alias_of = next(k for k, v in fw.items() if v == enum and k < 1000)
        ps = by_ic.get(num, [])
        cfg = ps[0]["config"] if ps else {}
        entry = {
            "panel_ic_type": num,
            "bb_epaper_enum": enum,
            "alias_of": f"0x{alias_of:04X}" if alias_of is not None else None,
            "presets": [p["id"] for p in ps],
            "preset_color_scheme": int(str(cfg["color_scheme"]), 0) if cfg else None,
            "preset_pixel_width": int(str(cfg["pixel_width"]), 0) if cfg else None,
            "preset_pixel_height": int(str(cfg["pixel_height"]), 0) if cfg else None,
            "preset_partial_update_support": int(str(cfg["partial_update_support"]), 0) if cfg else None,
            "in_bg22_2x_map": num in sil_ids,
            "unified_nordic_map": nordic.get(num),
        }
        out[f"0x{num:04X}"] = entry
    doc = {
        "about": "legacy panel_ic_type ids the 2.x firmware drives through bb_epaper; regenerate "
                 "with panels/tools/build_panel_ids.py",
        "sources": {
            "Firmware": subprocess.run(["git", "-C", str(WS / "Firmware"), "rev-parse", "--short",
                                        "origin/main"], capture_output=True, text=True).stdout.strip(),
            "opendisplay.org_2": subprocess.run(["git", "-C", str(WS / "opendisplay.org_2"), "rev-parse",
                                                 "--short", "origin/main"], capture_output=True,
                                                text=True).stdout.strip(),
            "Firmware_Silabs": subprocess.run(["git", "-C", str(WS / "Firmware_Silabs"), "rev-parse",
                                               "--short", "origin/main"], capture_output=True,
                                              text=True).stdout.strip(),
        },
        "ids": out,
    }
    pdlib.dump_json(doc, pdlib.PANELS / "panel_ids.json")
    print(f"{len(out)} ids")
    return 0


if __name__ == "__main__":
    sys.exit(main())
