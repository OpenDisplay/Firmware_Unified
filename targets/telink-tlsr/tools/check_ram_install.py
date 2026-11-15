#!/usr/bin/env python3
"""Prove from the linked image that the install routine runs only RAM-resident code.

od_tlsr_install() erases the flash the application executes from, so it and everything it calls
must lie in the part of the image the startup code copies to SRAM: .vectors and .ram_code. This
disassembles the routine, follows every direct call transitively, and fails on a call target
outside those sections or on an indirect branch other than a return.

  check_ram_install.py --objdump TC32_OBJDUMP --elf ELF [--map MAP]
"""
import argparse
import re
import subprocess
import sys

ROOT = "od_tlsr_install"


def sections(objdump, elf):
    out = subprocess.run([objdump, "-h", elf], check=True, capture_output=True, text=True).stdout
    found = {}
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 4 and f[1] in (".vectors", ".ram_code"):
            found[f[1]] = (int(f[3], 16), int(f[2], 16))  # vma, size
    if ".ram_code" not in found:
        sys.exit("no .ram_code section in the image")
    lo = min(v for v, _ in found.values())
    hi = max(v + s for v, s in found.values())
    return lo, hi


def disassemble(objdump, elf):
    out = subprocess.run([objdump, "-d", elf], check=True, capture_output=True, text=True).stdout
    funcs, cur = {}, None
    for line in out.splitlines():
        m = re.match(r"^([0-9a-f]+) <([^>]+)>:$", line)
        if m:
            cur = (int(m.group(1), 16), m.group(2))
            funcs[cur] = []
            continue
        if cur is not None and re.match(r"^\s+[0-9a-f]+:", line):
            funcs[cur].append(line)
    return funcs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--objdump", required=True)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--map")
    a = ap.parse_args()

    lo, hi = sections(a.objdump, a.elf)
    funcs = disassemble(a.objdump, a.elf)
    by_addr = {addr: name for addr, name in funcs}
    by_name = {name: addr for addr, name in funcs}
    if ROOT not in by_name:
        sys.exit(f"{ROOT} not found in the image")

    todo, seen, errors = [ROOT], set(), []
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        seen.add(name)
        addr = by_name[name]
        if not (lo <= addr < hi):
            errors.append(f"{name} at {addr:#x} is outside RAM code [{lo:#x}, {hi:#x})")
        for line in funcs[(addr, name)]:
            ins = line.split("\t")
            text = " ".join(ins[2:]) if len(ins) > 2 else line
            m = re.search(r"\btjl\s+([0-9a-f]+)\s*<([^>+]+)", text)
            if m:
                tgt, tname = int(m.group(1), 16), m.group(2)
                if not (lo <= tgt < hi):
                    errors.append(f"{name} calls {tname} at {tgt:#x}, outside RAM code")
                elif tname in by_name:
                    todo.append(tname)
                continue
            m = re.search(r"\btjex\s+(\w+)", text)
            if m and m.group(1) != "lr":
                errors.append(f"{name}: indirect branch '{text.strip()}'")
            m = re.search(r"\btj\w*\s+([0-9a-f]+)\s*<([^>+]+)", text)
            if m:
                tgt = int(m.group(1), 16)
                if not (lo <= tgt < hi):
                    errors.append(f"{name} branches to {m.group(2)} at {tgt:#x}, outside RAM code")
    if errors:
        print("install routine is not RAM-resident:", file=sys.stderr)
        for e in errors:
            print("  " + e, file=sys.stderr)
        return 1
    print(f"install routine RAM-resident: {', '.join(sorted(seen))} within [{lo:#x}, {hi:#x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
