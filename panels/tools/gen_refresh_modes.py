#!/usr/bin/env python3
"""Experimental refresh modes from the lab handover, as drive data on top of the stock presets.

Writes panels/generated/refresh_modes.json and tests/host/refresh_modes.inc (the framed configs
the host replay test drives). Each panel entry is the stock preset's packets with:

  - the 0xB1 tail v1 (drive key/rev, budget_limit, lut_family, busy_timeout_s) and LUT_SUPPORTED,
  - extra 0xAE modes, each with the 0xAE tail v1 (EXPERIMENTAL, budget_cost, scrub_before),
  - their waveforms as deflated 0xAF slots (bit 7, raw deflate, 10-bit window).

Every register sequence is the stream the lab drove on glass, transcribed; the source of each
is named next to it. Rails, VCOM and PLL are the values each table was gated with (lab
manifests); nothing here leaves the stock envelope the lab used. Mode indexes 0/1 stay the
stock modes, so a host that ignores the tails keeps its defaults.

    python3 panels/tools/gen_refresh_modes.py          # rewrite
    python3 panels/tools/gen_refresh_modes.py --check  # exit 1 when an output is stale
"""
from __future__ import annotations

import json
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import pdlib as P  # noqa: E402

REPO = HERE.parents[1]
PANELS = REPO / "panels"
WAVES = REPO / "panels" / "waves"
PRESETS = REPO / "panels" / "generated" / "drive_presets.json"
OUT_JSON = REPO / "panels" / "generated" / "refresh_modes.json"
OUT_INC = REPO / "tests" / "host" / "refresh_modes.inc"
EE02_INC = REPO / "tests" / "host" / "ee02_spectra_preset.inc"

FAMILY_S6, FAMILY_JD = 1, 2
ADVISORY, SUBSTITUTE = 0, 1
# Every experimental mode is enforced on the device to this window (decision 2026-11-25): outside
# it the device runs mode 0, the stock full, instead (temp_policy SUBSTITUTE). A mode's own lab
# band narrows it; mode 0 keeps no window, so the fallback is always there.
CUSTOM_WINDOW_C = (15, 30)
EXPERIMENTAL = 0x01
NO_SCRUB = 0xFF
DRIVE_REV = 1


def deflate(data: bytes) -> bytes:
    c = zlib.compressobj(9, zlib.DEFLATED, -10, 9)
    return c.compress(data) + c.flush()


def wave(name: str) -> bytes:
    return (WAVES / name).read_bytes()


def lut_packet(slot: int, data: bytes, compress: bool) -> bytes:
    if not compress:
        return struct.pack("<BBH", 0, slot, P.crc16_ccitt(data)) + data
    z = deflate(data)
    assert zlib.decompress(z, -10) == data
    return struct.pack("<BBH", 0, 0x80 | slot, P.crc16_ccitt(z)) + z


def mode_tail(cost: int, scrub_before: int = NO_SCRUB, hold_mask: int | None = None,
              held_k_code: int | None = None) -> bytes:
    tail = bytes([1, EXPERIMENTAL, cost, scrub_before])
    if hold_mask is not None:
        tail += bytes([hold_mask])
        if held_k_code is not None:
            tail += bytes([held_k_code])
    return tail


# 0xAE tail [4] hold_mask: bit n = frame code n leaves the pixel as it is. The host sends the
# lowest one for every pixel unchanged against its prior frame, so only changed pixels drive.
# Keyed by (panel, mode name): names repeat across panels with different code maps.
HOLD_MASK = {
    ("ep29yr_bwry", "partial_bw"): 0b1100,  # 0 = to K, 1 = to W, 2/3 hold
    ("ep75yr_bwry", "partial_bw"): 0b1100,  # 0 = to K, 1 = to W, 2/3 hold
    ("ep35yr_bwry", "partial_bw"): 0b1100,  # 0 = to K, 1 = to W, 2/3 hold
    ("ep42yr_bwry", "partial_bw"): 0b1100,  # 0 = to K, 1 = to W, 2/3 hold
    ("ep154yr_bwry", "partial_bw"): 0b1100,  # 0 = to K, 1 = to W, 2/3 hold
    ("ep266yr_bwry", "partial_bw"): 0b1000,  # 0 = to K, 1 = to W, 2 = held K top-up, 3 hold
    ("s73_spectra6", "partialcool"): 1 << 4,  # code 4 = hold (R-0234)
    ("ee02_709_spectra6", "changedonly"): (1 << 4) | (1 << 7),  # 4/7 hardwired holds (R-0032)
    ("ee02_709_spectra6", "partial4ink"): (1 << 4) | (1 << 7),  # codes 4/7 hold (R-0240)
}


# 0xAE tail [5] held_k_code (provisional, after hold_mask): the code to send for an unchanged
# pixel whose prior code is 0 (black), in place of the lowest hold code. It drives a DC-balanced
# W|K pulse ending on K that re-darkens black faded by the per-update rail-up and reset; a code
# outside hold_mask, so a host that ignores the field sends the plain hold and black fades.
HELD_K_CODE = {
    ("ep266yr_bwry", "partial_bw"): 2,
}


def panel_tail(key: int, budget: int, family: int, timeout_s: int) -> bytes:
    return struct.pack("<BHHBBBB", 1, key, DRIVE_REV, 0, budget, family, timeout_s)


def cmd(c: int, *data: int) -> tuple:
    return ("cmd", c, bytes(data))


def hexcmd(s: str) -> tuple:
    b = bytes.fromhex(s)
    return ("cmd", b[0], b[1:])


BUSY, IMAGE, LUT = ("busy",), ("image",), ("lut",)


def replace_cmd(steps: list[tuple], c: int, data: bytes) -> list[tuple]:
    out, hit = [], 0
    for s in steps:
        if s[0] == "cmd" and s[1] == c:
            out.append(("cmd", c, data)); hit += 1
        else:
            out.append(s)
    assert hit, f"no {c:#04x} in sequence"
    return out


def base_packets(key: str) -> list[tuple[int, bytes]]:
    d = json.loads(PRESETS.read_text())["panels"][key]
    return [(pk["id"], bytes.fromhex(pk["body_hex"])) for pk in d["packets"]]


def mode_seq(pkts: list[tuple[int, bytes]], index: int) -> list[tuple]:
    for pid, b in pkts:
        if pid == 0xAE and b[1] == index:
            n = b[35] | b[36] << 8
            return P.walk_seq(b[37:37 + n])
    raise KeyError(index)


def retail_panel(body: bytes, tail: bytes, mode_count: int) -> bytes:
    """Stock 0xB1 + LUT_SUPPORTED + the v1 tail; mode_count = highest index + 1."""
    sl = body[12]
    head = bytearray(body[:13 + sl])
    head[1] |= 0x02
    head[11] = mode_count
    return bytes(head) + tail


# ------------------------------------------------------------------------------- 7.3in S6 (0x0023)
# Product cycle of lab fw s73prod (ideas73.py product_cycle mode "prod", R-0222): the stock
# register block without PON, the custom registers, data, the 0x20 table, the custom registers
# again, PON, DRF, POF 02 00 (the 0xB1 sleep sequence).
S73_INIT_NOPON = ["AA 49 55 20 08 09 18", "01 3F 00 32 2A 0E 2A", "00 5F 69", "03 00 54 00 44",
                  "05 40 1F 1F 2C", "06 6F 1F 16 25", "08 6F 1F 1F 22", "13 00 04", "30 02", "41 00",
                  "50 3F", "60 02 00", "61 03 20 01 E0", "82 1E", "84 01", "86 00", "E3 2F",
                  "E0 00", "E6 00"]
S73_SET6 = "3F 00 1E 18 23 3E"
S73_SET7 = "3F 00 1C 19 27 3E"


def s73_custom(pwr: str, pll: int) -> list[tuple]:
    regs = [hexcmd("00 5F E9"), hexcmd("01 " + pwr), cmd(0x82, 0x0F), cmd(0x30, pll)]
    return ([hexcmd(s) for s in S73_INIT_NOPON] + regs + [cmd(0x10), IMAGE, cmd(0x20), LUT] + regs
            + [cmd(0x04), BUSY, cmd(0x12, 0x00), BUSY])


def build_s73() -> dict:
    key = 0x0023
    pkts = base_packets("0x0023")
    full = mode_seq(pkts, 0)
    pll08 = replace_cmd(full, 0x30, b"\x08")
    luts = {0: "s6_73_replica_set6.bin", 1: "s6_73_replica_set7.bin", 2: "s6_73_F1d6.bin",
            3: "s6_73_F1d7.bin", 4: "s6_73_S3_N0_wgap0.bin", 6: "s6_73_MA_t1.bin", 7: "s6_73_MB_t1.bin"}
    # (index, name, steps, slot, tmin, tmax, cost, note)
    modes = [
        (2, "full_quick", pll08, None, 0, 0, 0,
         "stock waveform, PLL 08 (dynamic rate): 19.2 s vs 22.3 s (R-0013)"),
        (3, "test_cool", s73_custom(S73_SET6, 0x02), 0, -40, 24, 1,
         "upload-path acceptance test: byte-true OTP set 6 at its rails, = stock (R-0186)"),
        (4, "test_warm", s73_custom(S73_SET7, 0x02), 1, 25, 60, 1,
         "upload-path acceptance test: byte-true OTP set 7 at its rails (R-0229)"),
        (5, "faster_cool", s73_custom(S73_SET6, 0x02), 2, -40, 24, 1,
         "F1d6 shake trim on set 6, 16.2 s, band <= 24.5 C (R-0205, R-0231)"),
        (6, "faster_warm", s73_custom(S73_SET7, 0x02), 3, 25, 60, 1,
         "F1d7 shake trim on set 7, 16.2 s, band > 24.5 C (R-0231)"),
        (7, "partialcool", s73_custom(S73_SET6, 0x02), 4, -40, 24, 1,
         "flashing partial S3_N0_wgap0 on set 6, 15.2 s; code 4 = hold (R-0234)"),
        (8, "adaptive", s73_custom(S73_SET6, 0x02), 5, 0, 0, 1,
         "host-built per-update table (N2): needs INLINE_LUT for slot 5 every time (R-0239)"),
        (9, "colour24", s73_custom(S73_SET7, 0x02), [3, 6, 7], 25, 60, 1,
         "3 passes F1d7 -> MA (SKn) -> MB (KSn) on set 7: pure S/K net-0 colour passes (n 1-20, code 4 "
         "holds), about 25 s; 64 distinct colours at dE00 5 (30 at 8) vs 6 stock, camera-provisional, "
         "band > 24.5 C only; the 0xB0 holds a 24-entry maximin palette (closest pair 8.5) in 3-bit codes"),
    ]
    return assemble(key, "0x0023", pkts, modes, luts, budget=10, family=FAMILY_S6, timeout_s=120,
                    cals={9: "palettes/s73_colour24.json"})


# ------------------------------------------------------------------------- 2.9in JD-BWRY (0x001E)
# Windowed partial on ODF4DA34 (oneway_20261124/followups/bwry29/odf/odf_counter.py update(),
# R-0237/R-0238): RST, JD init with LUT_EN and the table's own header rails, 0x20 table, 0x83
# window, data, CDI 97, PON, DRF, POF 02 00 + DSLP (0xB1 sleep). The window here is the whole
# panel (BG22 has no WINDOW escape): codes 0 = to K, 1 = to W, 2/3 = hold.
def jd29_partial(hdr: bytes) -> list[tuple]:
    w, h = 168, 384
    return [hexcmd("4D 78"), hexcmd("00 0F A9"), ("cmd", 0x01, bytes([0x07]) + hdr[0:5]),
            hexcmd("03 10 54 44"), hexcmd("06 05 00 3F 0A 25 12 1A"), cmd(0x30, hdr[6]),
            hexcmd("50 37"), hexcmd("60 02 02"), cmd(0x61, 0x00, w, h >> 8, h & 0xFF),
            hexcmd("E7 1C"), hexcmd("E3 22"), hexcmd("B4 D0"), hexcmd("B5 03"), hexcmd("E9 01"),
            cmd(0x82, hdr[5] ^ 0x80), cmd(0x20), LUT,
            cmd(0x83, 0x00, 0x00, 0x00, w - 1, 0x00, 0x00, (h - 1) >> 8, (h - 1) & 0xFF, 0x01),
            cmd(0x10), IMAGE, cmd(0x50, 0x97), cmd(0x04), BUSY, cmd(0x12, 0x00), BUSY]


def build_bwry29() -> dict:
    key = 0x001E
    pkts = base_packets("0x001E")
    full = mode_seq(pkts, 0)
    t = wave("bwry29_BW2_kick3_w10.bin")
    modes = [
        (2, "full_quick", replace_cmd(full, 0x30, b"\x07"), None, 0, 0, 0,
         "stock waveform at fixed PLL 07: 9.2 s, ~8 % chroma loss (R-0046)"),
        (3, "partial_bw", jd29_partial(t[:7]), 2, 0, 0, 1,
         "BW2_kick3_w10 (B4 N0) full-window partial: 0 = to K, 1 = to W, 2/3 hold; "
         "product cycle K 3.1 / W 2.9 dE00 at 100 updates (R-0237, R-0238) [1 ex]"),
    ]
    # Slots 0/1 hold the colour16 waves of the stock preset; the partial table takes slot 2.
    # Stored uncompressed: BG22 (the M3 Pro) has no inflate cache, so a deflated slot reads absent.
    return assemble(key, "0x001E", pkts, modes, {2: "bwry29_BW2_kick3_w10.bin"}, budget=20,
                    family=FAMILY_JD, timeout_s=120, compress=False)


# ------------------------------------------------------------- 3.5in / 4.2in JD-BWRY (0x0036, 0x003A)
# The 2.9in partial cycle on a panel's own colour16 init: rails, PLL and VCOM come from the table
# header (analog_from_wave), the table goes in before the frame, then a full-panel 0x83 window
# (BG22), data, CDI 97 and the init's own PON/DRF tail.
def jd_partial(colour16: list[tuple], w: int, h: int) -> list[tuple]:
    i = next(k for k, s in enumerate(colour16) if s[0] == "lut") + 1
    j = next(k for k, s in enumerate(colour16) if s[0] == "image") + 1
    assert colour16[i] == cmd(0x10) and colour16[j] == cmd(0x04), (colour16[i], colour16[j])
    win = cmd(0x83, 0x00, 0x00, (w - 1) >> 8, (w - 1) & 0xFF, 0x00, 0x00, (h - 1) >> 8, (h - 1) & 0xFF, 0x01)
    return colour16[:i] + [win] + colour16[i:j] + [cmd(0x50, 0x97)] + colour16[j:]


def build_bwry35() -> dict:
    key = 0x0036
    pkts = base_packets("0x0036")
    modes = [
        (2, "partial_bw", jd_partial(mode_seq(pkts, 1), 184, 384), 2, 0, 0, 1,
         "BW2_kick3_w10 (the 2.9in table) full-window flashing partial: 0 = to K, 1 = to W, "
         "2/3 hold; 3.1 s per update over BLE; counter 00-05 on OD507E22 with clean white, judged "
         "by eye, not measured [1 ex, prov]"),
    ]
    return assemble(key, "0x0036", pkts, modes, {2: "bwry29_BW2_kick3_w10.bin"}, budget=20,
                    family=FAMILY_JD, timeout_s=120, compress=False)


def build_bwry266() -> dict:
    key = 0x001C
    pkts = base_packets("0x001C")
    modes = [
        (2, "partial_bw", jd_partial(mode_seq(pkts, 1), 184, 360), 2, 0, 0, 1,
         "BW2_kick3_w10 plus a held-K top-up: full-window flashing partial, 0 = to K, 1 = to W, "
         "2 = unchanged K (W 4 | K 4, net 0, ends on K), 3 hold. Without the top-up held K "
         "fades a step per update from the rail-up/reset before the first command. 3.2 s per "
         "update; counter 00-10 on OD6FACF2 with K held, judged by eye, not measured [1 ex, prov]"),
    ]
    return assemble(key, "0x001C", pkts, modes, {2: "bwry266_BW2_kick3_w10_hk4.bin"}, budget=20,
                    family=FAMILY_JD, timeout_s=120, compress=False)


def build_bwry154() -> dict:
    key = 0x0038
    pkts = base_packets("0x0038")
    modes = [
        (2, "partial_bw", jd_partial(mode_seq(pkts, 1), 200, 200), 2, 0, 0, 1,
         "BW2_kick3_w10 (the 2.9in table) full-window flashing partial: 0 = to K, 1 = to W, "
         "2/3 hold; 2.9 s per update over BLE; counter 00-05 on OD1F9C0E with clean white, judged "
         "by eye, not measured [1 ex, prov]"),
    ]
    return assemble(key, "0x0038", pkts, modes, {2: "bwry29_BW2_kick3_w10.bin"}, budget=20,
                    family=FAMILY_JD, timeout_s=120, compress=False)


def build_bwry42() -> dict:
    key = 0x003A
    # BUSY polled, 35 s fixed delay kept as the cap: the stock record waits the fixed delay on
    # every @busy. Polling was A/B'd against the fixed delay on OD01AAA9 (2026-11-19, i1Pro:
    # 0.05-0.24 dE00, refresh not truncated).
    pkts = [(pid, bytes([b[0], b[1] | 0x01]) + b[2:] if pid == 0xB1 else b) for pid, b in base_packets("0x003A")]
    modes = [
        (2, "partial_bw", jd_partial(mode_seq(pkts, 1), 400, 300), 2, 0, 0, 1,
         "BW2_kick3_w10 (the 2.9in table) full-window flashing partial: 0 = to K, 1 = to W, "
         "2/3 hold; 5.5 s per update over BLE (2 s fixed settle after PON); counter 00-05 on "
         "OD01AAA9 with clean white, judged by eye, not measured [1 ex, prov]"),
    ]
    return assemble(key, "0x003A", pkts, modes, {2: "bwry29_BW2_kick3_w10.bin"}, budget=20,
                    family=FAMILY_JD, timeout_s=120, compress=False)


# ------------------------------------------------------------------------- 7.5in F52-BWRY (0x0040)
# Windowed partial on ODBF064E, the sequence of Firmware_Silabs scripts/f75_counter.py: RST, the
# stock colour16 init (LUT_EN, PON inside, booster tail, table after the tail: the only placement
# this panel honours), PLL 02 (the table is built for 20 ms frames; the init sets 08), 0x83
# window, data, DRF, then the 0xB1 sleep. The window is the whole panel (BG22 has no WINDOW
# escape): codes 0 = to K, 1 = to W, 2/3 = hold.
# The table is the 2.9in BW2_kick3_w10 recipe with 20-frame white runs (followups/bwry29
# b4_tables.build(0, wrun=20), DC rebalanced): w10 left grey on this glass. Only one partial
# table fits a BG22 config next to the two colour16 waves (3120 B cap).
def f75_partial(colour16: list[tuple]) -> list[tuple]:
    w, h = 800, 480
    i = next(k for k, s in enumerate(colour16) if s[0] == "lut") + 1
    assert colour16[i] == cmd(0x10), colour16[i]
    win = cmd(0x83, 0x00, 0x00, (w - 1) >> 8, (w - 1) & 0xFF, 0x00, 0x00, (h - 1) >> 8, (h - 1) & 0xFF, 0x01)
    return colour16[:i] + [cmd(0x30, 0x02), win] + colour16[i:]


def build_bwry75() -> dict:
    key = 0x0040
    pkts = base_packets("0x0040")
    modes = [
        (2, "partial_bw", f75_partial(mode_seq(pkts, 1)), 2, 0, 0, 1,
         "BW2_kick3_w20 (2.9in recipe, 20-frame white runs) full-window flashing partial: 0 = to K, "
         "1 = to W, 2/3 hold; about 2.4 s DRF, 7.3 s per update over BLE; counter 00-10 on ODBF064E "
         "with clean white, judged by eye, not measured [1 ex, prov]"),
    ]
    # Slots 0/1 hold the colour16 waves; stored uncompressed for BG22 (no inflate cache).
    return assemble(key, "0x0040", pkts, modes, {2: "bwry75_BW2_kick3_w20.bin"}, budget=20,
                    family=FAMILY_JD, timeout_s=120, compress=False)


# ---------------------------------------------------------------------------- 7.09in S6 (EE02)
# The lab-order stream (tests/host/ee02_spectra_preset.inc k_lab_order_seq, from
# ee02_spectra709/src/main.cpp) with PSR DF E9, the table's PWR on the main chip, PLL, and the
# 0x20 table on both chips before CCSET (handover 3.7). Not 0x0042 (shared with the 13.3in):
# these modes ride on the EE02 lab-order mode 0, so the drive key is 0 (not from a preset).
def ee02_lab_seq() -> bytes:
    txt = EE02_INC.read_text()
    a = txt.index("k_lab_order_seq[] = {")
    body = txt[txt.index("{", a) + 1:txt.index("};", a)]
    return bytes(int(x, 16) for x in body.replace(",", " ").split())


def before_ccset(steps: list[tuple], extra: list[tuple]) -> list[tuple]:
    """`extra` (ending on cs 0) right before the first CCSET (0xE0), where the lab staged it."""
    i = next(k for k, s in enumerate(steps) if s[0] == "cmd" and s[1] == 0xE0)
    while i > 0 and steps[i - 1][0] in ("cs", "delay"):
        i -= 1
    return steps[:i] + extra + steps[i:]


def s709_custom(pwr: str, pll: int) -> list[tuple]:
    out = []
    for s in P.walk_seq(ee02_lab_seq()):
        if s[0] == "cmd" and s[1] == 0x00:
            s = ("cmd", 0x00, bytes([0xDF, 0xE9]))
        elif s[0] == "cmd" and s[1] == 0x01:
            s = ("cmd", 0x01, bytes.fromhex(pwr))
        out.append(s)
    out = before_ccset(out, [("cs", 0), cmd(0x30, pll), cmd(0x20), LUT, ("cs", 1), cmd(0x20), LUT,
                             ("cs", 0)])
    assert sum(1 for x in out if x[0] == "lut") == 2
    assert sum(1 for x in out if x[0] == "cmd" and x[1] == 0x00 and x[2] == b"\xdf\xe9") == 2
    return out


def build_ee02() -> dict:
    key = 0x0000            # not a preset key: mode 0 is the EE02 lab-order stream, not 0x0042's
    stock = P.walk_seq(ee02_lab_seq())
    pll04 = before_ccset(stock, [("cs", 0), cmd(0x30, 0x04)])
    modes = [
        (1, "full_quick", pll04, None, 0, 0, 0,
         "stock waveform at PLL 04: 22.6 s vs 30.2 s, max dE 2.7 (R-0032)"),
        (2, "changedonly", stock, None, 0, 0, 1,
         "stock waveform; pixel codes 4 and 7 are hardwired holds on this chip, so the host "
         "writes 4 for unchanged pixels (R-0032) [1 ex]"),
        (3, "faster_cool", s709_custom("0F 00 1A 24 30 38", 0x08), 0, -40, 25, 1,
         "G13 on set 4, 17.5 s, worst 2.04; scrub every ~10 (R-0204, R-0221)"),
        (4, "partial4ink", s709_custom("0F 00 1A 22 24 26", 0x04), 1, 0, 0, 1,
         "G7_N8_mixB flashing partial, 11.5 s, band-robust (sets 4 and 5); K/W/Y/B targets only "
         "(no R/G drive: an R or G cell keeps its old ink); codes 4/7 hold (R-0240, R-0242) [1 ex]"),
        (5, "colour32", s709_custom("0F 00 1A 24 30 38", 0x08), [0, 2, 3], -40, 25, 1,
         "32 colours in 3 passes, G13 -> PS_best_A -> MIXg on set 4 (PLL-04 tables carry a rate-4 "
         "LUTC), about 35 s; 74 distinct at dE00 5 (38 at 8) vs 6 stock, camera-provisional; 0xB0 = "
         "32-entry maximin palette in 3-bit codes (T-115, R-0244)"),
        (6, "colour20", s709_custom("0F 00 1A 24 30 38", 0x08), [0, 2], -40, 25, 1,
         "20 colours in 2 passes, G13 -> PS_best_A on set 4, about 27 s; 29 distinct at dE00 5 (19 at "
         "8) vs 6 stock, camera-provisional; 0xB0 = 20-entry maximin palette in 3-bit codes (R-0244)"),
    ]
    luts = {0: "s6_709_G13.bin", 1: "s6_709_G7_N8_mixB.bin", 2: "s6_709_PSA_r4.bin",
            3: "s6_709_MIXg_r4.bin"}
    # Base: the 0x0042 packets with mode 0 replaced by the lab-order stream.
    pkts = []
    for pid, b in base_packets("0x0042"):
        if pid == 0xAE and b[1] == 0:
            seq = ee02_lab_seq()
            b = b[:35] + struct.pack("<H", len(seq)) + seq
        pkts.append((pid, b))
    return assemble(key, "0x0042", pkts, modes, luts, budget=10, family=FAMILY_S6, timeout_s=120,
                    cals={5: "palettes/ee02_709_colour32.json", 6: "palettes/ee02_709_colour20.json"})


# ------------------------------------------------------------------------------------- assembly
name_of = {"0x0023": "s73_spectra6", "0x001E": "ep29yr_bwry", "0x0036": "ep35yr_bwry", "0x001C": "ep266yr_bwry", "0x0038": "ep154yr_bwry", "0x003A": "ep42yr_bwry", "0x0040": "ep75yr_bwry",
           "0x0042": "ee02_709_spectra6"}

def assemble(key: int, base: str, pkts, modes, luts, *, budget: int, family: int,
             timeout_s: int, compress: bool = True, cals=None) -> dict:
    out_pkts, extra = [], []
    top = max(m[0] for m in modes) + 1
    for pid, b in pkts:
        if pid == 0xB1:
            b = retail_panel(b, panel_tail(key, budget, family, timeout_s), top)
        out_pkts.append((pid, b))
    for idx, name, steps, slot, tmin, tmax, cost, note in modes:
        tmin, tmax = P.custom_window(tmin, tmax, CUSTOM_WINDOW_C)
        seq = P.encode_steps(steps)
        # A list of slots is a multi-pass mode: pass n uploads slots[n]; the sequence runs once per pass.
        slots = list(slot) if isinstance(slot, (list, tuple)) else [slot] if slot is not None else None
        body = P.mode_body(idx, name, seq, slots=slots, passes=len(slots) if slots else 1,
                           tmin=tmin, tmax=tmax, policy=SUBSTITUTE) + mode_tail(
            cost, hold_mask=HOLD_MASK.get((name_of[base], name)),
            held_k_code=HELD_K_CODE.get((name_of[base], name)))
        out_pkts.append((0xAE, body))
        extra.append({"mode_index": idx, "name": name, "experimental": True,
                      "experimental_note": note, "budget_cost": cost, "lut_slot": slot,
                      "passes": len(slots) if slots else 1,
                      "hold_mask": HOLD_MASK.get((name_of[base], name)),
                      "held_k_code": HELD_K_CODE.get((name_of[base], name)),
                      "temp_window_c": [tmin, tmax], "temp_policy": "substitute",
                      "seq_steps": len(steps) + 1, "seq_bytes": len(seq)})
    lut_meta = []
    for slot, fn in sorted(luts.items()):
        data = wave(fn)
        body = lut_packet(slot, data, compress)
        out_pkts.append((0xAF, body))
        lut_meta.append({"slot": slot, "compressed": compress, "wave": fn, "raw_bytes": len(data),
                         "stored_bytes": len(body) - 4})
    cal_meta = []
    # cals: {mode_index: palette JSON path}; the 0xB0 a host needs to encode that multi-pass mode.
    for idx, fn in sorted((cals or {}).items()):
        pal = json.loads((PANELS / fn).read_text())
        entries = [(tuple(e["codes"]), tuple(e["lab"])) for e in pal["entries"]]
        out_pkts.append((0xB0, P.cal_body(idx, entries, pal["measured"], code_bits=pal["code_bits"])))
        cal_meta.append({"mode_index": idx, "palette": fn, "entries": len(entries),
                         "code_bits": pal["code_bits"], "measured": pal["measured"]})
    framed = b"".join(P.frame(i + 1, pid, b) for i, (pid, b) in enumerate(out_pkts))
    return {"name": name_of[base], "key": f"0x{key:04X}", "base_preset": base, "drive_rev": DRIVE_REV,
            "budget_limit": budget, "lut_family": family, "busy_timeout_s": timeout_s,
            "modes": extra, "luts": lut_meta, **({"calibrations": cal_meta} if cal_meta else {}), "config_bytes": len(framed),
            "packets": [{"id": pid, "body_hex": b.hex()} for pid, b in out_pkts],
            "_framed": framed}


def render() -> tuple[str, str]:
    panels = [build_s73(), build_bwry266(), build_bwry29(), build_bwry35(), build_bwry154(), build_bwry42(), build_bwry75(), build_ee02()]
    presets = json.loads(PRESETS.read_text())["panels"]
    js = {"schema_version": 1, "note": "experimental refresh modes; layouts provisional "
          "(docs/REFRESH_MODES.md)", "panels": {}}
    inc = ["/* Generated by panels/tools/gen_refresh_modes.py. Do not edit.", " * Bodies as in "
           "drive_panels.inc (instance byte first); k_rm_modes lists the experimental modes. */",
           ""]
    rows = []
    for i, p in enumerate(panels):
        p.pop("_framed")
        js["panels"][p["name"]] = p
        base = presets[p["base_preset"]]
        refs = []
        for j, pk in enumerate(p["packets"]):
            b = bytes.fromhex(pk["body_hex"])
            nm = f"k_rm_{i}_{j}"
            inc.append(f"static const uint8_t {nm}[{len(b)}] = {{")
            for o in range(0, len(b), 16):
                inc.append("    " + ", ".join(f"0x{x:02X}" for x in b[o:o + 16]) + ",")
            inc.append("};")
            refs.append(f"    {{ 0x{pk['id']:02X}, {nm}, {len(b)} }},")
        inc.append(f"static const fx_panel_pkt_t k_rm_pkts_{i}[] = {{")
        inc += refs + ["};"]
        idx = ", ".join(str(m["mode_index"]) for m in p["modes"])
        inc.append(f"static const uint8_t k_rm_modes_{i}[] = {{ {idx} }};")
        rows.append(f'    {{ {{ "{p["name"]}", {base["width"]}, {base["height"]}, '
                    f'{base["color_scheme"]}, k_rm_pkts_{i}, {len(p["packets"])} }}, '
                    f'k_rm_modes_{i}, sizeof k_rm_modes_{i}, {p["lut_family"]}u }},')
    inc += ["", "typedef struct {\n    fx_panel_t panel;\n    const uint8_t *modes;\n"
            "    unsigned n_modes;\n    uint8_t family;\n} rm_panel_t;", "",
            "static const rm_panel_t k_rm_panels[] = {"]
    inc += rows + ["};", ""]
    return json.dumps(js, indent=2) + "\n", "\n".join(inc)


def main() -> int:
    js, inc = render()
    if "--check" in sys.argv:
        stale = [p for p, t in ((OUT_JSON, js), (OUT_INC, inc))
                 if not p.exists() or p.read_text() != t]
        for p in stale:
            print(f"{p} is stale: run {Path(__file__).name}", file=sys.stderr)
        return 1 if stale else 0
    OUT_JSON.write_text(js)
    OUT_INC.write_text(inc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
