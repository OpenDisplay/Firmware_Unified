# BG22 RAM budget, heap gate and stranded-unit recovery

The EFR32BG22 has 32 KB of RAM and no kernel. The linker script gives the memory-manager heap
whatever the statics leave (`autogen/linkerfile.ld`: `heap = RAM end − __HeapBase`), so **every
static byte added anywhere comes silently out of the heap**. The Bluetooth stack allocates from
that heap at init and again at every connect. When the connect-time allocation fails, nothing fails
at boot: the unit advertises, accepts the link and serves no GATT, so it can no longer be sent
ENTER_DFU. It needs a wired debugger.

## 1. The rules

1. **Any change that grows static RAM on BG22 re-runs this budget.** The build prints the RAM map
   and fails below the floor (section 3); raising `static_ram_max_bytes` in
   `targets/efr32bg22-slc/ram_budget.json` is an explicit, reviewed step
   (`tools/bg22_ram_budget.py --update-baseline`), never a side effect.
2. **Test and trace builds must not grow RAM.** Put their state in RAM the release already
   reserves, or shrink something else by the same amount. A trace build that fails the gate is not
   flashed and not OTA'd.
3. **Never OTA a BG22 image that has not passed the heap gate.** `.s37/.hex/.bin/.gbl` are deleted
   when it fails, so a CMake build cannot produce one. An image built some other way must be
   checked by hand: `tools/bg22_ram_budget.py --elf <out> --baseline targets/efr32bg22-slc/ram_budget.json`.
4. **The floor (`heap_min_bytes`) moves only with a hardware measurement** of the heap high-water
   mark (section 5 shows how to read it), never to make a build pass.
5. **The first OTA of any new BG22 image goes to a unit with a debugger attached.**

## 2. RAM maps (from the ELFs, `tools/bg22_ram_budget.py`)

| | round-5 release (`v3/round5` 7f02f69) | SEC2 trace (`v3/round5-bg22test` d599854, stranded ODBEEBA6) | this branch (`v3/bg22-heap`) |
|---|---:|---:|---:|
| `.stack` (`SL_STACK_SIZE`) | 2,752 | 2,752 | 2,752 |
| `.bss` | 19,416 | 20,024 | 19,448 |
| `.noinit` | — | 1,040 (1 KiB ring) | 12 |
| `.data` | 380 | 380 | 380 |
| `text_application_ram` | 480 | 480 | 480 |
| `.bootloader_reset_section` | 4 | 4 | 4 |
| static total | 20,280 | 21,928 | 20,324 |
| **heap (`__HeapLimit − __HeapBase`)** | **9,728** | **8,064** (−1,664) | **9,664** |

The stack sits at the bottom of RAM (0x20000008–0x20000AC8); an overflow runs into the bootloader
reset word and then off the start of RAM into a fault, so it cannot silently eat the heap.

The trace build added `s_tr` (1,040 B, `.noinit`), `s_recs` (240 B), `s_tr_line` (176 B), a 213 B
reply buffer and small state: 1,648 B of statics plus alignment.

## 3. Heap budget

Read from the linked image's disassembly (the SDK ships the stack as libraries; addresses are the
round-5 release ELF). Every block costs its payload rounded up to 8 B plus an 8 B header
(`sli_block_metadata_t`). Confidence: **H** = size computed from the code and config, **M** = size
partly from a config byte or a data-dependent path, **L** = not traced.

**Allocated once, at stack init and boot (never freed):**

| What | Size | Source | Conf. |
|---|---:|---|---|
| stack buffer pool `bgbuf`: `SL_BT_CONFIG_BUFFER_SIZE` 3150 / 84 = 37 × 84 | 3,116 | `sl_btctrl_init_mem` 0x334f8 ← `fill_btctrl_config_from_host_config` | H |
| LL connection table, 4 × 328 (`SL_BT_CONFIG_MAX_CONNECTIONS` 4) | 1,320 | `ll_connAllocMem` 0x344a4 | H |
| host connection table, 4 × 140 | 568 | `sli_bt_connection_init` 0x257ec | H |
| BGAPI message pool, 10 × 48 | 488 | `bg_message_init` 0x3e770 | H |
| LL advertising sets, 3 × 144 (`SL_BT_CONFIG_USER_ADVERTISERS` 3) | 440 | `sli_ll_adv_allocate_memory` 0x33968 | H |
| host advertiser table, 3 × 72 | 224 | `sli_bt_advertiser_init` 0x25614 | H |
| BGAPI command buffer (4 + `SL_BGAPI_MAX_PAYLOAD_SIZE` 263) | 280 | `sli_bt_register_bgapi_device` 0x2e72c | H |
| security manager context | 176 | `sli_bt_sm_init` 0x255e0 | H |
| system event pool, 48 × (k+1), k a config byte (4 if 0) | ~248 | `sli_bt_system_init` 0x25744 | M |
| dynamic GATT DB: attribute table (3 static + 20) × 16, 20 B, 160 B | 576 | `allocate_gatt_db` 0x25432 | H |
| dynamic GATT DB value area: 500 B, replaced at commit by its exact size | ~300 | `alloc_data` / `gatt_db_commit_change` | M |
| bonding DB + stack IRK key material (PSA) | ~56 | `sli_bt_builtin_bonding_database_start` | M |
| channel classification, message receivers (16 B each), LL address list | ~150 | various | M/L |
| not traced (LL task schedules, anything reached only through function pointers) | 0–300 | — | L |
| **init total** | **≈ 7.7–8.1 KB** | | |

**Per connection** (`gap_conn_connection_complete` 0x222ea, in this order): CCC array 8n+12 (32),
**ATT bearer 72 (80)**, two message receivers 16 + 16 (48): **≈ 160 B**. When the ATT-bearer
allocation fails the function skips the bearer and the MTU request and carries on: the link is
reported open, stays up, and every ATT PDU on it is dropped. Each failure is also counted by the
stack (`bg_malloc` → `bg_notify_oom_observers(2, size)` → `sl_bt_evt_system_resource_exhausted`,
field `num_heap_allocation_failures`).

**Transient peaks:**

| What | Size | Conf. |
|---|---:|---|
| SEC2 session: AES-CCM key slot + P-256 ECDH key pair (PSA, freed after agree) | ~80 | M |
| one-shot CMAC / ECB key (imported and destroyed per call) | ~24 | M |
| peer-initiated pairing (SMP state zallocs) | ~100–250 | L |
| GATT Read Multiple response (up to the MTU) | ≤ 264 | M |
| ENTER_DFU / OTA entry: a reset into the AppLoader, which has its own RAM | 0 | H |

**Peak with one connection (this firmware stops connectable advertising while linked, so one is
the real maximum): ≈ 8.0–8.8 KB.**

| Image | heap | idle margin | margin at peak | observed |
|---|---:|---:|---:|---|
| trace | 8,064 | ≈ 0–360 B | negative | ODBEEBA6: links open, no ATT ever (4/4) |
| release | 9,728 | ≈ 1.6–2.0 KB | ≈ 0.9–1.7 KB | OD13FA1E (FMDN + FindMy, claimed): works |

**Floor: 9,600 B** = the upper estimate of the one-connection peak (8.8 KB) + 0.8 KB for what is
not traced. Nothing below 9,728 B has ever been shown to work, so the floor sits just under the
release figure and leaves this branch 64 B. It comes down only when a high-water mark measured on
hardware (section 5) justifies it.

### 3.1 The connection count (a maintainer decision, prepared as one line)

**The cheapest real margin** is the connection count. The stack reserves every connection's tables
from the heap at init (LL ≈ 328 B + host ≈ 140 B each), whether or not a link ever uses them, and
the firmware never serves more than one link at a time.

The count is set in exactly one place: `OD_BG22_MAX_CONNECTIONS_DEFAULT` in
`targets/efr32bg22-slc/cmake_gcc/CMakeLists.txt` (it feeds `SL_BT_CONFIG_MAX_CONNECTIONS` through
`config/sl_bluetooth_connection_config.h`). A one-off measurement build takes
`OD_BG22_MAX_CONNECTIONS=<1..4>` in the environment of `build-and-flash.sh`; it is not cached.

| `OD_BG22_MAX_CONNECTIONS` | heap section (build) | stack init reservation for connections (traced sizes) | free heap at the one-link peak (estimate) | status |
|---:|---:|---:|---:|---|
| **4** (default, shipped) | 9,664 B | ≈ 1,872 B | ≈ 0.9–1.7 KB | every hardware run so far |
| 2 | 9,664 B (identical) | ≈ 936 B | ≈ 1.8–2.6 KB (**+≈ 936 B**) | builds (`v3/head2`); never run |

The build-time heap section is identical (the reservation happens at run time); the two images
differ in exactly the two configuration bytes the controller and host read at init. The run-time
effect is an estimate from the traced allocation sizes, **not a measurement**: measure it by
reading `od_bg22_heap_free_at_boot` (section 5) on both images on one unit. Switching to 2 needs
release-checklist rows D11 and D12 on hardware with that image.

**The release image** (`v3/head2`, 4 connections): heap 9,664 B against the 9,600 B floor
(+64 B), stack 2,752 B, static 20,328 B (+4 B for the retained counters of section 4.5).

## 4. The runtime health gate

`shared/core/od_health.{h,c}` (host test `tests/host/health_test.c`), wired in
`targets/efr32bg22-slc/opendisplay_ble.c`. **Not yet run on hardware.**

### 4.1 Evidence it acts on

- A link **carried ATT** when any gatt (class 0x09) or gatt_server (class 0x0a) event reached the
  application during it. The stack sends an MTU request on every connect, so every link with a
  working bearer raises `gatt_mtu_exchanged`.
- The stack's own report: `sl_bt_evt_system_resource_exhausted.num_heap_allocation_failures`.

At each link close:

| Link | Verdict |
|---|---|
| carried ATT | healthy; the dead-link count returns to 0 |
| no ATT, stack heap failure during the link, nothing served yet this boot | **enter the AppLoader** |
| no ATT, lasted ≥ 8 s | one more dead link; **enter the AppLoader** at 3 consecutive ones |
| no ATT, shorter | ignored (scanners connect and drop) |
| overlapped another link | not judged |

"Enter the AppLoader" is the existing ENTER_DFU path: the bootloader's BOOTLOAD reset reason
(0x0202/0xF00F) at `__ResetReasonStart__` (0x20000000), then a system reset. The Gecko bootloader
starts the AppLoader, a separate image (0x00000–0x101A8 with the bootloader) that has its own BLE
stack and initialises its own RAM: nothing about the application's heap can stop it. It advertises
as `OTA` and accepts a `.gbl`. A power-on reset returns to the application (the reason word is only
honoured after a soft reset).

### 4.2 Why this cannot become a new brick path

- **Evidence only.** No prediction triggers it: three consecutive 8-second links without a single
  ATT event, or the stack reporting a heap failure on a link that carried no ATT before this boot
  served anything. A healthy peer answers the MTU request within one connection interval.
- **The worst false positive is recoverable.** It lands in the AppLoader, which can take an update,
  and a power cycle brings the application back.
- **Bounded.** At most 2 forced entries per power-on; the count sits in `.noinit` behind a check
  word (`s_health_ret`). If the bootloader reuses that RAM, it reads as 0, which costs at most one
  more round of fresh evidence. The AppLoader leaves only through an update or a reset, so nothing
  loops unattended.

### 4.3 Options considered and not built

- **A free-heap floor at boot as a trigger.** It predicts rather than observes, and its threshold
  is unmeasured: set too high, it would send every unit to the AppLoader on every boot. Kept as
  data only (`od_bg22_heap_free_at_boot`, `od_bg22_heap_hwm_at_boot`, diagnostic log line).
- **A "previous M boots never reached healthy" counter.** BG22 runs no watchdog
  (`od_claim_app_silabs.c`), and `app_assert` and a fault both hang rather than reset, so there is
  no boot loop to count. It would only matter together with a watchdog.

### 4.4 The power-cycle escape (built, off by default)

This is a third gesture next to the claim gesture (k ≥ 1 short boots) and the factory reset
(k ≥ 4). It counts the same short on-periods (`od_claim_mode`). It fires when a boot with exactly
**k = `OD_HEALTH_UPDATE_PATTERN_SHORT_BOOTS`** lives past T_CLEAR, which is when the pattern can no
longer continue toward a reset. The build refuses any value outside the open interval between the
two existing patterns. That leaves 2 or 3, i.e. three or four quick power cycles.

Why it is off:

- **Security.** The shipped bootloader accepts unsigned images (`BOOTLOADER_ENFORCE_SIGNED_UPGRADE 0`).
  A claimed device that enters the AppLoader from a power-cycle pattern would accept any firmware
  from anyone who can switch its power, without the session ENTER_DFU requires. That firmware could
  then read the key. So the escape is limited to UNCLAIMED devices, and that limits its value.
- **UX.** On an unclaimed device, k = 2 also starts claim mode (GESTURE); the escape then ends that
  session.
- **Wording.** "Three quick cycles = update mode, five = wipe" is one miscount away from a factory
  reset.

Recommendation: leave it off. The health gate covers the unattended case. A claimed device that
needs an escape without GATT should get one only together with signed upgrades in the bootloader.

### 4.5 The stack's resource-exhausted report is counted, not ignored

`sl_bt_evt_system_resource_exhausted` feeds the gate (its heap count) and a retained record,
`s_health_ret` in `.noinit` behind a check word: forced updater entries this power-on and the last
reason, plus saturating 16-bit counts of stack heap-allocation failures and of buffer/message
failures since power-on. It survives a soft reset and the AppLoader round trip, and a power-on
clears it. A diagnostic build prints it at boot; `tools/bg22_ram_dump.py` decodes it from a dump.

**Not client-readable in 3.0, on purpose.** The MSD has no free bit or byte, and the READ_IDENT
tail could grow by one byte (clients read it by length, and py-opendisplay already tolerates it),
but that is a wire change the website and HA clients would have to be checked against, for a value
that in the failure case cannot be read anyway (GATT is what failed; the device is then in the
AppLoader, which advertises as `OTA` — that is the client-visible signal). Proposal for the
maintainer: READ_IDENT tail byte 7 = saturating stack heap-failure count since power-on.

## 5. Recovering a stranded unit, and what to capture first

Do the captures **before** reflashing. A reflash erases the evidence.

**EM2 debug is disabled in this build** (`SL_POWER_MANAGER_INIT_EMU_EM2_DEBUG_ENABLE 0`). The
debugger cannot talk to the core while it sleeps, and it sleeps almost all the time. A reset does
not clear SRAM. So if a live attach fails, reset and halt at the first instruction, then dump:
neither the bootloader nor the application's startup code has touched RAM at that point.

```
JLinkExe -device EFR32BG22C224F512IM40 -if SWD -speed 1000 -autoconnect 1
J-Link> savebin ram_live.bin 0x20000000 0x8000        # 1. try while it runs (may fail in EM2)
J-Link> r                                             # 2. if 1 failed: reset + halt (no code runs)
J-Link> savebin ram_halted.bin 0x20000000 0x8000
J-Link> savebin flash_app.bin 0x12000 0x3E000         #    the exact image that is on the unit
J-Link> savebin nvm3.bin 0x50000 0x6000               #    NVM3 (config, OS2S, trace boot log)
```

Use `-autoconnect 1` without a script that resets. Do not use `loadfile`/`erase` yet. Then
decode:

```
tools/bg22_ram_dump.py --elf <the ELF of the image on the unit> --ram ram_halted.bin
```

For the stranding image (ODBEEBA6, `v3_worktrees/fw-round5-bg22test/.../build-trace/base/opendisplay-bg22.out`):

| What | Address | What it settles |
|---|---|---|
| `sli_general_purpose_heap` (40 B): base, size, **used_size (+8)**, **high_watermark (+12)** | 0x20001204 | size − high_watermark = least free heap ever. Near 0 → heap exhaustion. Hundreds of bytes → not the heap |
| heap blocks, walked from base 0x20006080 | 0x20006080–0x20008000 | the measured allocation map; replaces the estimates in section 3 |
| `system_resource_exhausted_event` (20 B) | 0x20004410 | the stack's own failure counters (heap byte) and whether the app read the event |
| `s_tr` ring (1,040 B, `.noinit`): magic, head, boots, check, 1 KiB text | 0x20005900 | each boot's `heap_free=` and `stack_free=` lines, as the trace logged them |
| `s_recs` boot log (240 B) | 0x200022e0 | reset causes of the last 10 boots |
| connection list head | 0x20004774 | with a link open: a connection object whose ATT bearer pointer (+72) is NULL proves the mechanism |
| `g_connection` | 0x20006059 | |

Then capture with a link open, because that is what proves the mechanism. Let it run (`g`) and
connect from a phone. Within the 30 s before the peer gives up, do `h`, then
`savebin ram_linked.bin 0x20000000 0x8000`, then `g`. Halting drops the link, which is fine.

Recover by flashing the application only, so the config and keys in NVM3 survive:
`JLinkExe … loadfile <release>.s37` or `commander flash <release>.s37`. No mass erase.

## 6. Other targets: the same rule

`shared/core/od_health.h` is one module for all three targets. BG22 uses the updater kind (this
section 4). Nordic and ESP32 use the rollback kind: an MCUboot test image / an ESP32 OTA image that
is still PENDING_VERIFY is confirmed only on the first GATT write it serves (or after 24 h with no
dead link, or by an explicit SMP confirm), and reverted after three ≥ 8 s links with no ATT.

- **Nordic (Zephyr):** `src/od_health_app.c`; proven on the EN04 (revert by three dead links,
  confirm on the first served write). The BT host's buffers are static, so the heap class itself
  cannot occur; the rule covers every other way BLE can come up broken.
- **ESP32:** `src/od_health_app.cpp`; host-tested and built, not run on hardware. NimBLE has no
  heap-failure event, so only the dead-link arm exists. Every deep-sleep wake runs the bootloader,
  which would roll back an image still PENDING_VERIFY, so an unconfirmed image holds deep sleep for
  up to 15 min of uptime, then confirms if no dead link was seen, else reverts.
