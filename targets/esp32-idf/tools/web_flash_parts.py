#!/usr/bin/env python3
"""Split a build into per-region images plus an esp-web-tools manifest.

The merged image (opendisplay-<board>-merged.bin) is one file from 0x0 to the end of the app,
padded with 0xFF in between -- so flashing it erases NVS at 0x9000-0xDFFF, which is where the
device config lives. The web flasher writes these parts instead, each at its own offset, and
leaves NVS alone: a firmware update from the browser keeps the device's config.

Offsets come from IDF's flash_args (its own record of what goes where), never from a table
here, because the classic ESP32 puts its bootloader at 0x1000 and every other chip at 0x0.
A part that would land inside NVS is refused: that would mean the partition table moved and
the "keeps NVS" promise no longer holds.

The app part is also the BLE/SMP OTA image (opendisplay-<board>-app.bin): it is exactly the
image an OTA slot takes.

usage: web_flash_parts.py --build-dir <dir> --board <board> --chip <idf target> --version <v>
writes <build-dir>/web/opendisplay-<board>-{bootloader,partition-table,ota-data,app}.bin
   and <build-dir>/web/opendisplay-<board>-manifest.json
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

CHIP_FAMILY = {
    "esp32": "ESP32",
    "esp32s2": "ESP32-S2",
    "esp32s3": "ESP32-S3",
    "esp32c3": "ESP32-C3",
    "esp32c6": "ESP32-C6",
}

PART_NAME = {
    "bootloader.bin": "bootloader",
    "partition-table.bin": "partition-table",
    "ota_data_initial.bin": "ota-data",
}

NVS_START, NVS_END = 0x9000, 0xE000  # half-open; must stay untouched by every part


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--board", required=True)
    ap.add_argument("--chip", required=True)
    ap.add_argument("--version", required=True)
    args = ap.parse_args()

    build = Path(args.build_dir)
    family = CHIP_FAMILY.get(args.chip)
    if family is None:
        print(f"web_flash_parts: unknown chip {args.chip!r}", file=sys.stderr)
        return 1

    entries = []
    for line in (build / "flash_args").read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("--"):
            continue
        offset_text, rel = line.split(None, 1)
        entries.append((int(offset_text, 0), rel))
    entries.sort()

    out = build / "web"
    out.mkdir(exist_ok=True)
    parts = []
    for offset, rel in entries:
        src = build / rel
        size = src.stat().st_size
        if offset < NVS_END and offset + size > NVS_START:
            print(f"web_flash_parts: {rel} at 0x{offset:X} (+{size}) overlaps NVS "
                  f"0x{NVS_START:X}-0x{NVS_END - 1:X}", file=sys.stderr)
            return 1
        name = PART_NAME.get(Path(rel).name, "app")
        dst_name = f"opendisplay-{args.board}-{name}.bin"
        shutil.copyfile(src, out / dst_name)
        parts.append({"path": dst_name, "offset": offset})

    if sum(1 for p in parts if p["path"].endswith("-app.bin")) != 1:
        print("web_flash_parts: expected exactly one app image in flash_args", file=sys.stderr)
        return 1

    manifest = {
        "name": "OpenDisplay Firmware",
        "version": args.version,
        "home_assistant_domain": "open_display",
        # Parts never touch NVS, so an update keeps the config; an erase would defeat that.
        "new_install_prompt_erase": False,
        "new_install_improv_wait_time": 0,
        "builds": [{"chipFamily": family, "parts": parts}],
    }
    (out / f"opendisplay-{args.board}-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
