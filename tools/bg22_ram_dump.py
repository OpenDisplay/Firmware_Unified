#!/usr/bin/env python3
"""Decode a BG22 RAM dump against the ELF it came from (docs/BG22_RAM_BUDGET.md section 5).

    JLinkExe ... > savebin ram.bin 0x20000000 0x8000
    bg22_ram_dump.py --elf opendisplay-bg22.out --ram ram.bin

Prints, for whichever of these the image has: the memory-manager heap handle (used, high-water
mark), a walk of every heap block (the measured allocation map), the Bluetooth stack's
resource-exhausted report, the health-gate state, the bootloader reset word, and the SEC2 test
trace ring of a trace build. Standard library only. Written without a dump to test against:
treat a decode that looks wrong as a decoder bug first.
"""

import argparse
import struct
import sys

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bg22_ram_budget import read_elf, RAM_ORIGIN  # noqa: E402


def symbols_with_size(data):
    """{name: (value, size)} from the ELF symbol table, LTO suffixes stripped as aliases."""
    sections, _ = read_elf(data)
    out = {}
    for s in sections:
        if s["type"] != 2:
            continue
        strtab = sections[s["link"]]
        for i in range(s["size"] // 16):
            st_name, val, size, _info, _o, _sh = struct.unpack_from("<IIIBBH", data, s["offset"] + i * 16)
            if not st_name:
                continue
            end = data.index(b"\0", strtab["offset"] + st_name)
            name = data[strtab["offset"] + st_name:end].decode("ascii", "replace")
            out[name] = (val, size)
            base = name.split(".lto_priv")[0]
            out.setdefault(base, (val, size))
    return out


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", required=True)
    ap.add_argument("--ram", required=True, help="raw dump of 0x20000000..0x20007FFF")
    a = ap.parse_args(argv)
    elf = open(a.elf, "rb").read()
    ram = open(a.ram, "rb").read()
    syms = symbols_with_size(elf)

    def rd(addr, n):
        off = addr - RAM_ORIGIN
        if off < 0 or off + n > len(ram):
            raise ValueError("0x%08x+%d outside the dump" % (addr, n))
        return ram[off:off + n]

    def u32(addr):
        return struct.unpack("<I", rd(addr, 4))[0]

    print("reset word @0x20000000: %s (BOOTLOAD request = 02 02 0f f0)" % rd(0x20000000, 4).hex(" "))

    if "sli_general_purpose_heap" in syms:
        h = syms["sli_general_purpose_heap"][0]
        base, size, used, hwm, nfree = struct.unpack("<IIIII", rd(h, 20))
        print("heap handle @0x%08x: base 0x%08x size %d used %d high-water %d free-blocks %d"
              % (h, base, size, used, hwm, nfree))
        print("  free now %d B, minimum free ever %d B" % (size - used, size - hwm))
        # Block walk: 8-byte metadata (flags:16, length:16 dwords, prev:16, next:16 dwords).
        addr, total_used, n = base, 0, 0
        print("  block walk (payload bytes, in use):")
        while base <= addr < base + size and n < 400:
            flags, length, _prev, nxt = struct.unpack("<HHHH", rd(addr, 8))
            in_use = flags & 1
            ln = (length | ((flags >> 4) & 0xF) << 16) * 8
            nx = (nxt | ((flags >> 12) & 0xF) << 16) * 8
            print("    0x%08x %6d %s" % (addr + 8, ln, "USED" if in_use else "free"))
            total_used += (ln + 8) if in_use else 0
            n += 1
            if nx == 0:
                break
            addr += nx
        print("  walked %d blocks, %d B in use incl. metadata" % (n, total_used))

    if "system_resource_exhausted_event" in syms:
        ev, sz = syms["system_resource_exhausted_event"]
        raw = rd(ev, max(sz, 20))
        print("system_resource_exhausted_event @0x%08x: %s" % (ev, raw.hex(" ")))
        print("  (the four counters discarded / buffer / HEAP / message are the 4 bytes at "
              "+12+raw[6]+4; bit 2 of raw[5] = still queued, i.e. the app never read it)")

    for name in ("od_bg22_health", "od_bg22_heap_free_at_boot", "od_bg22_heap_hwm_at_boot",
                 "s_health_ret", "g_connection", "s_pending_dfu", "od_tr_rstcause_raw"):
        if name in syms:
            v, sz = syms[name]
            print("%s @0x%08x: %s" % (name, v, rd(v, max(sz, 1)).hex(" ")))

    if "s_health_ret" in syms:
        v, sz = syms["s_health_ret"]
        if sz >= 16:
            magic, forced, exhausted, check = struct.unpack("<IIII", rd(v, 16))
            ok = magic == 0x4F444847 and check == (forced ^ ((exhausted * 0x9E3779B1) & 0xFFFFFFFF)
                                                  ^ (~0x4F444847 & 0xFFFFFFFF))
            print("retained health record: %s; forced entries %d (last reason %d), stack heap "
                  "failures %d, other stack resource failures %d (since power-on)"
                  % ("valid" if ok else "INVALID (power-on or overwritten)", forced & 0xFF,
                     (forced >> 8) & 0xFF, exhausted & 0xFFFF, exhausted >> 16))

    if "s_tr" in syms:
        v, _ = syms["s_tr"]
        magic, head, boots, check = struct.unpack("<IIII", rd(v, 16))
        buf = rd(v + 16, 1024)
        print("SEC2 trace ring @0x%08x: magic 0x%08x head %d boots %d check 0x%08x"
              % (v, magic, head, boots, check))
        start = head - min(head, 1024)
        text = bytes(buf[i % 1024] for i in range(start, head)).decode("ascii", "replace")
        print("---- ring (oldest first) ----\n" + text + "\n---- end ring ----")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
