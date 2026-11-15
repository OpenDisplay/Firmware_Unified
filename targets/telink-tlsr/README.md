# telink-tlsr — Telink TLSR825x e-paper tags

The TLSR8359 and related B85-family tags that the community OpenEPaperLink BLE firmware (called the legacy firmware below) runs on.
512 KB flash, 64 KB SRAM, one TC32 core, no kernel.

**Status: provisional bring-up target.** It builds, its plain-C modules have host tests, and
it runs on one tag, OD05B911 (docs/HARDWARE_VERIFICATION_CHECKLIST.md has what was shown
there). What it does today:

- advertises the OpenDisplay MSD and the 0x2446 service;
- runs the shared command, config and SEC2 stack, with claim mode: the power-cycle gestures
  (two quick cycles: claim; five: factory reset), the setup-family screens (SETUP / CODE /
  RESET) through the boot-screen path, and the 0x27 reset pin;
- stores config and secrets in flash, safely across power loss;
- carries an updater that can install another sealed Telink image, including the legacy firmware.

- drives the panel from the config's drive packets (shared `od_display`), over a bit-banged
  bit-banged SPI bus, for full uploads (0x0070, raw or zlib with a 9-bit window) and PIPE (window
  6), plus CMD_SPI_RAW (0x0084) for characterising a panel over the air. No partial or
  drive-write yet;
- reports the supply voltage and the glass temperature in the MSD.

Battery: `PowerOption.battery_sense_pin` names an unused ADC-capable pin (PB0..PB7, PC4, PC5),
which is driven high and sampled, so it reads the rail; there is no divider. OD05B911 uses PB3.
Temperature: the panel controller's own sensor (TSC, cmd 0x40) read over the 3-wire data line at
each panel power-up and kept for an hour; NaN in the MSD until the first one. The MCU's sensor
has no calibration and is not used.

Power: the chip sleeps between radio events whenever nothing is queued, no upload, refresh,
config read, OTA step or raw SPI session is open, and no boot screen is pending; any write turns
it off until the loop drains again. While advertising the sleep is deep retention: the first
32 KB of SRAM survives and main() runs again on the wake. `tools/retention_link.py` derives the
link script from the SDK's so that every static lives in that bank (the build fails if it
outgrows 32 KB; 688 B are left). Connections use plain suspend. Retention starts 60 s after a
boot and only if there has been no unplanned reset since power was applied, so a fault in the
wake path leaves the tag in suspend-only operation, still updatable. Advertising runs at
1000 ms, and at 20-30 ms for 3 s after boot, a disconnect or an `od_adv_app_boost()` caller, as
on Nordic and BG22. Measured on OD060F40 at 3.0 V: about 24 µA mean while idle, with a 4 µA
floor between events (suspend alone: 65 µA mean, 45 µA floor). The panel rail pin is pulled
toward off while the panel is down, because deep sleep keeps the pull resistors but not the
output driver. `build.sh -DOD_TLSR_PMTEST=ON` also builds
`opendisplay_tlsr_pmtest.bin`, a probe that cycles through 30 s of advertising (the window in
which it accepts an update), then plain suspend, deep sleep with retention and deep sleep without,
for reading each floor on a current meter. `-DOD_TLSR_FAULT_TEST=ON` builds the OpenDisplay
image with a hang that GPIO pin 0xFE arms for the next retention wake, to exercise the fallback.
Neither is ever a release image.

CMD_SPI_RAW's GPIO sub-opcode (0x07, provisional, `OD_CAP_SPI_RAW_GPIO`) sets and reads any pin
from PA0..PD7 except PA7, with or without a raw session open; a mode holds until the firmware
next drives that pin, or until the first deep-retention sleep (disconnected, 60 s after boot).

There is no LED. The setup-family screens and the key-state redraw are not wired to the panel.

### Pins and rail

A config pin byte is `group * 8 + bit`: PA0 = 0 … PD7 = 31; 0xFF is "not present". Port E is
refused: PE0..PE3 are the internal flash bus. These tags
switch the panel rail with a P-MOSFET, on when `pwr_pin` is LOW, which the config declares with
the provisional DeviceFlags bit 6 `OD_DEVICE_FLAG_PWR_PIN_ACTIVE_LOW`. OD05B911's panel:
RESET PD4 (28), BUSY PA1 (1), DC PB7 (15), CS PD2 (26), MOSI PB6 (14), CLK PD7 (31), rail PB5
(13), on panel id 0xF001 (`panels/panels.yaml`, provisional). PA7 is SWS: no config should name
it.

OD060F40 (3.5" BWY, legacy screen type 1): RESET PD4 (28), BUSY PA1 (1), DC PD7 (31), CS PB4
(12), MOSI PB6 (14), CLK PB5 (13), panel power PB7 (15) driven high (no active-low flag), battery
PB3 (11), on panel id 0xF002 (provisional). The legacy firmware also names PC5 as an enable; the panel refreshes
with PB7 alone.

## Build

```bash
TC32_DIR=/path/to/tc32 TELINK_SDK_DIR=/path/to/tc_ble_single_sdk targets/telink-tlsr/build.sh
```

Output: `targets/telink-tlsr/build/opendisplay_tlsr.bin`, a sealed image. The build:

- checks the image with `tools/tlsr_image.py check`;
- checks with `tools/check_ram_install.py` that the install routine and everything it calls
  sit in RAM.

Neither the toolchain nor the SDK is vendored or on `PATH`:

| What | Where it comes from |
|---|---|
| `TC32_DIR` | tc32-elf-gcc 4.5.1 (Telink TC32 v2.0), the Linux build that Telink's SIG Mesh SDK README links to. The directory holds `bin/tc32-elf-gcc`. |
| `TELINK_SDK_DIR` | `github.com/telink-semi/tc_ble_single_sdk`, tag `V3.4.3.0_Patch_0002`. The directory holds `proj_lib/liblt_825x.a`. |

`sdk_overlay/vendor/ble_sample/app_config.h` shadows the SDK's own copy of that file, so the
SDK tree is never edited.

The toolchain has no libc and a defective libgcc. `libc/` supplies what `shared/` needs. Its
`od_rt.c` explains why the build uses `-O2`.

## Flash layout (`src/od_tlsr_layout.h`)

| Range | Use |
|---|---|
| `0x00000–0x2D000` | running image (180 KB cap) |
| `0x40000–0x6D000` | update staging, the same size as the image cap |
| `0x6D000–0x71000` | config, two 8 KB banks |
| `0x71000–0x73000` | SEC2 secrets, two 4 KB banks |

`od_tlsr_flash.c` refuses any erase or program outside `[0, 0x2D000)` and `[0x40000, 0x73000)`.
These are inside the ranges the legacy updater writes, so going from the legacy firmware to OpenDisplay and back
leaves the legacy firmware's other data alone. That includes the MAC record at `0x76000`, which is only read.

The BLE address is the legacy firmware's own (`src/od_tlsr_mac.h`): derived from the flash chip's unique ID,
so a tag keeps the address and the three name bytes it had under the legacy firmware (`OD05B911` becomes
`OD05B911`). Without a usable UID it falls back to the record at `0x76000`, then to a random
address for that boot.

Each store (`src/od_tlsr_store.c`) keeps two banks. A write goes to the bank that does not
hold the newest record, payload first and header last, so a power cut leaves either the old
record or the new one. Erasing is done by writing a zero-length record (a tombstone).

## Updater (`src/od_tlsr_ota.h`)

It is the bring-up way back to the previous firmware; images are not signed. **SEC2-gated**
like the ESP32 and Nordic DFU transports: on a claimed tag, in claim mode or LOCKED, every op
but STATUS answers `locked` until an authenticated `CMD_ENTER_DFU` (0x0051) on the same link,
and the unlock ends with the link. `tools/tlsr_ota.py --sec2` does that through py-opendisplay.
A claimed tag whose key is lost is opened again by the factory-reset pattern (five quick
power-cycles); a LOCKED one never opens a session, so it needs SWS. It runs on its own
GATT service, `6f64f000-1e1e-4c4b-8b5a-7454534c5200`. Writes and notifications both go through
characteristic `…f001`.

| Op | Request | Reply |
|---|---|---|
| `0x01` BEGIN | `u32 len` | `81 00 len`, once staging is erased |
| `0x02` DATA | `u32 off`, bytes (sequential) | none; an error latches |
| `0x03` FINISH | — | `83 00 len`, once the header and CRC check out |
| `0x04` INSTALL | — | `84 00 len`; installs after 1.5 s and reboots |
| `0x05` STATUS | — | `85 state u32 next u32 len err boot resets` |
| `0x06` ABORT | — | `86 00` |

Integers are little-endian. In a reply, a non-zero second byte is an `enum od_tlsr_ota_err`;
8 (`locked`) answers any gated op, DATA included. 9 (`supply`): the supply measured on
`battery_sense_pin` is below 2.1 V (`OD_TLSR_FLASH_MIN_MV`), checked before BEGIN, each erase step,
DATA, the flag write and INSTALL; a refused INSTALL leaves the image staged and verified. The
config store refuses writes the same way. Without a sense pin the supply is not measured.
In STATUS, `boot` says why the tag last booted: 0 power applied (a brownout looks the same),
1 planned reboot or install, 2 unplanned (watchdog, reset pin). `resets` counts unplanned
resets since power was applied, saturating at 7. The probe reports `FF FF`.

The staged image's boot-flag byte, at offset 8, is held back until the image verifies. So the
ROM never sees a partly staged image as bootable.

Install runs from RAM with interrupts off:

1. Erase sector 0 first.
2. Copy sectors 1..n.
3. Copy sector 0 last.
4. Clear the staged copy's flag, then reset.

A power cut during install therefore leaves sector 0 without a boot flag. The ROM's fallback
to the image at `0x40000` then decides what boots. **That fallback has not been verified on
this part.**

## Tests

- `tests/host/tlsr_store_test.c`: power cut at every flash operation.
- `tests/host/tlsr_ota_test.c`: the updater.
- `tests/host/tlsr_mac_test.c`: the address order (UID, record, random), and that nothing is written.
- `tests/host/tlsr_crypto_test.c`: CCM and CMAC against RFC 4493 and OpenSSL, plus ECDH.
- `crypto_hal_contract_telink`: the HAL buffer contract.

All of them run over host stand-ins for the flash and the AES/P-256 primitives. The SDK-facing
files are covered only by the target build.

## Provisional numbers

- READ_IDENT `fw_target` is `0x06` (`docs/PROTOCOL_CHANGES_3.0.md`). It belongs in
  `../opendisplay-protocol` once accepted.
- DeviceFlags bit 6 `OD_DEVICE_FLAG_PWR_PIN_ACTIVE_LOW` and panel id `0xF001`
  (`docs/PROTOCOL_CHANGES_3.0.md` rows 11 and 12). Only this target honours the bit.
- CMD_SPI_RAW sub-opcode `0x07` GPIO (row 14). Only this target sets `OD_CAP_SPI_RAW_GPIO`.
- `OD_PROFILE_TELINK` lives in this directory's `CMakeLists.txt` until it moves to
  `shared/profiles.cmake`.
