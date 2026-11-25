"""Decompressor for the compressed waveform data stored in BWRY (JD7966x-class) controllers.

Bitstream, LSB-first. Token: flag bit 0 -> literal (8 bits); flag bit 1 -> match: length (8 bits) + 3, then
distance (14 bits) back into the output.

decode() only transforms bytes handed to it; reading the store off a controller is not part of this module.
"""
from __future__ import annotations


def decode(stream: bytes, start: int = 0, max_out: int = 20000) -> tuple[bytes, list, int]:
    """stream: the compressed bytes. start: first bit. Returns (output, tokens, end bit).
    tokens: ("L", bit, 1) | ("M", bit, length, distance) | ("BAD", bit, distance); decoding stops at a BAD token
    (distance 0 or beyond the output), at max_out bytes, or at the end of the stream."""
    nbits = len(stream) * 8
    pos = start
    out = bytearray()
    toks = []

    def rd(n: int) -> int:
        nonlocal pos
        v = 0
        for i in range(n):
            v |= ((stream[(pos + i) >> 3] >> ((pos + i) & 7)) & 1) << i
        pos += n
        return v

    while pos + 9 <= nbits and len(out) < max_out:
        p0 = pos
        if rd(1) == 0:
            out.append(rd(8))
            toks.append(("L", p0, 1))
            continue
        if pos + 22 > nbits:
            break
        n = rd(8) + 3
        d = rd(14)
        if d == 0 or d > len(out):
            toks.append(("BAD", p0, d))
            pos = p0
            break
        for _ in range(n):
            out.append(out[-d])
        toks.append(("M", p0, n, d))
    return bytes(out), toks, pos


def encode_literals(data: bytes) -> bytes:
    """the trivial encoding (every byte a literal token); for tests and round trips."""
    bits = []
    for b in data:
        bits.append(0)
        bits += [(b >> i) & 1 for i in range(8)]
    bits += [0] * (-len(bits) % 8)
    return bytes(sum(bit << i for i, bit in enumerate(bits[o:o + 8])) for o in range(0, len(bits), 8))
