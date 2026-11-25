"""Codec for the BWRY 535 B register LUT (JD79661-class, cmd 0x20), built on the Spectra 6 row codec (s6_codec).

Layout:
  bytes 0..6    header (rail / VCOM / PLL source bytes: VGHL, VSHL_0, VSH_1, VSL_1, VSHL_1, VCOM, PLL)
  byte  7       M of block 0 row 0
  8 + 88 k      block k, k = 0..5 (block 0 = LUTC/VCOM). 8 rows x 11 B, same row format as Spectra 6.
  The table stops at byte 534: block 5's byte 87 (the next block's row-0 M source) does not exist.
  decode() adds a virtual pad byte 0xEF and encode() drops it again.

Level names: the row codec names the three active-low level bits per phase "S", "W", "K". On BWRY treat them as raw
bit labels; code3() gives the raw 3-bit number (S = 4, W = 2, K = 1).

decode() / encode() are an exact bijection on bytes. build() makes a table from segment lists.
"""
from __future__ import annotations

from collections import Counter
from dataclasses import dataclass

from . import s6_codec as C

LEN = 535
PAD = 0xEF
NBLK = 6
HEADER_FIELDS = ("VGHL", "VSHL_0", "VSH_1", "VSL_1", "VSHL_1", "VCOM", "PLL")


def blk_off(k: int) -> int:
    return 8 + 88 * k


@dataclass
class Table535:
    header: bytes          # 8 bytes (7 header + byte 7 = block-0 row-0 M source)
    blocks: list           # 6 s6_codec.Block
    raw: bytes

    def frames(self) -> list:
        return [b.frames() for b in self.blocks]

    def duration_frames(self) -> int:
        return max(self.frames())

    def segments(self, k: int, merge: bool = True) -> list:
        return self.blocks[k].segments(merge=merge)

    def level_hist(self, k: int) -> Counter:
        h = Counter()
        for lv, n in self.blocks[k].segments(merge=False):
            h[lv] += n
        return h


def code3(lv: str) -> int:
    """raw 3-bit code of a level label (bit set = that switch on): S=4, W=2, K=1. '' = 0, 'SWK' = 7."""
    return (4 if "S" in lv else 0) | (2 if "W" in lv else 0) | (1 if "K" in lv else 0)


def decode(e: bytes) -> Table535:
    e = bytes(e)
    if len(e) != LEN:
        raise ValueError(f"table is {len(e)} B, expected {LEN}")
    p = e + bytes([PAD])
    return Table535(p[:8], [C.decode_block(p, blk_off(k)) for k in range(NBLK)], e)


def encode(t: Table535) -> bytes:
    out = bytes(t.header)
    for b in t.blocks:
        out += C.encode_block(b)
    if len(out) != LEN + 1:
        raise C.EncodeError(f"encoded {len(out)} B")
    return out[:LEN]


def blank_block() -> "C.Block":
    """all 8 rows zero time: the block contributes 0 frames."""
    return C.encode_segments([], m_in=1, tail_byte=PAD)


def terminate(blk) -> None:
    """every row after the last data row gets M = 0 (byte FF), the end-of-sequence marker. Without it the
    controller does not end the block: a 25-frame table has been seen to run for over 60 s."""
    last = max((i for i, r in enumerate(blk.rows) if r.frames()), default=0)
    for r in blk.rows[last + 1:]:
        r.M = 0


def build(header: bytes, block_segs: dict, n_frames_lutc: int | None = None, fill_unused: bool = True) -> bytes:
    """table from time-ordered (level, frames) lists per block index 0..5.
    header: the 7 header bytes (byte 7 is forced to EF = M 1, required by the canonical encoder).
    n_frames_lutc: if given and block 0 is not specified, block 0 = level '' for that many frames.
    fill_unused: blocks not given get level '' for the full table duration (stock tables never have an empty block).
    Rows after each block's data get M = 0 (terminate())."""
    segs_by = {k: list(v) for k, v in block_segs.items()}
    if 0 not in segs_by and n_frames_lutc:
        segs_by[0] = [("", n_frames_lutc)]
    dur = max([sum(n for _, n in v) for v in segs_by.values()] or [0])
    if fill_unused and dur:
        for k in range(NBLK):
            segs_by.setdefault(k, [("", dur)])
    hdr = bytes(header[:7]) + bytes([0xEF])
    blocks = []
    for k in range(NBLK):
        b = C.encode_segments(segs_by.get(k) or [], m_in=1, tail_byte=0xEF if k < NBLK - 1 else PAD)
        terminate(b)
        blocks.append(b)
    e = encode(Table535(hdr, blocks, b""))
    d = decode(e)                                      # self-check: frames as asked + terminator present
    for k in range(NBLK):
        want = sum(n for _, n in (segs_by.get(k) or []))
        rows = d.blocks[k].rows
        last = max((i for i, r in enumerate(rows) if r.frames()), default=0)
        if d.blocks[k].frames() != want or not (last == 7 or rows[last + 1].M == 0):
            raise C.EncodeError(f"block {k}: self-check failed")
    return e


def describe(e: bytes, name: str = "") -> str:
    t = decode(e)
    lines = [f"{name} header " + " ".join(f"{f}={b:02X}" for f, b in zip(HEADER_FIELDS, t.header[:7])),
             f"  frames per block {t.frames()} -> duration {t.duration_frames()}"]
    for k in range(NBLK):
        h = t.level_hist(k)
        lines.append(f"  block {k}: " + ", ".join(f"{lv or '0V'}(c{code3(lv)})={n}" for lv, n in
                                                  sorted(h.items(), key=lambda x: -x[1])))
    return "\n".join(lines)
