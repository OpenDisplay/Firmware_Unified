"""Offline charge calculator for Spectra 6 waveform tables (0x20 image, 800 B).

Electrical map (measured on one 7.3" and one 7.09" panel; see PANELS for the per-panel constants):
  level -> rail   S = VSL  W = VSL_LV  SW = VSL_LV2  K = VSH  SK = VSH_LV  WK = VSH_LV2
                  "" (level 0) = driven to ~0 V;  SWK = source floats (holds the previous level) -> counted 0, flagged.
  weak rails      custom mode (register table, LUT_EN = 1): PWR (0x01) data bytes 2..5, plain binary, in the order
                  VSH_LV / VSL_LV / VSH_LV2 / VSL_LV2.
                  stock mode (built-in table): 0x20 header bytes 1..4 (scrambled codec value) in the same order.
                  |V| = 3.0 + 0.1 code (VSL_LV2: 3.04 + 0.0992 code), sign of the parent rail.
  VCOM            DC during the refresh: custom = step x VDCS (0x82); stock = step x header byte 5 on the 7.3",
                  the VDCS register on the 7.09". LUTC lo -> DC + swing, hi -> DC - swing, both = counted as DC, flagged.
  border          CDI (0x50) bits 7-5 (VBD) = slot that drives the border (0x3F/0x37 -> slot 1, 0x1F -> slot 0, 0xFF float).
  frame time      PLL (0x30) bit 3 clear: fixed rate code = PLL & 7. Bit 3 set ("dynamic"): per LUTC row rate code;
                  frames past the LUTC end = code 0.

Charges per slot (= per pixel colour code), per refresh:
  Q_src   = sum over frames of V_source              [V x frames] and [V x s]   (relative to 0 V)
  Q_vcom  = sum of (V_source - V_com(t))             [V x frames] and [V x s]   (relative to VCOM)
            incl. the LUTC swing and the level-0 frames padded from the slot end to the refresh end.
  q_sf    = Q_src / V(S)  -> "S-frame" units (S = +1, K = -1); 1 S-frame = one frame at that panel's VSL.
Open question: which reference is DC-neutral for the ink (0 V or VCOM). The gate uses 0 V by default.

Import: charge(), gate(), Ledger, legacy_q(), volts_from_weights().  CLI: see presets/tools/lutlint.py.
Standard library only (Ledger's per-pixel map needs numpy and imports it on first use).
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field
from pathlib import Path

from . import s6_codec as C
from . import s6_lutc

LEVELS = ("", "S", "W", "SW", "K", "SK", "WK", "SWK")
RAIL_OF = {"S": "VSL", "W": "VSL_LV", "SW": "VSL_LV2", "K": "VSH", "SK": "VSH_LV", "WK": "VSH_LV2", "": "0V", "SWK": "float"}
WEAK_ORDER = ("VSH_LV", "VSL_LV", "VSH_LV2", "VSL_LV2")      # header bytes 1..4 / PWR bytes 2..5
VCOM_STEP = -0.0496                                          # V per header-byte-5 code (stock VCOM on the 7.3")
NAN = float("nan")


@dataclass
class Panel:
    name: str
    vsh: float = 14.98
    vsl: float = -14.98
    fixed_ms: dict = field(default_factory=dict)          # rate code -> ms at a fixed PLL (bit 3 clear)
    dyn_ms: dict = field(default_factory=dict)            # rate code -> ms at a dynamic PLL (bit 3 set)
    pwr: str = ""                                         # PWR data as written by the firmware init
    cdi: int = 0x3F
    vdcs: int | None = None                               # None = never written by the firmware
    hold_codes: tuple = ()
    assumed: str = ""                                     # anything NOT measured on this panel ("" = all measured)
    vcom_step: float = -0.0496                            # V per VDCS code (custom-mode VCOM DC during the refresh)
    stock_vcom: str = "header"                            # stock-mode VCOM DC: "header" (byte 5) or "vdcs" (register)
    lutc_swing: float = 14.8                              # LUTC lo/hi VCOM swing in V (0 = the bits are inert)
    vcom_rest: float = -0.8                               # VCOM after the refresh (keeper / float point)
    source: str = ""


# Every constant is a measurement on ONE unit of that panel type. Unit-to-unit spread is not characterised.
PANELS = {
    "7.3": Panel("7.3", fixed_ms={0: 36.82, 1: 31.45, 2: 23.46, 3: 20.18, 4: 15.55, 5: 13.45, 6: 11.88, 7: 10.12},
                 dyn_ms={0: 40.38, 1: 31.44, 2: 23.49, 3: 20.21, 4: 15.55, 5: 13.44, 6: 11.89, 7: 10.12},
                 pwr="3F 00 32 2A 0E 2A", cdi=0x3F, vdcs=0x1E,
                 source="measured on one 7.3in Spectra 6 panel (init: PWR 3F 00 32 2A 0E 2A, VDCS 0x1E, CDI 0x3F)"),
    # 7.09" dual-controller panel: rails VSH +14.82 / VSL -14.69 V; the main chip owns PWR; VCOM DC = register VDCS in
    # stock AND custom mode (default 0x18 -> -1.18 V, -49.3 mV/code); the LUTC lo/hi VCOM bits are inert;
    # frame = refresh / 40 with no fixed overhead; codes 4 and 7 are hardwired hold.
    "7.09": Panel("7.09", vsh=14.82, vsl=-14.69,
                  fixed_ms={1: 40.8, 2: 20.4, 3: 15.95, 4: 13.6, 5: 11.98, 6: 10.17, 7: 8.49},
                  dyn_ms={0: 81.1, 1: 40.55, 2: 20.35, 3: 15.68, 4: 13.58, 5: 12.0, 6: 10.2, 7: 8.53},
                  pwr="0F 00 28 2C 28 38", cdi=0x37, vdcs=0x18, hold_codes=(4, 7),
                  vcom_step=-0.0493, stock_vcom="vdcs", lutc_swing=0.0, vcom_rest=-0.9,
                  source="measured on one 7.09in dual-controller Spectra 6 panel (VDCS default 0x18 = -1.18 V; "
                         "after the refresh VCOM floats)"),
}


def _bytes(x) -> bytes:
    if isinstance(x, (bytes, bytearray)):
        return bytes(x)
    return bytes.fromhex(str(x).replace(" ", "").replace(",", ""))


def load_table(spec: str) -> bytes:
    """a file path (first 800 B) or a hex string."""
    p = Path(spec)
    try:
        if p.is_file():
            return p.read_bytes()[:C.TABLE_LEN]
    except OSError:
        pass
    return _bytes(spec)


def weak_codes(table: bytes, pwr=None, mode: str = "custom") -> dict:
    """weak-rail codes {VSH_LV, VSL_LV, VSH_LV2, VSL_LV2} from PWR (custom) or the 0x20 header (stock)."""
    if mode == "custom":
        p = _bytes(pwr)
        if len(p) == 7 and p[0] == 0x01:          # "01 3F 00 ..." = command byte included
            p = p[1:]
        return dict(zip(WEAK_ORDER, p[2:6]))
    return dict(zip(WEAK_ORDER, (C.val(b) for b in table[1:5])))


def rail_volts(codes: dict, vsh: float = 14.98, vsl: float = -14.98) -> dict:
    return {"VSH": vsh, "VSL": vsl,
            "VSH_LV": 3.0 + 0.1 * codes["VSH_LV"], "VSH_LV2": 3.0 + 0.1 * codes["VSH_LV2"],
            "VSL_LV": -(3.0 + 0.1 * codes["VSL_LV"]), "VSL_LV2": -(3.04 + 0.0992 * codes["VSL_LV2"]),
            "0V": 0.0, "float": 0.0}


def level_volts(rails: dict) -> dict:
    return {lv: rails[RAIL_OF[lv]] for lv in LEVELS}


def volts_from_weights(w: dict, vsl: float = -14.98) -> dict:
    """weight set (S = +1 ... in S-frames) -> level voltages that make q_sf == sum(w x frames) exactly."""
    out = {lv: w.get(lv, 0.0) * vsl for lv in LEVELS}
    out["SWK"] = 0.0
    return out


def frame_ms(panel: Panel, pll: int, rate: int | None) -> float | None:
    if pll & 0x08:
        return panel.dyn_ms.get(rate if rate is not None else 0)
    return panel.fixed_ms.get(pll & 7)


def _slot_levels(t: C.Table, k: int) -> list:
    out = []
    for lv, n in C.segments(t, k, merge=False, keep_swk=True):
        out += [lv] * n
    return out


def _lutc_frames(e: bytes) -> tuple[list, list]:
    st, rt = [], []
    for s, r, n in s6_lutc.lutc_timeline(e):
        st += [s] * n
        rt += [r] * n
    return st, rt


def charge(table, pwr=None, pll=0x04, panel: str = "7.09", mode: str = "custom", vdcs=None, vcom=None, cdi=None,
           codes=None, volts: dict | None = None, hold_s: float = 0.0, vsh=None, vsl=None) -> dict:
    """Full per-slot charge report for one refresh of `table`.
    table: 800 B (bytes or hex). pwr: PWR data (custom mode; default = the panel's firmware init). pll: 0x30 value.
    mode: "custom" (LUT_EN = 1, PWR rails, VDCS VCOM) or "stock" (header rails; VCOM per the panel).
    vdcs / vcom: VCOM DC during the refresh (vcom wins).
    cdi: CDI 0x50 value -> border slot. codes: slots used by the image (default all non-hold).
    volts: override level voltages (e.g. volts_from_weights()). hold_s: end-hold until power-off (added for slots
    that end on a driven level). Returns a JSON-able dict.

    This expands every frame into a list: check block lengths first (s6_lint rule L0) on untrusted input."""
    P = PANELS[panel]
    e = _bytes(table)
    if len(e) != C.TABLE_LEN:
        raise ValueError(f"table is {len(e)} B, expected {C.TABLE_LEN}")
    pll = int(pll, 16) if isinstance(pll, str) else int(pll)
    t = C.decode_table(e)
    pwr = P.pwr if pwr is None else pwr
    vsh = P.vsh if vsh is None else vsh
    vsl = P.vsl if vsl is None else vsl
    notes = []
    if volts is None:
        wc = weak_codes(e, pwr, mode)
        rails = rail_volts(wc, vsh, vsl)
        V = level_volts(rails)
    else:
        wc, rails, V = None, None, dict(volts)
        notes.append("level voltages overridden (weights)")
    if vcom is None:
        if mode == "stock" and P.stock_vcom == "header":
            vcom = VCOM_STEP * C.val(e[5])
        else:
            vd = P.vdcs if vdcs is None else (int(vdcs, 16) if isinstance(vdcs, str) else vdcs)
            if vd is None:
                vcom = P.vcom_rest
                notes.append(f"VDCS unknown on this panel: VCOM DC taken as the {P.vcom_rest} V rest point")
            else:
                vcom = P.vcom_step * vd
                if P.stock_vcom == "header" and abs(vcom - P.vcom_rest) > 0.15:
                    notes.append(f"VCOM DC {vcom:.2f} V is {vcom - P.vcom_rest:+.2f} V off the {P.vcom_rest} V rest point "
                                 f"(VDCS 0x0f matches stock)")
    cdi = P.cdi if cdi is None else (int(cdi, 16) if isinstance(cdi, str) else cdi)
    vbd = (cdi >> 5) & 7
    border = None if vbd == 7 else vbd
    if codes is None:
        codes = [k for k in range(8) if k not in P.hold_codes]
    lst, lrt = _lutc_frames(e)
    slv = {k: _slot_levels(t, k) for k in range(8)}
    used = sorted(set(codes) | ({border} if border is not None else set()))
    T = max([len(lst)] + [len(slv[k]) for k in used])
    Tmax = max([T] + [len(v) for v in slv.values()])          # unused slots may run past the refresh end
    rates = [lrt[i] if i < len(lrt) else None for i in range(Tmax)]
    dt = [frame_ms(P, pll, r) for r in rates]
    dt_ok = all(x is not None for x in dt[:T])
    if not dt_ok:
        notes.append(f"frame time unknown for PLL {pll:02X} on the {panel}: V x s not computed")
    dts = [x / 1000.0 if x is not None else NAN for x in dt]
    sw_v = P.lutc_swing
    swing = [(sw_v if s == "lo" else -sw_v if s == "hi" else 0.0) for s in lst] + [0.0] * (Tmax - len(lst))
    vcom_t = [vcom + s for s in swing]
    n_both = sum(1 for s in lst if s == "both")
    if n_both:
        notes.append(f"LUTC has {n_both} 'both' frames (weak/floating VCOM): counted as DC")
    lutc = {"frames": len(lst), "lo": lst.count("lo"), "hi": lst.count("hi"), "both": n_both,
            "swing_Vframes": round(math.fsum(swing), 3),
            "swing_Vs": round(math.fsum(s * d for s, d in zip(swing[:T], dts[:T])), 4) if dt_ok else None,
            "rates": sorted(set(int(r) for r in lrt)) if lrt else []}
    slots = {}
    for k in range(8):
        lv = slv[k]
        n = len(lv)
        full = lv + [""] * (T - n) if n <= T else lv
        vs = [V[x] for x in full]
        driven = [x != "SWK" for x in full]
        tt = dts[:len(full)]
        segs = C.merge_segments([(x, 1) for x in lv])
        fr = {}
        for x in lv:
            fr[x or "0"] = fr.get(x or "0", 0) + 1
        last_nz = next((x for x in reversed(full) if x), "")
        tail = 0
        for x in reversed(full):
            if x:
                break
            tail += 1
        tail0 = n > 0 and full[-1] != ""
        q_src = math.fsum(vs[:n])
        rel = [(v - c) if d else 0.0 for v, c, d in zip(vs, vcom_t, driven)]
        q_vcom = math.fsum(rel)
        hold_add = V[full[-1]] * hold_s if (tail0 and hold_s) else 0.0
        d = {"used": k in codes, "border": k == border, "frames": n, "tail_to_end": tail, "tail0": bool(tail0),
             "last_level": last_nz, "swk_frames": driven.count(False), "frames_per_level": fr,
             "q_sf": round(q_src / V["S"], 3),
             "Q_src_Vframes": round(q_src, 2), "Q_vcom_Vframes": round(q_vcom, 2),
             "Q_src_Vs": round(math.fsum(v * x for v, x in zip(vs[:n], tt[:n])) + hold_add, 4) if dt_ok else None,
             "Q_vcom_Vs": round(math.fsum(r * x for r, x in zip(rel, tt)) + hold_add, 4) if dt_ok else None,
             "timeline": [{"level": x or "0", "rail": RAIL_OF[x], "V": round(V[x], 3), "frames": m} for x, m in segs]}
        if hold_add:
            d["end_hold_Vs"] = round(hold_add, 4)
        slots[k] = d
    return {"panel": panel, "mode": mode, "pll": f"{pll:02X}", "dynamic_pll": bool(pll & 8),
            "refresh_frames": T, "refresh_s": round(math.fsum(dts[:T]), 4) if dt_ok else None,
            "frame_ms": sorted(set(round(x, 2) for x in dt[:T] if x is not None)),
            "weak_codes": wc, "rails_V": {k: round(v, 3) for k, v in rails.items()} if rails else None,
            "level_V": {(k or "0"): round(v, 3) for k, v in V.items()}, "vcom_dc": round(vcom, 3), "vcom_rest": P.vcom_rest,
            "cdi": f"{cdi:02X}", "border_slot": border, "codes": list(codes), "lutc": lutc, "slots": slots,
            "notes": notes, "assumed": P.assumed, "source": P.source, "sha": t.sha}


def legacy_q(table, weights: dict, codes=range(8)) -> dict:
    """plain per-slot arithmetic: sum(weight[level] x frames), SWK = 0."""
    t = C.decode_table(_bytes(table))
    return {k: sum(weights.get(lv, 0.0) * n for lv, n in C.segments(t, k, merge=False, keep_swk=True)) for k in codes}


def _zero_runs(e: bytes) -> list:
    bad = []
    for k in range(8):
        o = C.slot_offset(k)
        if b"\x00" * 8 in e[o:o + 88]:
            bad.append(k)
    return bad


def gate(table, header=None, pwr=None, pll=0x04, panel="7.09", mode="custom", tol_sf=1.0, ref="src", codes=None,
         allow_tail0=False, vdcs=None, vcom=None, cdi=None, tol_vcom_sf=None, rep=None, **kw) -> dict:
    """Automatic charge gate.
    FAIL if any used or border slot has |q_sf| > tol_sf (ref "src" = 0 V reference; ref "vcom" = VCOM reference),
    the LUTC VCOM swing is not balanced (lo != hi, in V x s), a slot ends on a driven level at the refresh end (tail 0,
    end-hold charge until power-off) unless allow_tail0, or a slot holds an 8 x 00 run (hang on the 7.09" slave chip).
    WARN: SWK frames (float), VCOM-reference |q| > tol_vcom_sf (default: report only), notes from charge().
    `header` (8 B) replaces the table's header bytes if given."""
    e = bytearray(_bytes(table))
    if header is not None:
        e[:8] = _bytes(header)[:8]
    r = rep or charge(bytes(e), pwr=pwr, pll=pll, panel=panel, mode=mode, codes=codes, vdcs=vdcs, vcom=vcom, cdi=cdi, **kw)
    vsl = r["level_V"]["S"]
    fail, warn = [], list(r["notes"])
    for k, s in r["slots"].items():
        if not (s["used"] or s["border"]) or s["frames"] == 0:
            continue
        tag = f"slot {k}" + (" (border)" if s["border"] else "")
        q = s["q_sf"] if ref == "src" else s["Q_vcom_Vframes"] / vsl
        if abs(q) > tol_sf + 1e-9:
            fail.append(f"{tag}: |q| {abs(q):.2f} > {tol_sf} S-frames ({ref} ref; {s['Q_src_Vframes']:+.1f} V x frames)")
        qv = s["Q_vcom_Vframes"] / vsl
        if tol_vcom_sf is not None and abs(qv) > tol_vcom_sf:
            warn.append(f"{tag}: VCOM-ref |q| {abs(qv):.2f} S-frames")
        if s["tail0"] and not allow_tail0:
            fail.append(f"{tag}: tail 0 (ends on {s['last_level']} at the refresh end -> end-hold charge until POF)")
        if s["swk_frames"]:
            warn.append(f"{tag}: {s['swk_frames']} SWK (floating) frames counted as 0")
    if r["border_slot"] is not None and r["slots"][r["border_slot"]]["frames"] == 0:
        warn.append(f"border slot {r['border_slot']} is blank: border sees level 0 (= VCOM) only")
    sw = r["lutc"]["swing_Vs"] if r["lutc"]["swing_Vs"] is not None else r["lutc"]["swing_Vframes"]
    if abs(r["lutc"]["swing_Vframes"]) > 1e-6 and abs(sw) > 0.01:
        fail.append(f"LUTC VCOM swing unbalanced: lo {r['lutc']['lo']} / hi {r['lutc']['hi']} frames, net {sw:+.3f}")
    z = _zero_runs(bytes(e))
    if z:
        fail.append(f"8 x 00 run in slot(s) {z} (slave hang)")
    return {"pass": not fail, "fail": fail, "warn": warn,
            "q_sf": {k: s["q_sf"] for k, s in r["slots"].items() if s["used"] or s["border"]}, "report": r}


class Ledger:
    """Cumulative per-code (and optionally per-pixel) charge over a sequence of shown tables.
    add(table, ...) books one refresh; code_map (2-D int array of slot codes) books per pixel (needs numpy)."""

    FIELDS = ("q_sf", "Q_src_Vs", "Q_vcom_Vs", "Q_vcom_Vframes")

    def __init__(self, panel="7.09", shape=None, **defaults):
        self.panel, self.defaults = panel, defaults
        self.per_code = {k: dict.fromkeys(self.FIELDS, 0.0) for k in range(8)}
        self.border = dict.fromkeys(self.FIELDS, 0.0)          # border electrode area
        self.px = None
        if shape is not None:
            import numpy as np
            self.px = np.zeros(shape)
        self.history = []

    def add(self, table, label="", count=1, code_map=None, **kw):
        args = {**self.defaults, **kw}
        r = charge(table, panel=self.panel, **args)
        inc = {}
        for k, s in r["slots"].items():
            if s["border"]:
                for f in self.border:
                    if s.get(f) is not None:
                        self.border[f] += count * s[f]
            if not s["used"]:
                continue
            for f in self.per_code[k]:
                v = s.get(f)
                if v is not None:
                    self.per_code[k][f] += count * v
            inc[k] = s["q_sf"]
        if code_map is not None and self.px is not None:
            import numpy as np
            self.px += count * np.array([r["slots"][k]["q_sf"] for k in range(8)])[np.asarray(code_map)]
        self.history.append({"label": label, "count": count, "sha": r["sha"], "pll": r["pll"], "q_sf": inc,
                             "border_slot": r["border_slot"]})
        return r

    def totals(self, field="q_sf", nd=2, codes=None):
        codes = codes if codes is not None else [k for k in range(8) if k not in PANELS[self.panel].hold_codes]
        return {k: round(self.per_code[k][field], nd) for k in codes}


def format_report(r: dict) -> str:
    L = [f"panel {r['panel']} mode {r['mode']} PLL {r['pll']} ({'dynamic' if r['dynamic_pll'] else 'fixed'}), "
         f"{r['refresh_frames']} frames = {r['refresh_s']} s, frame ms {r['frame_ms']}",
         f"level V {r['level_V']}  VCOM DC {r['vcom_dc']} V  CDI {r['cdi']} -> border slot {r['border_slot']}",
         f"LUTC: {r['lutc']}",
         "slot | use | frames | tail | q_sf | Q_src Vxfr | Q_src Vxs | Q_vcom Vxfr | Q_vcom Vxs | levels"]
    for k, s in r["slots"].items():
        if not s["frames"] and not s["border"]:
            continue
        L.append(f"{k}{'b' if s['border'] else ' '}  | {'y' if s['used'] else '-'} | {s['frames']} | {s['tail_to_end']} | "
                 f"{s['q_sf']:+.2f} | {s['Q_src_Vframes']:+.1f} | {s['Q_src_Vs']} | {s['Q_vcom_Vframes']:+.1f} | {s['Q_vcom_Vs']} | "
                 f"{s['frames_per_level']}")
    L += [f"note: {n}" for n in r["notes"]] + ([f"assumed: {r['assumed']}"] if r["assumed"] else [f"source: {r.get('source', '')}"])
    return "\n".join(L)
