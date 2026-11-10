# Changelog

All notable changes to the OpenDisplay firmware built from this repository. Versions follow
[semantic versioning](README.md#versioning-and-releases). One version line covers every target.

`release.yml` refuses to tag a version that has no `## [X.Y.Z]` section here, and uses that
section as the release notes. Write it before tagging, not after.

## [Unreleased]

## [3.0.0]

The first release of `Firmware_Unified`. It replaces three firmware lines with one source tree:

- `OpenDisplay/Firmware` 2.x (ESP32 and nRF52840, Arduino);
- `Firmware_NRF54` 1.1.x;
- `Firmware_Silabs` 2.x (EFR32BG22).

It is published **only** from `OpenDisplay/Firmware_Unified`, under asset names that no 2.x
updater matches. The reason: a 2.x device that an old updater moved to 3.0 would come up with
no way to drive its panel, so old updaters must not see 3.0 at all. See
[docs/RELEASE_ARTEFACTS.md](docs/RELEASE_ARTEFACTS.md) §1.

> Items marked *(pending merge: `v3/<stream>`)* are on a branch that has to land before the
> tag. [docs/RELEASE_CHECKLIST_3.0.md](docs/RELEASE_CHECKLIST_3.0.md) lists the branches and the
> order to merge them in. If a branch does not land, delete its lines here.

### Breaking changes

- **Panel driving is config-driven.**
  - The device drives its panel from drive packets `0xAE`-`0xB1` stored in its config:
    init/refresh sequences, LUTs, palette and code map. It no longer relies on a built-in
    panel table.
    - BG22 has no built-in table at all. A config without drive packets leaves the panel
      blank: no boot screen, and image uploads are refused. BLE, config and OTA still work.
    - ESP32 and Nordic still keep bb_epaper (and FastEPD on ESP32) beside the config-driven
      path for the panels they already know.
  - Config packets `0x2E`-`0x31` are retired. Firmware walks past them and never acts on them.
- **Config format Rev 2.** Packet types `0x80` and above form an indexed tail, and a config is
  now larger:
  - BG22 accepts up to 3120 B (2.x: 2048 B). A 3.0 config may therefore not be readable after a
    downgrade to 2.x.
  - Clients older than 3.0 cannot write drive packets. A full config write from such a client
    removes them, and the panel then goes dark until a 3.0-aware tool writes them again. The
    firmware accepts that write without complaint.
- **ESP32 keeps its config and key in NVS, not LittleFS.** 2.x `/config.bin` is not imported.
  Moving from 2.x is a fresh install: the config and the encryption key are both lost. On C3,
  C6 and the classic ESP32 the partition table also changes (`huge_app` → `min_spiffs_4MB`, so
  two app slots).
- **nRF52840 changes bootloader layout.** MCUboot dual-slot now sits behind the Adafruit
  bootloader, and the s140 SoftDevice is erased. Moving from 2.x needs **one USB serial-DFU
  visit per device**, in two packages. After that:
  - updates are BLE SMP (or USB);
  - the Adafruit BLE DFU (OTAFIX) path no longer exists.
- **Opcode `0x0052` is POWER_OFF.** `py-opendisplay` 7.15.0 sends it from `deep_sleep()`. On
  ESP32 latch boards that cuts the power rail.
- **Version numbers.** Every target reports 3.0.0 in `0x0043` (major, minor, SHA, patch).
  - BG22's `SL_APPLICATION_VERSION` becomes `(major<<16)|(minor<<8)|patch` = 196608. 2.x used
    `(major<<8)|minor`. *(pending merge: `v3/bg22`)*
  - "major ≥ 2 means Unified" no longer holds: `Firmware` is at 2.27 and `Firmware_Silabs` at
    2.3.

### Added

- **Config-driven EPD engine** (`od_epd`), shared across targets, with these opcodes:
  - `0x0085` drive write;
  - `0x0084` SPI_RAW;
  - `0x0046` READ_IDENT.
- **16-colour drive** on BWRY panels where measured: EN04 2.9in, ODBEEBA6 2.9in, 3.5in, 1.54in.
- **BLE OTA:**
  - ESP32: mcumgr SMP, with rollback (a new bootloader is needed, and arrives with the USB
    install).
  - nRF52840: MCUboot SMP, with rollback.
  - nRF54: MCUboot SMP, the same partition map as `Firmware_NRF54` 1.1.1.
  - BG22: `.gbl` through AppLoader, as before.
- **Image store** on external SPI NOR (BG22, nRF52840/XIAO).
- **BG22 action system** (button, NFC and temperature triggers) and Find My network support.
  Treat both as experimental; see Known limitations.
- **Release contract:** `manifest.json` (schema `opendisplay-release/1`) and `SHA256SUMS`
  describe every asset, its board, its flash offset and the flows that use it. The ESP32 web
  flasher gets two manifests:
  - an install manifest that erases the config;
  - an update manifest that keeps it, because it never writes 0x9000-0xDFFF.

  *(update parts and the `s3-n4r2` / `esp32-wrover-e-n4r8` boards pending merge:
  `v3/esp32-resync`)*

### Changed

- One version line for all targets, and one CI pipeline that builds all three toolchains.
  Release builds are drafts until a maintainer publishes them.
- ESP32 is built with ESP-IDF 5.5.4 rather than Arduino. Nordic uses nRF Connect SDK 3.3.1.
  BG22 uses Simplicity SDK 2025.12.2.

### Removed

- The nRF52840 Adafruit-only image (UF2 drag-and-drop with no OTA). It remains a build option
  (`OD_NRF52840_MCUBOOT=0`) but is not released.
- `-debug` board variants are no longer release assets. They are still built in CI.

### Upgrade path

Full steps per target, with what has and has not been run on hardware:
[docs/UPGRADING.md](docs/UPGRADING.md).

| From | To 3.0.0 | Config kept? |
|---|---|---|
| ESP32 `Firmware` 2.x | USB install (`-merged.bin`, install manifest), then configure again and provision the key again | **No** |
| ESP32 3.x | USB update manifest, or BLE SMP with `-app.bin` | Yes |
| nRF52840 `Firmware` 2.x | **One-time** USB serial DFU: `usb-1-sd-erase.zip`, then `usb-2-install.zip` | Treat as no |
| nRF52840 3.x | BLE SMP with `-ota.bin` | Yes |
| nRF54 `Firmware_NRF54` 1.1.x | BLE SMP with `-ota.bin` / `-dfu.zip` (UNTESTED from 1.1.1) | Not verified |
| BG22 `Firmware_Silabs` 2.x | BLE `.gbl`, then a **mandatory** config migration that writes the drive packets | Yes, but blank until migrated |

### Known limitations

- **Nordic images are signed with MCUboot's published debug key**, unless the release was
  built with project keys (`manifest.json` → `signing.mcuboot`). Anyone can sign an image
  such a device will accept. The key is one swappable input (`OD_MCUBOOT_KEY_DIR`, or the CI
  secrets `OD_MCUBOOT_KEY_P256` / `OD_MCUBOOT_KEY_ED25519`). Changing it later means one USB
  visit per device, because MCUboot accepts only images signed with the key it was built with.
- **ESP32 and BG22 OTA images are unsigned.** Only image checksums are checked.
- **OTA is gated only by the app-layer session.**
  - With encryption off, the SMP service (ESP32, Nordic) and AppLoader entry (BG22) are open to
    any peer in range.
  - With encryption on, one authenticated `ENTER_DFU` opens SMP to any peer until reboot.
- **Bidirectional CCM nonce reuse.** Both directions of a session use the same key and the
  same nonce sequence. That can leak the overlapping plaintext of host and device frames
  (confidentiality, not forgery). Fixing it needs a protocol revision, so firmware alone
  cannot.
- **First key provisioning crosses the air in plaintext.**
- **ESP32 and nRF52840 confirm a new OTA image right after BLE starts**, so rollback catches only early
  crashes.
- **BG22 temperature trigger has no deadband** and can retrigger repeatedly ("chatter").
  BUTTON_LONG is never emitted.
- **EN04 boot refresh takes about 34 s**, past the 30 s limit, so BLE comes up late.
- **Encrypted SMP OTA, and 0x0085/SPI_RAW through the shared path on an ESP32 SPI board, are
  not yet run on hardware.** [docs/HARDWARE_VERIFICATION_CHECKLIST.md](docs/HARDWARE_VERIFICATION_CHECKLIST.md)
  is the per-board record.
- **No downgrade support.** Old flashers can write 2.x over a 3.0 unit. Whether 2.x then reads
  what it finds is not verified.
- **`esp32-s3-N4R2` and `esp32-wrover-e-N4R8`** exist only once `v3/esp32-resync` merges.
  Without it they stay on `Firmware` 2.27.x.
