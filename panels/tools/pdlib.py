"""Panel drive data: encoder, bb_epaper capture, derivation, od_epd equivalence.

Stdlib only (PyYAML is needed only to re-encode panels.yaml). Wire layouts follow
shared/protocol/opendisplay_structs.h; the C replay harness static-asserts the same offsets.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

PANELS = Path(__file__).resolve().parents[1]
REPO = PANELS.parent
HARNESS = REPO / "tests" / "panels" / "harness"
GENERATED = PANELS / "generated"
BUILD = Path(os.environ.get("OD_PANELS_BUILD", str(REPO / "build" / "panels")))

PKT_MODE, PKT_LUT, PKT_CAL, PKT_PANEL = 0xAE, 0xAF, 0xB0, 0xB1

# od_epd executor bounds (shared/core/od_epd.h)
SEQ_MAX = 128
MAX_STEPS = 64
SLEEP_MAX = 16
LUT_MAX = 535
MAX_LUT_SLOTS = 8

# config ceilings (whole blob) and the legacy-only config the fits check assumes:
# outer header 3 + crc 2 + 0x01 system 24 + 0x02 manufacturer 24 + 0x04 power 32
# + 0x20 display 48 + 0x21 led 24 + 0x23 sensor 32 + 0x25 binary_inputs 32
# + 0x27 security 66 + 0x2D findmy 130 (each body + 2-byte packet header)
CAPS = {"bg22": 3120, "esp32": 4096, "nordic": 4096}
# Interpreter limits per target class (shared/core/od_epd.h defaults; BG22 overrides them in
# shared/profiles.cmake and builds without WINDOW/FILL). "config" is the config-size cap.
CLASSES = {
    "default": {"seq_max": 512, "max_steps": 96, "window": True, "config": 4096,
                "targets": ["esp32", "nordic"]},
    "bg22": {"seq_max": 128, "max_steps": 64, "window": False, "config": 3120, "targets": ["bg22"]},
}
TYPICAL_BASE = 3 + 2 + 24 + 24 + 32 + 48 + 24 + 32 + 32 + 66 + 130

BBEP_REF_REV = "4edfa7f"   # what Firmware origin/main pins (platformio.ini)
BBEP_VENDORED_REV = "5dccfbb"

FLAG_NAMES = {
    0x0001: "RED_SWAPPED", 0x0002: "BITBANG", 0x0004: "3COLOR", 0x0008: "4COLOR",
    0x0010: "4GRAY", 0x0020: "7COLOR", 0x0040: "16GRAY", 0x0080: "CS_EVERY_BYTE",
    0x0100: "PARTIAL2", 0x0200: "4BPP_DATA", 0x0400: "SPLIT_BUFFER",
    0x0800: "HAS_SECOND_PLANE", 0x1000: "NEEDS_EXTRA_INIT", 0x2000: "2BIT_BW",
}
CHIP = {1: "SSD16xx", 2: "UC81xx", 3: "IT8951"}

# ----------------------------------------------------------------------------- encoding

def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(number: int, pid: int, body: bytes) -> bytes:
    return bytes([number & 0xFF, pid]) + struct.pack("<H", len(body)) + body


def mode_body(index: int, name: str, seq: bytes, *, passes: int = 1, scheme: int = 0xFF,
              slots: list[int] | None = None, vsn1: list[int] | None = None,
              tmin: int = 0, tmax: int = 0, policy: int = 0, instance: int = 0) -> bytes:
    if len(name) > 11:
        raise ValueError(f"mode name {name!r} longer than 11 characters")
    s = (list(slots or []) + [0xFF] * 8)[:8]
    v = (list(vsn1 or []) + [0] * 8)[:8]
    return (bytes([instance, index]) + name.encode().ljust(12, b"\0") + bytes([scheme, passes])
            + struct.pack("<bbB", tmin, tmax, policy) + bytes(s) + bytes(v)
            + struct.pack("<H", len(seq)) + seq)


def lut_body(slot: int, data: bytes, instance: int = 0) -> bytes:
    return struct.pack("<BBH", instance, slot, crc16_ccitt(data)) + data


def panel_body(*, busy_usable: bool, idle: int, timeout: int, fixed: int, code_map: list[int],
               mode_count: int, sleep: bytes, lut_supported: bool, instance: int = 0) -> bytes:
    flags = (1 if busy_usable else 0) | (2 if lut_supported else 0)
    return (struct.pack("<BBBHH", instance, flags, idle, min(timeout, 0xFFFF), min(fixed, 0xFFFF))
            + bytes((list(code_map) + [0] * 4)[:4]) + bytes([mode_count, len(sleep)]) + sleep)


def cal_body(mode_index: int, entries: list[tuple[tuple[int, ...], tuple[float, float, float]]],
             measured: bool, instance: int = 0) -> bytes:
    out = bytearray([instance, mode_index, 1 if measured else 0, len(entries)])
    for codes, (L, a, b) in entries:
        cw = 0
        for p, c in enumerate(codes):
            cw |= (c & 3) << (2 * p)
        out += struct.pack("<Hhhh", cw, round(L * 100), round(a * 100), round(b * 100))
    return bytes(out)


ESC = {"end": 0x00, "busy": 0x01, "subst": 0x02, "image": 0x03, "lut": 0x04, "delay": 0x05,
       "pin": 0x06, "slice": 0x07, "cs": 0x08, "lutslice": 0x0B, "window": 0x0C, "fill": 0x0D}
PINREF_RESET = 0x10


def encode_steps(steps: list[tuple]) -> bytes:
    """Step tuples -> od_epd sequence bytes (grammar: od_epd.h). END is appended."""
    out = bytearray()
    for st in steps:
        k = st[0]
        if k == "cmd":
            data = bytes(st[2])
            if len(data) + 1 > 255:
                raise ValueError("step longer than 255 bytes")
            out += bytes([1 + len(data), st[1]]) + data
        elif k in ("busy", "image", "lut"):
            out += bytes([0, ESC[k]])
        elif k == "delay":
            out += bytes([0, ESC["delay"], st[1] & 0xFF, st[1] >> 8])
        elif k == "pin":
            out += bytes([0, ESC["pin"], st[1], st[2]])
        elif k == "slice":
            out += bytes([0, ESC["slice"], st[1], st[2]])
        elif k == "cs":
            out += bytes([0, ESC["cs"], st[1]])
        elif k == "lutslice":
            out += bytes([0, ESC["lutslice"]]) + struct.pack("<HH", st[1], st[2])
        elif k == "window":            # ("window", ((kind, val), ...)), cmd literal first
            out += bytes([0, ESC["window"], len(st[1])]) + bytes(b for kv in st[1] for b in kv)
        elif k == "fill":              # ("fill", cmd, value, flags)
            out += bytes([0, ESC["fill"], st[1], st[2], st[3]])
        else:
            raise ValueError(st)
    out += bytes([0, 0])
    return bytes(out)


def count_steps(steps: list[tuple]) -> int:
    return len(steps) + 1   # END is a step for od_epd's cap


def walk_seq(seq: bytes) -> list[tuple]:
    """Decode a sequence back into step tuples (inverse of encode_steps; also handles SUBST)."""
    out, i = [], 0
    while i < len(seq):
        n = seq[i]
        if n:
            if i + 1 + n > len(seq):
                raise ValueError("step overruns sequence")
            out.append(("cmd", seq[i + 1], bytes(seq[i + 2:i + 1 + n])))
            i += 1 + n
            continue
        e = seq[i + 1]
        if e == 0:
            return out
        if e in (1, 3, 4):
            out.append(({1: "busy", 3: "image", 4: "lut"}[e],)); i += 2
        elif e == 5:
            out.append(("delay", seq[i + 2] | seq[i + 3] << 8)); i += 4
        elif e == 6:
            out.append(("pin", seq[i + 2], seq[i + 3])); i += 4
        elif e == 7:
            out.append(("slice", seq[i + 2], seq[i + 3])); i += 4
        elif e == 8:
            out.append(("cs", seq[i + 2])); i += 3
        elif e == 0x0B:
            out.append(("lutslice",) + struct.unpack("<HH", seq[i + 2:i + 6])); i += 6
        elif e == 2:
            c = seq[i + 2]
            out.append(("subst", bytes(seq[i + 3:i + 3 + 2 * c]))); i += 3 + 2 * c
        elif e == 0x0C:
            c = seq[i + 2]
            out.append(("window", tuple((seq[i + 3 + 2 * k], seq[i + 4 + 2 * k]) for k in range(c))))
            i += 3 + 2 * c
        elif e == 0x0D:
            out.append(("fill", seq[i + 2], seq[i + 3], seq[i + 4])); i += 5
        else:
            raise ValueError(f"unknown escape {e:#x}")
    raise ValueError("sequence has no END")


def parse_framed(blob: bytes) -> list[tuple[int, int, bytes]]:
    out, o = [], 0
    while o < len(blob):
        if o + 4 > len(blob):
            raise ValueError("truncated packet header")
        num, pid, ln = blob[o], blob[o + 1], struct.unpack("<H", blob[o + 2:o + 4])[0]
        body = blob[o + 4:o + 4 + ln]
        if len(body) != ln:
            raise ValueError("truncated packet body")
        out.append((num, pid, body))
        o += 4 + ln
    return out

# ----------------------------------------------------------------------------- toolchain

def _cc(kind: str) -> str:
    env = os.environ.get("CXX" if kind == "c++" else "CC")
    if env:
        return env
    for c in (("g++", "clang++") if kind == "c++" else ("gcc", "clang", "cc")):
        if shutil.which(c):
            return c
    raise RuntimeError(f"no host {kind} compiler")


def bbep_source(rev: str | None = None) -> tuple[Path, str]:
    """-> (bb_epaper src dir, revision). The reference revision comes from the upstream clone
    (OD_BBEP_UPSTREAM, default ../bb_epaper) via git archive; else third_party (vendored)."""
    rev = rev or os.environ.get("OD_BBEP_REV", BBEP_REF_REV)
    if rev != BBEP_VENDORED_REV:
        up = Path(os.environ.get("OD_BBEP_UPSTREAM", str(REPO.parent / "bb_epaper")))
        if not (up / ".git").exists():
            up = REPO.parents[1] / "bb_epaper"
        if (up / ".git").exists():
            dst = BUILD / f"bb_epaper-{rev}"
            if not (dst / "src" / "bb_ep.inl").exists():
                dst.mkdir(parents=True, exist_ok=True)
                arch = subprocess.run(["git", "-C", str(up), "archive", rev, "src"],
                                      check=True, capture_output=True).stdout
                subprocess.run(["tar", "-x", "-C", str(dst)], input=arch, check=True)
            return dst / "src", rev
        raise RuntimeError(f"bb_epaper {rev} needs the upstream clone (set OD_BBEP_UPSTREAM)")
    return REPO / "third_party" / "bb_epaper" / "src", BBEP_VENDORED_REV


def build_capture(src: Path, rev: str) -> Path:
    out = BUILD / f"bbep_capture-{rev}"
    hdr = (src / "bb_epaper.h").read_text(errors="replace")
    dt = "long" if re.search(r"void\s+delay\s*\(\s*long", hdr) else "int"
    BUILD.mkdir(parents=True, exist_ok=True)
    subprocess.run([_cc("c++"), "-std=gnu++17", "-w", f"-DREC_DELAY_T={dt}", f"-I{src}",
                    f"-I{HARNESS}", str(HARNESS / "bbep_capture.cpp"), "-o", str(out)], check=True)
    return out


def build_replay(cls: str = "default") -> Path:
    out = BUILD / f"od_epd_replay-{cls}"
    BUILD.mkdir(parents=True, exist_ok=True)
    c = CLASSES[cls]
    defs = [] if cls == "default" else [f"-DOD_EPD_SEQ_MAX={c['seq_max']}u",
                                        f"-DOD_EPD_MAX_SEQ_STEPS={c['max_steps']}u",
                                        f"-DOD_EPD_WITH_WINDOW={1 if c['window'] else 0}"]
    subprocess.run([_cc("c"), "-std=c11", "-Wall", "-Wextra", "-Werror", *defs,
                    f"-I{REPO / 'shared' / 'core'}", f"-I{REPO / 'shared' / 'protocol'}",
                    str(HARNESS / "od_epd_replay.c"), str(REPO / "shared" / "core" / "od_epd.c"),
                    "-o", str(out)], check=True)
    return out


def enum_names(src: Path) -> list[str]:
    """bb_epaper panel enum, index -> name, parsed from bb_epaper.h (comment lines skipped)."""
    h = (src / "bb_epaper.h").read_text(errors="replace")
    blk = h[h.index("EP_PANEL_UNDEFINED=0"):h.index("EP_PANEL_COUNT")]
    names = []
    for line in blk.splitlines():
        line = line.strip()
        if not line or line.startswith("//"):
            continue
        m = re.match(r"([A-Za-z0-9_]+)\s*(=\s*0)?\s*,", line)
        if m:
            names.append(m.group(1))
    return names

# ----------------------------------------------------------------------------- capture

def capture(rev: str | None = None) -> dict:
    src, rev = bbep_source(rev)
    exe = build_capture(src, rev)
    names = enum_names(src)
    text = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    text += subprocess.run([str(exe), "rects"], check=True, capture_output=True, text=True).stdout
    panels: dict[str, dict] = {}
    cur = flow = None
    for line in text.splitlines():
        t = line.split()
        if t[0] == "P" and names[int(t[1])] in panels:
            cur = panels[names[int(t[1])]]
        elif t[0] == "F" and len(t) == 6:
            r = cur.setdefault("rects", {}).setdefault(t[1], {"rect": [int(x) for x in t[2:6]], "events": []})
            flow = r["events"]
        elif t[0] == "P":
            idx = int(t[1])
            cur = {"enum": idx, "name": names[idx], "width": int(t[2]), "height": int(t[3]),
                   "chip": CHIP.get(int(t[4]), str(t[4])), "flags": int(t[5]),
                   "x_offset": int(t[6]), "has_fast": t[7] == "1", "has_part": t[8] == "1",
                   "flows": {}}
            panels[cur["name"]] = cur
        elif t[0] == "L":
            cur["lookup"] = [int(x) for x in t[1:]]
        elif t[0] == "F":
            flow = cur["flows"].setdefault(t[1], [])
        elif t[0] == "E":
            flow = None
        else:
            flow.append(line)
    return {"bb_epaper": rev, "panels": panels}


def flag_names(flags: int) -> list[str]:
    return [n for b, n in FLAG_NAMES.items() if flags & b]

# ----------------------------------------------------------------------------- normalise

CSNAME = {1: "0", 2: "1", 3: "01", 0: "none"}


def normalise(events: list[str], side: str) -> list[tuple]:
    """Raw harness events -> tokens (README § Normalised stream). side: 'bbep' | 'odepd'."""
    toks: list[tuple] = []
    manual = {1: 1, 2: 1}
    i = 0
    ev = [e.split() for e in events]

    def cs_of(v: str) -> str:
        c = int(v)
        if c == 0:
            sel = (1 if manual[1] == 0 else 0) | (2 if manual[2] == 0 else 0)
            return CSNAME.get(sel, "none")
        return CSNAME[c]

    while i < len(ev):
        t = ev[i]
        k = t[0]
        if side == "bbep" and k == "R" and t[1] == "0":
            j = i + 1
            while j < len(ev) and not (ev[j][0] == "R" and ev[j][1] == "1"):
                j += 1
            j += 1
            while j < len(ev) and ev[j][0] == "W" and ev[j][1] != "10":
                j += 1
            toks.append(("RESET",)); i = j; continue
        if side == "odepd" and k == "P" and int(t[1]) == PINREF_RESET and t[2] == "0":
            j = i + 1
            while j < len(ev) and not (ev[j][0] == "P" and int(ev[j][1]) == PINREF_RESET and ev[j][2] == "1"):
                j += 1
            j += 1
            while j < len(ev) and ev[j][0] == "W" and ev[j][1] == "20":
                j += 1
                break
            toks.append(("RESET",)); i = j; continue
        if k == "W":
            if side == "bbep" and t[1] == "10" and i + 2 < len(ev) and ev[i + 1] == ["W", "1"] and ev[i + 2] == ["B"]:
                toks.append(("BUSY",)); i += 3; continue
            toks.append(("DELAY", int(t[1]))); i += 1; continue
        if k == "B":
            toks.append(("BUSY",)); i += 1; continue
        if k == "T":
            toks.append(("RESET",)); i += 1; continue
        if k == "S":
            manual[int(t[1])] = int(t[2]); i += 1; continue
        if k == "C":
            toks.append(("CMD", int(t[1], 16), b"", cs_of(t[2]))); i += 1; continue
        if k == "D":
            data = bytes.fromhex(t[1]); cs = cs_of(t[2])
            if toks and toks[-1][0] == "CMD" and toks[-1][3] == cs:
                last = toks.pop(); toks.append(("CMD", last[1], last[2] + data, cs))
            else:
                toks.append(("DATA", data, cs))
            i += 1; continue
        if k == "I":
            if side == "bbep":
                n, cs = int(t[1]), cs_of(t[2])
                if toks and toks[-1][0] == "IMAGE" and toks[-1][2] == cs:
                    last = toks.pop(); toks.append(("IMAGE", last[1] + n, cs))
                else:
                    toks.append(("IMAGE", n, cs))
            else:
                toks.append(("IMAGE", (int(t[1]), int(t[2])), cs_of(t[3]) if len(t) > 3 else None))
            i += 1; continue
        if k == "P":
            toks.append(("PIN", int(t[1]), int(t[2]))); i += 1; continue
        if k == "M":
            i += 1; continue
        if k == "X":
            toks.append(("UNSUPPORTED", " ".join(t[1:]))); i += 1; continue
        raise ValueError(f"unknown event {t}")
    # D3: back-to-back resets (each optionally followed by its BUSY) are one reset.
    out: list[tuple] = []
    for tk in toks:
        if tk == ("RESET",) and len(out) >= 1 and (out[-1] == ("RESET",) or
                                                    (out[-1] == ("BUSY",) and len(out) >= 2 and out[-2] == ("RESET",))):
            if out[-1] == ("BUSY",):
                out.pop()
            continue
        out.append(tk)
    return out


def expand_cs_both(toks: list[tuple]) -> list[tuple]:
    """D6: a transfer to both controllers is compared as the same transfer to cs0 then cs1."""
    out = []
    for t in toks:
        if t[0] == "CMD" and t[3] == "01":
            out.append(("CMD", t[1], t[2], "0")); out.append(("CMD", t[1], t[2], "1"))
        else:
            out.append(t)
    return out


def comparable(toks: list[tuple]) -> list[tuple]:
    return [("IMAGE", t[2]) if t[0] == "IMAGE" else t for t in toks]

# ----------------------------------------------------------------------------- derive

def frame_layout(p: dict) -> str:
    f = p["flags"]
    if f & 0x0400:
        return "split"
    if f & (0x0004 | 0x0010):
        return "planes"
    return "single"


def tokens_to_steps(toks: list[tuple], layout: str, flow: str) -> tuple[list[tuple], list[str], list[str]]:
    """Mechanical tokens -> step tuples. -> (steps, requires_escapes, blocked_on)."""
    steps: list[tuple] = []
    req: set[str] = set()
    blocked: set[str] = set()
    cs = "0"
    nimg = sum(1 for t in toks if t[0] == "IMAGE")
    k_img = 0
    for t in toks:
        k = t[0]
        if k == "RESET":
            steps += [("pin", PINREF_RESET, 0), ("delay", 20), ("pin", PINREF_RESET, 1), ("delay", 20)]
            req.add("SET_PIN")
        elif k == "BUSY":
            steps.append(("busy",))
        elif k == "DELAY":
            steps.append(("delay", t[1]))
        elif k == "CMD":
            targets = ["0", "1"] if t[3] == "01" else [t[3]]
            for c in targets:
                if c not in ("0", "1"):
                    blocked.add("CS_BOTH")
                    continue
                if c != cs:
                    steps.append(("cs", int(c))); cs = c; req.add("CS_SELECT")
                steps.append(("cmd", t[1], t[2]))
        elif k == "DATA":
            blocked.add("DATA_WITHOUT_CMD")
        elif k == "IMAGE":
            c = t[2]
            if c in ("0", "1") and c != cs:
                steps.append(("cs", int(c))); cs = c; req.add("CS_SELECT")
            if nimg == 1:
                steps.append(("image",))
            elif layout == "planes" and flow != "partial":
                steps.append(("image",))
            else:
                steps.append(("slice", k_img, nimg))
            k_img += 1
        elif k == "WINDOW":            # ("WINDOW", cmd, ((kind, val), ...), cs)
            if t[3] != cs:
                steps.append(("cs", int(t[3]))); cs = t[3]; req.add("CS_SELECT")
            steps.append(("window", ((WIN_LIT, t[1]),) + t[2]))
        elif k == "FILL":              # ("FILL", cmd, value, flags, cs)
            if t[4] != cs:
                steps.append(("cs", int(t[4]))); cs = t[4]; req.add("CS_SELECT")
            steps.append(("fill", t[1], t[2], t[3]))
        elif k == "UNSUPPORTED":
            blocked.add("IT8951")
        else:
            raise ValueError(t)
    return steps, sorted(req), sorted(blocked)


def fit_steps(steps: list[tuple], limit: int, allow_lut: bool = True,
              before_image_only: bool = False, max_steps: int | None = None
              ) -> tuple[list[tuple], bytes, list[str]]:
    """Move the largest register payloads into a LUT slot (streamed with LUT_SLICE) until the
    sequence fits. -> (steps, slot_bytes, blocked).

    before_image_only restricts the moves to steps ahead of the first image marker: od_epd
    resumes after a marker without the pass's slot, so a LUT_SLICE there fails with RC_LUT."""
    steps = list(steps)
    slot = bytearray()
    cap_steps = MAX_STEPS if max_steps is None else max_steps
    while len(encode_steps(steps)) > limit or count_steps(steps) > cap_steps or \
            any(s[0] == "cmd" and len(s[2]) > 254 for s in steps):
        if not allow_lut:
            return steps, bytes(slot), ["SEQ_MAX"]
        first_img = next((i for i, s in enumerate(steps) if s[0] in ("image", "slice")), len(steps))
        cand = [(len(s[2]), i) for i, s in enumerate(steps) if s[0] == "cmd" and len(s[2]) > 6
                and (not before_image_only or i < first_img)]
        if not cand:
            return steps, bytes(slot), ["SEQ_MAX" if len(encode_steps(steps)) > limit else "MAX_STEPS"]
        _, i = max(cand)
        s = steps[i]
        off = slot.find(s[2]) if s[2] in slot else -1
        if off < 0:
            off = len(slot); slot += s[2]
        steps[i:i + 1] = [("cmd", s[1], b""), ("lutslice", off, len(s[2]))]
    if len(slot) > LUT_MAX:
        return steps, bytes(slot), ["LUT_MAX"]
    if 0 < len(slot) < 7:
        slot += bytes(7 - len(slot))   # lut_info needs the 7-byte analog header to exist
    return steps, bytes(slot), []


def busy_policy(p: dict) -> dict:
    f = p["flags"]
    timeout = 60000 if f & 0x0020 else 40000 if f & (0x0004 | 0x0008) else 30000
    return {"usable": True, "idle": 1 if p["chip"] == "UC81xx" else 0, "timeout": timeout}


def native_scheme(p: dict) -> int:
    f = p["flags"]
    if f & 0x0400:
        return 8
    if f & 0x0020:
        return 4
    if f & 0x0008:
        return 3
    if f & 0x0004:
        return 1
    if f & 0x0010:
        return 5
    return 0


def pixel_blockers(p: dict) -> list[str]:
    f = p["flags"]
    out = []
    if f & 0x0080:
        out.append("CS_EVERY_BYTE")
    if p["chip"] == "IT8951":
        out.append("IT8951")
    return sorted(set(out))

# ----------------------------------------------------------------------------- replay

def replay(packets: bytes, mode: str, cls: str = "default",
           geom: list[int] | None = None) -> tuple[int, list[str]]:
    """geom: [W, H] or [W, H, x, y, w, h] (panel size, optional partial rectangle)."""
    exe = BUILD / f"od_epd_replay-{cls}"
    if not exe.exists():
        build_replay(cls)
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(packets)
        path = f.name
    try:
        r = subprocess.run([str(exe), path, mode, *[str(g) for g in (geom or [])]],
                           capture_output=True, text=True)
    finally:
        os.unlink(path)
    return r.returncode, r.stdout.splitlines()


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dump_json(obj, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=1, sort_keys=False) + "\n")

# ----------------------------------------------------------------------------- panels.yaml

_WAVE_TOKENS = {f"@w{i}": i for i in range(7)}
_WAVE_TOKENS["@vcom"] = 0xFF
_POLICY = {"advisory": 0, "substitute": 1, "reject": 2, "advise": 0, "discourage": 1, "refuse": 2}


def encode_yaml_steps(steps: list[str]) -> bytes:
    """panels.yaml step strings -> sequence bytes; same encoding as py-opendisplay
    panel_drive.encode_sequence (the encoder the website presets were built with)."""
    out = bytearray()
    for raw in steps:
        s = raw.strip()
        if s in ("@busy", "@image", "@lut"):
            out += bytes([0, ESC[s[1:]]])
        elif s.startswith("@delay"):
            ms = int(s.split()[1], 10)
            out += bytes([0, ESC["delay"], ms & 0xFF, ms >> 8])
        else:
            toks = s.split()
            parsed = [(1, _WAVE_TOKENS[t]) if t in _WAVE_TOKENS else (0, int(t, 16)) for t in toks]
            if any(w for w, _ in parsed):
                out += bytes([0, ESC["subst"], len(parsed)])
                for w, v in parsed:
                    out += bytes([0xFF if w else 0x00, v])
            else:
                out += bytes([len(parsed)]) + bytes(v for _, v in parsed)
    return bytes(out + bytes([0, 0]))


def yaml_panel_packets(entry: dict, waves: dict[str, bytes], palette: dict) -> list[tuple[int, int, bytes]]:
    """One panels.yaml entry -> [(number, pid, body)] in the order the website ships them:
    0xB1, 0xAE per mode, 0xAF per waveform slot, 0xB0 per multi-pass mode."""
    seqs = {k: encode_yaml_steps(v) for k, v in (entry.get("init_sequences") or {}).items()}
    slot_of: dict[str, int] = {}
    luts: list[tuple[int, int, bytes]] = []
    modes: list[tuple[int, int, bytes]] = []
    mode_meta = []
    for mi, m in enumerate(entry.get("modes") or []):
        slots = []
        for w in (m.get("lut_per_pass") or [None] * m.get("passes", 1))[:8]:
            if not w:
                slots.append(0xFF)
                continue
            if w not in slot_of:
                slot_of[w] = len(slot_of)
                luts.append((slot_of[w], PKT_LUT, lut_body(slot_of[w], waves[w])))
            slots.append(slot_of[w])
        tg = m.get("temp_gate") or entry.get("temp_gate") or {}
        body = mode_body(mi, m["name"], seqs[m["init"]], passes=m.get("passes", 1),
                         scheme=int(m.get("color_scheme", 0xFF)), slots=slots,
                         vsn1=[v or 0 for v in (m.get("vsn1_per_pass") or [])],
                         tmin=int(tg.get("min_c", 0)), tmax=int(tg.get("max_c", 0)),
                         policy=_POLICY.get(str(tg.get("policy", "advise")), 0))
        modes.append((mi, PKT_MODE, body))
        mode_meta.append(m)
    busy = entry.get("busy") or {}
    colour = entry.get("colour") or {}
    sleep = encode_yaml_steps(entry["sleep_sequence"]) if entry.get("sleep_sequence") else b""
    pan = panel_body(busy_usable=bool(busy.get("usable", True)), idle=int(busy["idle_level"]),
                     timeout=int(busy.get("timeout_ms", 30000)), fixed=int(busy.get("fixed_delay_ms", 0)),
                     code_map=list(colour.get("code_map") or [0, 1, 2, 3]), mode_count=len(modes),
                     sleep=sleep, lut_supported=bool((entry.get("lut") or {}).get("supported")))
    out = [(0, PKT_PANEL, pan)] + modes + luts
    swap = colour.get("palette_code_swap")
    for (mi, _, _), m in zip(modes, mode_meta):
        if m.get("passes", 1) <= 1:
            continue
        ents = []
        for e in palette["entries"]:
            codes = tuple(swap[c] for c in e["codes"]) if swap else tuple(e["codes"])
            ents.append((codes, tuple(e["lab"])))
        out.append((PKT_CAL, PKT_CAL, cal_body(mi, ents, measured=False)))
    return out


# ----------------------------------------------------------------------------- partial window

WIN_LIT, WIN_GEOM = 0x00, 0x01
WIN_DIV8, WIN_MIRROR, WIN_HIGH = 0x08, 0x10, 0x20


def win_value(desc: int, rect: list[int], W: int, H: int) -> int:
    """What od_epd's WINDOW geometry byte `desc` evaluates to (od_epd.c win_value)."""
    x, y, w, h = rect
    sel = desc & 7
    v = [x, x + w - 1, y, y + h - 1, W - 1, H - 1, w, h][sel]
    if desc & WIN_MIRROR:
        dim = H if sel in (2, 3, 5) else W
        v = dim - 1 - v if v < dim else 0
    if desc & WIN_DIV8:
        v >>= 3
    return (v >> 8) & 0xFF if desc & WIN_HIGH else v & 0xFF


_WIN_CANDIDATES = [sel | f | hi for f in (0, WIN_DIV8, WIN_MIRROR, WIN_MIRROR | WIN_DIV8)
                   for sel in range(8) for hi in (0, WIN_HIGH)
                   if not (f & WIN_MIRROR and sel >= 6)]


def infer_window(runs: list[tuple[list[int], list[tuple]]], W: int, H: int) -> tuple[list[tuple], str | None]:
    """Partial-flow token lists captured for several rectangles -> one token list where every
    command byte that follows the rectangle is a WINDOW geometry byte and every whole-plane
    constant write is a FILL. -> (tokens, None) or (first run's tokens, reason it failed)."""
    base = runs[0][1]
    if any(len(t) != len(base) for _, t in runs):
        return base, "token count differs between rectangles"
    plane = ((W + 7) // 8) * H
    out = []
    for i, t in enumerate(base):
        col = [r[1][i] for r in runs]
        if t[0] == "IMAGE":
            if any(c[0] != "IMAGE" or c[2] != t[2] for c in col):
                return base, f"token {i} differs"
            out.append(t); continue
        if t[0] != "CMD":
            if any(c != t for c in col):
                return base, f"token {i} differs"
            out.append(t); continue
        if any(c[0] != "CMD" or c[1] != t[1] or c[3] != t[3] or len(c[2]) != len(t[2]) for c in col):
            return base, f"command {i} differs in shape"
        data = t[2]
        if len(data) == plane and len(set(data)) == 1 and all(c[2] == data for c in col):
            out.append(("FILL", t[1], data[0], 0, t[3])); continue
        if all(c[2] == data for c in col):
            out.append(t); continue
        kv = []
        for j, b in enumerate(data):
            vals = [c[2][j] for c in col]
            if len(set(vals)) == 1:
                kv.append((WIN_LIT, b)); continue
            d = next((d for d in _WIN_CANDIDATES
                      if all(win_value(d, r, W, H) == c[2][j] for r, c in zip([x[0] for x in runs], col))), None)
            if d is None:
                return base, f"command {t[1]:#04x} byte {j} matches no WINDOW geometry"
            kv.append((WIN_GEOM, d))
        if len(kv) + 1 > 32:
            return base, f"command {t[1]:#04x} longer than a WINDOW step"
        out.append(("WINDOW", t[1], tuple(kv), t[3]))
    return out, None
