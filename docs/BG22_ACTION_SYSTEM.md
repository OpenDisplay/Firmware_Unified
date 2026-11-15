# The action system: trigger → sequence of effects

A table in the device config says what the device does locally, with no host
connected. It replaces the pattern that had already accreted four times as
single-purpose flag bits.

## What it replaces

| flag | behaviour |
|---|---|
| `OD_LED_FLAG_BUTTON_PRESS` (LedFlags bit 4) | short LED flash on button press |
| `OD_BUZZER_FLAG_BUTTON_PRESS` (BuzzerFlags bit 1) | chirp on the same event |
| `OD_LED_FLAG_NFC` (bit 5, Firmware-only) | white flash on NFC field detect |
| `OD_LED_FLAG_NFC_FANCY` (bit 6, Firmware-only) | multi-colour blink on the same edge |

Between them those spent **three of LedFlags' four spare bits**, and the last
two needed a documented override rule because flag bits cannot express
precedence — `NFC_FANCY` exists only because `NFC` could not say "a different
flash".

**On this target they were already dead.** The two canonical ones are defined
in the contract and referenced by no code in `shared/` or `targets/`; the two
NFC ones were never canonical at all. So nothing was removed and no shipped
behaviour changed — the action table is simply the first implementation, and
the reason the fifth such request does not cost another bit.

The bits stay reserved rather than being reused. A device whose config happens
to set one gets the same nothing it got before.

**ESP32 runs the same table** (`OD_CAP_ACTION`, `src/od_action_app_esp.cpp`) and
does act on the two canonical bits. There the table takes precedence: a config
carrying a `0xB2` routes a button press to the table's `BUTTON_PRESS` entry, and
the LED/buzzer flag bits apply only to a config without one. `DISPLAY_SLOT`
(no image store) and `BUZZER` (no melody table) entries load as inactive there;
`BOOT` fires on power-on or reset, not on a deep-sleep wake.

## Shape

One length-prefixed config packet, **`0xB2`**, holding a sequence of entries:

```
  +0 entry_len     total bytes including this one
  +1 trigger
  +2 trigger_arg   2 bytes, meaning depends on the trigger
  +4 flags         bit0 = enabled
  +5 step_count
  +6 steps[]       4 bytes each: effect, arg0, arg1, delay_cs
```

| trigger | arg |
|---|---|
| 1 `BOOT` | — |
| 2 `BUTTON_PRESS` | pin bitmask, 0 = any |
| 3 `BUTTON_LONG` | hold time, centiseconds |
| 4 `NFC_FIELD` | — |
| 5 `HOST` | host's own id, fired by `0x004D` |
| 6 `TEMP_ENTER` | low byte = min °C, high = max °C (signed) |
| 7 `TEMP_EXIT` | same window |

| effect | args |
|---|---|
| 0 `NONE` | delay only |
| 1 `LED` | colour byte, brightness 1..16 |
| 2 `LED_STOP` | — |
| 3 `DISPLAY_SLOT` | image-store slot |
| 4 `BUZZER` | melody id |

**One packet, not one per entry.** `OD_CFG_INDEX_MAX` is 32 and the index walk
*stops* when it fills — and the drive packets sort last, so an over-full index
costs the panel its drive data. A real config already uses ~14 of those slots.

**`delay_cs` is centiseconds**, capping a step at 2.55 s, so a sequence's worst
case is `step_count × 2.55 s` by inspection. That bound is the point: a table
has to be judgeable without executing it.

**Display is by slot index, not digest.** The CRC-32 digest is 4 bytes and a
step argument is 2. Digest addressing stays in the flash commands, which are
not length-constrained.

## Rules that are not obvious

**Structural faults are rejected; unknown types are not.** A bad `entry_len`,
a `step_count` that disagrees with it, or a truncated entry is a bug and stops
the walk. An entry whose trigger or effect *this build* does not know is kept
and counted **inactive** — it is newer, not broken, and a firmware update
should start running it with no config rewrite. `0x004C` reports
entries/active/inactive/rejected so that distinction is observable rather than
asserted.

**Effects that cannot work report inactive at load, not silence at trigger.**
`od_action_app_has_buzzer()` is false on this board, so a buzzer entry is
inactive from the moment it is written — the difference between a diagnosable
config and a mystery.

**The enable bit is read live, not latched.** Clearing it in a config write
stops a sequence already running. An action that cannot be stopped by
rewriting the config is an action that can brick a tag in the field.

**Every step yields.** A table whose delays are all zero still returns between
steps. Without that a superloop with no watchdog spins forever on a table any
client can write.

**A trigger during a running sequence is dropped, not queued.** One-deep queues
produce the "why did it do that twice, ten seconds later" behaviour that makes
these systems feel broken.

**Triggers are recorded, not executed, at the edge.** A button or NFC edge
arrives interrupt-adjacent; starting an LED or a panel refresh there would run
that work in the wrong context. The service tick picks it up.

**Temperature triggers are EDGES.** A level would re-fire every poll and turn a
warm room into a refresh loop that flattens the battery. The inside/outside
state starts *unknown*, so the first sample sets a baseline rather than firing:
a device booting inside its window has not entered it.

## Replacing the table: a client must remove the old `0xB2`, not add one

**Duplicate `0xB2` packets are legal, silent, and useless.** `od_cfg_find` returns
the *first* match, so a config carrying two tables runs the older one and ignores
the newer one entirely. Nothing reports this — `0x004C` describes the table it
loaded, and a table that is being shadowed looks exactly like a write that did
not happen.

This is easy to hit, because the obvious client code hits it. py-opendisplay
round-trips unknown `0x80+` packets through `unknown_ex_packets`, so
`interrogate()` → append a table → `write_config()` **keeps the old one and adds
a new one**. Observed on OD58FFC4: three successive writes left four tables
stored, the config grew by exactly 24 bytes each time (1964 → 1988 → 2012 B),
and the device kept running the original table throughout. At a 2048-byte cap
that is also four writes from the config being refused outright.

A client replacing the table must **drop the existing `0xB2` entries first**:

```python
cfg.unknown_ex_packets = [p for p in cfg.unknown_ex_packets if p[1] != 0xB2]
cfg.unknown_ex_packets.append((0, 0xB2, table))
```

First-match is unchanged, but it is no longer invisible: `0x004C` appends a
10th byte, `tables`, the number of `0xB2` packets in the config (> 1 means all
but the first are shadowed). Not yet in the canonical `ActionInfoResponse`.
Still open: the host library should model the action table or refuse to
serialize a second copy.

## Temperature hysteresis and per-entry state

A hard `lo <= t <= hi` against the die reading (+/-2 C dither, 2 s poll) fired a
32..48 C window four times, alternating EXIT/ENTER, for one slow crossing
(OD58FFC4, 2026-11-06). `od_action.c` now keeps edge state **per temperature
entry** (two bits each, up to 16 entries; later ones never fire) and applies
**3 C of hysteresis on the exit side**: an entry is entered at the configured
edges and left only when the reading is more than 3 C outside them. So ENTER
fires exactly at the edge the host set, EXIT 4 C past it (`lo - 4` or `hi + 4`
in whole degrees), and one crossing is one edge. Host-tested against the
measured ramp shape (`tests/host/action_test.c`); not re-run on hardware.

`0x004C` still has one cumulative fire count, so chatter would only be visible
as a count that jumps; per-trigger counters remain a possible extension.

## Temperature source

`EMU_TemperatureGet()` — **die** temperature, the same source the advertisement
publishes. It reads high from self-heating and is **not** the panel's
temperature. Fine for a window trigger, wrong for anything claiming to measure
the environment.

## Opcodes

`0x004C` `ACTION_INFO` — entries, active, inactive, rejected, last trigger,
fire count, last temperature, tables (count of `0xB2` packets; byte 9, BG22
only so far).

`BUTTON_PRESS`'s arg is a pin mask: an entry fires for any button in it
(`0` = any). Other triggers match their arg exactly.
`0x004D` `ACTION_FIRE` — fire `OD_TRIG_HOST`, so a table can be exercised
without the physical event it is really for.

Both are **experimental**, defined beside the dispatch table rather than in
`shared/protocol/`, which mirrors the canonical repo. Landing them upstream is
required follow-up.
