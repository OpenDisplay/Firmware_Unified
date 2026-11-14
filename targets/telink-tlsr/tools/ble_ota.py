#!/usr/bin/env python3
"""Update a TLSR825x OpenDisplay tag over BLE.

Arms the tag's Telink OTA service with OpenDisplay's ENTER_DFU (0x0051) and streams the image
with Telink's legacy OTA protocol:

    0xFF00 (version), 0xFF01 (start)
    per 16 image bytes: index u16 LE | 16 bytes | CRC-16/MODBUS(index + bytes) u16 LE
    0xFF02 | last index u16 LE | ~last index u16 LE

The tag writes the image to its other flash bank, checks the image's CRC32 tail, and reboots into
it only if that passes; a failed or interrupted update leaves the running firmware in place. The
status byte is read back every 8 blocks (0 = fine), which also paces write-without-response.

Encrypted devices are not supported yet: ENTER_DFU is sent in plaintext.

    ble_ota.py --device ODxxxxxx|ADDRESS IMAGE.bin
"""

from __future__ import annotations

import argparse
import asyncio
import struct
import sys
import time
import zlib

from bleak import BleakClient, BleakScanner

OD_CHAR = "00002446-0000-1000-8000-00805f9b34fb"
OTA_CHAR = "00010203-0405-0607-0809-0a0b0c0d2b12"
CMD_ENTER_DFU = b"\x00\x51"
STATUS_EVERY = 8

OTA_ERRORS = {
    1: "packet sequence error", 2: "invalid packet", 3: "packet CRC error", 4: "flash write error",
    5: "data incomplete", 6: "flow error", 7: "firmware CRC check failed", 8: "version compare",
    9: "PDU length", 10: "firmware mark (not a Telink image)", 11: "firmware size",
    12: "data packet timeout", 13: "OTA timeout", 14: "connection terminated",
}


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def check_image(image: bytes) -> None:
    if image[8:12] != b"KNLT":
        raise SystemExit("not a Telink image (no KNLT at offset 8)")
    if struct.unpack_from("<I", image, 24)[0] != len(image):
        raise SystemExit("length field does not match the file size")
    if len(image) % 16 != 4:
        raise SystemExit("size is not 4 mod 16: the OTA server aborts on such an image (run finish_image.py)")
    if struct.unpack_from("<I", image, len(image) - 4)[0] != zlib.crc32(image[:-4]) ^ 0xFFFFFFFF:
        raise SystemExit("CRC32 tail missing or wrong: the tag would reject this image (run finish_image.py)")


def blocks(image: bytes) -> list[bytes]:
    padded = image + b"\xff" * (-len(image) % 16)
    out = []
    for i in range(len(padded) // 16):
        body = struct.pack("<H", i) + padded[i * 16:(i + 1) * 16]
        out.append(body + struct.pack("<H", crc16_modbus(body)))
    return out


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--device", required=True, help="name (ODxxxxxx) or BLE address")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--no-arm", action="store_true",
                    help="skip ENTER_DFU (tests that an unarmed tag ignores the upload)")
    args = ap.parse_args()

    image = open(args.image, "rb").read()
    check_image(image)
    pkts = blocks(image)

    want = args.device
    dev = await BleakScanner.find_device_by_filter(
        lambda d, a: d.address.lower() == want.lower() or (a.local_name or d.name or "") == want,
        timeout=args.timeout)
    if dev is None:
        print(f"{want} not found", file=sys.stderr)
        return 1

    async with BleakClient(dev, timeout=args.timeout) as c:
        acked = asyncio.Event()

        def on_od(_: object, data: bytearray) -> None:
            if bytes(data[:2]) == b"\x00\x51":
                acked.set()

        await c.start_notify(OD_CHAR, on_od)
        if not args.no_arm:
            await c.write_gatt_char(OD_CHAR, CMD_ENTER_DFU, response=True)
            try:
                await asyncio.wait_for(acked.wait(), 10)
            except asyncio.TimeoutError:
                print("no ENTER_DFU ack: the firmware may predate OTA support", file=sys.stderr)
                return 1
        print(f"{'NOT armed' if args.no_arm else 'armed'}; sending {len(image)} bytes as {len(pkts)} blocks",
              flush=True)

        t0 = time.monotonic()
        await c.write_gatt_char(OTA_CHAR, b"\x00\xff", response=True)
        await c.write_gatt_char(OTA_CHAR, b"\x01\xff", response=True)
        await asyncio.sleep(0.3)
        for i, pkt in enumerate(pkts):
            await c.write_gatt_char(OTA_CHAR, pkt, response=False)
            if (i + 1) % STATUS_EVERY == 0:
                status = await c.read_gatt_char(OTA_CHAR)
                if status and status[0] != 0:
                    print(f"\ntag reported error {status[0]}: {OTA_ERRORS.get(status[0], '?')}", file=sys.stderr)
                    return 1
            if i % 200 == 0:
                print(f"\r{i * 100 // len(pkts):3d}%  block {i}/{len(pkts)}", end="", flush=True)
        last = len(pkts) - 1
        await c.write_gatt_char(OTA_CHAR, struct.pack("<HHH", 0xFF02, last, ~last & 0xFFFF), response=True)
        print(f"\r100%  sent in {time.monotonic() - t0:.1f} s; the tag verifies the image and reboots into it")
        await asyncio.sleep(3)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
