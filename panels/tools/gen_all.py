#!/usr/bin/env python3
"""Generate panels/generated/* from the bb_epaper reference streams and panels.yaml.

    python3 panels/tools/gen_all.py           regenerate
    python3 panels/tools/gen_all.py --check   exit 1 if a committed output is stale

Pipeline per legacy panel_ic_type (panels/panel_ids.json):
  1. capture: build bb_epaper (BBEP_REF_REV) against the recording backend and record the
     2.x full / fast / partial / sleep flows, plus the partial flow for three sub-rectangles
     (tests/panels/harness/bbep_capture.cpp);
  2. derive, once per target class (pdlib.CLASSES: the od_epd defaults, and BG22 without
     WINDOW/FILL): normalise each flow, turn it into od_epd steps, move oversize
     payloads into a LUT slot, give partial modes WINDOW/FILL where the class has them, encode
     0xB1 / 0xAE / 0xAF (panels.yaml entries are encoded from the YAML instead);
  3. replay: run the encoded packets through shared/core/od_epd.c built with that class's
     limits and compare its bus stream with the bb_epaper one (README accepted differences);
  4. emit drive_presets.json, bbep_streams.json and coverage.md.
The IT8951 / FastEPD parallel ids get no drive data: drive_presets.json `native_controllers`
carries their 0xB3 display_controller packet, encoded from panels.yaml.
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
SCHEMA_MINOR = 3
DIFFS_BASE = ["D1", "D2", "D3", "D4", "D5"]


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


def run_replay(blob: bytes, mode: str, cls: str = "default",
               geom: list[int] | None = None) -> tuple[int, list[tuple]]:
    rc, lines = P.replay(blob, mode, cls, geom)
    return rc, P.normalise(lines, "odepd")


def frame_all(pk: list[tuple[int, int, bytes]]) -> bytes:
    return b"".join(P.frame(n, pid, body) for n, pid, body in pk)


def packets_json(pk: list[tuple[int, int, bytes]]) -> list[dict]:
    return [{"id": pid, "number": n, "body_hex": body.hex()} for n, pid, body in pk]


def sizes(blob: bytes) -> tuple[int, dict]:
    n = len(blob)
    return n, {k: P.TYPICAL_BASE + n <= cap for k, cap in P.CAPS.items()}


def seq_stats(pk: list[tuple[int, int, bytes]]) -> list[dict]:
    """Per 0xAE: sequence bytes, steps, WINDOW/FILL use."""
    out = []
    for _, pid, b in pk:
        if pid != P.PKT_MODE:
            continue
        seq = b[37:]
        st = P.walk_seq(seq)
        out.append({"index": b[1], "seq_bytes": len(seq), "steps": P.count_steps(st),
                    "window": any(s[0] in ("window", "fill") for s in st)})
    return out


def fits_class(stat: dict, cls: str) -> bool:
    c = P.CLASSES[cls]
    return stat["seq_bytes"] <= c["seq_max"] and stat["steps"] <= c["max_steps"] and \
        (c["window"] or not stat["window"])


# ----------------------------------------------------------------------------- derive

def needed_caps(raw: list[tuple]) -> dict:
    """Smallest packing target (8-byte steps) and step count at which this flow would fit."""
    for lim in range(128, 2049, 8):
        st, _, bl = P.fit_steps(raw, lim, max_steps=255)
        if not bl:
            return {"seq_max": lim, "steps": P.count_steps(st)}
    return {"seq_max": None, "steps": None}


def partial_tokens(p: dict, window: bool) -> tuple[list[tuple], list[tuple[list[int] | None, list[tuple]]], list[str]]:
    """-> (tokens to derive from, [(rect or None, reference tokens)], notes)."""
    full = strip_wake(P.normalise(p["flows"]["partial"], "bbep"))
    if not window or not p.get("rects"):
        return full, [(None, full)], []
    runs = [(r["rect"], strip_wake(P.normalise(r["events"], "bbep")))
            for _, r in sorted(p["rects"].items())]
    wt, why = P.infer_window(runs, p["width"], p["height"])
    if why:
        return full, [(None, full)], [f"partial: WINDOW not derivable ({why}); whole-panel partial only"]
    return wt, runs, []


def derive(p: dict, cls: str) -> dict:
    c = P.CLASSES[cls]
    layout = P.frame_layout(p)
    scheme = P.native_scheme(p)
    modes, blocked_modes, slots, notes = [], [], [], []
    req: set[str] = set()
    detail: dict[str, list[str]] = {}
    for flow in FLOW_ORDER:
        if flow not in p["flows"]:
            continue
        if flow == "partial":
            toks, refs, n = partial_tokens(p, c["window"])
            notes += n
        else:
            toks = strip_wake(P.normalise(p["flows"][flow], "bbep"))
            refs = [(None, toks)]
        raw, r, bl = P.tokens_to_steps(toks, layout, flow)
        steps, slot, bl2 = P.fit_steps(raw, c["seq_max"], max_steps=c["max_steps"])
        bl = sorted(set(bl + bl2))
        nimg = sum(1 for t in toks if t[0] == "IMAGE")
        if bl:
            bm = {"name": flow, "blocked_on": bl, "seq_bytes": len(P.encode_steps(steps)),
                  "steps": P.count_steps(steps)}
            if {"SEQ_MAX", "MAX_STEPS"} & set(bl):
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
                      "scheme": 0 if flow == "partial" else scheme, "refs": refs,
                      "window": any(s[0] in ("window", "fill") for s in steps)})
    sleep_toks = P.normalise(p["flows"]["sleep"], "bbep")
    if p.get("bleed_ms"):
        sleep_toks = sleep_toks + [("DELAY", p["bleed_ms"])]
    sst, sr, sbl = P.tokens_to_steps(sleep_toks, layout, "sleep")
    sst, _, sbl2 = P.fit_steps(sst, P.SLEEP_MAX, allow_lut=False, max_steps=c["max_steps"])
    sbl = sorted(set(sbl + sbl2))
    if sbl:
        detail["sleep"] = sbl
    req.update(sr)
    if len(slots) > P.MAX_LUT_SLOTS:
        detail["lut_slots"] = ["LUT_SLOTS"]
    return {"layout": layout, "scheme": scheme, "modes": modes, "modes_blocked": blocked_modes,
            "slots": slots, "sleep_steps": sst, "sleep_ref": sleep_toks, "sleep_blocked": sbl,
            "requires": sorted(req), "detail": detail, "notes": notes}


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


def equivalence(p: dict, d: dict, blob: bytes, cls: str) -> dict:
    W, H = p["width"], p["height"]
    eq: dict = {"modes": {}, "sleep": None}
    for i, m in enumerate(d["modes"]):
        res = []
        for rect, ref in m["refs"]:
            geom = [W, H] + (rect or [])
            rc, got = run_replay(blob, str(i), cls, geom)
            res.append({"rc": rc, "rect": rect, **compare(ref, strip_wake(got))})
        bad = next((x for x in res if not (x["rc"] == 0 and x["equal"])), None)
        eq["modes"][str(i)] = bad or {"rc": 0, "equal": True,
                                      **({"rects": [x["rect"] for x in res]} if res[0]["rect"] else {})}
    if not d["sleep_blocked"]:
        rc, got = run_replay(blob, "sleep", cls, [W, H])
        eq["sleep"] = {"rc": rc, **compare(d["sleep_ref"], got)}
    eq["all_equal"] = all(v["rc"] == 0 and v["equal"] for v in eq["modes"].values()) and \
        (eq["sleep"] is None or (eq["sleep"]["rc"] == 0 and eq["sleep"]["equal"]))
    return eq


def class_view(p: dict, cls: str, glass_pk=None, glass_modes=None) -> dict:
    """Drive data for one target class. glass_pk: the panels.yaml packets (not re-derived)."""
    W, H = p["width"], p["height"]
    d = derive(p, cls)
    v: dict = {"limits": {k: P.CLASSES[cls][k] for k in ("seq_max", "max_steps", "window")}}
    if glass_pk is not None:
        pk = glass_pk
        modes = glass_modes
        runs_ok = all(P.replay(frame_all(pk), str(m["index"]), cls, [W, H])[0] == 0 for m in modes) and \
            P.replay(frame_all(pk), "sleep", cls, [W, H])[0] == 0
        v.update({"status": "glass" if runs_ok else "blocked", "verified": "glass" if runs_ok else "untested",
                  "blocked_on": [] if runs_ok else ["REPLAY_REFUSED"], "blocked_detail": {},
                  "modes_blocked": [], "requires_escapes": [], "notes": []})
    else:
        pk = encode_derived(p, d)
        blob = frame_all(pk)
        modes = [{"index": i, "name": m["flow"], "refresh": m["flow"], "passes": 1,
                  "color_scheme": m["scheme"], "planes": m["planes"], "window": m["window"]}
                 for i, m in enumerate(d["modes"])]
        blocked = sorted(set(P.pixel_blockers(p) + d["detail"].get("full", []) +
                             d["detail"].get("sleep", []) + d["detail"].get("lut_slots", [])))
        if not d["modes"] or d["modes"][0]["flow"] != "full":
            blocked = sorted(set(blocked + (["NO_FULL_FLOW"] if "full" not in d["detail"] else [])))
        eq = equivalence(p, d, blob, cls)
        if blocked:
            status = "blocked"
        elif eq["all_equal"]:
            status = "equivalent"
        else:
            status = "differs"
        v.update({"status": status,
                  "verified": "equivalent" if status == "equivalent" else "untested",
                  "blocked_on": blocked, "blocked_detail": d["detail"],
                  "modes_blocked": d["modes_blocked"], "requires_escapes": d["requires"],
                  "equivalence": eq, "notes": d["notes"]})
        if blocked:
            pk, modes = [], []
    blob = frame_all(pk)
    stats = {s["index"]: s for s in seq_stats(pk)}
    for m in modes:
        s = stats.get(m["index"])
        if s:
            m.update({"seq_bytes": s["seq_bytes"], "steps": s["steps"],
                      "fits": {k: fits_class(s, k) for k in P.CLASSES}})
    n, fits = sizes(blob)
    v.update({"modes": modes, "packets": packets_json(pk), "framed_hex": blob.hex(), "drive_bytes": n,
              "fits_config": P.TYPICAL_BASE + n <= P.CLASSES[cls]["config"], "fits": fits})
    return v


# ----------------------------------------------------------------------------- glass

def glass_packets(entry: dict, website: list | None) -> list[tuple[int, int, bytes]]:
    try:
        waves = {w: (P.PANELS / "waves" / f"{w}.bin").read_bytes() for w in ("wave_colour", "wave_edit")}
        pal = json.loads((P.PANELS / "palettes" / "palette16_photo_ODBEEBA6.json").read_text())
        return P.yaml_panel_packets(entry, waves, pal)
    except Exception:
        if website is None:
            raise
        return [(x["number"], x["id"], bytes.fromhex(x["body_hex"])) for x in website]


def load_yaml_doc() -> dict | None:
    try:
        import yaml
    except ImportError:
        return None
    return yaml.safe_load((P.PANELS / "panels.yaml").read_text())


def load_yaml() -> dict[int, dict]:
    doc = load_yaml_doc()
    return {int(str(e["id"]), 0): e for e in doc["panels"]} if doc else {}


def native_controllers(prev: dict | None) -> dict:
    """panels.yaml native_controllers -> one 0xB3 packet per legacy panel_ic_type."""
    doc = load_yaml_doc()
    if doc is None:
        return (prev or {}).get("native_controllers", {})
    out = {}
    for ent in doc.get("native_controllers") or []:
        num = int(str(ent["id"]), 0)
        body = P.controller_body(ent)
        out["0x%04X" % num] = {
            "panel_ic_type": num, "name": ent["name"], "board": ent["board"],
            "controller_class": ent["class"],
            "packets": [{"id": P.PKT_CONTROLLER, "number": 0, "body_hex": body.hex()}],
            "framed_hex": P.frame(0, P.PKT_CONTROLLER, body).hex(),
            "display_pins": ent.get("display_pins") or {},
        }
    return out


# ----------------------------------------------------------------------------- main

TOP_FROM_CLASS = ("status", "verified", "blocked_on", "blocked_detail", "modes", "modes_blocked",
                  "requires_escapes", "packets", "framed_hex", "drive_bytes", "fits", "equivalence")


def build(prev: dict | None) -> tuple[dict, dict, str]:
    ids = json.loads((P.PANELS / "panel_ids.json").read_text())["ids"]
    for cls in P.CLASSES:
        P.build_replay(cls)
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
            "flows": {f: [tok_json(t) for t in P.normalise(ev, "bbep")] for f, ev in p["flows"].items()},
            "rects": {k: {"rect": r["rect"], "tokens": [tok_json(t) for t in P.normalise(r["events"], "bbep")]}
                      for k, r in sorted((p.get("rects") or {}).items())}}
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
        if enum in P.POST_SLEEP_BLEED:
            p["bleed_ms"] = P.POST_SLEEP_BLEED[enum]
            e["notes"].append(f"sleep sequence ends with a {p['bleed_ms']} ms DELAY so the rail does not "
                              "drop before the boost caps and VCOM have bled (upstream Firmware d28582c)")
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
        if p["flags"] & (0x2000 | 0x0200):
            e["notes"].append("bb_epaper expands pixels for this panel in its own write path; the 2.x "
                              "direct path streams the host bytes raw, so the host must send the "
                              "controller format")
        if enum.startswith("EP397"):
            e["notes"].append("2.x Firmware detects SSD1677 vs SSD2677 at runtime (reg 0x70) and repacks "
                              "to 2bpp for SSD2677; config-driven drive needs READBACK and PIXEL_XFORM")

        glass = num in yml
        if glass:
            ent = yml[num]
            web = (prev_entries.get(key) or {}).get("packets")
            pk = glass_packets(ent, web if web else None)
            gmodes = [{"index": i, "name": m["name"], "refresh": "full", "passes": m.get("passes", 1),
                       "color_scheme": int(m.get("color_scheme", 0xFF))}
                      for i, m in enumerate(ent.get("modes") or [])]
            e["name"] = str(ent.get("name", enum))
            e["color_scheme"] = int((ent.get("modes") or [{}])[0].get("color_scheme", native))
            e["notes"].append("hand-written panels.yaml sequences, kept byte-identical to the website "
                              "presets; not derived from bb_epaper")
            if num == 0x003A:
                e["notes"].append("DOUBT: 16-colour palette is the 2.9in (ODBEEBA6) photo palette, not "
                                  "measured on this panel")
            e["notes"].append("DOUBT: 0xB1 code_map is carried but od_epd_cfg never applies it")
            views = {cls: class_view(p, cls, pk, json.loads(json.dumps(gmodes))) for cls in P.CLASSES}
            # bb_epaper comparison for the record only: the glass sequences are not derived from it
            rc, got = run_replay(frame_all(pk), "0", "default", [p["width"], p["height"]])
            ref = strip_wake(P.normalise(p["flows"]["full"], "bbep"))
            for v in views.values():
                v["equivalence"] = {"modes": {"0": {"rc": rc, **compare(ref, strip_wake(got))}},
                                    "sleep": None, "all_equal": False}
        else:
            views = {cls: class_view(p, cls) for cls in P.CLASSES}
            if p["flags"] & 0x0008:
                e["notes"].append(f"DOUBT: code_map {code_map_of(p)} is bb_epaper's pColorLookup; for EP42YR "
                                  "and EP75YR that lookup is the identity while the glass panels.yaml "
                                  "entries use [0,1,3,2], so the BWRY code order of this panel is unproven")
        top = views["default"]
        for k in TOP_FROM_CLASS:
            e[k] = top[k]
        for k in ("notes",):
            e[k] = e[k] + top[k]
        e["targets"] = {cls: {k: v for k, v in views[cls].items() if k != "equivalence" or cls != "default"}
                        for cls in P.CLASSES}
        e["fits_target"] = {cls: bool(top["packets"]) and top["status"] != "blocked" and
                            all(m.get("fits", {}).get(cls, False) for m in top["modes"]) and
                            P.TYPICAL_BASE + top["drive_bytes"] <= P.CLASSES[cls]["config"]
                            for cls in P.CLASSES}
        diffs = list(DIFFS_BASE)
        if p["flags"] & 0x0400:
            diffs += ["D6", "D7"]
        if any(m.get("window") for m in top["modes"]):
            diffs.append("D8")
        e["differences"] = sorted(set(diffs))
        out[key] = e

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

    bg22_out = {}
    for k, e in out.items():
        b = (e.get("targets") or {}).get("bg22")
        if b and b["status"] == "blocked":
            need = [m.get("fits_with") for m in b["modes_blocked"] if m["name"] == "full"]
            bg22_out[k] = {"name": e["name"], "blocked_on": b["blocked_on"],
                           "full_needs": need[0] if need and need[0] else None,
                           "single_init_alternative": False}

    presets = {
        "schema": "opendisplay.drive_presets", "schema_version": 1, "schema_minor": SCHEMA_MINOR,
        "sources": {"bb_epaper": cap["bb_epaper"], "bb_epaper_vendored": P.BBEP_VENDORED_REV,
                    "panel_ids": json.loads((P.PANELS / "panel_ids.json").read_text())["sources"],
                    "panels_yaml_sha256": P.sha256(P.PANELS / "panels.yaml")},
        "caps": {**P.CAPS, "typical_base_config_bytes": P.TYPICAL_BASE,
                 "seq_max": P.CLASSES["default"]["seq_max"], "max_steps": P.CLASSES["default"]["max_steps"],
                 "sleep_max": P.SLEEP_MAX, "lut_max": P.LUT_MAX},
        "target_classes": P.CLASSES,
        "bg22_unavailable": bg22_out,
        "names": {e["name"]: k for k, e in out.items() if not e.get("alias_of")},
        "panels": out,
        "native_controllers": native_controllers(prev),
    }
    streams["drift_vs_vendored"] = drift
    return presets, streams, coverage_md(out, drift, bg22_out)


def _why(e: dict, v: dict) -> str:
    if v["status"] == "differs":
        bad = [m for m, x in v["equivalence"]["modes"].items() if not (x["rc"] == 0 and x["equal"])]
        s = v["equivalence"]["sleep"]
        if s and not (s["rc"] == 0 and s["equal"]):
            bad.append("sleep")
        return "stream differs in " + ", ".join(bad)
    return ", ".join(v.get("blocked_on") or [])


def coverage_md(out: dict, drift: list[str], bg22_out: dict) -> str:
    lines = ["# Drive-data coverage", "",
             "Generated by `panels/tools/gen_all.py`; do not edit. `default` = od_epd defaults "
             f"({P.CLASSES['default']['max_steps']} steps, WINDOW/FILL; ESP32, Nordic); `bg22` = "
             f"{P.CLASSES['bg22']['max_steps']} steps, no WINDOW/FILL. Sequences are streamed from the "
             f"stored record, so neither class has a sequence-length cap; a sequence over "
             f"{P.CLASSES['default']['seq_max']} B has register payloads packed into a LUT slot.", "",
             "| id | name | ctrl | legacy scheme | default | default modes | bytes | bg22 | bg22 modes | bytes | escapes | why / notes |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    counts: dict[str, dict[str, int]] = {c: {} for c in P.CLASSES}
    for k, e in out.items():
        t = e.get("targets") or {}
        cells = []
        for cls in P.CLASSES:
            v = t.get(cls) or {"status": e["status"], "modes": [], "drive_bytes": 0}
            counts[cls][v["status"]] = counts[cls].get(v["status"], 0) + 1
            mb = ",".join(m["name"] for m in v.get("modes_blocked", []))
            modes = ",".join(m["name"] + ("+W" if m.get("window") else "") for m in v["modes"])
            if mb:
                modes += f" (-{mb})"
            cells += [v["status"], modes, str(v.get("drive_bytes", 0))]
        why = "; ".join(x for x in [_why(e, t["default"]) if t else "",
                                    ("bg22: " + _why(e, t["bg22"])) if t and t["bg22"]["status"] == "blocked" else "",
                                    f"alias of {e['alias_of']}" if e.get("alias_of") else ""] if x)
        lines.append(f"| {k} | {e['name']} | {e.get('controller', '-')} | {e['legacy']['color_scheme']} | "
                     + " | ".join(cells[:3]) + " | " + " | ".join(cells[3:]) +
                     f" | {','.join(e.get('requires_escapes') or [])} | {why} |")
    lines += [""]
    for cls in P.CLASSES:
        lines.append(f"Totals {cls}: " + ", ".join(f"{s} {n}" for s, n in sorted(counts[cls].items())))
    lines += ["", "`+W`: the mode uses WINDOW/FILL (any partial rectangle). `(-x)`: 2.x flow x not shipped "
              "for that class (see `modes_blocked`).", "",
              "## Unavailable on BG22", "",
              "| id | name | blocked on | full mode needs |",
              "|---|---|---|---|"]
    for k, b in bg22_out.items():
        need = b["full_needs"]
        need_s = f"{need['seq_max']} B / {need['steps']} steps" if need else "-"
        lines.append(f"| {k} | {b['name']} | {','.join(b['blocked_on'])} | {need_s} |")
    lines += ["", f"## bb_epaper drift {P.BBEP_REF_REV} vs vendored {P.BBEP_VENDORED_REV}", ""]
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
    for line in cov.splitlines():
        if line.startswith("Totals"):
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
