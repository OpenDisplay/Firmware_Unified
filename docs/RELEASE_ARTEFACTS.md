# Release artefacts: the contract

This file defines what a Firmware_Unified release publishes, what each file is for and how a
consumer finds it. Consumers are the website toolbox and its firmware-sync workflow
(`opendisplay.org`), py-opendisplay's OTA helpers and through them Home Assistant, and people
flashing by hand.

The machine-readable form of everything here is `manifest.json`, which is attached to every
release (schema in § 5). **Consumers should read `manifest.json` and not pattern-match file
names.** The names are stable and listed here so a human can find them, and so the old-name
rule in § 1 can be checked mechanically.

Producer: `.github/workflows/release.yml` → `build.yml` → each target's build script →
`tools/release/collect.py`, which normalises names, adds the generated files and writes the
manifest. `tools/release/collect.py --check-only <dir>` re-validates a directory.

## 1. Rules

1. **3.x is published ONLY from `OpenDisplay/Firmware_Unified`.** A 3.x image is never
   attached to a release of `OpenDisplay/Firmware`, `Firmware_Silabs`, `Firmware_NRF54` or
   `Firmware_NRF`.
2. **3.x never uses a 2.x asset name.** The 2.x updaters match exact names in those repos:
   - HA's BG22 updater wants `opendisplay-bg22-v<tag>.gbl` from `Firmware_Silabs`;
   - the website ESP32 flasher wants `<env>_full.bin` from `OpenDisplay/Firmware`;
   - the nRF52840 flashers want `NRF52840.{zip,uf2,hex}`;
   - the nRF54 sync wants `^nrf54l15-.*\.hex` / `^nrf54lm20a-.*\.hex`.

   A 2.x device has no drive packets in its config. If one of these updaters pushes 3.x onto
   it, the update reports success and the panel stays blank, and the old client cannot repair
   it. `collect.py` refuses to write a manifest if any file matches a 2.x name pattern:
   `*_full.bin`, `NRF52840.*`, `nrf54l15-*`, `nrf54lm20a-*`, `opendisplay-bg22-v*`,
   `bootloader-apploader-v*`, `*app_update.bin`, `EPD-nRF52-*`, `esp32-*.bin`.
3. Every 3.x asset name is `opendisplay-<board>-<kind>.<ext>`. `<board>` is the tag in § 3.
   The only files without a board are `manifest.json`, `SHA256SUMS`, `MANIFEST-*.txt`,
   `LICENSE` and `NOTICE.md`.
4. Debug boards (`*-debug`) are built by CI but are **not** release assets.
5. Tags: **releases are tagged `3.0.0`, without a `v`** (decided 2026-11-11), as the other
   OpenDisplay repos do. The toolbox's `tbCompareVersions` parses `"v3"` as 0, so a `v3.0.0`
   tag would sort *below* every 2.x release there. `release.yml` still accepts `v3.0.0` and
   builds the same version from it; `manifest.json` always carries the bare `version`.

## 2. Release flow: draft, then publish

1. Push the tag `X.Y.Z` on the release commit.
2. `release.yml` builds every target with `X.Y.Z` stamped in, runs `collect.py` and creates a
   **draft** GitHub release. The draft carries all assets, `manifest.json`, `SHA256SUMS`,
   `LICENSE`, `NOTICE.md`, and notes taken from that version's `CHANGELOG.md` section.
3. The maintainer reviews the draft and the hardware checklist rows, then **publishes** it.
4. Consumers see the release only after step 3. GitHub hides drafts from
   `releases/latest`, and the toolbox filters `!r.draft`. This is deliberate: the sync can
   never pick up a half-built release, which removes the "published but assets missing"
   window that `sync-firmware-release.sh` has to guard against for `OpenDisplay/Firmware`.
5. A sync that adopts 3.x therefore needs no "assets missing" retry logic. It still checks
   that `manifest.json` exists, and it uses the asset `sha256` values from it.

`workflow_dispatch` with a version runs the same build and collection and uploads the result
as a workflow artifact. It creates no release (a dry run).

## 3. Boards

| `<board>` | target | chip | 2.x equivalent (`legacy_names`) |
|---|---|---|---|
| `s3-n16r8` | esp32-idf | esp32s3 | `esp32-s3-N16R8` |
| `s3-n16r8-extuart` | esp32-idf | esp32s3 | `esp32-s3-N16R8-extuart` |
| `s3-n32r8` | esp32-idf | esp32s3 | `esp32-s3-N32R8` |
| `s3-n32r8-extuart` | esp32-idf | esp32s3 | `esp32-s3-N32R8-extuart` |
| `s3-n8r8` | esp32-idf | esp32s3 | `esp32-s3-N8R8` |
| `s3-n4r2` | esp32-idf | esp32s3 | `esp32-s3-N4R2` |
| `c3-n4` | esp32-idf | esp32c3 | `esp32-c3-N4` |
| `c3-n16` | esp32-idf | esp32c3 | `esp32-c3-N16` |
| `c6-n4` | esp32-idf | esp32c6 | `esp32-c6-N4` |
| `esp32-n4` | esp32-idf | esp32 | `esp32-N4` |
| `esp32-wrover-e-n4r8` | esp32-idf | esp32 | `esp32-wrover-e-N4R8` |
| `xiao_nrf52840` | nordic-zephyr | nrf52840 | `NRF52840` |
| `xiao_nrf54l15` | nordic-zephyr | nrf54l15 | `nrf54l15` |
| `xiao_nrf54lm20a` | nordic-zephyr | nrf54lm20a | `nrf54lm20a` |
| `bg22` | efr32bg22-slc | efr32bg22 | `opendisplay-bg22` |

`legacy_names` exists so a sync can map a website board entry to its 3.x files one-to-one. It
says nothing about whether the 2.x → 3.x step is an update. Usually it is not; see
`docs/UPGRADING.md`. `s3-n4r2` and `esp32-wrover-e-n4r8` come from the esp32-resync branch;
until that branch merges they are absent.

## 4. Files per target

"Offset" is the flash address the file is written at. `—` means the file carries its own
addresses (`.hex`, `.s37`) or is not written raw (zip, gbl).

### 4.1 ESP32 (`esp32-idf`), per board `B`

| File | `type` | Offset | Use |
|---|---|---|---|
| `opendisplay-B-merged.bin` | `merged` | 0x0 | **Fresh install over USB.** Bootloader, partition table, otadata and app in one image. **Overwrites NVS (0x9000-0xDFFF) with 0xFF, so it erases the config and the encryption key.** |
| `opendisplay-B-bootloader.bin` | `bootloader` | 0x1000 on classic ESP32, 0x0 elsewhere (from IDF `flash_args`) | update part |
| `opendisplay-B-partition-table.bin` | `partition-table` | 0x8000 | update part |
| `opendisplay-B-ota-data.bin` | `ota-data` | 0xE000 | update part. It resets the boot selection to `app0`, which is where the app part goes. |
| `opendisplay-B-app.bin` | `app` | 0x10000 | update part, **and the BLE OTA image** (mcumgr SMP, image 0) |
| `opendisplay-B-manifest.json` | `web-manifest-update` | — | esp-web-tools manifest over the four parts, `new_install_prompt_erase: false`. It keeps NVS. Use it only when the device already runs 3.x. |
| `opendisplay-B-install-manifest.json` | `web-manifest-install` | — | esp-web-tools manifest over `merged.bin` at 0. It erases the config (fresh install, and every 2.x → 3.x move). |

The four update parts never touch 0x9000-0xDFFF. The build fails if one would
(`targets/esp32-idf/tools/web_flash_parts.py`). Offsets come from IDF's `flash_args`, never
from a table.

### 4.2 nRF54L15 / nRF54LM20A (`nordic-zephyr`), per board `T`

| File | `type` | Offset | Use |
|---|---|---|---|
| `opendisplay-T-merged.hex` | `merged-hex` | — | SWD / web DAP fresh install (MCUboot + app) |
| `opendisplay-T-ota.bin` | `ota` | — | MCUboot-signed app image for SMP upload to slot 1 |
| `opendisplay-T-dfu.zip` | `smp-dfu-zip` | — | NCS `dfu_application.zip` (nRF Connect Device Manager) |

### 4.3 nRF52840 (`nordic-zephyr`, board `xiao_nrf52840`)

The 3.0 layout is dual-slot MCUboot chained behind the Adafruit bootloader, with the
SoftDevice erased: MBR 0x0, MCUboot 0x1000, slot 0 0xD000, slot 1 0x7B000, settings 0xE9000,
Adafruit bootloader 0xF4000.

| File | `type` | Load address | Use |
|---|---|---|---|
| `opendisplay-xiao_nrf52840-usb-1-sd-erase.zip` | `nrfutil-dfu-zip` | 0x27000 | **USB step 1, once per device.** An adafruit-nrfutil serial-DFU package (`--dev-type 0x0052`) of a one-shot app. The app erases 0x1000-0x27000 (the s140 SoftDevice) and resets. The device comes back in serial DFU. Only for a device that still has the SoftDevice (every 2.x unit). |
| `opendisplay-xiao_nrf52840-usb-2-install.zip` | `nrfutil-dfu-zip` | 0x1000 | **USB step 2.** A serial-DFU package of `merged.hex` (MCUboot + signed app). With the SoftDevice gone, the bootloader writes it at 0x1000. |
| `opendisplay-xiao_nrf52840-merged.hex` | `merged-hex` | — | SWD install, and the source of step 2 |
| `opendisplay-xiao_nrf52840-ota.bin` | `ota` | — | MCUboot-signed app for SMP upload. Only for a device that already runs the 3.x layout. |
| `opendisplay-xiao_nrf52840-dfu.zip` | `smp-dfu-zip` | — | NCS `dfu_application.zip` for nRF Connect Device Manager (same layout requirement) |

The steps are serial DFU over USB CDC: `adafruit-nrfutil dfu serial --package <zip> -p <port>
-b 115200 --singlebank`. EN04/EN05 present **no UF2 drive**, so drag-and-drop is not a path
for them. No `.uf2` is shipped for the 3.0 layout. `docs/UPGRADING.md` § nRF52840 says
exactly which parts of this were run on hardware.

`-dfu.zip` (MCUboot SMP) and `-usb-*.zip` (Adafruit serial DFU) are different formats. A
consumer must pick by `type`, not by extension. py-opendisplay's `perform_nrf_dfu` already
refuses an MCUboot zip.

Opt-in builds with `OD_NRF52840_MCUBOOT=0` (Adafruit-only layout) produce
`opendisplay-xiao_nrf52840-adafruit-{merged.hex,.uf2}`. They are not release assets.

### 4.4 EFR32BG22 (`efr32bg22-slc`, board `bg22`)

| File | `type` | Offset | Use |
|---|---|---|---|
| `opendisplay-bg22-ota.gbl` | `gbl` | — | **BLE OTA** through the Gecko bootloader's AppLoader (unsigned GBL) |
| `opendisplay-bg22-app.s37` | `s37` | — | Application, SWD (`commander flash`) |
| `opendisplay-bg22-app.hex` | `hex` | — | Application, SWD |
| `opendisplay-bg22-bootloader.s37` | `bootloader-s37` | — | Gecko bootloader + AppLoader, virgin boards only. The collector renames the target's `bootloader-apploader.s37` to this name. The blob arrives with the bg22 branch (`v3/bg22` 0db5265). Until that merges, CI releases carry no bootloader and the `usb-install` flow says so. Flash it before `-app.s37`. |

The build itself writes `opendisplay-bg22.{gbl,s37,hex,out}`. `collect.py` renames those to
the names above and drops the `.out` ELF.

**A BG22 2.x → 3.x OTA needs a config migration right after it.** 3.x drives the panel only
from 0xAE-0xB1 config packets, and no 2.x config has them. A host must not offer the `.gbl` to
a 2.x unit unless it can write the migrated config afterwards (py-opendisplay
`prepare_v3_upgrade` / `finish_v3_upgrade`).

### 4.5 Release-level files

| File | Content |
|---|---|
| `manifest.json` | § 5 |
| `SHA256SUMS` | `sha256sum` format over every other asset |
| `MANIFEST-<target>.txt` | Human-readable per-target build record (commit, toolchain, sizes) |
| `LICENSE`, `NOTICE.md` | GPL-3.0 and the third-party notices (`third_party/NOTICE.md`) |

## 5. `manifest.json` (schema `opendisplay-release/1`)

```json
{
  "schema": "opendisplay-release/1",
  "repository": "OpenDisplay/Firmware_Unified",
  "version": "3.0.0",
  "tag": "3.0.0",
  "commit": "<40-hex>",
  "built_at": "2026-11-11T00:00:00Z",
  "signing": {
    "mcuboot": "debug-key",
    "esp32": "unsigned",
    "bg22": "unsigned"
  },
  "boards": [
    {
      "board": "c3-n4",
      "target": "esp32-idf",
      "chip": "esp32c3",
      "legacy_names": ["esp32-c3-N4"],
      "flows": {
        "usb-install": {
          "files": ["opendisplay-c3-n4-merged.bin"],
          "web_manifest": "opendisplay-c3-n4-install-manifest.json",
          "erases_config": true
        },
        "usb-update": {
          "files": ["opendisplay-c3-n4-bootloader.bin", "opendisplay-c3-n4-partition-table.bin",
                    "opendisplay-c3-n4-ota-data.bin", "opendisplay-c3-n4-app.bin"],
          "web_manifest": "opendisplay-c3-n4-manifest.json",
          "erases_config": false,
          "requires": "device runs 3.x"
        },
        "ble-ota": {
          "files": ["opendisplay-c3-n4-app.bin"],
          "protocol": "mcumgr-smp",
          "erases_config": false,
          "requires": "device runs 3.x"
        }
      }
    }
  ],
  "artefacts": [
    {
      "file": "opendisplay-c3-n4-app.bin",
      "board": "c3-n4",
      "type": "app",
      "offset": 65536,
      "size": 742592,
      "sha256": "<64-hex>"
    }
  ]
}
```

Field rules:

- `version` is always `X.Y.Z` with no `v`. `tag` is the git tag exactly as pushed.
- `signing.mcuboot` is `debug-key` (MCUboot's published key; anyone can sign an image these
  bootloaders accept) or `project-key`. `esp32` and `bg22` are `unsigned` in 3.0.
- `offset` is an integer byte address (the same convention as esp-web-tools `parts[].offset`),
  or `null` where § 4 says `—`. nRF52840 USB packages carry `load_address` instead.
- `type` is one of: `merged`, `bootloader`, `partition-table`, `ota-data`, `app`,
  `web-manifest-update`, `web-manifest-install`, `merged-hex`, `ota`, `smp-dfu-zip`,
  `nrfutil-dfu-zip`, `gbl`, `s37`, `hex`, `bootloader-s37`. An unknown `type` must be ignored,
  not guessed at.
- `erases_config` is `true` when the device's existing config (and encryption key) is not
  usable afterwards, `false` when it is kept, and `null` when that has not been established.
  A consumer must treat `null` as `true` for anything it tells the user.
- `flows` keys:
  - `usb-install`: first install, or a 2.x → 3.x move over USB;
  - `usb-update`: 3.x → 3.x over USB, keeping the config;
  - `ble-ota`: 3.x → 3.x over the air;
  - `ble-ota-from-2x`: BG22 only; `requires` names the mandatory config migration.

  `files` is the ordered list to apply. For `usb-install` on nRF52840 it is step 1, then step 2.
- A consumer that does not find the flow it wants must offer nothing. It must not fall back to
  another flow.

Per-board flows in 3.0:

| board(s) | `usb-install` | `usb-update` | `ble-ota` | `ble-ota-from-2x` |
|---|---|---|---|---|
| ESP32 (all) | merged.bin @0 (erases config) | 4 parts (keeps NVS) | app.bin over SMP | — (2.x has no SMP; 2.x → 3.x is a fresh install) |
| nRF54 | merged.hex (SWD/DAP) | merged.hex | ota.bin / dfu.zip over SMP | ota.bin from Firmware_NRF54 1.1.x: same partition map and key, **untested** |
| nRF52840 | usb-1-sd-erase.zip, then usb-2-install.zip (serial DFU) | usb-2-install.zip | ota.bin / dfu.zip over SMP | — (2.x has no SMP; the bootloader move needs USB) |
| BG22 | SWD: bootloader-s37 (when shipped) + app.s37 | — | ota.gbl via AppLoader | ota.gbl, **then the config migration** |

## 6. What the consumers have to change (as of 2026-11-10)

- **Website sync**: a new workflow that reads `OpenDisplay/Firmware_Unified` `releases/latest`
  and `manifest.json`. ESP32 boards use `opendisplay-B-manifest.json` for updates and
  `-install-manifest.json` for new installs or 2.x devices. nRF52840 uses a two-package serial
  DFU (the toolbox's `NrfWebTools` takes one package today). nRF54 uses `merged.hex`. The
  existing `OpenDisplay/Firmware` / `Firmware_NRF54` syncs stay for the 2.x lines.
- **py-opendisplay** (`v3/py`, `models/firmware.py` `_V3_OTA_ASSETS`), compared with this
  contract:
  - BG22: py expects `opendisplay-bg22.gbl`; the contract name is **`opendisplay-bg22-ota.gbl`**.
  - nRF52840: py has None; the contract has **`opendisplay-xiao_nrf52840-ota.bin`** (SMP, 3.x
    layout only).
  - ESP32: py has None; the contract has **`opendisplay-<board>-app.bin`** (SMP). The board
    is not derivable from the IC type alone; see `READ_IDENT` / `legacy_names`.
  - nRF54: `opendisplay-xiao_nrf54l15-ota.bin` and `opendisplay-xiao_nrf54lm20a-ota.bin`
    match.
- **Home Assistant**: watches `Firmware_Silabs` for BG22. Under rule 1 it sees no 3.x there,
  which is the safe failure. Moving it to Firmware_Unified is gated on the migration flow.
