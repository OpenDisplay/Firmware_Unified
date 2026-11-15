#!/usr/bin/env python3
"""Telink TLSR825x firmware images: seal a raw objcopy output, or check a sealed image.

A sealed image is what the ROM boots, what the legacy firmware's updater accepts and what
src/od_tlsr_ota.c verifies:

  offset 8     "KNLT"  (the byte at 8 is the ROM's boot flag)
  offset 0x18  u32 LE  total length of the sealed file
  body padded with 0xFF to a multiple of 16, then
  u32 LE       CRC-32 of everything before it, without the final inversion
               (zlib.crc32(body) ^ 0xFFFFFFFF), as Telink's tl_check_fw writes it

  tlsr_image.py seal  RAW OUT     write OUT from RAW
  tlsr_image.py check IMAGE...    verify each sealed image; exit 1 on any failure
"""
import struct
import sys
import zlib

MAX_IMAGE = 0x2D000  # src/od_tlsr_layout.h OD_TLSR_IMAGE_MAX


def seal(raw: bytes) -> bytes:
    if raw[8:12] != b"KNLT":
        raise ValueError("no KNLT marker at offset 8: not a Telink B85 image")
    body = bytearray(raw)
    while len(body) % 16:
        body.append(0xFF)
    total = len(body) + 4
    struct.pack_into("<I", body, 0x18, total)
    crc = zlib.crc32(bytes(body)) ^ 0xFFFFFFFF
    return bytes(body) + struct.pack("<I", crc)


def check(img: bytes) -> str:
    if len(img) < 0x40:
        return "too short"
    if img[8:12] != b"KNLT":
        return "no KNLT marker at offset 8"
    (size,) = struct.unpack_from("<I", img, 0x18)
    if size != len(img):
        return f"size field {size} != file length {len(img)}"
    if len(img) > MAX_IMAGE:
        return f"{len(img)} bytes exceeds the {MAX_IMAGE}-byte image cap"
    (crc,) = struct.unpack_from("<I", img, len(img) - 4)
    want = zlib.crc32(img[:-4]) ^ 0xFFFFFFFF
    if crc != want:
        return f"CRC {crc:08x} != {want:08x}"
    return ""


def main(argv):
    if len(argv) == 4 and argv[1] == "seal":
        raw = open(argv[2], "rb").read()
        out = seal(raw)
        err = check(out)
        if err:
            print(f"{argv[3]}: {err}", file=sys.stderr)
            return 1
        open(argv[3], "wb").write(out)
        print(f"{argv[3]}: {len(out)} bytes sealed ({MAX_IMAGE - len(out)} below the cap)")
        return 0
    if len(argv) >= 3 and argv[1] == "check":
        rc = 0
        for p in argv[2:]:
            err = check(open(p, "rb").read())
            print(f"{p}: {err or 'ok'}")
            rc |= bool(err)
        return rc
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
