#!/usr/bin/env python3
"""Write a firmware image to a TLSR825x over its single-wire debug pin (SWS), write-only.

Wiring: USB-serial adapter TX -> 1..1.8 kOhm -> SWS (PA7), GND -> GND. RX is not needed: nothing
is read back, so verify the result some other way (the tag advertising the new build over BLE).
3.3 V logic only.

Each SWS bit is sent as one UART byte (0x80 = 1, 0xFE = 0); the series resistor turns those into
the pulse widths the SWS slave expects. This is the register sequence ATC_BLE_OEPL's web flasher
and pvvx's TlsrComProg825x use: halt the MCU through 0x0602, drive the flash controller through
0x0c/0x0d (SPI command bytes, chip-select), and restart through 0x006f.

    sws_flash.py --port /dev/cu.usbserial-0001 IMAGE.bin
    sws_flash.py --port ... --invalidate-bank2 atc_fw.bin     going back to a firmware that lives at 0x0

The image always goes to 0x0. A tag updated over the air may be running from the second bank
(0x40000, where OpenDisplay's updates land), whose boot marker stays valid after this write;
--invalidate-bank2 erases that bank's first sector so only the image just written can boot.
"""

from __future__ import annotations

import argparse
import sys
import time

import serial

SECTOR = 0x1000
PAGE = 256
BANK2 = 0x40000


def sws_packet(addr: int, data: bytes) -> bytes:
    """One SWS write: start + 0x5a command, 24-bit address, 0x00 (write), data bytes, stop."""
    out = bytearray()

    def byte(value: int, first: bool) -> None:
        cell = bytearray(10)
        cell[0] = 0x80 if first else 0xFE          # start: 1 for the command byte, 0 after
        for i in range(8):
            cell[1 + i] = 0x80 if value & (0x80 >> i) else 0xFE
        cell[9] = 0xFE                              # stop bit
        out.extend(cell)

    header = (0x5A, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF, 0x00)
    for n, b in enumerate(header):
        byte(b, n == 0)
    for b in data:
        byte(b, False)
    out.extend(b"\x80" * 9 + b"\xfe")              # end-of-command marker (0xff)
    return bytes(out)


class Sws:
    def __init__(self, port: str, baud: int) -> None:
        self.s = serial.Serial(port, baud, bytesize=8, parity="N", stopbits=1, timeout=1, write_timeout=5)

    def wr(self, addr: int, data: bytes) -> None:
        self.s.write(sws_packet(addr, data))

    def flush(self) -> None:
        self.s.flush()

    # -- flash controller: 0x0d = chip-select (0 asserts), 0x0c = SPI data byte -------------
    def flash_cmd(self, cmd: int) -> None:
        self.wr(0x0D, b"\x00")
        self.wr(0x0C, bytes([cmd, 0x01]))

    def write_enable(self) -> None:
        self.flash_cmd(0x06)

    def unlock(self) -> None:
        # Status register write: 0x01 0x00 (and again with a 16-bit status) clears block protection.
        self.write_enable()
        self.wr(0x0D, b"\x00"); self.wr(0x0C, b"\x01"); self.wr(0x0C, b"\x00\x01")
        self.write_enable()
        self.wr(0x0D, b"\x00"); self.wr(0x0C, b"\x01"); self.wr(0x0C, b"\x00"); self.wr(0x0C, b"\x00\x01")

    def erase_sector(self, addr: int) -> None:
        self.write_enable()
        self.wr(0x0D, b"\x00")
        self.wr(0x0C, b"\x20")
        self.wr(0x0C, bytes([(addr >> 16) & 0xFF]))
        self.wr(0x0C, bytes([(addr >> 8) & 0xFF]))
        self.wr(0x0C, bytes([addr & 0xFF, 0x01]))
        self.flush()
        time.sleep(0.3)

    def write_page(self, addr: int, data: bytes) -> None:
        self.write_enable()
        self.wr(0x0D, b"\x00")
        self.wr(0x00B3, b"\x80")                   # SWS FIFO mode: stream to 0x0c
        self.wr(0x0C, bytes([0x02, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF]) + data)
        self.wr(0x00B3, b"\x00")
        self.wr(0x0D, b"\x01")
        self.flush()
        time.sleep(0.01)

    def activate(self, ms: int) -> None:
        """Soft-reset the MCU through SWS, then keep halting it while it boots."""
        self.s.dtr = True; self.s.rts = True       # reset lines, if wired (harmless if not)
        time.sleep(0.1)
        self.s.dtr = False; self.s.rts = False
        self.wr(0x006F, b"\x20")                   # soft reset
        halt = sws_packet(0x0602, b"\x05")
        end = time.monotonic() + ms / 1000
        while time.monotonic() < end:
            self.s.write(halt)
        self.wr(0x00B2, bytes([55]))               # SWS speed
        self.s.write(halt)
        self.flash_cmd(0xAB)                       # release flash from deep power-down
        self.flush()

    def analog_write(self, reg: int, value: int) -> None:
        """Write an analog register through the analog bus (0xb8 addr, 0xb9 data, 0xba ctrl), as
        the SDK's analog_write() does. SWS is slow enough that the busy bit never needs polling."""
        self.wr(0x00B8, bytes([reg]))
        self.wr(0x00B9, bytes([value]))
        self.wr(0x00BA, b"\x60")                  # FLD_ANA_CYC0 | FLD_ANA_RW: start a write
        self.wr(0x00BA, b"\x00")
        self.flush()

    def reset(self) -> None:
        # The firmware's breadcrumb (analog 0x3b) survives every reset but a power cycle, and holds
        # whatever step the MCU was in when SWS halted it. Clear it, or the next boot reports the
        # flash as a watchdog hang.
        self.analog_write(0x3B, 0x00)
        self.wr(0x006F, b"\x20")
        self.flush()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--activate-ms", type=int, default=1000)
    ap.add_argument("--no-reset", action="store_true", help="leave the MCU halted after writing")
    ap.add_argument("--invalidate-bank2", action="store_true",
                    help="also erase the header sector of the image at 0x40000, so it cannot boot")
    args = ap.parse_args()

    image = open(args.image, "rb").read()
    if image[8:12] != b"KNLT":
        print("refusing: not a Telink image (no KNLT at offset 8)", file=sys.stderr)
        return 1

    sws = Sws(args.port, args.baud)
    t0 = time.monotonic()
    print(f"activate ({args.activate_ms} ms) ...", flush=True)
    sws.activate(args.activate_ms)
    sws.write_enable()
    sws.unlock()
    sws.flush()
    time.sleep(1.5)

    for addr in range(0, len(image), PAGE):
        if addr % SECTOR == 0:
            print(f"\r{addr * 100 // len(image):3d}%  erase 0x{addr:06x}", end="", flush=True)
            sws.erase_sector(addr)
        sws.write_page(addr, image[addr:addr + PAGE])
    print(f"\r100%  wrote {len(image)} bytes in {time.monotonic() - t0:.1f} s (not verified: SWS is write-only here)")
    if args.invalidate_bank2:
        if len(image) > BANK2:
            print("refusing --invalidate-bank2: the image itself reaches 0x40000", file=sys.stderr)
            return 1
        sws.erase_sector(BANK2)
        print("erased 0x040000: the second bank's image can no longer boot")

    if not args.no_reset:
        sws.reset()
        print("reset: the MCU is starting the new image")
    return 0


if __name__ == "__main__":
    sys.exit(main())
