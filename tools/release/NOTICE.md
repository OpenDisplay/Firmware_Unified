# Third-party notices for OpenDisplay firmware release images

This file ships with every release. It lists the third-party code that is part of the release
images. The firmware itself is licensed under GPL-3.0 (`LICENSE`).

The repository's `third_party/NOTICE.md` is the full record of every vendored tree, including
code that is used only by host-side tests and tools and is not part of any image.

## Compiled from this repository

| Component | In which images | Author / upstream | Licence |
|---|---|---|---|
| FastEPD | ESP32 images, boards with parallel e-paper panels | Larry Bank, https://github.com/bitbank2/FastEPD | Apache-2.0 |
| mdns 1.11.3 | every ESP32 image | Espressif Systems, https://github.com/espressif/esp-protocols | Apache-2.0 |
| QR code encoder (trimmed) | every image (boot screen) | Richard Moore, https://github.com/ricmoo/QRCode, derived from Project Nayuki's QR Code generator | MIT |
| Inflate, based on uzlib / tinf | every image | Paul Sokolovsky (uzlib), Joergen Ibsen (tinf) | zlib licence |
| Boot logo | every image (boot screen) | OpenDisplay | GPL-3.0 |

Apache-2.0, MIT and zlib-licensed code inside a GPL-3.0 work is permitted; the combined work is
GPL-3.0 and each component keeps its own licence and notice. Licence texts are in the source
repository next to each component.

## Linked from toolchains

Release images also contain code that is not in the repository but is linked from the pinned
SDKs. It keeps its own licence:

| Component | In which images | Licence |
|---|---|---|
| MCUboot (via nRF Connect SDK v3.3.1) | every Nordic `-merged.hex` and the nRF52840 `usb-2-install.zip` | Apache-2.0 |
| Zephyr RTOS, nRF Connect SDK (SoftDevice Controller, nrfx, Oberon PSA crypto) | every Nordic image | Apache-2.0 and the LicenseRef-Nordic-5-Clause terms of their sources |
| ESP-IDF (bootloader, NimBLE, mbedTLS, FreeRTOS) | every ESP32 image | Apache-2.0 (FreeRTOS: MIT) |
| Simplicity SDK (Bluetooth stack, Gecko bootloader, emlib, SEGGER RTT when enabled) | every BG22 image and `opendisplay-bg22-bootloader.s37` | Silicon Labs MSLA / Zlib / SEGGER RTT BSD-style, per file |
| Adafruit nRF52 Bootloader | not shipped. It is the bootloader already on the device; the nRF52840 USB packages are written through it | MIT |

Notices for the SDK components are in the SDK sources at the pinned versions
(docs/TOOLCHAINS.md in the source repository).
