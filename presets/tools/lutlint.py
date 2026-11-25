#!/usr/bin/env python3
"""Lint and inspect e-paper waveform tables (offline; never talks to hardware).

  lutlint.py lint   TABLE [--panel 7.3|7.09] [--mode custom|stock] [--pwr "3F 00 32 2A 0E 2A"] [--pll 04]
                          [--cdi 3F] [--codes 0,1,2,3,5,6] [--busy-timeout 60] [--forbid 00,01,11] [--json]
  lutlint.py charge TABLE [--panel ...] [--mode ...] [--pwr ...] [--pll ...] [--vdcs 0f] [--vcom -0.8] [--cdi ...]
                          [--codes ...] [--tol 1.0] [--ref src|vcom] [--allow-tail0] [--json]
  lutlint.py show   TABLE            (800 B Spectra 6 or 535 B BWRY: decoded summary)
  lutlint.py seq    FILE             (one event per line: RESET | TSC | UPLOAD | REFRESH; # comments)

TABLE = a .bin file or a hex string. Exit 0 = pass, 1 = fail. The rules are in odlut/s6_lint.py.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from odlut import bwry535, s6_charge, s6_lint, s6_lutc  # noqa: E402


def _codes(s):
    return [int(x) for x in s.split(",")] if s else None


def _common(p):
    p.add_argument("table")
    p.add_argument("--panel", default="7.3", choices=sorted(s6_charge.PANELS))
    p.add_argument("--mode", default="custom", choices=("custom", "stock"))
    p.add_argument("--pwr")
    p.add_argument("--pll")
    p.add_argument("--cdi")
    p.add_argument("--codes")
    p.add_argument("--json", action="store_true")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("lint")
    _common(p)
    p.add_argument("--busy-timeout", type=float)
    p.add_argument("--forbid", help="comma-separated hex byte values the upload path refuses")
    p = sub.add_parser("charge")
    _common(p)
    p.add_argument("--vdcs")
    p.add_argument("--vcom", type=float)
    p.add_argument("--tol", type=float, default=1.0)
    p.add_argument("--ref", default="src", choices=("src", "vcom"))
    p.add_argument("--allow-tail0", action="store_true")
    sub.add_parser("show").add_argument("table")
    p = sub.add_parser("seq")
    p.add_argument("file")
    p.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)

    if a.cmd == "lint":
        forbid = tuple(int(x, 16) for x in a.forbid.split(",")) if a.forbid else ()
        r = s6_lint.lint_table(a.table, a.panel, a.mode, a.pwr, a.pll, a.cdi, _codes(a.codes), a.busy_timeout, forbid)
        print(json.dumps(r.as_dict(), indent=1) if a.json else r.text())
        return 0 if r.ok else 1
    if a.cmd == "charge":
        pll = a.pll or "04"
        g = s6_charge.gate(s6_charge.load_table(a.table), pwr=a.pwr, pll=pll, panel=a.panel, mode=a.mode, vdcs=a.vdcs,
                           vcom=a.vcom, cdi=a.cdi, codes=_codes(a.codes), tol_sf=a.tol, ref=a.ref,
                           allow_tail0=a.allow_tail0)
        if a.json:
            print(json.dumps(g, indent=1, default=str))
        else:
            print(s6_charge.format_report(g["report"]))
            print("GATE", "PASS" if g["pass"] else "FAIL")
            for x in g["fail"]:
                print("  fail:", x)
            for x in g["warn"]:
                print("  warn:", x)
        return 0 if g["pass"] else 1
    if a.cmd == "show":
        p = Path(a.table)
        e = p.read_bytes() if p.is_file() else bytes.fromhex(a.table.replace(" ", ""))
        if len(e) == bwry535.LEN:
            print(bwry535.describe(e, p.name))
            return 0 if bwry535.encode(bwry535.decode(e)) == e else 1
        if len(e) < 800:
            print(f"{len(e)} B: neither an 800 B Spectra 6 image nor a 535 B BWRY table", file=sys.stderr)
            return 1
        print(s6_lutc.describe(p.name, e[:800]))
        return 0
    lines = [x.split("#")[0].strip() for x in Path(a.file).read_text().splitlines()]
    r = s6_lint.lint_events([x for x in lines if x])
    print(json.dumps(r.as_dict(), indent=1) if a.json else "\n".join(
        ["PASS" if r.ok else "FAIL"] + [f"  {x.level} {x.rule}: {x.msg}" for x in r.findings]))
    return 0 if r.ok else 1


if __name__ == "__main__":
    sys.exit(main())
