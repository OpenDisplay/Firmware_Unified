"""Spectra 6 0x20 table linter.

Works on the 800 B 0x20 image shared by the 7.3" and the 7.09" Spectra 6 panels (s6_codec, s6_charge).
BWRY 535 B tables are out of scope (see bwry535).

Table rules (FAIL unless noted; `mode` = how the table will run: "custom" = register table with LUT_EN,
"stock" = a copy of a built-in table):
  L0 length          any block longer than MAX_FRAMES (20000; the longest stock table is 2241 = 52.6 s) FAILs and the
                     charge rules are skipped: the charge calculator expands every frame into a list, and a corrupt
                     image (all 00 = 662 M frames per block) would take tens of GB. Lengths come from arithmetic only.
  L1 longest block   refresh = the longest block, used or not (LUTC + all 8 slots). Reports the predicted time and the
                     BUSY ceiling it needs (max(90 s, 1.5 x refresh)); FAIL if busy_timeout_s is given and shorter;
                     WARN if an UNUSED slot is the longest (time spent on nothing).
  L2 equal lengths   stock: every non-empty block has the same length; a stock table that fails is a wrong decode or
                     read. Custom: INFO only.
  L3 DC vs 0 V       per used + border slot, q_sf = sum(V_level x frames) / VSL in NOMINAL volts (strong rails +-15 V,
                     weak rails 3.0 + 0.1 x code, from PWR in custom mode and from the header in stock mode); the
                     measured-rail q is reported as INFO only. custom |q| <= 1.0; stock |q| <= 1.5.
                     The LUTC VCOM swing must balance. SWK (floating) frames: WARN.
  L4 end frame       custom: a used/border slot must end on level 0 (>= 1 zero frame) or it holds its last level
                     until power-off. Stock: INFO (stock relies on the end hold; send POF right after BUSY).
  L5 PWR present     custom mode runs on the PWR (0x01) rails, not the header: explicit PWR bytes must be given
                     (6 data bytes, or 7 with the 01 command byte). Weak codes > 0x3F: WARN.
  L6 border filled   the slot CDI VBD selects (CDI bits 7-5; 0x3F/0x37 -> slot 1) must have frames in custom mode.
  L7 hang patterns   8 x 00 in a row anywhere (hangs the 7.09" slave chip), all-zero 8 B header.
  L8 byte gate       opt-in (forbid_bytes): byte values the upload path refuses. FAIL for custom, WARN for stock.

Sequence rule (lint_events): a table upload must come AFTER the last reset / init / temperature read and before the
refresh that uses it. A reset reloads the built-in table; on the 7.09" a temperature read (0x40) reloads 0x20.

The limits are lab rules derived on one unit per panel type. L4 in particular is under review: whether a zero end
frame is required, or power-off right after BUSY is enough, is not settled.
"""
from __future__ import annotations

from dataclasses import dataclass, field

from . import s6_codec as C
from . import s6_charge as Q
from . import s6_lutc

BUSY_MIN_S = 90.0
STOCK_TOL = 1.5
CUSTOM_TOL = 1.0
MAX_FRAMES = 20000
NOMINAL_V = 15.0
EVENTS = ("RESET", "TSC", "UPLOAD", "REFRESH")


@dataclass
class Lint:
    rule: str
    level: str          # FAIL | WARN | INFO
    msg: str


@dataclass
class Result:
    findings: list = field(default_factory=list)
    facts: dict = field(default_factory=dict)

    def add(self, rule, level, msg):
        self.findings.append(Lint(rule, level, msg))

    @property
    def ok(self) -> bool:
        return not any(f.level == "FAIL" for f in self.findings)

    def text(self) -> str:
        head = "PASS" if self.ok else "FAIL"
        f = self.facts
        out = [f"{head}  refresh {f.get('refresh_frames')} frames = {f.get('refresh_s')} s"
               f"  -> BUSY ceiling >= {f.get('busy_ceiling_s')} s  (blocks {f.get('blocks')})"]
        out += [f"  {x.level:4s} {x.rule}: {x.msg}" for x in self.findings if x.level != "INFO"]
        out += [f"  info {x.rule}: {x.msg}" for x in self.findings if x.level == "INFO"]
        return "\n".join(out)

    def as_dict(self) -> dict:
        return {"pass": self.ok, "facts": self.facts, "findings": [x.__dict__ for x in self.findings]}


def _pwr_bytes(pwr) -> bytes | None:
    if pwr is None:
        return None
    p = Q._bytes(pwr)
    if len(p) == 7 and p[0] == 0x01:
        p = p[1:]
    return p


def block_frames(e: bytes) -> dict:
    """frames per block {"LUTC": n, 0..7: n} from arithmetic only: never expands a frame list."""
    return C.decode_table(e).block_frames()


def _byte_rules(R: Result, e: bytes, mode: str, forbid_bytes) -> None:
    if e[:8] == b"\x00" * 8:
        R.add("L7 hang", "FAIL", "all-zero header (hangs the 7.09in slave chip)")
    i = e.find(b"\x00" * 8)
    if i >= 0 and not any(x.rule == "L7 hang" and "8 x 00" in x.msg for x in R.findings):
        R.add("L7 hang", "FAIL", f"8 x 00 run ending at byte {i + 7}")
    if forbid_bytes:
        bad = [i for i, b in enumerate(e) if b in forbid_bytes]
        if bad:
            R.add("L8 byte gate", "FAIL" if mode == "custom" else "WARN",
                  f"{len(bad)} byte(s) in {{{' '.join(f'{b:02X}' for b in forbid_bytes)}}} (first at {bad[0]}): "
                  f"the upload path refuses this table")


def lint_table(table, panel: str = "7.3", mode: str = "custom", pwr=None, pll=None, cdi=None, codes=None,
               busy_timeout_s: float | None = None, forbid_bytes=()) -> Result:
    """table: 800 B as bytes, or a file path / hex string. See the module docstring for the rules."""
    e = Q.load_table(table) if isinstance(table, str) else bytes(table)
    R = Result()
    if len(e) != C.TABLE_LEN:
        R.add("format", "FAIL", f"table is {len(e)} B, expected {C.TABLE_LEN}")
        return R
    if mode not in ("custom", "stock"):
        raise ValueError(mode)
    P = Q.PANELS[panel]
    if pll is None:
        pll = 0x02 if (panel == "7.3" and mode == "stock") else 0x04
    pll = int(pll, 16) if isinstance(pll, str) else int(pll)
    t = C.decode_table(e)
    bf = t.block_frames()
    if codes is None:
        codes = [k for k in range(8) if k not in P.hold_codes]
    cdi_v = P.cdi if cdi is None else (int(cdi, 16) if isinstance(cdi, str) else cdi)
    vbd = (cdi_v >> 5) & 7
    border = None if vbd == 7 else vbd

    # L0 before anything expands frames
    if max(bf.values()) > MAX_FRAMES:
        R.facts.update({"panel": panel, "mode": mode, "blocks": bf, "refresh_frames": max(bf.values()),
                        "refresh_s": None, "busy_ceiling_s": None, "sha": t.sha})
        R.add("L0 length", "FAIL", f"block length {max(bf.values())} frames > {MAX_FRAMES}: corrupt or mis-decoded "
                                   f"table; charge rules skipped")
        _byte_rules(R, e, mode, forbid_bytes)
        return R

    # L5 PWR (before the charge: custom charge needs the rails)
    p = _pwr_bytes(pwr)
    if mode == "custom":
        if p is None:
            R.add("L5 PWR", "FAIL", "custom table without explicit PWR bytes: custom mode takes the weak rails from "
                                    "PWR (0x01), not the header; give the bytes the firmware will write")
            p = _pwr_bytes(P.pwr)
        elif len(p) != 6:
            R.add("L5 PWR", "FAIL", f"PWR must be 6 data bytes, got {len(p)}")
            p = _pwr_bytes(P.pwr)
        hi = [f"{b:02X}" for b in p[2:6] if b > 0x3F]
        if hi:
            R.add("L5 PWR", "WARN", f"weak-rail code(s) {' '.join(hi)} > 0x3F: check the field takes them")

    # The DC rule is in NOMINAL volts. Measured rails are slightly asymmetric (7.09": +14.82 / -14.69 V), which alone
    # adds about one S-frame over a few hundred S/K frames; that q is reported as INFO, not gated.
    kw = dict(pwr=p if mode == "custom" else None, pll=pll, panel=panel, mode=mode, cdi=cdi_v, codes=codes)
    rep = Q.charge(e, vsh=NOMINAL_V, vsl=-NOMINAL_V, **kw)
    meas = Q.charge(e, **kw)
    g = Q.gate(e, tol_sf=CUSTOM_TOL if mode == "custom" else STOCK_TOL, allow_tail0=(mode == "stock"), rep=rep, **kw)

    # L1 longest block
    longest = max(bf.values())
    who = [k for k, v in bf.items() if v == longest]
    fm = rep["frame_ms"][0] if rep["frame_ms"] else Q.frame_ms(P, pll, None)
    refresh_s = rep["refresh_s"] if rep["refresh_s"] is not None else (longest * fm / 1000.0 if fm else None)
    if refresh_s is not None and longest > rep["refresh_frames"] and fm:
        refresh_s = refresh_s * longest / max(rep["refresh_frames"], 1)
    ceil_s = round(max(BUSY_MIN_S, 1.5 * refresh_s), 1) if refresh_s else BUSY_MIN_S
    R.facts.update({"panel": panel, "mode": mode, "pll": f"{pll:02X}", "blocks": bf, "refresh_frames": longest,
                    "refresh_s": round(refresh_s, 2) if refresh_s else None, "busy_ceiling_s": ceil_s,
                    "border_slot": border, "codes": list(codes), "q_sf": g["q_sf"], "sha": t.sha})
    used_long = [k for k in who if k == "LUTC" or k in codes or k == border]
    if not used_long:
        R.add("L1 longest", "WARN", f"the longest block ({longest} frames) is unused slot(s) {who}: the refresh lasts it anyway")
    if busy_timeout_s is not None and refresh_s and busy_timeout_s < refresh_s:
        R.add("L1 longest", "FAIL", f"BUSY timeout {busy_timeout_s} s < predicted refresh {refresh_s:.1f} s (truncated refresh)")
    elif busy_timeout_s is not None and busy_timeout_s < ceil_s:
        R.add("L1 longest", "WARN", f"BUSY timeout {busy_timeout_s} s below the {ceil_s} s ceiling (>= 90 s, 1.5 x refresh)")

    # L2 equal lengths
    ne = {k: v for k, v in bf.items() if v}
    if len(set(ne.values())) > 1:
        msg = "non-empty blocks differ in length: " + ", ".join(f"{k}={v}" for k, v in ne.items())
        R.add("L2 equal lengths", "FAIL" if mode == "stock" else "INFO",
              msg + (" -> stock tables are all-equal; this decode/read is wrong" if mode == "stock" else ""))

    # L3 / L4 from the gate
    for f in g["fail"]:
        if "tail 0" in f:
            R.add("L4 end frame", "FAIL", f + "; add one level-0 end frame")
        elif "8 x 00" in f:
            R.add("L7 hang", "FAIL", f)
        else:
            R.add("L3 DC", "FAIL", f)
    for w in g["warn"]:
        if "border slot" in w and "blank" in w:
            continue
        R.add("L3 DC", "WARN" if "SWK" in w else "INFO", w)
    qm = {k: v["q_sf"] for k, v in meas["slots"].items() if v["used"] or v["border"]}
    R.facts["q_sf_measured_rails"] = qm
    over = {k: v for k, v in qm.items() if abs(v) > (CUSTOM_TOL if mode == "custom" else STOCK_TOL)}
    if over:
        R.add("L3 DC", "INFO", f"on the MEASURED {panel} rails (VSH {P.vsh:+.2f} / VSL {P.vsl:+.2f}) "
                               f"|q| exceeds the limit in slots {over} (not gated: the rule is in nominal volts)")
    if mode == "stock":
        n0 = [k for k, s in rep["slots"].items() if (s["used"] or s["border"]) and s["tail0"]]
        if n0:
            R.add("L4 end frame", "INFO", f"stock slots {n0} end on a driven level (end hold until POF: send POF right after BUSY)")

    # L6 border
    if border is not None and bf[border] == 0:
        R.add("L6 border", "FAIL" if mode == "custom" else "WARN",
              f"border slot {border} (CDI {cdi_v:02X}) is empty: the border sees no drive (custom tables must fill it)")

    _byte_rules(R, e, mode, forbid_bytes)
    return R


def lint_events(events, tsc_reloads: bool = True) -> Result:
    """events: the panel operations in order, each one of EVENTS ("RESET", "TSC", "UPLOAD", "REFRESH"); anything else
    is ignored. tsc_reloads: a temperature read reloads the table register (true on the 7.09"; keep it on unless the
    panel is known not to)."""
    R = Result()
    pending = None          # index (1-based) of the upload not yet consumed by a refresh
    for n, ev in enumerate(events, 1):
        k = str(ev).upper()
        if k == "UPLOAD":
            pending = n
        elif pending and (k == "RESET" or (k == "TSC" and tsc_reloads)):
            why = "a reset reloads the built-in table" if k == "RESET" else "a temperature read reloads 0x20"
            R.add("S1 order", "FAIL", f"event {n} ({k}) after the upload at event {pending} and before its refresh: {why}")
            pending = None
        elif k == "REFRESH" and pending:
            pending = None
    if pending:
        R.add("S1 order", "INFO", f"upload at event {pending} is never refreshed")
    return R
