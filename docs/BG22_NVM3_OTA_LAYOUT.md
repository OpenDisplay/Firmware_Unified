# BG22: what happens to stored config on the first boot after a `.gbl` OTA to 3.0

Analysis from source and build output only. **Nothing here has been run on hardware.**

## Layouts

| Image | NVM3 region | Pages | `NVM3_DEFAULT_MAX_OBJECT_SIZE` | Config cap | Image end |
|---|---|---|---|---|---|
| Firmware_Silabs 2.3.0 (`origin/main` `fd3bae8`) | `0x50000`–`0x56000` | 3 × 8 KB | 2112 | 2096 | ~`0x4E400` (2.3.0-based local build) |
| `feat/od-epd-driver` prototype (`76691a3`) | `0x4E000`–`0x56000` | 4 × 8 KB | 4096 | 4080 | `0x4B598` |
| Unified 3.0 (this branch) | `0x50000`–`0x56000` | 3 × 8 KB | 3136 | 3120 | `0x4E660` (measured, FMDN included) |

All three place NVM3 the same way (`autogen/linkerfile.ld`: `linker_nvm_end = __main_flash_end__`,
`__nvm3Base = linker_nvm_end - SIZEOF(.nvm)`), so the region always ends at `0x56000` and the
page count decides where it begins. All three store the config as ONE NVM3 object, key
`0x0F4401`, with the same 16-byte header `{magic 0xDEADBEEF, version, crc32, data_len}`
(`opendisplay_config_storage.c` on both old refs; `shared/core/od_config_store.h`
`OD_CONFIG_STORE_*`). The session/encryption key is a packet inside that config, not a separate
object; Bluetooth bondings are the stack's own NVM3 objects in the same region.

The `.gbl` OTA goes through the AppLoader, which erases and writes only the flash pages the
new application occupies (`0x12000` up to the image end). It does not touch other pages.

## (a) From 2.3.0: config, key and bondings preserved

- Same region, same key, same record format: unified's `od_config_store_load` reads the 2.3.0
  record as-is (magic, CRC32 and `data_len` checks are the same algorithm).
- The image ends at `0x4E660`, below `0x50000`, so the OTA erases no NVM3 page.
- Raising max-object 2112 → 3136 changes only the write ceiling and `nvm3_open`'s size gates.
  With the SDK 2025.12.2 formula (`thrSize = (1+objPages)·(8192−20) + 2·objLen + headroom + 4`)
  `thrSize(3136) = 2·8172 + 2·3144 + 4 = 22,636`, below both gates (`nvmSize` 24,576 and
  `validNvmPageCnt · 8172` = 24,516). Stored objects are never re-validated against
  max-object.
- Every 2.3.0 record is ≤ 2112 B, inside the 3136 B record cap.

What the unit then lacks is drive data (`0xAE`–`0xB1`), not config — the separate B1/B2 issue.

## (b) From the 4-page prototype: the lowest page is destroyed by the OTA itself

The prototype's page 0 is `0x4E000`–`0x50000`. The 3.0 image runs to `0x4E660`, so the
AppLoader **erases page `0x4E000` and writes code into its first 1,632 bytes** before 3.0 ever
runs. No firmware-side guard can prevent this; it happens in the bootloader. (Going back to
4 pages is not possible: it costs 8 KB against ~6.5 KB of headroom.)

On first boot 3.0 opens NVM3 over `0x50000`–`0x56000`, i.e. the prototype's pages 1–3. Those
pages still carry valid headers. `nvm3_open` → `findFirstPage` finds the FIFO start from the
erase counts of the good pages (`nvm3.c` `findFirstPage`); a contiguous subset of a valid FIFO
still satisfies its `max − min ≤ 1` check, so the open succeeds and the gates above pass. The
outcome then depends on what the prototype had last written into page `0x4E000`, which cannot
be known without a flash dump:

| Where the newest copy of the config object was | Result on first 3.0 boot |
|---|---|
| Entirely in pages `0x50000`–`0x55FFF` | Preserved — then subject to the size rule below |
| In page `0x4E000`, with an older copy not yet repacked out of pages 1–3 | **The older config comes back** (older drive data, older key if it changed) |
| In page `0x4E000`, no older copy left | Object not found → device boots unconfigured (`ENOENT`) |
| Fragmented across `0x4E000`/`0x50000` (a > 8 KB-page-boundary object) | Incomplete object; NVM3 ignores it, falling to one of the two rows above |

NVM3 writes its FIFO in address order and wraps, and the prototype formatted `0x4E000` when it
grew downward on 2026-11-02, so page `0x4E000` was being written to by any unit that has since
taken more than roughly a region's worth (~24 KB, about ten 2–3 KB configs) of writes.
Calibration sessions on ODBB1B76 / ODBF2156 / ODCF0ED5 make that plausible; it is unverified.

**Size rule, independent of the above.** The prototype allowed 4080 B configs. 3.0 caps at
3120: `od_config_store_load` returns `OD_CONFIG_STORE_TOO_BIG` for a larger record and
`od_cfg_index_build` refuses to index it. The record is **left in place, not erased**; the
unit runs unconfigured until a host writes a config ≤ 3120 B, which replaces it.

Nothing in any of these paths corrupts NVM3 itself or bricks the unit: OTA (`.gbl`) and
config write keep working, so every case is recoverable from the host.

## Procedure for the prototype units (no firmware guard)

A guard was considered and not added: the loss happens in the AppLoader, before 3.0 runs, and
"newest copy lost, older copy visible" is indistinguishable on the device from a normal config.
Instead, for every unit that ran `feat/od-epd-driver` (at least ODBB1B76, ODBF2156, ODCF0ED5):

1. Before the OTA, read the config back (`CONFIG_READ`) and keep it, plus the encryption key
   if security is on.
2. If the config is over 3120 B, shrink it first (e.g. share waveform slots across displays,
   py-opendisplay `packets_for_displays()`).
3. OTA to 3.0, then **always** write the saved (≤ 3120 B) config back, even if the unit looks
   configured — a resurrected older config looks configured.

2.3.0 units need none of this beyond the drive-packet migration.

## What would verify it

- 2.3.0 → 3.0 on one unit, then `CONFIG_READ` byte-compare against a pre-OTA read
  (checklist row; 2.3.0 → 3.0 has not been shown on any board).
- On a prototype unit: `commander readmem` of `0x4E000`–`0x56000` before the OTA tells which
  row of the table applies.
