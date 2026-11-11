#!/usr/bin/env python3
"""BG22 RAM budget gate: read the linked ELF, print the RAM map, refuse a heap that is too small.

The BG22 linker script gives the memory-manager heap whatever RAM is left
(heap = __HeapLimit - __HeapBase, autogen/linkerfile.ld), so every static byte added anywhere
shrinks it silently. The Bluetooth stack takes its buffer pools, connection tables, advertiser
sets and dynamic GATT database from that heap at init and the per-connection ATT bearer from it
at connect; a shortfall at connect leaves a link with no GATT server and no boot failure
(docs/BG22_RAM_BUDGET.md). This gate turns that into a build error.

    bg22_ram_budget.py --elf opendisplay-bg22.out --baseline targets/efr32bg22-slc/ram_budget.json
    bg22_ram_budget.py --elf ... --baseline ... --update-baseline   # after a measured re-budget
    bg22_ram_budget.py --selftest

Fails (exit 1) when
  - heap < baseline heap_min_bytes                      (the justified floor, see the doc)
  - the stack reservation differs from stack_bytes        (either way: smaller risks overflow,
                                                          larger silently takes heap)
  - static RAM (everything but stack and heap) grows past static_ram_max_bytes
Standard library only: it runs inside the CMake post-build step and in tools/check.sh.
"""

import argparse
import json
import os
import struct
import sys

RAM_ORIGIN = 0x20000000
RAM_LENGTH = 0x8000          # EFR32BG22C224F512IM40: 32 KB


class ElfError(Exception):
    pass


def read_elf(data):
    """Return (sections, symbols) of a little-endian ELF32 image.

    sections: list of dicts {name, addr, size, nobits}; symbols: {name: value}.
    """
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ElfError("not a little-endian ELF32 file")
    (e_shoff,) = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    if e_shentsize != 40 or e_shnum == 0:
        raise ElfError("unexpected section header table")
    raw = []
    for i in range(e_shnum):
        raw.append(struct.unpack_from("<IIIIIIIIII", data, e_shoff + i * 40))
    shstr = raw[e_shstrndx]

    def cstr(off):
        end = data.index(b"\0", off)
        return data[off:end].decode("ascii", "replace")

    sections = []
    symtab = None
    for sh in raw:
        name = cstr(shstr[4] + sh[0])
        sections.append({"name": name, "type": sh[1], "flags": sh[2], "addr": sh[3],
                         "offset": sh[4], "size": sh[5], "link": sh[6],
                         "entsize": sh[9], "nobits": sh[1] == 8})
        if sh[1] == 2:
            symtab = sections[-1]
    symbols = {}
    if symtab is not None:
        strtab = sections[symtab["link"]]
        for i in range(symtab["size"] // 16):
            st_name, st_value, _st_size, _info, _other, _shndx = struct.unpack_from(
                "<IIIBBH", data, symtab["offset"] + i * 16)
            if st_name:
                symbols[cstr(strtab["offset"] + st_name)] = st_value
    return sections, symbols


def budget(sections, symbols):
    """The RAM map of one image, in bytes."""
    need = ["__HeapBase", "__HeapLimit", "__StackLimit", "__StackTop"]
    missing = [n for n in need if n not in symbols]
    if missing:
        raise ElfError("missing linker symbols: " + ", ".join(missing))
    ram = [s for s in sections
           if (s["flags"] & 0x2) and RAM_ORIGIN <= s["addr"] < RAM_ORIGIN + RAM_LENGTH
           and s["size"] > 0]
    out = {"sections": {}}
    static = 0
    for s in ram:
        out["sections"][s["name"]] = s["size"]
        if s["name"] not in (".stack", ".memory_manager_heap"):
            static += s["size"]
    out["stack"] = symbols["__StackTop"] - symbols["__StackLimit"]
    out["heap"] = symbols["__HeapLimit"] - symbols["__HeapBase"]
    out["heap_base"] = symbols["__HeapBase"]
    out["static"] = static
    out["ram"] = RAM_LENGTH
    return out


def evaluate(b, base):
    """List of failure strings (empty = pass) for budget b against baseline base."""
    errs = []
    if b["heap"] < base["heap_min_bytes"]:
        errs.append("heap %d B is below the floor %d B (short by %d B)"
                    % (b["heap"], base["heap_min_bytes"], base["heap_min_bytes"] - b["heap"]))
    if b["stack"] != base["stack_bytes"]:
        errs.append("stack reservation %d B differs from the baseline %d B"
                    % (b["stack"], base["stack_bytes"]))
    if b["static"] > base["static_ram_max_bytes"]:
        errs.append("static RAM %d B grew past the baseline %d B (+%d B)"
                    % (b["static"], base["static_ram_max_bytes"],
                       b["static"] - base["static_ram_max_bytes"]))
    return errs


def report(b, base):
    lines = ["od: BG22 RAM budget (32,768 B)"]
    for name, size in sorted(b["sections"].items(), key=lambda kv: -kv[1]):
        lines.append("od:   %-28s %6d B" % (name, size))
    lines.append("od:   static (all but stack/heap) %6d B   baseline max %d"
                 % (b["static"], base["static_ram_max_bytes"] if base else -1))
    lines.append("od:   stack                       %6d B   baseline %d"
                 % (b["stack"], base["stack_bytes"] if base else -1))
    lines.append("od:   heap  (__HeapLimit-__HeapBase) %4d B   floor %d, margin %+d"
                 % (b["heap"], base["heap_min_bytes"] if base else -1,
                    b["heap"] - base["heap_min_bytes"] if base else 0))
    return "\n".join(lines)


def selftest():
    base = {"heap_min_bytes": 9600, "stack_bytes": 2752, "static_ram_max_bytes": 20288}
    ok = {"heap": 9728, "stack": 2752, "static": 20288, "sections": {}}
    assert evaluate(ok, base) == []
    trace = dict(ok, heap=8064, static=21952)          # the stranding image's numbers
    errs = evaluate(trace, base)
    assert len(errs) == 2 and "below the floor" in errs[0] and "grew past" in errs[1], errs
    assert evaluate(dict(ok, stack=2048), base)[0].startswith("stack reservation")
    assert evaluate(dict(ok, stack=3072), base)[0].startswith("stack reservation")
    assert evaluate(dict(ok, heap=9600), base) == []
    assert evaluate(dict(ok, heap=9599), base) != []
    assert evaluate(dict(ok, static=20000), base) == []   # shrinking is allowed
    # A minimal ELF round trip: header + 3 sections (null, .shstrtab, a RAM NOBITS section).
    shstr = b"\0.shstrtab\0.bss\0"
    hdr = bytearray(52)
    hdr[0:6] = b"\x7fELF\x01\x01"
    shoff = 52 + len(shstr)
    struct.pack_into("<I", hdr, 0x20, shoff)
    struct.pack_into("<HHH", hdr, 0x2E, 40, 3, 1)
    secs = struct.pack("<10I", *([0] * 10))
    secs += struct.pack("<10I", 1, 3, 0, 0, 52, len(shstr), 0, 0, 1, 0)
    secs += struct.pack("<10I", 11, 8, 3, 0x20000100, 0, 0x40, 0, 0, 4, 0)
    s, _ = read_elf(bytes(hdr) + shstr + secs)
    assert [x["name"] for x in s] == ["", ".shstrtab", ".bss"] and s[2]["size"] == 0x40
    try:
        read_elf(b"not an elf")
        raise AssertionError("accepted garbage")
    except ElfError:
        pass
    print("bg22_ram_budget selftest: ok")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf")
    ap.add_argument("--baseline")
    ap.add_argument("--update-baseline", action="store_true",
                    help="rewrite stack/static in the baseline from this image (never the floor)")
    ap.add_argument("--fail-removes-artefacts", action="store_true",
                    help="on failure delete the .s37/.hex/.bin/.gbl next to the ELF")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.elf or not a.baseline:
        ap.error("--elf and --baseline are required")
    with open(a.elf, "rb") as f:
        b = budget(*read_elf(f.read()))
    with open(a.baseline) as f:
        base = json.load(f)
    if a.update_baseline:
        base["stack_bytes"] = b["stack"]
        base["static_ram_max_bytes"] = b["static"]
        base["recorded"] = {"heap": b["heap"], "static": b["static"], "stack": b["stack"]}
        with open(a.baseline, "w") as f:
            json.dump(base, f, indent=2)
            f.write("\n")
        print("od: baseline updated: static %d B, stack %d B (heap floor unchanged: %d B)"
              % (b["static"], b["stack"], base["heap_min_bytes"]))
    print(report(b, base))
    errs = evaluate(b, base)
    for e in errs:
        print("od: RAM BUDGET FAIL: " + e, file=sys.stderr)
    if errs and a.fail_removes_artefacts:
        stem = os.path.splitext(a.elf)[0]
        for ext in (".s37", ".hex", ".bin", ".gbl"):
            if os.path.exists(stem + ext):
                os.remove(stem + ext)
                print("od: removed " + stem + ext, file=sys.stderr)
    if errs:
        print("od: see docs/BG22_RAM_BUDGET.md. A BG22 image that fails this gate must not be "
              "OTA'd: it can come up advertising with a GATT server that never answers.",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
