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
import hashlib
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import pdlib as P  # noqa: E402

OUT_PRESETS = P.GENERATED / "drive_presets.json"
OUT_STREAMS = P.GENERATED / "bbep_streams.json"
OUT_COVERAGE = P.GENERATED / "coverage.md"

FLOW_ORDER = ("full", "fast", "partial")
SCHEMA_MINOR = 5
GLASS_REPORTS = P.PANELS / "glass_reports.yaml"
REPORT_FIELDS = ("panel", "target_class", "framed_sha256", "modes", "board", "firmware", "date",
                 "reporter", "source")
DIFFS_BASE = ["D1", "D2", "D3", "D4", "D5"]

# A 4-gray bb_epaper enum is the same glass as a BW one driven another way, so it is a mode of the
# BW panel, not a panel: 4-gray key -> (BW key, {4-gray flow: mode name}). The 4-gray keys get no
# entry; a device configured with one is switched to the BW key by hand. 0x003C is dropped without
# a mode: its OTP 4-gray draws only K and W on GDEY075T7 glass, and 0x003B's 4-gray is the
# register-LUT one in refresh_modes.json (gen_refresh_modes.py).
GRAY_INTO = {
    "0x0008": ("0x0007", {"full": "gray4"}),
    "0x0015": ("0x0014", {"full": "gray4"}),
    "0x0016": ("0x0014", {}),
    "0x0018": ("0x0017", {"full": "gray4"}),
    "0x0028": ("0x0027", {"full": "gray4"}),
    "0x002F": ("0x002E", {"full": "gray4"}),
    "0x0031": ("0x0030", {"full": "gray4"}),
    "0x003C": ("0x003B", {}),
    "0x0043": ("0x001A", {"full": "gray4"}),
    "0x0044": ("0x0002", {"full": "gray4"}),
    "0x0046": ("0x0045", {"full": "gray4"}),
    "0x0048": ("0x0047", {"full": "gray4"}),
    "0x004C": ("0x004B", {"full": "gray4"}),
}
# The host maps gray level 0..3 (black..white) to panel codes 3, 1, 2, 0 (u8Colors_4gray) for
# every panel. A 4-gray enum with u8Colors_4gray_v2 (3, 2, 1, 0) takes its two planes the other
# way round, which swaps codes 1 and 2, so one host table serves every gray mode.
GRAY_LOOKUP_V2 = [3, 2, 1, 0]


def swap_gray_planes(toks: list[tuple]) -> list[tuple]:
    """Exchange the commands that load the two image planes (1 <-> 2 in the stored codes)."""
    at = [i - 1 for i, t in enumerate(toks) if t[0] == "IMAGE"]
    assert len(at) == 2 and all(toks[i][0] == "CMD" for i in at), at
    out = list(toks)
    a, b = at
    out[a] = (toks[a][0], toks[b][1]) + tuple(toks[a][2:])
    out[b] = (toks[b][0], toks[a][1]) + tuple(toks[b][2:])
    return out


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
        seq = b[37:37 + (b[35] | b[36] << 8)]
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


# 2.x Firmware keeps these panels powered and initialised from the full refresh into a partial
# (display_service.cpp panel_skips_reinit_on_partial_refresh), so bb_epaper's partial flow for
# them has no driver output control (0x01: MUX, interlaced gate scan) or booster setup. 3.0
# resets the panel before every update, so their partial starts with the full's init. Without
# it the EP426 partial lands its rows split around the window, with a grey band across the panel.
PARTIAL_REINIT = {"EP426_800x480", "EP397_800x480"}


def full_init_prefix(full: list) -> list:
    """The full flow's tokens before the command that takes its first image plane."""
    k = next(i for i, t in enumerate(full) if t[0] == "IMAGE")
    return list(full[:k - 1])


def partial_tokens(p: dict, window: bool) -> tuple[list[tuple], list[tuple[list[int] | None, list[tuple]]], list[str]]:
    """-> (tokens to derive from, [(rect or None, reference tokens)], notes)."""
    pre = full_init_prefix(strip_wake(P.normalise(p["flows"]["full"], "bbep"))) \
        if p.get("partial_reinit") else []
    full = pre + strip_wake(P.normalise(p["flows"]["partial"], "bbep"))
    if not window or not p.get("rects"):
        return full, [(None, full)], []
    runs = [(r["rect"], pre + strip_wake(P.normalise(r["events"], "bbep")))
            for _, r in sorted(p["rects"].items())]
    wt, why = P.infer_window(runs, p["width"], p["height"])
    if why:
        return full, [(None, full)], [f"partial: WINDOW not derivable ({why}); whole-panel partial only"]
    return wt, runs, []


def derive(p: dict, cls: str, skip: frozenset[str] = frozenset()) -> dict:
    c = P.CLASSES[cls]
    layout = P.frame_layout(p)
    scheme = P.native_scheme(p)
    modes, blocked_modes, slots, notes = [], [], [], []
    req: set[str] = set()
    detail: dict[str, list[str]] = {}
    for flow in FLOW_ORDER + tuple(p.get("gray_flows", {})):
        if flow not in p["flows"] and flow not in p.get("gray_flows", {}):
            continue
        if flow in skip:
            blocked_modes.append({"name": flow, "blocked_on": ["CONFIG_SIZE"]})
            continue
        gray = p.get("gray_flows", {}).get(flow)
        if gray is not None:
            toks = strip_wake(P.normalise(gray["events"], "bbep"))
            if gray["swap"]:
                toks = swap_gray_planes(toks)
            refs = [(None, toks)]
            raw, r, bl = P.tokens_to_steps(toks, gray["layout"], "full")
            steps, slot, bl2 = P.fit_steps(raw, c["seq_max"], max_steps=c["max_steps"])
            bl = sorted(set(bl + bl2))
            if slot and slot not in slots and len(slots) >= P.MAX_LUT_SLOTS:
                bl.append("LUT_SLOTS")
            if bl:
                blocked_modes.append({"name": flow, "blocked_on": bl})
                continue
            slot_idx = 0xFF
            if slot:
                if slot not in slots:
                    slots.append(slot)
                slot_idx = slots.index(slot)
            req.update(r)
            modes.append({"flow": flow, "steps": steps, "slot": slot_idx,
                          "planes": sum(1 for t in toks if t[0] == "IMAGE"), "scheme": gray["scheme"],
                          "refs": refs, "window": False,
                          "source": {"enum": gray["enum"], "flow": gray["flow"],
                                     "planes_swapped": gray["swap"]}})
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
                      "window": any(s[0] in ("window", "fill") for s in steps),
                      **({"init_from_full": True} if flow == "partial" and p.get("partial_reinit") else {})})
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


# BW panels: partials allowed between two full refreshes, as the 0xB1 tail's budget_limit with
# budget_cost 1 on the partial. Host information only; the device does not count.
BW_PARTIAL_BUDGET = 50
DRIVE_REV = 1


def panel_tail(key: int, budget: int) -> bytes:
    """0xB1 tail v1: lut_family 0 and busy_timeout_s 0, so the device reads nothing new from it."""
    return struct.pack("<BHHBBBB", 1, key, DRIVE_REV, 0, budget, 0, 0)


def encode_derived(p: dict, d: dict) -> list[tuple[int, int, bytes]]:
    bp = P.busy_policy(p)
    sleep = P.encode_steps(d["sleep_steps"]) if not d["sleep_blocked"] else b""
    budget = p.get("budget_key") is not None and any(m["flow"] == "partial" for m in d["modes"])
    pk = [(0, P.PKT_PANEL, P.panel_body(busy_usable=bp["usable"], idle=bp["idle"],
                                         timeout=bp["timeout"], fixed=0, code_map=code_map_of(p),
                                         mode_count=len(d["modes"]), sleep=sleep,
                                         lut_supported=False)
           + (panel_tail(p["budget_key"], BW_PARTIAL_BUDGET) if budget else b""))]
    for i, m in enumerate(d["modes"]):
        if "source" in m:
            # Gray modes are experimental: the device runs mode 0 outside the custom window.
            tmin, tmax = P.custom_window(0, 0)
            pk.append((i, P.PKT_MODE, P.mode_body(i, m["flow"], P.encode_steps(m["steps"]), passes=1,
                                                  scheme=m["scheme"], slots=[m["slot"]], tmin=tmin,
                                                  tmax=tmax, policy=P.POLICY_SUBSTITUTE)
                       + P.mode_tail(P.MODE_EXPERIMENTAL, 0)))
            continue
        pk.append((i, P.PKT_MODE, P.mode_body(i, m["flow"], P.encode_steps(m["steps"]),
                                              passes=1, scheme=m["scheme"], slots=[m["slot"]])
                   + (P.mode_tail(0, 1) if budget and m["flow"] == "partial" else b"")))
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
    # Gray modes ride on the BW panel's config: drop them, last first, until the class's cap fits.
    if glass_pk is None:
        gray = list(p.get("gray_flows", {}))
        skip: set[str] = set()
        while gray and P.TYPICAL_BASE + len(frame_all(encode_derived(p, d))) > P.CLASSES[cls]["config"]:
            skip.add(gray.pop())
            d = derive(p, cls, frozenset(skip))
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
                  "color_scheme": m["scheme"], "planes": m["planes"], "window": m["window"],
                  **({"source": m["source"], "experimental": True} if "source" in m else {}),
                  **({"init_from_full": True} if m.get("init_from_full") else {})}
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
                  "requires_escapes", "packets", "framed_hex", "drive_bytes", "fits", "equivalence",
                  "glass_reports")


def load_glass_reports() -> list[dict]:
    """panels/glass_reports.yaml: on-glass sightings of generated drive data."""
    import yaml
    doc = yaml.safe_load(GLASS_REPORTS.read_text()) or {}
    reports = doc.get("reports") or []
    for i, r in enumerate(reports):
        missing = [f for f in REPORT_FIELDS if f not in r]
        if missing:
            raise SystemExit(f"glass_reports.yaml report {i}: missing {', '.join(missing)}")
        r["date"] = str(r["date"])
        r["firmware"] = str(r["firmware"])
        if r["target_class"] not in P.CLASSES:
            raise SystemExit(f"glass_reports.yaml report {i}: unknown target_class {r['target_class']!r}")
        if not isinstance(r["modes"], list) or not r["modes"]:
            raise SystemExit(f"glass_reports.yaml report {i}: modes must be a non-empty list of mode names")
        if len(str(r["framed_sha256"])) != 64:
            raise SystemExit(f"glass_reports.yaml report {i}: framed_sha256 must be 64 hex digits")
    return reports


def apply_glass_reports(key: str, views: dict[str, dict], reports: list[dict]) -> list[dict]:
    """Mark the modes a matching report saw on glass. A report matches only while the class's
    framed bytes hash to its framed_sha256, so regenerated data is never inherited.
    Every view's modes gain `verified`; a view gains `glass_reports` (possibly empty), and its
    `verified` becomes glass once mode 0 has been seen. Returns the reports that no longer match."""
    stale = []
    for cls, v in views.items():
        base = "glass" if v["status"] == "glass" else ("equivalent" if v["status"] == "equivalent"
                                                       else "untested")
        sha = hashlib.sha256(bytes.fromhex(v["framed_hex"])).hexdigest()
        mine = [r for r in reports if r["panel"] == key and r["target_class"] == cls]
        names = {m["name"] for m in v["modes"]}
        applied = []
        for r in mine:
            unknown = sorted(set(r["modes"]) - names)
            if unknown:
                raise SystemExit(f"glass_reports.yaml: {key} {cls} has no mode {', '.join(unknown)}")
            if r["framed_sha256"] != sha:
                stale.append(r)
                continue
            applied.append({f: r[f] for f in REPORT_FIELDS if f not in ("panel", "target_class")}
                           | ({"note": r["note"]} if r.get("note") else {}))
        seen = {n for r in applied for n in r["modes"]}
        for m in v["modes"]:
            m["verified"] = "glass" if m["name"] in seen or base == "glass" else base
        v["glass_reports"] = applied
        if v["modes"] and v["modes"][0]["verified"] == "glass":
            v["verified"] = "glass"
    return stale


def build(prev: dict | None) -> tuple[dict, dict, str]:
    ids = json.loads((P.PANELS / "panel_ids.json").read_text())["ids"]
    for cls in P.CLASSES:
        P.build_replay(cls)
    cap = P.capture(P.BBEP_REF_REV)
    vend = P.capture(P.BBEP_VENDORED_REV)
    yml = load_yaml()
    reports = load_glass_reports()
    known = set(ids)
    for r in reports:
        if r["panel"] not in known or ids[r["panel"]]["alias_of"]:
            raise SystemExit(f"glass_reports.yaml: {r['panel']} is not a panel key (or is an alias)")
    stale_reports: list[dict] = []
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

    gray_notes: dict[str, list[str]] = {}
    for gkey, (bw, flows) in GRAY_INTO.items():
        g = cap["panels"][ids[gkey]["bb_epaper_enum"]]
        b = cap["panels"][ids[bw]["bb_epaper_enum"]]
        swap = list(g["lookup"][:4]) == GRAY_LOOKUP_V2
        for flow, name in flows.items():
            b.setdefault("gray_flows", {})[name] = {
                "events": g["flows"][flow], "layout": P.frame_layout(g), "scheme": P.native_scheme(g),
                "swap": swap, "enum": ids[gkey]["bb_epaper_enum"], "flow": flow}
        if flows:
            gray_notes.setdefault(bw, []).append(
                f"{', '.join(flows.values())}: the {', '.join(flows)} flow of {ids[gkey]['bb_epaper_enum']} "
                f"(formerly {gkey})" + (", image planes swapped (u8Colors_4gray_v2)" if swap else ""))

    out: dict[str, dict] = {}
    for key, idinfo in ids.items():
        if key in GRAY_INTO:
            continue
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
        if enum in PARTIAL_REINIT:
            p["partial_reinit"] = True
            e["notes"].append("partial starts with the full's init: 2.x kept this panel initialised from "
                              "the full into a partial, 3.0 resets it before every update")
        if enum in P.POST_SLEEP_BLEED:
            p["bleed_ms"] = P.POST_SLEEP_BLEED[enum]
            e["notes"].append(f"sleep sequence ends with a {p['bleed_ms']} ms DELAY so the rail does not "
                              "drop before the boost caps and VCOM have bled (upstream Firmware d28582c)")
        e.update({"width": p["width"], "height": p["height"], "controller": p["chip"],
                  "bbep_flags": P.flag_names(p["flags"])})
        e["notes"] += gray_notes.get(key, [])
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
                       "color_scheme": int(m.get("color_scheme", 0xFF)),
                       **({"experimental": True} if m["name"] in P.EXPERIMENTAL_YAML_MODES else {})}
                      for i, m in enumerate(ent.get("modes") or [])]
            e["name"] = str(ent.get("name", enum))
            e["color_scheme"] = int((ent.get("modes") or [{}])[0].get("color_scheme", native))
            e["notes"].append("hand-written panels.yaml sequences, kept byte-identical to the website "
                              "presets; not derived from bb_epaper")
            for m in ent.get("modes") or []:
                src = m.get("palette")
                if isinstance(src, dict):
                    e["notes"].append(f"0xB0 for {m['name']}: i1Pro stock colours of {src['unit']} "
                                      f"({src['fleet']}), a type default; measured bit 0")
                elif src:
                    e["notes"].append(f"0xB0 for {m['name']}: {src}, i1Pro on "
                                      f"{P.load_palette(src)['device']}, a type default; measured bit 0")
                elif m.get("passes", 1) > 1:
                    e["notes"].append(f"DOUBT: 0xB0 for {m['name']} is the 2.9in (ODBEEBA6) photo palette, "
                                      "not measured on this panel")
            e["notes"].append("DOUBT: 0xB1 code_map is carried but od_epd_cfg never applies it")
            views = {cls: class_view(p, cls, pk, json.loads(json.dumps(gmodes))) for cls in P.CLASSES}
            # bb_epaper comparison for the record only: the glass sequences are not derived from it
            rc, got = run_replay(frame_all(pk), "0", "default", [p["width"], p["height"]])
            ref = strip_wake(P.normalise(p["flows"]["full"], "bbep"))
            for v in views.values():
                v["equivalence"] = {"modes": {"0": {"rc": rc, **compare(ref, strip_wake(got))}},
                                    "sleep": None, "all_equal": False}
        else:
            if native == 0:
                p["budget_key"] = int(key, 16)
            views = {cls: class_view(p, cls) for cls in P.CLASSES}
            if native == 0 and any(m["name"] == "partial" for v in views.values() for m in v.get("modes", [])):
                e["notes"].append(f"at most {BW_PARTIAL_BUDGET} partials between full refreshes "
                                  "(0xB1 budget_limit, host-enforced)")
            if p["flags"] & 0x0008:
                e["notes"].append(f"DOUBT: code_map {code_map_of(p)} is bb_epaper's pColorLookup; for EP42YR "
                                  "and EP75YR that lookup is the identity while the glass panels.yaml "
                                  "entries use [0,1,3,2], so the BWRY code order of this panel is unproven")
        stale_reports += apply_glass_reports(key, views, reports)
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
            diffs += ["D6", "D7", "D9"]
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
        # Keys with no drive data any more: a device configured with one is moved to `use` by hand.
        "removed": {k: {"name": ids[k]["bb_epaper_enum"], "use": bw, "modes": sorted(fl.values())}
                    for k, (bw, fl) in GRAY_INTO.items()},
        "native_controllers": native_controllers(prev),
    }
    streams["drift_vs_vendored"] = drift
    return presets, streams, coverage_md(out, drift, bg22_out, stale_reports)


def _why(e: dict, v: dict) -> str:
    if v["status"] == "differs":
        bad = [m for m, x in v["equivalence"]["modes"].items() if not (x["rc"] == 0 and x["equal"])]
        s = v["equivalence"]["sleep"]
        if s and not (s["rc"] == 0 and s["equal"]):
            bad.append("sleep")
        return "stream differs in " + ", ".join(bad)
    return ", ".join(v.get("blocked_on") or [])


def coverage_md(out: dict, drift: list[str], bg22_out: dict, stale_reports: list[dict]) -> str:
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
            modes = ",".join(m["name"] + ("+W" if m.get("window") else "") +
                             ("+G" if m.get("verified") == "glass" and v["status"] != "glass" else "")
                             for m in v["modes"])
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
    lines += ["", "`+W`: the mode uses WINDOW/FILL (any partial rectangle). `+G`: these generated bytes "
              "were seen drawing correctly on glass (`panels/glass_reports.yaml`, mode `verified: glass`). "
              "`(-x)`: 2.x flow x not shipped for that class (see `modes_blocked`).", "",
              "## On-glass reports for generated entries", "",
              "| id | class | modes | board | firmware | date | reporter | source |",
              "|---|---|---|---|---|---|---|---|"]
    for k, e in out.items():
        if e.get("alias_of"):
            continue
        for cls, v in (e.get("targets") or {}).items():
            for r in v.get("glass_reports") or []:
                lines.append(f"| {k} | {cls} | {','.join(r['modes'])} | {r['board']} | {r['firmware']} | "
                             f"{r['date']} | {r['reporter']} | {r['source']} |")
    lines += ["", "Reports whose bytes changed since (not applied; a new sighting is needed):", ""]
    lines += [f"- {r['panel']} {r['target_class']} {','.join(r['modes'])}, {r['board']}, {r['date']}"
              for r in stale_reports] or ["- none"]
    lines += ["",
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
