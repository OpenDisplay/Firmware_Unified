#!/usr/bin/env python3
"""Fail the build unless the link layer's ACL FIFOs are real storage (src/od_tlsr_fifo.c).

liblt_825x.a declares blt_rxfifo_b / blt_txfifo_b as common symbols. If no object defines them
the linker allocates one byte each, and the image advertises but cannot hold a connection.

    check_ll_fifos.py --nm <tc32-elf-nm> --elf <image.elf>
"""
import argparse
import subprocess
import sys

MIN = {"blt_rxfifo_b": 64 * 4, "blt_txfifo_b": 40 * 8, "blt_rxfifo": 12, "blt_txfifo": 12}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", required=True)
    ap.add_argument("--elf", required=True)
    a = ap.parse_args()
    out = subprocess.run([a.nm, "-S", a.elf], check=True, capture_output=True, text=True).stdout
    sizes = {}
    for line in out.splitlines():
        f = line.split()
        if len(f) == 4 and f[3] in MIN:
            sizes[f[3]] = int(f[1], 16)
    bad = [f"{n}: {sizes.get(n, 'missing')} B, need >= {m}" for n, m in MIN.items()
           if sizes.get(n, 0) < m]
    if bad:
        sys.exit("link-layer FIFOs not defined (src/od_tlsr_fifo.c):\n  " + "\n  ".join(bad))
    print("link-layer FIFOs: " + ", ".join(f"{n} {sizes[n]} B" for n in MIN))


if __name__ == "__main__":
    main()
