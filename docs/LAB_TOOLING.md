# Lab tooling: where it lives today, and a proposed home

**Status: proposal only.** Nothing has been moved. Each move needs its owner's agreement, and
some cross repository boundaries that CLAUDE.md treats as read-only from here.

## What exists (2026-11-10)

| Tool | Where it is | What it does | Touches |
|---|---|---|---|
| `rig.py` | `Firmware_Silabs` `scripts/` (branch `feat/rig-remote-tooling`) | Measurement rig: thermal chamber, lamp and camera. It checks the lamp and the chamber around every capture. | network devices (Pi, chamber) |
| `chamber_ws.py` | same | Thermal chamber control over WebSocket | network |
| `ble_ota.py` (+ `requirements-ble-ota.txt`) | `Firmware_Silabs` `scripts/`; copies under `OpenDisplay_org/` and `newdriver/` | BG22 `.gbl` OTA over BLE through AppLoader | BLE |
| `show16_raw.py`, `test_spi_raw.py` | `Firmware_Silabs` `feat/od-epd-driver` `scripts/` | Bench drivers for 16-colour and SPI_RAW | BLE |
| `calibrate16.py`, `measure_photo.py`, `render16.py`, `drive_from_packets.py`, `migrate_drive_config.py`, `seq_conformance.*`, `cfgtest/` | imported by `v3/panel-data` into `panels/legacy/` | Palette calibration from photos, sequence conformance, config migration | camera images; BLE for `migrate_drive_config.py` |
| `panels.yaml`, palettes, waveforms, generators | `v3/panel-data`: `panels/`, `panels/tools/` | Panel database and drive-preset generator | host only |
| PPK2 + camera harness | `/home/jonas/Desktop/opendisplay/hil_rig` (no repo, deliberately) | `harness/{ppk2,camera,runner}.py`, one case `refresh_energy.py` | USB (PPK2), network (camera Pi) |

## Proposal

1. **Rig control goes in `hil_rig`, as a git repo of its own.** That covers `rig.py`,
   `chamber_ws.py` and the PPK2 harness: they drive bench hardware and network devices, not
   firmware.
   - Move `rig.py` and `chamber_ws.py` next to `harness/`, as `harness/rig.py` and
     `harness/chamber.py`, and make `runner.py` use them.
   - Make `hil_rig` a repository, for example `OpenDisplay/hil-rig`. Today it is outside every
     repo so that it can never be committed into firmware by accident. A separate repo keeps
     that property and still gives it history.
   - Keep the host and Pi addresses in environment variables (`RIG_PI_HOST`,
     `RIG_CHAMBER_HOST`), as `rig.py` already does. Commit no addresses.
2. **Device clients that every target uses go in `Firmware_Unified/tools/`.** That is
   `ble_ota.py` (BG22 AppLoader) plus an SMP helper for ESP32 and Nordic, if one is written.
   They are release-adjacent: `docs/UPGRADING.md` refers to them, and they should version with
   the artefacts they push.
   - Their Python dependencies go in a separate `tools/requirements-ble.txt`, not
     `tools/requirements-host.txt`. The host gate must stay installable without BLE stacks.
   - They are never run by `tools/check.sh` or CI, because they need a radio.
3. **Panel data and calibration stay in `panels/`**, where `v3/panel-data` put them.
   `panels/legacy/` should shrink as each script is either ported to `panels/tools/` with a
   host test or deleted.
4. **Delete the duplicate copies** of `ble_ota.py` under `OpenDisplay_org/` and `newdriver/`
   once (2) has landed. They are working copies, not sources.
5. **Bench drivers** (`show16_raw.py`, `test_spi_raw.py`) go to `hil_rig/cases/` as cases,
   which then get the runner's camera check for free.

## Open questions

- Does `hil_rig` become a public repo, or a private one? It holds lab procedures, not secrets,
  once the addresses are environment variables.
- Should `ble_ota.py` live in `py-opendisplay` instead? That would avoid a second BLE client,
  but it ties a bench tool to the library's release cycle.
