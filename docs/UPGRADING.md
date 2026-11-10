# Upgrading to 3.0

Per-target steps from each 2.x firmware line to `Firmware_Unified` 3.0, and between 3.x
releases. File names are those of [RELEASE_ARTEFACTS.md](RELEASE_ARTEFACTS.md). Every step
says what evidence backs it:

- **VERIFIED (date, board)**: run on hardware, recorded in
  [HARDWARE_VERIFICATION_CHECKLIST.md](HARDWARE_VERIFICATION_CHECKLIST.md).
- **UNTESTED**: written from the layout and the code, never run. Expect surprises, and record
  the outcome in the checklist when you run it.

No 3.0.0-stamped image has run on any board yet. The verified rows used builds of the same
code under development versions (`9.9.x`, `f012ca3`).

## Before any 2.x → 3.0 move

1. **Back up the config.** On ESP32 and nRF52840 the move erases it. On BG22 it survives but
   is rewritten. With `py-opendisplay` 3.x support:

   ```
   opendisplay backup-config od-<mac>.json
   ```

   The backup holds the encryption key if one is set. Store it like a password.
2. **Use a 3.0-aware client afterwards.** Older clients cannot create drive packets. A full
   config write from one removes them, and the panel then goes dark. The website's 3.x
   toolbox, `py-opendisplay` with the migration support, and an HA integration built on it
   are 3.0-aware.
3. **Old updaters will not offer 3.0.** It is published under different names in a different
   repository on purpose, so they cannot move a device by accident.

## ESP32 (all boards)

### From `Firmware` 2.x (Arduino): fresh install

Config and key are lost. 3.0 reads NVS, and the 2.x LittleFS `/config.bin` is not imported, by
decision. On C3, C6 and classic ESP32 the partition table changes as well.

1. Flash `opendisplay-<board>-merged.bin` at 0x0 over USB, by either route:
   - the website toolbox, which uses `opendisplay-<board>-install-manifest.json` and erases
     first;
   - or `esptool.py --chip <chip> write_flash 0x0 opendisplay-<board>-merged.bin`, after an
     `erase_flash`.

   UNTESTED with a 3.0.0 release asset. The same merged image from local builds booted on
   `s3-n16r8-extuart` (VERIFIED 2026-11, E1003).
2. Configure the device again with drive packets. Either restore and migrate the backup:

   ```
   opendisplay migrate --from-backup od-<mac>.json
   ```

   or pick a preset in the 3.x toolbox. UNTESTED end to end.
3. Provision the encryption key again, if one was used.

This install also brings the rollback-enabled bootloader that BLE OTA needs. Rollback is
VERIFIED 2026-11 on `s3-n16r8-extuart`, plaintext only.

`esp32-s3-N4R2` and `esp32-wrover-e-N4R8` have 3.0 images only if `v3/esp32-resync` merged
before the tag. Otherwise stay on 2.27.x.

### From 3.x

- **BLE:** mcumgr SMP upload of `opendisplay-<board>-app.bin`.
  - VERIFIED with encryption off on `s3-n16r8-extuart`.
  - UNTESTED: with encryption on (unlock via `ENTER_DFU`), on C3/C6/classic chips, and from
    non-BlueZ hosts.
- **USB, keeping config:** `opendisplay-<board>-manifest.json` writes the bootloader,
  partition table, ota-data and app at their own offsets and never 0x9000-0xDFFF. UNTESTED
  on hardware. The offsets are checked at build time.

## nRF52840 (Seeed XIAO nRF52840, EN04)

3.0 runs dual-slot MCUboot behind the Adafruit bootloader, with the s140 SoftDevice erased.
Every 2.x unit needs **one USB visit**, after which updates go over BLE.

### From `Firmware` 2.x: one-time USB serial DFU

Needs `adafruit-nrfutil` (0.5.3.post16 was used) and a USB cable.

1. Put the device in serial DFU, by either route:
   - double-tap reset; the XIAO then enumerates as `2886:0064`;
   - or send `CMD_ENTER_DFU` from a client that holds the key.
2. Erase the SoftDevice:

   ```
   adafruit-nrfutil dfu serial --package opendisplay-xiao_nrf52840-usb-1-sd-erase.zip \
       -p /dev/ttyACM0 -b 115200 --singlebank
   ```

   The package's app erases 0x1000-0x27000 and resets after about 12 s. The device comes back
   in serial DFU by itself, because the bootloader finds 0x1000 erased.
3. Install MCUboot and the app:

   ```
   adafruit-nrfutil dfu serial --package opendisplay-xiao_nrf52840-usb-2-install.zip \
       -p /dev/ttyACM0 -b 115200 --singlebank
   ```

4. Configure the device again, with `opendisplay migrate --from-backup od-<mac>.json` or the
   toolbox. Assume the 2.x config is gone: its LittleFS area overlaps the new settings
   partition. UNTESTED.

Evidence for steps 2-3: VERIFIED 2026-11-06 on EN04 OD114608, with the same procedure and a
package of the same size (49,241 B) as the released step-1 zip. Step 3 used that day's
`merged.hex`; the released `usb-2-install.zip` is the same packaging of the release's
`merged.hex`. The chain then booted and advertised: Adafruit → MCUboot → app.

UNTESTED:

- the stock XIAO bootloader (only the EN04 has been done);
- a unit that still has a 2.x app with a different bootloader version;
- running step 2 on a unit that has already had it.

### From 3.x

- **BLE:** SMP upload of `opendisplay-xiao_nrf52840-ota.bin` (mcumgr / nRF Connect Device
  Manager, or `-dfu.zip`). VERIFIED 2026-11-06 on OD114608, both the update and the rollback
  of a bad image.
- **USB:** step 3 alone (`usb-2-install.zip`). UNTESTED. Whether the config survives is
  **unknown**: the bootloader preserves 40 kB below itself (0xEA000-0xF4000), but settings
  start at 0xE9000. Plan for it to be lost.

### Back to 2.x: UNTESTED

Flash `s140_nrf52_7.3.0_softdevice.hex` over serial DFU, then the 2.x `NRF52840.zip`. The
erase is reversible in principle. This was never run.

## nRF54L15 / nRF54LM20A

### From `Firmware_NRF54` 1.1.x

SMP upload of `opendisplay-<board>-ota.bin`, or `-dfu.zip`. **UNTESTED.** The partition
map, the sysbuild and the MCUboot configs are byte-identical to `Firmware_NRF54` main, and
both use MCUboot's default key, so MCUboot should accept the image. Whether 1.1.x settings
keys are read by 3.0 is not verified. Plan for configuring the device again.

If the 3.0 release was signed with a **project key** (`manifest.json` → `signing.mcuboot`
says `project-key`), a 1.1.x unit will refuse the image. It then needs an SWD flash of
`opendisplay-<board>-merged.hex` once.

### From 3.x

SMP, as above. UNTESTED on this branch.

## EFR32BG22

### From `Firmware_Silabs` 2.x: OTA plus mandatory migration

The config survives the OTA, but it has no drive packets, so after the OTA **the panel stays
blank** until they are written. BLE keeps working, so the fix is remote.

1. Plan the migration while the device still runs 2.x: `prepare_v3_upgrade` in
   `py-opendisplay`, or `opendisplay migrate --dry-run`. It picks the drive preset for the
   panel, or asks you to choose one when the panel id is ambiguous. Panels marked `blocked`
   have no preset and should not be upgraded.
2. OTA `opendisplay-bg22-ota.gbl` through AppLoader (HA or `ble_ota.py`).
3. Reconnect and write the planned config: `finish_v3_upgrade`, or `opendisplay migrate`.

UNTESTED end to end from a real 2.3.0 image. What is VERIFIED is narrower:

- the `.gbl` OTA keeps the NVM3 config (twice on ODBEEBA6, from a Unified build);
- the migrated drive packets render (ODBEEBA6, OD58FFC4).

### From 3.x

`.gbl` OTA. VERIFIED twice on ODBEEBA6, with the config kept.

### Virgin part

Flash `opendisplay-bg22-bootloader.s37`, then `opendisplay-bg22-app.s37`, over SWD. Only
possible when the release carries the bootloader (`v3/bg22` merged).

### Back to 2.x: UNTESTED

A 2.3.0 `.gbl` works as an OTA, but a 3.0 config larger than 2048 B may be unreadable to
2.x.

## Signing and what it means for the next upgrade

`manifest.json` → `signing` says how this release was signed:

- `mcuboot: debug-key` means anyone can produce an image that 3.0 Nordic units accept.
- `esp32: unsigned` and `bg22: unsigned` mean the same, permanently, for those targets.

A later change to a project MCUboot key needs a USB (nRF52840) or SWD (nRF54) visit per
device, because MCUboot checks only against the key it was built with.
