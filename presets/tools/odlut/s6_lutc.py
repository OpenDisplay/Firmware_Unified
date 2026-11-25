"""LUTC (VCOM) block helpers for Spectra 6 0x20 images, on top of s6_codec.

The LUTC block uses the same row format as a pixel slot, but its level bits are read as 2-bit fields.
Per group byte (b0 for the A group, b1 for the B group):
    f0 (phase 1): lo = bit 2, hi = bit 5        f1 (phase 2): lo = bit 0, hi = bit 3
    f2 (phase 3): lo = bit 6, hi = bit 1        f3 (no phase): bit 4, bit 7 = frame-rate code bits
  lo -> VCOM = DC + swing, hi -> VCOM = DC - swing, both / none -> VCOM = DC.
  Rate code (used only when PLL 0x30 has bit 3 set, "dynamic"):
    b0 = A byte bit 4, b1 = A byte bit 7, b2 = B byte bit 4.
All bits are active low, as everywhere in the table.

Offline only: these functions build and read bytes, they never talk to hardware.
"""
from __future__ import annotations

from . import s6_codec as C

P, Row = C.Phase, C.Row
FORBIDDEN_BYTES = (0x00, 0x01, 0x11)


def blank(header7: bytes) -> C.Table:
    """all-FF table (every row M = 0, no time, no level) with the given 7 header bytes."""
    e = bytearray(b"\xff" * C.TABLE_LEN)
    e[:7] = header7[:7]
    return C.decode_table(bytes(e))


def empty_row() -> Row:
    return Row(0, 0, 0, [P("", 0) for _ in range(6)])


def lutc_group(fields, rate_lo2: int = 0, rate_b2: int = 0):
    """fields: up to three (state, tp) in time order, state in '0' | 'lo' | 'hi' | 'both' -> 3 codec Phases.
    rate_lo2: 0..3 puts rate bits b0 (bit 4) / b1 (bit 7) into this group's byte (A group only);
    rate_b2: 1 puts bit 4 of this group's byte (use on the B group = rate bit b2)."""
    f = list(fields) + [("0", 0)] * (3 - len(fields))
    lo = [s in ("lo", "both") for s, _ in f]
    hi = [s in ("hi", "both") for s, _ in f]
    p1 = ("S" if lo[0] else "") + ("W" if hi[0] else "") + ("K" if hi[1] else "")
    p2 = ("S" if hi[2] else "") + ("W" if lo[1] else "") + ("K" if lo[2] else "")
    p3 = ("S" if (rate_lo2 & 1) or rate_b2 else "") + ("W" if rate_lo2 & 2 else "")
    return [P(p1, f[0][1]), P(p2, f[1][1]), P(p3, f[2][1])]


def lutc_row(a=(), b=(), rate: int = 0, M: int = 1, RA: int = 1, RB: int | None = None) -> Row:
    """one LUTC row from physical VCOM fields (see the module docstring). rate = 3-bit frame-rate code 0..7."""
    if RB is None:
        RB = 1 if any(tp for _, tp in b) else 0
    return Row(M, RA, RB, lutc_group(a, rate_lo2=rate & 3) + lutc_group(b, rate_b2=(rate >> 2) & 1))


def lutc_rate_only(rows: list, rate: int) -> list:
    """give 0-length (M = 0) LUTC rows the rate bits too, as the unused stock rows have."""
    out = list(rows)
    while len(out) < 8:
        r = empty_row()
        r.phases = lutc_group((), rate_lo2=rate & 3) + lutc_group((), rate_b2=(rate >> 2) & 1)
        out.append(r)
    return out


def pix_row(a=(), b=(), M: int = 1, RA: int = 1, RB: int | None = None) -> Row:
    """pixel-slot row from codec levels: a, b = up to three (level, tp), level in '', S, W, K, SW, SK, WK, SWK."""
    a = list(a) + [("", 0)] * (3 - len(a))
    b = list(b) + [("", 0)] * (3 - len(b))
    if RB is None:
        RB = 1 if any(tp for _, tp in b) else 0
    return Row(M, RA, RB, [P(lv, tp) for lv, tp in a + b])


def pad(rows: list) -> list:
    return list(rows) + [empty_row() for _ in range(8 - len(rows))]


def finalize(t: C.Table, forbid: tuple = FORBIDDEN_BYTES) -> bytes:
    """set every row-0 M source (header byte 7, previous block's byte 87) from the rows, encode, self-check.
    forbid: byte values that must not appear in the image (some upload paths refuse 00 / 01 / 11); () = no check."""
    hdr = bytearray(t.header)
    hdr[7] = C.enc(t.lutc.rows[0].M)
    t.header = bytes(hdr)
    blocks = [t.lutc] + list(t.slots)
    for i, blk in enumerate(blocks):
        blk.m_in = blk.rows[0].M
        blk.tail_byte = C.enc(blocks[i + 1].rows[0].M) if i + 1 < len(blocks) else 0xFF
    e = C.encode_table(t)
    if len(e) != C.TABLE_LEN:
        raise C.EncodeError(f"encoded {len(e)} B")
    d = C.decode_table(e)
    if C.encode_table(d) != e or d.block_frames() != t.block_frames():
        raise C.EncodeError("round trip mismatch")
    if any(x in forbid for x in e):
        raise C.EncodeError("forbidden byte in table: " + " ".join(f"{x:02X}" for x in forbid))
    for k in range(8):
        if b"\x00" * 8 in e[C.slot_offset(k):C.slot_offset(k) + 88]:
            raise C.EncodeError(f"8 x 00 run in slot {k}")
    return e


def lutc_phys(e: bytes) -> list:
    """decode the LUTC with the 2-bit-field reading: per row a dict (row, M, RA, RB, rate, A, B, raw);
    A and B are three (state, tp) each."""
    out = []
    for r in range(8):
        o = C.LUTC_OFF + 11 * r
        b = e[o:o + 11]
        M = C.val(e[o - 1])

        def grp(x, tps):
            st = []
            for (lo, hi), tp in zip(((2, 5), (0, 3), (6, 1)), tps):
                l, h = not (x >> lo) & 1, not (x >> hi) & 1
                st.append(("both" if l and h else "lo" if l else "hi" if h else "0", tp))
            return st
        ta = [C.val(b[2]), C.val(b[3]), C.val(b[4], skip5=True)]
        tb = [C.val(b[6]), C.val(b[7]), C.val(b[8], skip5=True)]
        rate = (0 if (b[0] >> 4) & 1 else 1) | (0 if (b[0] >> 7) & 1 else 2) | (0 if (b[1] >> 4) & 1 else 4)
        out.append(dict(row=r, M=M, RA=C.val(b[5]), RB=C.val(b[9]), rate=rate, A=grp(b[0], ta), B=grp(b[1], tb),
                        raw=b[:10].hex(" ")))
    return out


def lutc_frames(e: bytes) -> int:
    """LUTC length in frames, from arithmetic only (safe on corrupt images)."""
    return sum(r["M"] * (r["RA"] * sum(tp for _, tp in r["A"]) + r["RB"] * sum(tp for _, tp in r["B"]))
               for r in lutc_phys(e))


def lutc_timeline(e: bytes) -> list:
    """time-ordered, merged (vcom_state, rate, frames) from lutc_phys()."""
    seq = []
    for r in lutc_phys(e):
        for _ in range(r["M"]):
            for _ in range(r["RA"]):
                seq += [(s, r["rate"], tp) for s, tp in r["A"] if tp]
            for _ in range(r["RB"]):
                seq += [(s, r["rate"], tp) for s, tp in r["B"] if tp]
    out = []
    for s, rt, n in seq:
        if out and out[-1][0] == s and out[-1][1] == rt:
            out[-1] = (s, rt, out[-1][2] + n)
        else:
            out.append((s, rt, n))
    return out


def describe(name: str, e: bytes) -> str:
    t = C.decode_table(e)
    lines = [f"{name}: header {e[:8].hex(' ')}  frames {t.block_frames()}",
             f"   LUTC (vcom, rate, frames): {lutc_timeline(e)}"]
    for r in lutc_phys(e):
        if r["M"] or r["raw"] != "ff ff ff ff ff ff ff ff ff ff":
            lines.append(f"     row {r['row']} M={r['M']} RA={r['RA']} RB={r['RB']} rate={r['rate']}  {r['raw']}")
    for k in range(8):
        s = C.segments(t, k, keep_swk=True)
        if s:
            lines.append(f"   slot {k}: {s}")
    return "\n".join(lines)
