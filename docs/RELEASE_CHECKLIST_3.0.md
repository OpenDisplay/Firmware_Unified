# 3.0.0 release checklist (go / no-go)

Work through the list in order. A row is done only when its evidence is written down: a commit,
a CI run, or a [HARDWARE_VERIFICATION_CHECKLIST.md](HARDWARE_VERIFICATION_CHECKLIST.md) entry.
"Builds" and "host-tested" are not "verified".

Owner:
- **agent-done**: done on a `v3/*` branch, host-tested or built only. It still has to merge.
- **needs maintainer**: a decision or an account action.
- **needs hardware**: a person at the bench.

Status values: `[x]` done, `[ ]` open, `[~]` done on a branch that has not merged yet.

## The rule that overrides every row

**3.0 is never published under the old repositories or the old asset names.** That means
`OpenDisplay/Firmware`, `Firmware_Silabs` and `Firmware_NRF54`, and their asset names
(`*_full.bin`, `NRF52840.*`, `opendisplay-bg22-v*.gbl`, `nrf54l15-*`, ...). Old updaters (HA
beta.10, the 2.x website flasher, `migration_tool`) would install it onto devices whose configs
have no drive packets, and those panels would go dark. `tools/release/collect.py` refuses
legacy names, and `release.yml` publishes only from this repository. Do not hand-upload
around them.

## A. Branches into `feat/silabs-config-driven-driver`

An in-memory trial merge (`git merge-tree`, 2026-11-10) applied every branch below in this order
with **no textual conflicts**. Semantics were checked only by the host gate on that merged tree
(see the release handoff report). Merge with merge commits, not squash: the hardware
checklist cites build SHAs.

| # | Item | Owner | Status |
|---|---|---|---|
| A1 | `v3/release`: artefact contract, collector, CI, nRF52840 MCUboot default, docs | agent-done | [~] |
| A2 | `v3/host-tests` | agent-done | [~] |
| A3 | `v3/panel-data`: `panels/`, drive preset database | agent-done | [~] |
| A4 | `v3/display-path` | agent-done | [~] |
| A5 | `v3/bg22`: SL_APPLICATION_VERSION 3.x encoding, `bootloader-apploader.s37`, FMDN | agent-done | [~] |
| A6 | `v3/esp32-resync`: web parts and update manifest, `s3-n4r2`, `esp32-wrover-e-n4r8`, 2.27.x drift | agent-done | [~] |
| A7 | `v3/esp32-display` | agent-done | [~] |
| A8 | Decide on `origin/fix/nrf52840-buttons` and `wip/nrf52840-console`. `fix/axp2101-datasheet-registers` came in through A6. | needs maintainer | [ ] |

## B. CI green

| # | Item | Owner | Status |
|---|---|---|---|
| B1 | Re-record the ESP32 sdkconfig baselines for every board under IDF 5.5.4 (`tools/sdkconfig_baseline.sh --update`), **including the two new boards `esp32-wrover-e-n4r8` and `s3-n4r2`**. They are red at `6df2d6e` because 7372072 enabled app rollback. | needs maintainer (or an agent with IDF 5.5.4) | [ ] |
| B2 | First fully green `ci.yml` run on the merged branch. No green run exists on this branch yet. | needs maintainer | [ ] |
| B3 | `actionlint` on `.github/workflows/*.yml`. It was not installed for the agent run, so the workflows were only YAML-parsed and `bash -n`-checked. | needs maintainer | [ ] |
| B4 | A `workflow_dispatch` dry run of `release.yml` (version `3.0.0`): download the `release-3.0.0-dry-run` artifact and check that `collect.py check` passed, that the file list matches RELEASE_ARTEFACTS.md §4, and that `notes.md` reads well. | needs maintainer | [ ] |

## C. Decisions

| # | Item | Owner | Status |
|---|---|---|---|
| C1 | **Tag format.** `release.yml` accepts both `3.0.0` and `v3.0.0`. **Recommended: `3.0.0`**, the form the other OpenDisplay repos and the website's version compare expect. | needs maintainer | [ ] |
| C2 | **MCUboot key policy.** Options: (a) ship with the debug key and document it (the default today; CHANGELOG says so); (b) add the `OD_MCUBOOT_KEY_P256` and `OD_MCUBOOT_KEY_ED25519` secrets before tagging. With (b), `Firmware_NRF54` 1.1.x units cannot SMP-update without one SWD flash, and changing keys later costs a USB visit per nRF52840. | needs maintainer | [ ] |
| C3 | The nRF52840 ships dual-slot MCUboot (now the `build.sh` default). Adafruit-only remains an unreleased opt-in. Confirm. | needs maintainer | [ ] |
| C4 | ESP32 2.x → 3.0 erases config and key, by decision. Confirm it is acceptable with the backup/migrate CLI as the mitigation. | needs maintainer | [ ] |
| C5 | Action system and image store: supported, or "experimental" in the notes. CHANGELOG says experimental. | needs maintainer | [ ] |
| C6 | `third_party/boot_logo`: state its licence, or replace it. `third_party/qrcode` and uzlib: add the upstream licence text and copyright lines (`third_party/NOTICE.md` open items). | needs maintainer | [ ] |

## D. Hardware pass at the release SHA

All with a 3.0.0-stamped image built by CI (B2) from the merged branch. Record each in
HARDWARE_VERIFICATION_CHECKLIST.md.

| # | Item | Owner | Status |
|---|---|---|---|
| D1 | ESP32 SPI board (e.g. `s3-n16r8`): USB install via the install manifest; 0x0085 drive and SPI_RAW through the shared path; boot screen. | needs hardware | [ ] |
| D2 | ESP32: USB **update** manifest keeps config and key (read config before and after). | needs hardware | [ ] |
| D3 | ESP32: SMP OTA with `-app.bin` with **encryption on**, plus rollback. | needs hardware | [ ] |
| D4 | nRF52840 EN04 from a **2.x unit**: `usb-1-sd-erase.zip` then `usb-2-install.zip` from the release assets, then 0x0085, then SMP OTA and rollback. | needs hardware | [ ] |
| D5 | nRF52840 XIAO (stock Seeed bootloader): the same two-step USB install. | needs hardware | [ ] |
| D6 | nRF52840: `usb-2-install.zip` over an already-3.x unit. Does the config survive? The result sets `erases_config` in collect.py, currently `null`. | needs hardware | [ ] |
| D7 | nRF54L15 board: boot, BLE, SMP update **from `Firmware_NRF54` 1.1.1**, and whether settings survive. | needs hardware | [ ] |
| D8 | BG22: **2.3.0 → 3.0 `.gbl`**, first without drive packets (the panel must stay reachable while blank), then `finish_v3_upgrade` (the panel renders). | needs hardware | [ ] |
| D9 | BG22 virgin part: `-bootloader.s37` then `-app.s37` over SWD (needs A5). | needs hardware | [ ] |
| D10 | Update the release matrix in HARDWARE_VERIFICATION_CHECKLIST.md (`:742-760`). It still says "Adafruit UF2" and "Not started". | needs hardware | [ ] |

## E. Clients (outside this repo)

| # | Item | Owner | Status |
|---|---|---|---|
| E1 | Website (`v3/website`): the sync reads `manifest.json` from Firmware_Unified into `toolbox/v3/`. The two-step nRF52840 install is documented, not offered on the web. ESP32 uses the install/update manifests. The 2.x entries stay untouched. The offline sync was run against a real collected BG22 + nRF52840 `manifest.json` (agent, 2026-11-10). The ESP32 part was run only against the fixture. | agent-done | [~] |
| E2 | `py-opendisplay` (`v3/py`): 3.x asset names must follow RELEASE_ARTEFACTS.md §6. The branch currently assumes BG22 `opendisplay-bg22.gbl` (the release name is `opendisplay-bg22-ota.gbl`), and has no ESP32 or nRF52840 OTA asset (the release has `opendisplay-<board>-app.bin` and `opendisplay-xiao_nrf52840-ota.bin`). | needs maintainer (py owner) | [ ] |
| E3 | `py-opendisplay` point release for old users: `write_config` refuses major ≥ 3 unless opted in, and the 0x0052/0x0053 naming is fixed. | needs maintainer | [ ] |
| E4 | HA (`v3/ha`): update entity on Firmware_Unified, with BG22 OTA plus migration through `prepare_v3_upgrade` / `finish_v3_upgrade`. | needs maintainer | [ ] |

## F. Release

| # | Item | Owner | Status |
|---|---|---|---|
| F1 | CHANGELOG.md `## [3.0.0]`: delete the lines for any branch that did not merge; set the date. | needs maintainer | [ ] |
| F2 | Tag on the merged commit (format per C1) and push the tag. `release.yml` builds and creates a **draft**. | needs maintainer | [ ] |
| F3 | Review the draft: `manifest.json` `signing` matches C2, every board in RELEASE_ARTEFACTS.md §3 is present, and no 2.x names appear. | needs maintainer | [ ] |
| F4 | **Publish** the draft. Consumers (`releases/latest`, the toolbox) never see drafts. | needs maintainer | [ ] |
| F5 | Run the website sync against the published release and release the website. | needs maintainer | [ ] |
| F6 | Release py-opendisplay and HA with 3.x support. | needs maintainer | [ ] |
