#!/usr/bin/env python3
"""Turn an i1Pro fleet read (fleet_summary.csv + the swatch manifests) into a palettes/ file.

usage: import_i1pro_fleet.py <fleet_summary.csv> <manifest dir> <out.json>

Values are copied from the CSV, never recomputed. Per-unit facts that the CSV does not
carry (glazing, exclusions, cautions) are the tables below.
"""
import csv
import json
import sys
from pathlib import Path

PATCH = {"K": "black", "W": "white", "Y": "yellow", "R": "red"}

# Units read with no transparent front; every other unit was read through its plastic front.
BARE = {"OD507E22", "OD182A7A"}

USE = {
    "OD98430A": ("exclude", "chart rendered faded (contrast 3.8), confirmed on the glass: "
                            "a drive or panel fault, not representative"),
    "OD545743": ("caution", "consistent but weak: white about L* 50, contrast 14"),
    "OD0295D8": ("caution", "consistent but weak: white about L* 50, contrast 12"),
}


def main(csv_path, manifest_dir, out_path):
    units = {}
    with open(csv_path, newline="") as f:
        for r in csv.DictReader(f):
            u = units.setdefault(r["display"], {"rows": [], "row0": r})
            u["rows"].append(r)

    out_units = []
    for disp, u in units.items():
        m = json.loads((Path(manifest_dir) / f"{disp}.json").read_text())
        panel = m["panel"]
        fw = m.get("firmware")
        r0 = u["row0"]
        use, why = USE.get(disp, ("ok", None))
        entry = {
            "device": disp,
            "panel": r0["panel"],
            "panel_ic_type": panel.get("panel_ic_type"),
            "controller": panel.get("controller") or m.get("controller"),
            "pixels": panel.get("pixels") or [panel.get("pixel_width"), panel.get("pixel_height")],
            "firmware": fw.get("sha") if isinstance(fw, dict) else m.get("firmware_build"),
            "glazing": "bare" if disp in BARE else "plastic front",
            "use": use,
            "mcu_temp_c": float(r0["mcu_temp_c"]) if r0["mcu_temp_c"] else None,
            "refresh_end": r0["refresh_end"],
            "contrast": float(r0["contrast"]),
            "entries": [],
        }
        if why:
            entry["use_note"] = why
        # The code is the one the unit's own chart was drawn with: BWR tags use 2 for red,
        # BWRY units 2 for yellow and 3 for red. A missing name is an error, never a guess.
        codes = {c["name"]: c["code"] for c in m["palette"]}
        for r in sorted(u["rows"], key=lambda r: codes[PATCH[r["patch"]]]):
            name = PATCH[r["patch"]]
            code = codes[name]
            entry["entries"].append({
                "name": name,
                "code": code,
                "lab": [float(r["L"]), float(r["a"]), float(r["b"])],
                "lab_rel_white": [float(r["Lrel"]), float(r["arel"]), float(r["brel"])],
                "Y": float(r["Y"]),
                "reads": int(r["n"]),
                "rep_de00": float(r["rep_de00"]),
                "t_read": r["t_read"],
            })
        out_units.append(entry)

    doc = {
        "_comment": "i1Pro spot reads of stock swatch charts on 18 fleet displays, 2026-11-17. "
                    "Per-unit values: unit-to-unit spread is large (two 2.9in BWR units differ by "
                    "4.2 dE00 in red), so a type default taken from here must name its unit. "
                    "`code` is the palette code in that unit's swatch manifest (source.charts), so it differs "
                    "by colour scheme: red is 2 on BWR units and 3 on BWRY units. Match colours by `name`.",
        "date": "2026-11-17",
        "source": {
            "csv": "i1pro/20261117_fleet/fleet_summary.csv (analyse_fleet.py)",
            "raw": "i1pro/20261117_fleet/fleet.log (spectra 380-730 nm, 10 nm)",
            "charts": "i1pro/swatch_fleet_20261117/<device>.json",
        },
        "conditions": {
            "instrument": "X-Rite i1Pro S/N 324492, reflective, M0 (no UV filter), flat on the front",
            "colour_space": "CIELAB, D50 / 2 degree observer",
            "white_calibration": "tile at 17:10; tile drift 0.20 dE00 by 17:26",
            "drive": "stock full refresh (OTP waveform), no custom LUT",
            "read_delay_min": [4, 33],
            "reads_per_patch": 2,
            "repeatability_de00": {"median": 0.11, "max": 0.63},
            "panel_temperature": "not readable on any unit; mcu_temp_c is the MCU die temperature",
            "glazing": "measured as installed. The plastic front costs about 16-36 % of the light on "
                       "every patch with hue unchanged, so bare and glazed values are not interchangeable",
            "excluded_reads": ["OD5BCEB3 black, 17:11:46, off-centre"],
        },
        "units": out_units,
    }
    Path(out_path).write_text(json.dumps(doc, indent=1) + "\n")
    print(f"{len(out_units)} units, {sum(len(u['entries']) for u in out_units)} patches -> {out_path}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
