#!/usr/bin/env python3
"""Send a sealed Telink image to a tag running the legacy firmware, through the legacy updater.

    legacy_ota.py info    ADDRESS             0x0005 display info and 0x0011 config, read only
    legacy_ota.py install ADDRESS IMAGE.bin   send the image as a firmware update (data type 0x03)

This is how OpenDisplay gets onto a legacy tag, and how the legacy firmware is proven to update itself first. The
wire is the legacy firmware's OpenEPaperLink block transfer, over characteristic 0x1337 (writes without
response, replies as notifications):

    host  0064 <checksum u8><data_ver u64><size u32><type u8><arg u8><next_check_in u16>
    tag   00C6 ... block_id at byte 11      request one 4096-byte block
    host  0002                              prepare block
    host  0065 <sum u8><block u8><part u8><230 bytes>   parts of [len u16][sum16 u16] + block
    tag   00C5 next part | 00C4 resend part | 00C9 firmware stored, installing

The legacy firmware's install is an erase-and-copy of its own image area from RAM and is not power-fail safe:
the tag must stay powered until it advertises again. The image is checked locally first
(tlsr_image.py) so a file the tag would refuse is never sent.
"""
import argparse
import asyncio
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tlsr_image  # noqa: E402

from bleak import BleakClient, BleakScanner  # noqa: E402

CHAR = "00001337-0000-1000-8000-00805f9b34fb"
BLOCK = 4096
PART = 230
DATATYPE_FW_UPDATE = 0x03


def part_packet(block_id, part_id, data):
    buf = bytearray(3 + PART)
    buf[1] = block_id & 0xFF
    buf[2] = part_id & 0xFF
    buf[3:3 + len(data)] = data
    buf[0] = sum(buf[1:]) & 0xFF
    return bytes(buf)


def block_parts(image, block_id):
    body = image[block_id * BLOCK:(block_id + 1) * BLOCK]
    blob = struct.pack("<HH", len(body), sum(body) & 0xFFFF) + body
    return [part_packet(block_id, i, blob[o:o + PART]) for i, o in enumerate(range(0, len(blob), PART))]


class Tag:
    def __init__(self, client):
        self.c = client
        self.q = asyncio.Queue()

    async def start(self):
        await self.c.start_notify(CHAR, lambda _h, d: self.q.put_nowait(bytes(d)))
        await asyncio.sleep(0.45)

    async def send(self, data):
        await self.c.write_gatt_char(CHAR, data, response=False)

    async def next(self, timeout=10.0):
        return await asyncio.wait_for(self.q.get(), timeout)

    async def read(self, cmd, done):
        await self.send(cmd)
        out = []
        try:
            while True:
                r = await self.next(3.0)
                out.append(r)
                if r[:2] in done:
                    break
        except asyncio.TimeoutError:
            pass
        return out

    async def install(self, image):
        info = struct.pack("<BQIBBH", 0xFF, zlib.crc32(image) & 0xFFFFFFFF, len(image),
                           DATATYPE_FW_UPDATE, 0, 0)
        await self.send(b"\x00\x64" + info)
        parts, idx, blocks = [], 0, (len(image) + BLOCK - 1) // BLOCK
        while True:
            r = await self.next(15.0)
            code = r[:2]
            if code == b"\x00\xc6" and len(r) >= 12:
                block = r[11]
                if block >= blocks:
                    raise RuntimeError(f"tag asked for block {block} of {blocks}")
                await self.send(b"\x00\x02")
                await asyncio.sleep(0.05)
                parts, idx = block_parts(image, block), 0
                await self.send(b"\x00\x65" + parts[idx])
                print(f"  block {block + 1}/{blocks}", flush=True)
            elif code == b"\x00\xc4":
                await self.send(b"\x00\x65" + parts[idx])
            elif code == b"\x00\xc5":
                idx += 1
                if idx < len(parts):
                    await self.send(b"\x00\x65" + parts[idx])
            elif code == b"\x00\xc9":
                print("  firmware stored; the tag installs it and restarts")
                return
            elif code in (b"\x00\xc7", b"\x00\xc8"):
                raise RuntimeError(f"tag took the transfer as an image ({code.hex()}), not firmware")
            elif code != b"\x00\x63":
                print(f"  (tag: {r.hex()})")


async def run(args):
    image = None
    if args.cmd == "install":
        with open(args.image, "rb") as f:
            image = f.read()
        problem = tlsr_image.check(image)
        if problem:
            sys.exit(f"{args.image}: {problem}")
    dev = await BleakScanner.find_device_by_address(args.address, timeout=15.0)
    if dev is None:
        sys.exit(f"{args.address} not seen")
    # The legacy firmware advertises about every 4 s, so the connect window is long.
    async with BleakClient(dev, timeout=45.0) as c:
        t = Tag(c)
        await t.start()
        if args.cmd == "info":
            for cmd, done in ((b"\x00\x05", (b"\x00\x05",)), (b"\x00\x11", (b"\x00\xcd",))):
                for r in await t.read(cmd, done):
                    print(f"{cmd.hex()} -> [{len(r)}] {r.hex()}")
            return
        print(f"{len(image)} bytes, {(len(image) + BLOCK - 1) // BLOCK} blocks")
        await t.install(image)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["info", "install"])
    ap.add_argument("address")
    ap.add_argument("image", nargs="?")
    args = ap.parse_args()
    if args.cmd == "install" and not args.image:
        ap.error("an image is required")
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
