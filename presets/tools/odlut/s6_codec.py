"""Codec between Spectra 6 0x20 register-LUT images (800 B) and segment lists.

Format (same layout on the 7.09" dual-controller panel and the 7.3" panel):
  bytes 0..7    header. byte 0 = FF, bytes 1..4 = weak-rail codes (hypothesis: SK, W, WK, SW),
                bytes 5..6 constant (20, 8 decoded), byte 7 = M of LUTC row 0 (EF = 1).
  bytes 8..95   LUTC block (8 rows x 11 B).
  96 + 88 k     slot k = pixel code k (8 rows x 11 B). Codes 4 and 7 are hardwired hold on the 7.09".
Row (11 B): b0, b1 level bits | b2 b3 b4 TP A1..A3 | b5 R_A | b6 b7 b8 TP B1..B3 | b9 R_B | b10 = M of the NEXT row.
  Row 0's M is the byte before the block (header byte 7 / previous block's byte 87).
  Time order of a row: repeat M x [ R_A x (A1 A2 A3) then R_B x (B1 B2 B3) ].
Byte coding: active-low, scrambled binary: value = 1 b4 + 2 b7 + 4 b6 + 8 b1 + 16 b0 + 32 b3 + 64 b2 + 128 b5.
  In b4 and b8, bit 5 is the K level bit of A3 / B3, not part of TP (TP max 127 there).
Level bits (active low, 0 = on): per phase S (strong +), W (weak +), K (negative); SWK = no drive.
  Levels: "" (0 V), S, W, SW, K, SK, WK.

Every bit of a block is meaningful, so decode_table <-> encode_table is an exact bijection on bytes.
Segment lists (time-ordered (level, frames)) are lossy; encode_segments gives one canonical byte form.
"""
from __future__ import annotations

from dataclasses import dataclass, field
import hashlib

BIT_W = {4: 1, 7: 2, 6: 4, 1: 8, 0: 16, 3: 32, 2: 64, 5: 128}
LEVEL_BITS = {"A1": ((0, 2), (0, 5), (0, 3)), "A2": ((0, 1), (0, 0), (0, 6)), "A3": ((0, 4), (0, 7), (4, 5)),
              "B1": ((1, 2), (1, 5), (1, 3)), "B2": ((1, 1), (1, 0), (1, 6)), "B3": ((1, 4), (1, 7), (8, 5))}
TP_BYTE = {"A1": 2, "A2": 3, "A3": 4, "B1": 6, "B2": 7, "B3": 8}
PHASES = ("A1", "A2", "A3", "B1", "B2", "B3")
LEVELS = ("", "S", "W", "SW", "K", "SK", "WK")
LUTC_OFF = 8
TABLE_LEN = 800
HOLD_CODES_709 = (4, 7)
DRIVEN_CODES = (0, 1, 2, 3, 5, 6)

# frame time per PLL value (ms) on the 7.09" panel; s6_charge.PANELS holds the full per-panel maps.
FRAME_MS = {"04": 13.5, "05": 11.90}
REFRESH_OVERHEAD_MS = 69.0

# set-4 charge weights (strong-rail units) and the header-rail hypothesis
WT_SET4 = {"": 0.0, "S": 1.0, "K": -1.002, "W": 0.440, "SW": 0.574, "SK": -0.373, "WK": -0.518}
RAIL_BYTE = {"SK": 1, "W": 2, "WK": 3, "SW": 4}


def slot_offset(k: int) -> int:
    return 96 + 88 * k


def val(x: int, skip5: bool = False) -> int:
    v = (~x) & 0xFF
    return sum(w for b, w in BIT_W.items() if (v >> b) & 1 and not (skip5 and b == 5))


def enc(v: int, skip5: bool = False) -> int:
    """inverse of val for the value bits (bit 5 left at 1 = off when skip5)."""
    assert 0 <= v <= (127 if skip5 else 255), v
    x = 0xFF
    for b, w in BIT_W.items():
        if skip5 and b == 5:
            continue
        if v & w:
            x &= ~(1 << b) & 0xFF
    return x


@dataclass
class Phase:
    level: str
    tp: int


@dataclass
class Row:
    M: int                 # repeat of the whole row (from the previous byte)
    RA: int
    RB: int
    phases: list           # 6 Phase, order A1 A2 A3 B1 B2 B3

    def frames(self) -> int:
        a = sum(p.tp for p in self.phases[:3])
        b = sum(p.tp for p in self.phases[3:])
        return self.M * (self.RA * a + self.RB * b)

    def segments(self) -> list:
        out = []
        for _ in range(self.M):
            for _ in range(self.RA):
                out += [(p.level, p.tp) for p in self.phases[:3] if p.tp]
            for _ in range(self.RB):
                out += [(p.level, p.tp) for p in self.phases[3:] if p.tp]
        return out


@dataclass
class Block:
    rows: list             # 8 Row
    m_in: int              # M of row 0 (byte before the block), decoded value
    tail_byte: int         # raw byte 87 (= M source of the next block), kept verbatim

    def segments(self, merge: bool = True) -> list:
        s = [x for r in self.rows for x in r.segments()]
        return merge_segments(s) if merge else s

    def frames(self) -> int:
        return sum(r.frames() for r in self.rows)


@dataclass
class Table:
    header: bytes
    lutc: Block
    slots: list            # 8 Block
    raw: bytes = field(repr=False, default=b"")

    @property
    def sha(self) -> str:
        return hashlib.sha1(self.raw).hexdigest()[:12]

    def rails(self) -> dict:
        """header weak-rail codes and the hypothesised charge weights (0.200 + 0.00665 code)."""
        out = {}
        for lv, b in RAIL_BYTE.items():
            code = val(self.header[b])
            w = 0.200 + 0.00665 * code
            out[lv] = {"code": code, "weight": round(w, 4), "volt_est": round(15 * w, 2),
                       "volt": round(3.0 + 0.1 * code, 2)}      # 3.0 V + 0.1 V per code
        return out

    def weights(self, source: str = "set4") -> dict:
        if source == "set4":
            return dict(WT_SET4)
        r = self.rails()
        w = {"": 0.0, "S": 1.0, "K": -1.0}
        w.update({"W": r["W"]["weight"], "SW": r["SW"]["weight"], "SK": -r["SK"]["weight"], "WK": -r["WK"]["weight"]})
        return w

    def block_frames(self) -> dict:
        d = {"LUTC": self.lutc.frames()}
        d.update({k: s.frames() for k, s in enumerate(self.slots)})
        return d

    def refresh_frames(self, codes=DRIVEN_CODES) -> int:
        """frames until the refresh ends: max over the LUTC and the slots in use."""
        return max([self.lutc.frames()] + [self.slots[c].frames() for c in codes])

    def refresh_ms(self, pll: str = "04", codes=DRIVEN_CODES) -> float:
        return REFRESH_OVERHEAD_MS + FRAME_MS[pll] * self.refresh_frames(codes)

    def charge(self, k: int, weights=None) -> float:
        w = weights or WT_SET4
        return sum(w[lv] * n for lv, n in self.slots[k].segments(merge=False))


def merge_segments(s: list) -> list:
    out = []
    for lv, n in s:
        if n == 0:
            continue
        if out and out[-1][0] == lv:
            out[-1] = (lv, out[-1][1] + n)
        else:
            out.append((lv, n))
    return out


# ----------------------------------------------------------------- decode
def _level(b: bytes, ph: str) -> str:
    lv = "".join(n for n, (by, bit) in zip("SWK", LEVEL_BITS[ph]) if not (b[by] >> bit) & 1)
    return "" if lv == "SWK" else lv


def _level_raw(b: bytes, ph: str) -> str:
    """level bits incl. the SWK (= 0 V) combination, needed for an exact byte round trip."""
    return "".join(n for n, (by, bit) in zip("SWK", LEVEL_BITS[ph]) if not (b[by] >> bit) & 1)


def decode_block(e: bytes, o: int) -> Block:
    rows = []
    for r in range(8):
        b = e[o + 11 * r:o + 11 * r + 11]
        M = val(e[o + 11 * r - 1])
        ph = [Phase(_level_raw(b, p), val(b[TP_BYTE[p]], skip5=TP_BYTE[p] in (4, 8))) for p in PHASES]
        rows.append(Row(M, val(b[5]), val(b[9]), ph))
    return Block(rows, val(e[o - 1]), e[o + 87])


def decode_table(e: bytes) -> Table:
    e = bytes(e)
    assert len(e) == TABLE_LEN, len(e)
    return Table(e[:8], decode_block(e, LUTC_OFF), [decode_block(e, slot_offset(k)) for k in range(8)], e)


def norm_level(lv: str, keep_swk: bool = False) -> str:
    """canonical level name. SWK (all three switches on) is merged with "" (none on) unless keep_swk:
    "" is driven to 0 V, SWK leaves the source floating. No stock table contains an SWK frame."""
    if lv == "SWK":
        return "SWK" if keep_swk else ""
    return "".join(c for c in "SWK" if c in lv)


def segments(e_or_t, k, merge=True, keep_swk=False) -> list:
    """time-ordered (level, frames) for slot k ('LUTC' for the LUTC block). keep_swk: see norm_level."""
    t = e_or_t if isinstance(e_or_t, Table) else decode_table(e_or_t)
    blk = t.lutc if k == "LUTC" else t.slots[k]
    s = [(norm_level(lv, keep_swk), n) for lv, n in blk.segments(merge=False)]
    return merge_segments(s) if merge else s


# ----------------------------------------------------------------- encode (exact)
def _encode_row(row: Row, next_M_byte: int) -> list:
    b = [0xFF] * 11
    for p, ph in zip(PHASES, row.phases):
        for n, (by, bit) in zip("SWK", LEVEL_BITS[p]):
            if n in ph.level:
                b[by] &= ~(1 << bit) & 0xFF
    for p, ph in zip(PHASES, row.phases):
        i = TP_BYTE[p]
        sk = i in (4, 8)
        x = enc(ph.tp, skip5=sk)
        if sk:
            x = (x & 0xDF) | (b[i] & 0x20)       # keep the level bit already placed in bit 5
        b[i] = x
    b[5], b[9] = enc(row.RA), enc(row.RB)
    b[10] = next_M_byte
    return b


def encode_block(blk: Block) -> bytes:
    out = []
    for r, row in enumerate(blk.rows):
        nxt = enc(blk.rows[r + 1].M) if r < 7 else blk.tail_byte
        out += _encode_row(row, nxt)
    return bytes(out[:87] + [blk.tail_byte])


def encode_table(t: Table) -> bytes:
    """header verbatim (byte 7 = LUTC row-0 M); each block's byte 87 = the next block's row-0 M source."""
    e = bytes(t.header) + encode_block(t.lutc)
    for s in t.slots:
        e += encode_block(s)
    return e


# ----------------------------------------------------------------- encode (canonical from segments)
class EncodeError(ValueError):
    pass


def _phases_for(lv: str, n: int):
    """split one segment into (level, tp, repeat) chunks for a row: TP <= 127 keeps every phase usable."""
    out = []
    while n > 0:
        if n <= 127:
            out.append((lv, n, 1))
            break
        R = min(255, n // 127)
        out.append((lv, 127, R))
        n -= 127 * R
    return out


def encode_segments(segs: list, m_in: int = 1, tail_byte: int = 0xEF) -> Block:
    """canonical Block for a time-ordered segment list. Row M = 1 everywhere (needs m_in == 1).
    Each row uses only its A group: [A1 A2 A3] x R_A; long single-level runs use R_A > 1 with one phase."""
    if m_in != 1:
        raise EncodeError("canonical encoder needs M = 1 on row 0 (byte before the block = EF)")
    rows = []
    pend = []                       # up to 3 single-repeat phases waiting for a row

    def flush():
        while pend:
            ph = pend[:3]
            del pend[:3]
            rows.append(Row(1, 1, 0, [Phase(lv, n) for lv, n in ph] + [Phase("", 0)] * (6 - len(ph))))

    for lv, n in merge_segments([(norm_level(a, keep_swk=True), b) for a, b in segs]):  # "SWK" in -> SWK out
        for l2, tp, R in _phases_for(lv, n):
            if R == 1:
                pend.append((l2, tp))
            else:
                flush()
                rows.append(Row(1, R, 0, [Phase(l2, tp)] + [Phase("", 0)] * 5))
    flush()
    if len(rows) > 8:
        raise EncodeError(f"needs {len(rows)} rows > 8")
    while len(rows) < 8:
        rows.append(Row(1, 0, 0, [Phase("", 0)] * 6))
    return Block(rows, m_in, tail_byte)


def with_slot(e: bytes, k: int, blk: Block) -> bytes:
    """replace slot k's bytes 0..86 (byte 87 = next block's M source is kept)."""
    e = bytearray(e)
    o = slot_offset(k)
    if val(e[o - 1]) != blk.rows[0].M:
        raise EncodeError("row-0 M of the block differs from the byte before it")
    e[o:o + 87] = encode_block(blk)[:87]
    return bytes(e)
