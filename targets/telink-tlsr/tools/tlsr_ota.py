#!/usr/bin/env python3
"""Install a sealed Telink image on a tag running the OpenDisplay TLSR updater.

    tlsr_ota.py [--sec2 [--key-store PATH]] status  ADDRESS
    tlsr_ota.py [--sec2 [--key-store PATH]] stage   ADDRESS IMAGE.bin   upload and verify only
    tlsr_ota.py [--sec2 [--key-store PATH]] install ADDRESS IMAGE.bin   ... then install, reboot

Wire protocol: src/od_tlsr_ota.h. The image is checked locally first (tlsr_image.py's rules),
so a file the tag would refuse is never sent. This tool is the bring-up way back to the tag's
previous firmware, not a release mechanism.

A claimed tag (or one in claim mode) refuses everything but STATUS with `locked` until an
authenticated CMD_ENTER_DFU on the same link. --sec2 does that through py-opendisplay (3.x,
importable on PYTHONPATH): it opens a SEC2 session with the key from the key store (default
~/.config/opendisplay/sec2_keys.json), sends ENTER_DFU, then runs the updater on that link.
"""
import argparse
import asyncio
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tlsr_image  # noqa: E402

from bleak import BleakClient, BleakScanner  # noqa: E402

OTA_CHAR = "6f64f001-1e1e-4c4b-8b5a-7454534c5200"
STATES = ["idle", "erasing", "receiving", "verifying", "verified", "installing", "failed"]
BOOTS = ["power-on", "planned", "unplanned"]
ERRORS = ["ok", "state", "length", "offset", "flash", "header", "crc", "opcode", "locked", "supply"]
ENTER_DFU = b"\x00\x51"


def err_name(e):
    return ERRORS[e] if e < len(ERRORS) else f"err{e}"


class Updater:
    def __init__(self, client):
        self.c = client
        self.q = asyncio.Queue()

    async def start(self):
        await self.c.start_notify(OTA_CHAR, lambda _h, d: self.q.put_nowait(bytes(d)))

    async def write(self, data, response=True):
        await self.c.write_gatt_char(OTA_CHAR, data, response=response)

    async def reply(self, op, timeout):
        while True:
            r = await asyncio.wait_for(self.q.get(), timeout)
            if r and r[0] == (op | 0x80):
                return r

    async def status(self):
        await self.write(b"\x05")
        r = await self.reply(0x05, 5.0)
        state, nxt, ln, err = r[1], *struct.unpack_from("<II", r, 2), r[10]
        out = (STATES[state] if state < len(STATES) else state, nxt, ln, err_name(err))
        if len(r) >= 13 and r[11] != 0xFF:
            # Why the tag last booted, and its unplanned resets since power was applied.
            out += (BOOTS[r[11]] if r[11] < len(BOOTS) else f"boot{r[11]}", r[12])
        return out

    async def stage(self, image, chunk):
        await self.write(b"\x01" + struct.pack("<I", len(image)))
        r = await self.reply(0x01, 30.0)
        if r[1] == ERRORS.index("locked"):
            raise RuntimeError("BEGIN refused: locked (claimed tag: run with --sec2)")
        if r[1] != 0:
            raise RuntimeError(f"BEGIN refused: {err_name(r[1])}")
        for off in range(0, len(image), chunk):
            part = image[off:off + chunk]
            await self.write(b"\x02" + struct.pack("<I", off) + part)
            if (off // chunk) % 64 == 0:
                print(f"  {off + len(part)}/{len(image)}", flush=True)
        await self.write(b"\x03")
        r = await self.reply(0x03, 30.0)
        if r[1] != 0:
            st = await self.status()
            raise RuntimeError(f"FINISH refused: {err_name(r[1])} (status {st})")
        print("  staged and verified")

    async def install(self):
        await self.write(b"\x04")
        r = await self.reply(0x04, 5.0)
        if r[1] != 0:
            raise RuntimeError(f"INSTALL refused: {err_name(r[1])}")
        print("  installing; the tag reboots in a few seconds")


async def acquire_mtu(c):
    # BlueZ reports 23 until asked; the exchange itself happened at connect. Ask before
    # anything else is in flight on the link.
    acquire = getattr(c._backend, "_acquire_mtu", None)
    if acquire is not None:
        try:
            await acquire()
        except Exception as e:  # noqa: BLE001 -- fall back to the default, loudly
            print(f"  MTU not acquired ({type(e).__name__}: {e}); using 23")


async def updater_steps(c, args, image):
    u = Updater(c)
    await u.start()
    mtu = c.mtu_size or 23
    # One write is ATT_MTU - 3 bytes: the 5-byte DATA header, then the chunk.
    chunk = min(239, mtu - 3 - 5)
    if args.cmd == "status":
        print(await u.status())
        return
    print(f"MTU {mtu}, {chunk}-byte chunks, {len(image)} bytes")
    await u.stage(image, chunk)
    if args.cmd == "install":
        await u.install()


async def run_sec2(args, image):
    try:
        from epaper_dithering import ColorScheme
        from opendisplay import OpenDisplayDevice
        from opendisplay.models.capabilities import DeviceCapabilities
        from opendisplay.sec2.keystore import FileKeyStore
        from opendisplay.sec2.options import SecurityOptions
    except ImportError as e:
        sys.exit(f"--sec2 needs py-opendisplay 3.x on PYTHONPATH ({e})")
    sec = SecurityOptions(key_store=FileKeyStore(args.key_store))
    caps = DeviceCapabilities(width=1, height=1, color_scheme=ColorScheme.MONO)   # no config read
    async with OpenDisplayDevice(mac_address=args.address, security=sec, capabilities=caps,
                                 timeout=30.0) as dev:
        # Sealed when a session is up; the reply is read so the unlock is in place before the
        # first updater write, which the tag handles outside the command queue.
        await dev._write(ENTER_DFU)
        r = await dev._read(10.0)
        if r[:2] != ENTER_DFU:
            sys.exit(f"ENTER_DFU refused: {r.hex()}")
        print("  ENTER_DFU acknowledged; updater unlocked for this link")
        c = dev._connection._client
        await acquire_mtu(c)
        await updater_steps(c, args, image)


async def run(args):
    image = None
    if args.cmd in ("stage", "install"):
        with open(args.image, "rb") as f:
            image = f.read()
        problem = tlsr_image.check(image)
        if problem:
            sys.exit(f"{args.image}: {problem}")
    if args.sec2:
        await run_sec2(args, image)
        return
    dev = await BleakScanner.find_device_by_address(args.address, timeout=15.0)
    if dev is None:
        sys.exit(f"{args.address} not seen")
    async with BleakClient(dev, timeout=30.0) as c:
        await acquire_mtu(c)
        await updater_steps(c, args, image)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sec2", action="store_true", help="authenticate and send ENTER_DFU first")
    ap.add_argument("--key-store", default=None, help="SEC2 key store (py-opendisplay default)")
    ap.add_argument("cmd", choices=["status", "stage", "install"])
    ap.add_argument("address")
    ap.add_argument("image", nargs="?")
    args = ap.parse_args()
    if args.cmd != "status" and not args.image:
        ap.error("an image is required")
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
