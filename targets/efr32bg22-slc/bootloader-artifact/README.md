# Prebuilt bootloader + AppLoader

`opendisplay-bootloader.s37` is the combined Gecko Bootloader + Bluetooth AppLoader image for
virgin EFR32BG22 boards. `build-and-flash.sh` finds it here, flashes it before the application
(commander backend, unless `--no-bootloader`), and stages it as `bootloader-apploader.s37` in
`release/`. Field units never need it: `.gbl` OTA replaces the application only.

## Provenance

- Byte-identical to `Firmware_Silabs` `bootloader-artifact/opendisplay-bootloader.s37`
  (tracked since `0de8d80`, unchanged through 2.3.0 `fd3bae8` and `feat/od-epd-driver`),
  md5 `63a25bda30916c8bcba42ef75f05fc93`, 764,948 bytes.
- Same md5 as `artifact/opendisplay-bootloader.s37` in the separate `opendisplay/bootloader`
  Simplicity project (not a git repo; project `bt-bootloader-apploader`, Simplicity SDK
  2025.12.2, components `bootloader_apploader`, `bootloader_core`, `bootloader_image_parser`).
  That project's `config/btl_core_cfg.h` has `BOOTLOADER_ROLLBACK_PROTECTION 0`,
  `BOOTLOADER_ENFORCE_SIGNED_UPGRADE 0`, `BOOTLOADER_ENFORCE_ENCRYPTED_UPGRADE 0`,
  `BOOTLOADER_ENFORCE_SECURE_BOOT 0`; `btl_apploader_config.h` advertises as `OTA`. That this
  config is the one the blob was built from is inferred from the matching artifact, not
  rebuilt here.

## Contents

| Range | Bytes | What |
|---|---|---|
| `0x00000`–`0x101A8` | 65,960 | Gecko Bootloader + AppLoader |
| `0x12000`–`0x4023C` | 188,988 | an example application in the app slot |

The app-slot part is overwritten by the OpenDisplay application, which links at `0x12000` and
is longer (it ends past `0x4E000`), so flash this first and `opendisplay-bg22.s37` second. A
full-flash factory image is these two files in that order.

## Refreshing

Rebuild the `opendisplay/bootloader` project and copy its `artifact/opendisplay-bootloader.s37`
here; update the md5 above. Changing rollback protection or signing requires matching changes to
the application's `SL_APPLICATION_VERSION` encoding (`cmake_gcc/CMakeLists.txt`) and `.gbl`
creation (`gbl create --app` is unsigned).
