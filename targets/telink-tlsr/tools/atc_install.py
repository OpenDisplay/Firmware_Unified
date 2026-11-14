#!/usr/bin/env python3
"""Install this firmware on a tag running ATC_BLE_OEPL, over BLE -- what ATC's web uploader does
under "Select Firmware".

ATC takes a firmware image through its ordinary block transfer: data info 0x0064 with data type
0x03 (and data_ver 123, as the web page sends), then 4 KB blocks the tag requests one at a time.
It answers 00C9 once it has checked the image's CRC32 tail and copied it over itself, then reboots
into it. One way: there is no ATC left to fall back to, so read the tag's config first
(`atc-ble od-config`).

Needs py-atc-ble-oepl (https://github.com/OpenDisplay/py-atc-ble-oepl) for the transfer:

    uv run --with py-atc-ble-oepl atc_install.py --device ADDRESS IMAGE.bin
"""

from __future__ import annotations

import argparse
import asyncio
import math
import struct
import sys
import zlib

from bleak import BleakScanner
from py_atc_ble_oepl import ATCDevice
from py_atc_ble_oepl.imaging import uploader as up
from py_atc_ble_oepl.imaging.encoding import create_data_info

DATA_TYPE_FIRMWARE = 0x03
FW_ACCEPTED = "00C9"


def check_image(image: bytes) -> None:
    if image[8:12] != b"KNLT" or struct.unpack_from("<I", image, 24)[0] != len(image):
        raise SystemExit("not a finished Telink image (KNLT magic / length field)")
    if struct.unpack_from("<I", image, len(image) - 4)[0] != zlib.crc32(image[:-4]) ^ 0xFFFFFFFF:
        raise SystemExit("CRC32 tail missing or wrong: ATC would keep its own firmware (run finish_image.py)")


class FirmwareUploader(up.BLEImageUploader):
    """py-atc-ble-oepl's block uploader, plus the firmware-accepted reply it does not know."""

    async def _handle_response(self, response: bytes) -> bool:
        code = response[:2].hex().upper()
        if code == FW_ACCEPTED:
            print(f"\n{FW_ACCEPTED}: ATC accepted the image and reboots into it", flush=True)
            self._upload_complete.set()
            return False
        if code == "00C6" and len(response) >= 12:
            print(f"\rblock {response[11] + 1}/{self._total_blocks}", end="", flush=True)
        return await super()._handle_response(response)


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--device", required=True, help="BLE address of the ATC tag")
    ap.add_argument("--timeout", type=float, default=90.0)
    args = ap.parse_args()

    image = open(args.image, "rb").read()
    check_image(image)
    dev = await BleakScanner.find_device_by_address(args.device, timeout=args.timeout)
    if dev is None:
        print(f"{args.device} not found", file=sys.stderr)
        return 1
    async with ATCDevice(args.device, ble_device=dev, connection_timeout=args.timeout) as tag:
        u = FirmwareUploader(tag._connection, args.device)
        u._img_array = image
        u._img_array_len = len(image)
        u._total_blocks = math.ceil(len(image) / up.BLE_BLOCK_SIZE)
        u._upload_complete.clear()
        print(f"sending {len(image)} bytes in {u._total_blocks} blocks", flush=True)
        await tag._connection.write_command(
            bytes.fromhex("0064") + create_data_info(255, 123, len(image), DATA_TYPE_FIRMWARE, 0, 0))
        while not u._upload_complete.is_set():
            response = await u._wait_for_response(timeout=30)
            if response is None:
                print("\ntimeout waiting for the tag", file=sys.stderr)
                return 1
            await u._handle_response(response)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
