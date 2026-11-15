#!/usr/bin/env python3
"""Derive the OpenDisplay image's link script from the SDK's B85 boot.link.

Deep-retention sleep keeps the first 32 KB of SRAM and restarts at main(); the startup code then
copies .data and zeroes .bss again, so the SDK's own layout loses every ordinary static on every
wake. This places .data and .bss inside .retention_data instead. The ROM loads that section from
flash on a cold boot only (zeros for the .bss part), so statics start as C requires and survive
every wake, and the startup code's own .data/.bss loops run over empty ranges. The SDK's
"_retention_size_ < 0x8000" assert then covers all of it.

The SDK script is read, never copied into this repository: TELINK_SDK_DIR stays the only source.

  retention_link.py SDK_BOOT_LINK OUT
"""
import sys

ANCHOR = "*(.retention_data.*)"
ADDED = """
        *(.data)
        *(.data.*)
        *(.sbss)
        *(.sbss.*)
        *(.bss)
        *(.bss.*)
        *(COMMON)"""


def main():
    src, out = sys.argv[1], sys.argv[2]
    text = open(src, encoding="utf-8").read()
    if text.count(ANCHOR) != 1:
        sys.exit(f"{src}: expected one '{ANCHOR}' in .retention_data, found {text.count(ANCHOR)}")
    text = text.replace(ANCHOR, ANCHOR + ADDED, 1)
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)


if __name__ == "__main__":
    main()
