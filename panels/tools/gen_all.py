#!/usr/bin/env python3
"""Generate panels/generated/* from the bb_epaper reference streams and panels.yaml.

    python3 panels/tools/gen_all.py           regenerate
    python3 panels/tools/gen_all.py --check   exit 1 if a committed output is stale

Pipeline per legacy panel_ic_type (panels/panel_ids.json):
  1. capture: build bb_epaper (BBEP_REF_REV) against the recording backend and record the
     2.x full / fast / partial / sleep flows (tests/panels/harness/bbep_capture.cpp);
  2. derive: normalise each flow, turn it into od_epd steps, move oversize payloads into a
     LUT slot, encode 0xB1 / 0xAE / 0xAF (panels.yaml entries are encoded from the YAML
     instead and keep their hand-written sequences);
  3. replay: run the encoded packets through shared/core/od_epd.c and compare its bus
     stream with the bb_epaper one under the README's accepted differences;
  4. emit drive_presets.json, bbep_streams.json and coverage.md.
Needs a C/C++ compiler and the bb_epaper clone (OD_BBEP_UPSTREAM); PyYAML only for the
panels.yaml entries (falls back to the committed presets when it is missing).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import pdlib as P  # noqa: E402

OUT_PRESETS = P.GENERATED / "drive_presets.json"
OUT_STREAMS = P.GENERATED / "bbep_streams.json"
OUT_COVERAGE = P.GENERATED / "coverage.md"

FLOW_ORDER = ("full", "fast", "partial")
REFRESH = {"full": "full", "fast": "fast", "partial": "partial"}


def tok_json(t: tuple) -> list:
    out = []
    for v in t:
        if isinstance(v, bytes):
            out.append(v.hex())
        elif isinstance(v, tuple):
            out.append(list(v))
        else:
            out.append(v)
    return out


def strip_wake(toks: list[tuple]) -> list[tuple]:
    """begin_pass supplies the leading reset + wait; both sides drop it before comparing."""
    return toks[2:] if toks[:2] == [("RESET",), ("BUSY",)] else toks


def compare(ref: list[tuple], got: list[tuple]) -> dict:
    a = P.comparable(P.expand_cs_both(ref))
    b = P.comparable(got)
    if a == b:
        return {"equal": True}
    i = next((k for k in range(min(len(a), len(b))) if a[k] != b[k]), min(len(a), len(b)))
    return {"equal": False, "first_diff": i, "ref_len": len(a), "got_len": len(b),
            "ref": [tok_json(t) for t in a[i:i + 3]], "got": [tok_json(t) for t in b[i:i + 3]]}


def run_replay(blob: bytes, mode: str) -> tuple[int, list[tuple]]:
    rc, lines = P.replay(blob, mode)
    return rc, P.normalise(lines, "odepd")


def frame_all(pk: list[tuple[int, int, bytes]]) -> bytes:
    return b"".join(P.frame(n, pid, body) for n, pid, body in pk)


def packets_json(pk: list[tuple[int, int, bytes]]) -> list[dict]:
    return [{"id": pid, "number": n, "body_hex": body.hex()} for n, pid, body in pk]


def sizes(blob: bytes) -> tuple[int, dict]:
    n = len(blob)
    return n, {k: P.TYPICAL_BASE + n <= cap for k, cap in P.CAPS.items()}


# ----------------------------------------------------------------------------- derive

def needed_caps(raw: list[tuple]) -> dict:
    """Smallest OD_EPD_SEQ_MAX (8-byte steps) and step cap at which this flow would fit."""
    saved = P.MAX_STEPS
    try:
        P.MAX_STEPS = 255
        for lim in range(P.SEQ_MAX, 1025, 8):
            st, _, bl = P.fit_steps(raw, lim, before_image_only=True)
            if not bl:
                return {"seq_max": lim, "steps": P.count_steps(st)}
    finally:
        P.MAX_STEPS = saved
    return {"seq_max": None, "steps": None}


def derive(p: dict) -> dict:
    layout = P.frame_layout(p)
    scheme = P.native_scheme(p)
    modes, blocked_modes, slots = [], [], []
    req: set[str] = set()
    detail: dict[str, list[str]] = {}
    for flow in FLOW_ORDER:
        if flow not in p["flows"]:
            continue
        toks = strip_wake(P.normalise(p["flows"][flow], "bbep"))
        steps, r, bl = P.tokens_to_steps(toks, layout, flow)
        raw = steps
        steps, slot, bl2 = P.fit_steps(raw, P.SEQ_MAX, before_image_only=True)
        if bl2:
            alt, _, bl3 = P.fit_steps(raw, P.SEQ_MAX)
            if not bl3:
                steps, bl2 = alt, ["LUT_AFTER_IMAGE"]
        bl = sorted(set(bl + bl2))
        nimg = sum(1 for t in toks if t[0] == "IMAGE")
        if bl:
            bm = {"name": flow, "blocked_on": bl, "seq_bytes": len(P.encode_steps(steps)),
                  "steps": P.count_steps(steps)}
            if "SEQ_MAX" in bl:
                bm["fits_with"] = needed_caps(raw)
            blocked_modes.append(bm)
            detail[flow] = bl
            continue
        slot_idx = 0xFF
        if slot:
            if slot not in slots:
                slots.append(slot)
            slot_idx = slots.index(slot)
        req.update(r)
        modes.append({"flow": flow, "steps": steps, "slot": slot_idx, "planes": nimg,
                      "scheme": 0 if flow == "partial" else scheme, "ref": toks})
    sleep_toks = P.normalise(p["flows"]["sleep"], "bbep")
    sst, sr, sbl = P.tokens_to_steps(sleep_toks, layout, "sleep")
    sst, _, sbl2 = P.fit_steps(sst, P.SLEEP_MAX, allow_lut=False)
    sbl = sorted(set(sbl + sbl2))
    if sbl:
        detail["sleep"] = sbl
    req.update(sr)
    if len(slots) > P.MAX_LUT_SLOTS:
        detail["lut_slots"] = ["LUT_SLOTS"]
    return {"layout": layout, "scheme": scheme, "modes": modes, "modes_blocked": blocked_modes,
            "slots": slots, "sleep_steps": sst, "sleep_ref": sleep_toks, "sleep_blocked": sbl,
            "requires": sorted(req), "detail": detail}


def code_map_of(p: dict) -> list[int]:
    if p["flags"] & 0x0008:
        return list(p["lookup"][:4])
    return [0, 1, 2, 3]


def encode_derived(p: dict, d: dict) -> list[tuple[int, int, bytes]]:
    bp = P.busy_policy(p)
    sleep = P.encode_steps(d["sleep_steps"]) if not d["sleep_blocked"] else b""
    pk = [(0, P.PKT_PANEL, P.panel_body(busy_usable=bp["usable"], idle=bp["idle"],
                                         timeout=bp["timeout"], fixed=0, code_map=code_map_of(p),
                                         mode_count=len(d["modes"]), sleep=sleep,
                                         lut_supported=False))]
    for i, m in enumerate(d["modes"]):
        pk.append((i, P.PKT_MODE, P.mode_body(i, m["flow"], P.encode_steps(m["steps"]),
                                              passes=1, scheme=m["scheme"], slots=[m["slot"]])))
    for s, data in enumerate(d["slots"]):
        pk.append((s, P.PKT_LUT, P.lut_body(s, data)))
    return pk


# ----------------------------------------------------------------------------- glass

def glass_packets(entry: dict, website: dict | None) -> list[tuple[int, int, bytes]]:
    try:
        waves = {w: (P.PANELS / "waves" / f"{w}.bin").read_bytes() for w in ("wave_colour", "wave_edit")}
        pal = json.loads((P.PANELS / "palettes" / "palette16_photo_ODBEEBA6.json").read_text())
        return P.yaml_panel_packets(entry, waves, pal)
    except Exception:
        if website is None:
            raise
        return [(x["number"], x["id"], bytes.fromhex(x["body_hex"])) for x in website]


def load_yaml() -> dict[int, dict]:
    try:
        import yaml
    except ImportError:
        return {}
    doc = yaml.safe_load((P.PANELS / "panels.yaml").read_text())
    return {int(str(e["id"]), 0): e for e in doc["panels"]}


# ----------------------------------------------------------------------------- main

def build(prev: dict | None) -> tuple[dict, dict, str]:
    ids = json.loads((P.PANELS / "panel_ids.json").read_text())["ids"]
    P.build_replay()
    cap = P.capture(P.BBEP_REF_REV)
    vend = P.capture(P.BBEP_VENDORED_REV)
    yml = load_yaml()
    prev_entries = (prev or {}).get("panels", {})

    streams = {"bb_epaper": cap["bb_epaper"], "panels": {}}
    drift = []
    for name, p in cap["panels"].items():
        streams["panels"][name] = {
            "chip": p["chip"], "flags": P.flag_names(p["flags"]), "width": p["width"],
            "height": p["height"],
            "flows": {f: [tok_json(t) for t in P.normalise(ev, "bbep")] for f, ev in p["flows"].items()}}
        v = vend["panels"].get(name)
        if v is None:
            drift.append(f"{name}: absent in {P.BBEP_VENDORED_REV}")
        else:
            diff = [f for f in p["flows"] if P.normalise(p["flows"][f], "bbep") !=
                    P.normalise(v["flows"].get(f, []), "bbep")]
            if diff:
                drift.append(f"{name}: {', '.join(diff)} differ in {P.BBEP_VENDORED_REV}")

    out: dict[str, dict] = {}
    for key, idinfo in ids.items():
        num = idinfo["panel_ic_type"]
        enum = idinfo["bb_epaper_enum"]
        p = cap["panels"].get(enum)
        legacy_scheme = idinfo["preset_color_scheme"]
        e: dict = {"panel_ic_type": num, "name": enum, "aliases": [], "notes": [], "differences": []}
        if idinfo["alias_of"]:
            e["alias_of"] = idinfo["alias_of"]
        if p is None:
            e.update({"status": "not_attempted", "verified": "untested", "blocked_on": ["NO_BBEP_ENUM"],
                      "packets": [], "legacy": {"panel_ic_type": num, "color_scheme": legacy_scheme}})
            out[key] = e
            continue
        native = P.native_scheme(p)
        e.update({"width": p["width"], "height": p["height"], "controller": p["chip"],
                  "bbep_flags": P.flag_names(p["flags"])})
        if legacy_scheme is None:
            legacy_scheme = native
            e["notes"].append("no website preset; legacy color_scheme is the bb_epaper native scheme")
        elif legacy_scheme != native:
            e["notes"].append(f"website preset color_scheme {legacy_scheme} != bb_epaper native {native}; "
                              "legacy value kept for old clients")
        e["legacy"] = {"panel_ic_type": num, "color_scheme": legacy_scheme,
                       "pixel_width": idinfo["preset_pixel_width"],
                       "pixel_height": idinfo["preset_pixel_height"],
                       "presets": idinfo["presets"]}
        e["color_scheme"] = native
        if p["flags"] & 0x0400:
            e["differences"].append("D7")
        if p["flags"] & (0x2000 | 0x0200):
            e["notes"].append("bb_epaper expands pixels for this panel in its own write path; the 2.x "
                              "direct path streams the host bytes raw, so the host must send the "
                              "controller format")
        if enum.startswith("EP397"):
            e["notes"].append("2.x Firmware detects SSD1677 vs SSD2677 at runtime (reg 0x70) and repacks "
                              "to 2bpp for SSD2677; config-driven drive needs READBACK and PIXEL_XFORM")

        d = derive(p)
        pix = P.pixel_blockers(p)
        glass = num in yml
        if glass:
            ent = yml[num]
            web = (prev_entries.get(key) or {}).get("packets")
            pk = glass_packets(ent, web if web else None)
            e["verified"] = "glass"
            e["name"] = str(ent.get("name", enum))
            e["color_scheme"] = int((ent.get("modes") or [{}])[0].get("color_scheme", native))
            e["modes"] = [{"index": i, "name": m["name"], "refresh": "full", "passes": m.get("passes", 1),
                           "color_scheme": int(m.get("color_scheme", 0xFF))}
                          for i, m in enumerate(ent.get("modes") or [])]
            e["notes"].append("hand-written panels.yaml sequences, kept byte-identical to the website "
                              "presets; not derived from bb_epaper")
            if num == 0x003A:
                e["notes"].append("DOUBT: 16-colour palette is the 2.9in (ODBEEBA6) photo palette, not "
                                  "measured on this panel")
            e["notes"].append("DOUBT: 0xB1 code_map is carried but od_epd_cfg never applies it")
        else:
            pk = encode_derived(p, d)
            if p["flags"] & 0x0008:
                e["notes"].append(f"DOUBT: code_map {code_map_of(p)} is bb_epaper's pColorLookup; for EP42YR "
                                  "and EP75YR that lookup is the identity while the glass panels.yaml "
                                  "entries use [0,1,3,2], so the BWRY code order of this panel is unproven")
            e["modes"] = [{"index": i, "name": m["flow"], "refresh": REFRESH[m["flow"]], "passes": 1,
                           "color_scheme": m["scheme"], "planes": m["planes"]}
                          for i, m in enumerate(d["modes"])]
        if glass:
            # the bb_epaper-derived modes are not what this entry ships; keep them for reference only
            d["detail"], d["modes_blocked"] = {}, []
        e["modes_blocked"] = d["modes_blocked"]
        e["requires_escapes"] = d["requires"] if not glass else []
        blob = frame_all(pk)

        # equivalence
        eq: dict = {"modes": {}, "sleep": None}
        if glass:
            # compare the glass mode 0 with the bb_epaper full flow, for the record only
            rc, got = run_replay(blob, "0")
            ref = strip_wake(P.normalise(p["flows"]["full"], "bbep"))
            eq["modes"]["0"] = {"rc": rc, **compare(ref, strip_wake(got))}
            rc, got = run_replay(blob, "sleep")
            eq["sleep"] = {"rc": rc, **compare(d["sleep_ref"], got)}
        else:
            for i, m in enumerate(d["modes"]):
                rc, got = run_replay(blob, str(i))
                eq["modes"][str(i)] = {"rc": rc, **compare(m["ref"], strip_wake(got))}
            if not d["sleep_blocked"]:
                rc, got = run_replay(blob, "sleep")
                eq["sleep"] = {"rc": rc, **compare(d["sleep_ref"], got)}
        all_eq = all(v["rc"] == 0 and v["equal"] for v in eq["modes"].values()) and \
            (eq["sleep"] is None or (eq["sleep"]["rc"] == 0 and eq["sleep"]["equal"]))
        eq["all_equal"] = all_eq
        e["equivalence"] = eq

        blocked = sorted(set(pix + d["detail"].get("full", []) + d["detail"].get("sleep", []) +
                             d["detail"].get("lut_slots", [])))
        if "full" not in [m["flow"] for m in d["modes"]] and not glass and "full" not in d["detail"]:
            blocked.append("NO_FULL_FLOW")
        e["blocked_on"] = blocked
        e["blocked_detail"] = d["detail"]
        if glass:
            e["status"] = "glass"
        elif blocked:
            e["status"] = "blocked"
        elif all_eq:
            e["status"] = "equivalent"
        else:
            e["status"] = "differs"
        if not glass:
            e["verified"] = "equivalent" if e["status"] == "equivalent" else "untested"
        if blocked and not glass:
            pk, blob = [], b""
        e["differences"] = sorted(set(e["differences"] + ["D1", "D2", "D3", "D4", "D5"] +
                                      (["D6"] if p["flags"] & 0x0400 else [])))
        e["packets"] = packets_json(pk)
        e["framed_hex"] = blob.hex()
        e["drive_bytes"], e["fits"] = sizes(blob)
        out[key] = e

    # aliases: an alias id carries its target's drive data under its own legacy id
    for key, e in out.items():
        if e.get("alias_of"):
            tgt = out[e["alias_of"]]
            tgt["aliases"].append(key)
            keep = {"panel_ic_type": e["panel_ic_type"], "alias_of": e["alias_of"], "legacy": e["legacy"]}
            e.clear()
            e.update(json.loads(json.dumps(tgt)))
            e.update(keep)
            e["aliases"] = []
            e["notes"] = e["notes"] + [f"alias of {keep['alias_of']}: same bb_epaper panel, same drive data"]

    presets = {
        "schema": "opendisplay.drive_presets", "schema_version": 1, "schema_minor": 0,
        "sources": {"bb_epaper": cap["bb_epaper"], "bb_epaper_vendored": P.BBEP_VENDORED_REV,
                    "panel_ids": json.loads((P.PANELS / "panel_ids.json").read_text())["sources"],
                    "panels_yaml_sha256": P.sha256(P.PANELS / "panels.yaml")},
        "caps": {**P.CAPS, "typical_base_config_bytes": P.TYPICAL_BASE, "seq_max": P.SEQ_MAX,
                 "max_steps": P.MAX_STEPS, "sleep_max": P.SLEEP_MAX, "lut_max": P.LUT_MAX},
        "names": {e["name"]: k for k, e in out.items() if not e.get("alias_of")},
        "panels": out,
    }
    streams["drift_vs_vendored"] = drift
    return presets, streams, coverage_md(out, drift)


def coverage_md(out: dict, drift: list[str]) -> str:
    lines = ["# Drive-data coverage", "",
             "Generated by `panels/tools/gen_all.py`; do not edit.", "",
             "| id | name | ctrl | legacy scheme | status | verified | modes | blocked modes | bytes | BG22 | ESP32/Nordic | needs escapes | blocked on / why |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    counts: dict[str, int] = {}
    for k, e in out.items():
        counts[e["status"]] = counts.get(e["status"], 0) + 1
        why = ", ".join(e.get("blocked_on") or [])
        if e["status"] == "differs":
            bad = [m for m, v in e["equivalence"]["modes"].items() if not (v["rc"] == 0 and v["equal"])]
            s = e["equivalence"]["sleep"]
            if s and not (s["rc"] == 0 and s["equal"]):
                bad.append("sleep")
            why = "stream differs in " + ", ".join(bad)
        if e.get("alias_of"):
            why = (why + "; " if why else "") + f"alias of {e['alias_of']}"
        modes = ",".join(m["name"] for m in e.get("modes", []))
        mb = ",".join(f"{m['name']}({'/'.join(m['blocked_on'])})" for m in e.get("modes_blocked", []))
        fits = e.get("fits", {})
        lines.append(f"| {k} | {e['name']} | {e.get('controller', '-')} | {e['legacy']['color_scheme']} | "
                     f"{e['status']} | {e['verified']} | {modes} | {mb} | {e.get('drive_bytes', 0)} | "
                     f"{'y' if fits.get('bg22') else 'n'} | {'y' if fits.get('esp32') else 'n'} | "
                     f"{','.join(e.get('requires_escapes') or [])} | {why} |")
    lines += ["", "Totals: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())), "",
              f"## bb_epaper drift {P.BBEP_REF_REV} vs vendored {P.BBEP_VENDORED_REV}", ""]
    lines += [f"- {x}" for x in drift] or ["- none"]
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    prev = json.loads(OUT_PRESETS.read_text()) if OUT_PRESETS.exists() else None
    presets, streams, cov = build(prev)
    targets = [(OUT_PRESETS, presets), (OUT_STREAMS, streams)]
    if a.check:
        stale = [str(path) for path, obj in targets
                 if not path.exists() or json.loads(path.read_text()) != json.loads(json.dumps(obj))]
        if not OUT_COVERAGE.exists() or OUT_COVERAGE.read_text() != cov:
            stale.append(str(OUT_COVERAGE))
        for s in stale:
            print(f"stale: {s}")
        return 1 if stale else 0
    for path, obj in targets:
        P.dump_json(obj, path)
    OUT_COVERAGE.write_text(cov)
    print(cov.split("Totals: ")[1].splitlines()[0])
    return 0


if __name__ == "__main__":
    sys.exit(main())
