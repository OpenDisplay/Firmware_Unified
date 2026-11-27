#!/usr/bin/env python3
"""Generate panels/generated/drive_presets_nrf52811.json from panels/nrf52811/presets.yaml.

    python3 panels/tools/gen_nrf52811.py           regenerate
    python3 panels/tools/gen_nrf52811.py --check   exit 1 if the committed output is stale

The nRF52811 (Firmware_NRF) presets are a separate family from the bb_epaper-derived ones in
drive_presets.json: they come from the 2.x M3 / EPD-nRF5 C drivers, carry provisional ids
(0xE0xx / 0xE1xx) and ship only once seen right on glass. presets.yaml holds each preset's
drive steps and metadata; provenance.json holds the frozen evidence from their derivation
(the deviations from the 2.x source and the packets of the uncorrected alternatives).

Per preset:
  1. encode 0xB1 (panel: BUSY usable, idle level, 0xFFFF ms timeout, code_map 0 1 2 3, sleep
     sequence), 0xAE mode 0 "full", and 0xB0 for mode 0 when the preset names a palette unit;
  2. replay the packets through shared/core/od_epd.c built with -DOD_EPD_WITH_WINDOW=0 (the
     nRF52811 build) and compare the bus stream with the steps, for the mode and for sleep;
  3. fits: the drive bytes plus 600 B of other config stay within the 2000 B config cap.
Needs a C compiler and PyYAML.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import pdlib as P  # noqa: E402

SRC = P.PANELS / "nrf52811" / "presets.yaml"
PROVENANCE = P.PANELS / "nrf52811" / "provenance.json"
OUT = P.GENERATED / "drive_presets_nrf52811.json"
GENERATOR = "Firmware_Unified panels/tools/gen_nrf52811.py"
REPLAY = P.BUILD / "od_epd_replay-nrf52811"
P_CAP = 2000                # Firmware_NRF OD_CONFIG_MAX_SIZE
OTHER_CONFIG = 600          # the rest of a typical nRF52811 config, for fits_target
BUSY_TIMEOUT = 0xFFFF       # 2.x waited UINT16_MAX ms for a refresh
PLANES = {0: 1, 1: 2, 2: 2, 3: 1}
# Panel code of each colour in a 0xB0 entry on these BWR / BWY presets: the 2-bit code with
# plane 0 (the B/W plane) as bit 0 and plane 1 (the colour plane) as bit 1, as in
# py-opendisplay's encode_gray4_bitplanes. The i1Pro swatch charts (2026-11-17) were sent with
# the colour setting BOTH planes, on BWR and BWY alike, so code 3 is what was measured. Matched
# by colour NAME; the swatch manifests' own "code" (2 for the colour) is the logical index.
PANEL_CODE = {"black": 0, "white": 1, "red": 3, "yellow": 3}
SCHEME_COLOURS = {0: ("black", "white"), 1: ("black", "white", "red"),
                  2: ("black", "white", "yellow")}


def build_replay() -> Path:
    P.BUILD.mkdir(parents=True, exist_ok=True)
    subprocess.run([P._cc("c"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-DOD_EPD_WITH_WINDOW=0",
                    f"-I{P.REPO / 'shared' / 'core'}", f"-I{P.REPO / 'shared' / 'protocol'}",
                    str(P.HARNESS / "od_epd_replay.c"), str(P.REPO / "shared" / "core" / "od_epd.c"),
                    str(P.REPO / "shared" / "core" / "od_epd_lut.c"), "-o", str(REPLAY)], check=True)
    return REPLAY


def replay(packets: bytes, mode: str, w: int, h: int) -> tuple[int, list[str]]:
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(packets)
        path = f.name
    try:
        r = subprocess.run([str(REPLAY), path, mode, str(w), str(h)], capture_output=True,
                           text=True, check=False)
    finally:
        os.unlink(path)
    return r.returncode, r.stdout.splitlines()


def tokens(lines: list[str]) -> list[tuple]:
    """Replay bus events -> (CMD c data) / (BUSY) / (DELAY ms) / (IMAGE) / (RESET ms) tokens."""
    out: list[tuple] = []
    for ln in lines:
        t = ln.split()
        k = t[0]
        if k == "I":
            out.append(("IMAGE",))
        elif k == "C":
            out.append(("CMD", int(t[1], 16), b""))
        elif k == "D":
            d = bytes.fromhex(t[1])
            if not out or out[-1][0] != "CMD":
                raise ValueError(f"data with no command: {ln}")
            c = out.pop()
            out.append(("CMD", c[1], c[2] + d))
        elif k == "B":
            out.append(("BUSY",))
        elif k == "W":
            out.append(("DELAY", int(t[1])))
        elif k == "T":
            out.append(("RESET", int(t[1])))
        elif k in ("M", "N", "S", "P"):
            continue
        else:
            raise ValueError(ln)
    return out


def steps_tokens(seq: bytes) -> list[tuple]:
    out = []
    for s in P.walk_seq(seq):
        if s[0] == "cmd":
            out.append(("CMD", s[1], bytes(s[2])))
        elif s[0] == "delay":
            out.append(("DELAY", s[1]))
        elif s[0] in ("busy", "image"):
            out.append((s[0].upper(),))
        else:
            raise ValueError(f"step {s[0]} is not used by the nRF52811 presets")
    return out


def check(framed: bytes, w: int, h: int, seq: bytes, sleep: bytes) -> dict:
    res = {}
    for key, s in (("0", seq), ("sleep", sleep)):
        rc, out = replay(framed, key, w, h)
        got = tokens(out)
        if key == "0" and got and got[0][0] == "RESET":
            got = got[1:]                       # od_epd_begin_pass resets and waits itself
            if got and got[0] == ("BUSY",):
                got = got[1:]
        want = steps_tokens(s)
        res[key] = {"rc": rc, "equal": rc == 0 and got == want}
        if not res[key]["equal"]:
            res[key]["first_diff"] = next((i for i, (a, b) in enumerate(zip(got, want)) if a != b),
                                          min(len(got), len(want)))
    res["all_equal"] = all(v["equal"] for v in res.values())
    return res


def palette_packet(p: dict) -> tuple[bytes, str] | None:
    src = p.get("palette")
    if not src:
        return None
    fleet = P.load_palette(src["fleet"])
    u = next(x for x in fleet["units"] if x["device"] == src["unit"])
    want = SCHEME_COLOURS[p["color_scheme"]]
    rows = {e["name"]: e for e in u["entries"]}
    if set(rows) != set(want):
        raise SystemExit(f"{p['key']}: {u['device']} has {sorted(rows)}, scheme {p['color_scheme']} "
                         f"wants {list(want)}")
    if u.get("use") == "exclude":
        raise SystemExit(f"{p['key']}: {u['device']} is use=exclude in {src['fleet']}")
    body = P.cal_body(0, [((PANEL_CODE[n],), tuple(rows[n]["lab_rel_white"])) for n in want],
                      measured=False)
    note = (f"0xB0 for full: i1Pro stock colours of {u['device']} ({src['fleet']}), a type default; "
            f"measured bit 0; {u['glazing']}, use={u['use']}"
            + (f" ({u['use_note']})" if u.get("use_note") else ""))
    return body, note


def entry(p: dict, prov: dict) -> dict:
    key = int(p["key"], 16)
    scheme = p["color_scheme"]
    seq = P.encode_yaml_steps(p["init"])
    sleep = P.encode_yaml_steps(p["sleep"]) if p["sleep"] else b""
    if len(sleep) > P.SLEEP_MAX:
        raise SystemExit(f"{p['key']}: sleep sequence {len(sleep)} B > {P.SLEEP_MAX}")
    nsteps = len(P.walk_seq(seq)) + 1
    if nsteps > P.MAX_STEPS:
        raise SystemExit(f"{p['key']}: {nsteps} steps > {P.MAX_STEPS}")
    if sum(1 for s in p["init"] if s.strip() == "@image") != PLANES[scheme]:
        raise SystemExit(f"{p['key']}: image markers do not match {PLANES[scheme]} planes")
    pkts = [(0, P.PKT_PANEL, P.panel_body(busy_usable=True, idle=p["busy_idle_level"],
                                          timeout=BUSY_TIMEOUT, fixed=0, code_map=[0, 1, 2, 3],
                                          mode_count=1, sleep=sleep, lut_supported=False)),
            (0, P.PKT_MODE, P.mode_body(0, "full", seq, scheme=scheme))]
    pal = palette_packet(p)
    if pal:
        if pal[1] not in p["notes"]:
            raise SystemExit(f"{p['key']}: notes lack the palette note {pal[1]!r}")
        pkts.append((P.PKT_CAL, P.PKT_CAL, pal[0]))
    framed = b"".join(P.frame(n, i, b) for n, i, b in pkts)
    eq = check(framed, p["width"], p["height"], seq, sleep)
    if not eq["all_equal"]:
        raise SystemExit(f"{p['key']}: replay differs from the steps: {eq}")
    e = {
        "panel_ic_type": key, "name": p["name"], "aliases": p["aliases"], "notes": p["notes"],
        "source": p["source"], "width": p["width"], "height": p["height"], "legacy": p["legacy"],
        "color_scheme": scheme, "status": p["status"], "verified": p["verified"], "blocked_on": [],
        "modes": [{"index": 0, "name": "full", "refresh": "full", "passes": 1,
                   "color_scheme": scheme, "planes": PLANES[scheme], "window": False,
                   "seq_bytes": len(seq), "steps": nsteps, "fits": {"nrf52811": True}}],
        "modes_blocked": [], "requires_escapes": [],
        "packets": [{"id": i, "number": n, "body_hex": b.hex()} for n, i, b in pkts],
        "framed_hex": framed.hex(), "drive_bytes": len(framed), "equivalence": eq,
        "fits_target": {"nrf52811": len(framed) + OTHER_CONFIG <= P_CAP},
    }
    e.update(prov.get(p["key"], {}))
    return e



def build() -> str:
    src = yaml.safe_load(SRC.read_text())
    prov = json.loads(PROVENANCE.read_text())
    meta = src["meta"]
    if meta["caps"]["nrf52811"] != P_CAP:
        raise SystemExit("meta.caps.nrf52811 and P_CAP disagree")
    build_replay()
    panels = {p["key"]: entry(p, prov) for p in sorted(src["presets"], key=lambda q: q["key"])}
    out = {
        "schema": meta["schema"], "schema_version": meta["schema_version"],
        "schema_minor": meta["schema_minor"], "generator": GENERATOR,
        "provisional_ids": meta["provisional_ids"], "caps": meta["caps"],
        "target_classes": meta["target_classes"], "transforms": meta["transforms"],
        "statuses": meta["statuses"],
        "names": {e["name"]: k for k, e in panels.items()},
        "panels": panels, "native_controllers": {},
    }
    return json.dumps(out, indent=1) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    text = build()
    if a.check:
        if not OUT.exists() or OUT.read_text() != text:
            print(f"{OUT.relative_to(P.REPO)} is stale: run panels/tools/gen_nrf52811.py")
            return 1
        print(f"{OUT.relative_to(P.REPO)} is current")
        return 0
    OUT.write_text(text)
    d = json.loads(text)
    for k, e in d["panels"].items():
        print(f"{k} {e['name'][:24]:24} {e['width']}x{e['height']} s{e['color_scheme']} "
              f"{e['status']:6} bytes={e['drive_bytes']:4} "
              f"{'0xB0 ' + next(n.split('colours of ')[1].split()[0] for n in e['notes'] if n.startswith('0xB0')) if any(p['id'] == 0xB0 for p in e['packets']) else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
