#!/usr/bin/env python3
"""Fail if any switch jump table in a tc32 image points outside its own function.

tc32-elf-gcc at -Os emits switch tables as byte offsets but dispatches them with a 32-bit
address load (tloadr rX,[rX,rI]; tmov pc,rX), so taking the switch jumps into garbage. It links
and passes every host test; on the chip it hangs on the first command. This walks the objdump
disassembly for that dispatch shape, reads the table the literal points at, and checks each entry
is an address inside the function that owns the table.

    check_jump_tables.py OBJDUMP ELF BIN
"""

from __future__ import annotations

import re
import struct
import subprocess
import sys

FUNC = re.compile(r"^([0-9a-f]+) <([^>]+)>:$")
INSN = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
LIT = re.compile(r"\[pc, #\d+\]\s*; \(([0-9a-f]+) ")
CMP = re.compile(r"^r(\d+), #(\d+)$")


def main() -> int:
    objdump, elf, binpath = sys.argv[1:4]
    image = open(binpath, "rb").read()
    dis = subprocess.run([objdump, "-d", "--no-show-raw-insn", elf], capture_output=True, text=True,
                         check=True).stdout.splitlines()

    funcs: list[tuple[int, str]] = []
    insns: list[tuple[int, str, str, int]] = []  # addr, mnemonic, operands, function index
    for line in dis:
        m = FUNC.match(line)
        if m:
            funcs.append((int(m.group(1), 16), m.group(2)))
            continue
        m = INSN.match(line)
        if m and funcs:
            insns.append((int(m.group(1), 16), m.group(2), m.group(3), len(funcs) - 1))

    def func_end(i: int) -> int:
        return funcs[i + 1][0] if i + 1 < len(funcs) else len(image)

    tables = bad = 0
    for k in range(2, len(insns)):
        a0, m0, o0, fi = insns[k - 2]
        _, m1, o1, _ = insns[k - 1]
        _, m2, o2, _ = insns[k]
        if not (m2 == "tmov" and o2.startswith("pc, r") and m1 == "tloadr" and m0 == "tloadr"):
            continue
        lit = LIT.search(o0)
        if not lit or not re.match(r"r\d+, \[r\d+, r\d+\]", o1):
            continue
        lit_addr = int(lit.group(1), 16)
        if lit_addr + 4 > len(image):
            continue
        table = struct.unpack_from("<I", image, lit_addr)[0]
        # Entry count from the nearest preceding bound check (tcmp rN, #max).
        count = None
        for j in range(k - 3, max(k - 12, 0), -1):
            if insns[j][1] == "tcmp":
                c = CMP.match(insns[j][2])
                if c:
                    count = int(c.group(2)) + 1
                    break
        if count is None or table + 4 * count > len(image):
            continue
        start, name = funcs[fi]
        end = func_end(fi)
        tables += 1
        entries = struct.unpack_from(f"<{count}I", image, table)
        outside = [e for e in entries if not (start <= e < end)]
        if outside:
            bad += 1
            print(f"BAD jump table in {name} @0x{a0:x}: {count} entries at 0x{table:x}, "
                  f"{len(outside)} outside 0x{start:x}..0x{end:x} (e.g. 0x{outside[0]:x})")
    print(f"jump tables checked: {tables}, bad: {bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
