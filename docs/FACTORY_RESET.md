# Factory reset, reset pin and reset screen — specification

Status: **DRAFT revision 5, 2026-11-11, implemented on `v3/head` (host-tested; the five-cycle
reset and the erase ordering were run on one ESP32-S3, nothing else on hardware, SECURITY_V2
§ 19).** Values marked provisional need adopting in the protocol repo. This is a companion to [SECURITY_V2.md](SECURITY_V2.md); the work
breakdown is in [SECURITY_V2_IMPLEMENTATION_PLAN.md](SECURITY_V2_IMPLEMENTATION_PLAN.md).

**Revision 5 changes:**
- the RESET screen's QR uses the revision 5 setup URL grammar on the existing `/l/` entry point,
  `https://opendisplay.org/l/?f=1&k=2&…` (SECURITY_V2 § 7.8.1); `/s/` is withdrawn (never
  shipped);
- the reset clears 0x27 `flags` bit 1 SHOW_KEY_ON_SCREEN with the config; the RESET screen never
  shows a key (SECURITY_V2 § 10.7);
- the page runs the same wizard for a reset as for a first setup (recovery is setup, SECURITY_V2
  § 15.2).

**Revision 4 changes:**
- the five-cycle pattern stays **on by default** on every target, and a config can switch it off
  per device with 0x27 `flags` bit 6 `NO_RESET_PATTERN` (provisional; § 3.4, SECURITY_V2
  § 7.10.1). A LOCKED device ignores the bit. A claimed device with the bit set and no reset pin
  has no lost-key recovery except a wired reflash, so clients warn before writing that;
- R6 also sets OS2S PURGE: the next boot purges the superseded secrets from the raw storage
  (SECURITY_V2 § 10.4a). ESP32's R5 already erases the whole NVS partition (measured: nothing of
  the owner left in raw NVS);
- the boot count is written on time while the panel draws (SECURITY_V2 § 7.2).

**Revision 3 changes:**
- **five quick power-cycles are a second trigger** (§ 3.4), on every device, normative in
  SECURITY_V2 § 7.10. Same erase as the pin; the pin stays. Physical access to the power supply
  is therefore a factory reset: a DoS, never a data leak (SECURITY_V2 § 1.4 item 19);
- the reset pin check also feeds the power-cycle count: a pin hold is an uncounted boot and
  clears it.

**Revision 2 changes:**
- the reset pin is the one configured in 0x27, with a per-preset default. It is no longer the
  "first button";
- the secrets are scrubbed before the key is erased (new step R1b, review A4);
- a key erase that keeps failing ends in LOCKED;
- the reset ends in **fresh claim mode** (SECURITY_V2 § 7);
- the reset screen is the RESET kind of the setup screen family, and its URL is `/s/?k=r` (revision 5: `/l/?f=1&k=2`);
- the reset pin is sticky on claimed devices (review A9);
- the ESP32 WiFi NVS copy is erased (review A16);
- the five-power-cycle alternative is an option only (SECURITY_V2 Appendix B.1; adopted in
  revision 3).

## 1. What a factory reset is

A factory reset is the **physical** recovery path for a device whose key is lost or that changes
owner:
1. the user holds the configured reset pin (a button) while the device boots, **or switches the
   power off and on five times, each on-period 2-10 s** (§ 3.4);
2. the device draws the RESET screen with a setup QR;
3. it scrubs the secrets, then erases its key, stored images, bonds and configuration
   (maintainer decision);
4. it reboots, unclaimed, into **fresh claim mode**.

The user can then claim it again with the fresh-install flow of SECURITY_V2 § 13.1.

It is the only way to remove ownership without the key: SEC2 has no remote re-claim (SECURITY_V2
§ 11). The remote equivalents for the *owner* are SEC_KEY UNCLAIM + CONFIG_CLEAR +
FLASH_ERASE_ALL, but those leave the device in normal mode. Claiming it again then needs the
gesture.

Today only `targets/esp32-idf/src/encryption.cpp:96` `checkResetPin()` implements a reset pin. It
is called once from `config_parser.cpp:310` after config load, with one 100 ms delay, one sample
and no hold time; it erases the config and reboots. 3.0 replaces it with one shared
implementation on every target.

## 2. Configuration: the reset pin configured in 0x27

| Field (config packet 0x27, layout unchanged) | Meaning on 3.0 |
|---|---|
| `reset_pin` | the GPIO, in the **same encoding the target uses for binary-input pins** (packet 0x25 `inputpinN`): the ESP32 GPIO number; on nRF the packed/absolute port-pin byte decoded by `od_pin_codec`; on BG22 the port/pin byte its config parser uses for buttons. `0xFF` = none |
| `flags` bit 2 `RESET_PIN_ENABLED` | must be set, otherwise there is no reset pin |
| `flags` bit 3 `RESET_PIN_POLARITY` | 0 = LOW asserts (button to GND, the common case), 1 = HIGH asserts |
| `flags` bit 4 / bit 5 | internal pull-up / pull-down while sampling |

The reset pin is **the one configured in 0x27**. Its **default comes from the preset** of the
device's simple-config id (board, i.e. `simple_config_driver_index`). When a client writes a
config from a preset, it writes that preset's 0x27 reset-pin fields; the user may change them.
The per-preset pin definitions are **generated later** from the board data (maintainer decision).
This specification fixes only the rules they must follow (§ 2.1).

Lifecycle:
- **Claimed device**: the reset pin is **sticky-enabled** (SECURITY_V2 § 10.6). CONFIG_CLEAR
  and config writes keep an enabled pin enabled. A write may move it or change its polarity and
  pulls; disabling it requires UNCLAIM first. A careless write or a CONFIG_CLEAR therefore cannot
  remove the owner's recovery path. That protection ends at the firmware: someone holding the key
  can install firmware that ignores the pin.
- **Unclaimed device**: the pin is whatever the config says; the device is open anyway.
- **After a factory reset** there is no config, and so no reset pin, until the fresh-install flow
  writes a config (SECURITY_V2 § 13.1 step 3). That flow writes the preset's default reset pin.

### 2.1 Rules for the generated preset defaults

The website's `simple-config-presets.json` and py-opendisplay's copy gain a `securityConfig`
block per board. It holds the reset-pin fields; the encryption fields stay 0 (SECURITY_V2 § 10.3)
and `session_timeout_seconds` stays 0 (= 3600 s). The generator and the website's preset
validator (`.github/scripts/validate-simple-config-presets.py`) enforce:

- **Never a strapping pin.** Holding a strapping pin during reset changes how the chip boots, so
  the firmware never runs and the reset never happens. The excluded pins:
  - ESP32: GPIO 0, 2, 5, 12, 15;
  - ESP32-S3: GPIO 0, 3, 45, 46;
  - ESP32-C3: GPIO 2, 8, 9;
  - ESP32-C6: GPIO 8, 9, 15;
  - nRF52840: P0.18 when it is used as nRESET.

  Example: nm-epd-420-4c lists GPIO 45 and GPIO 0 as buttons. Neither may be the reset pin, so
  pick another button or none.
- The pin is one of the board's buttons, **or a solder pad / jumper** that the board data names
  as a reset input. The latter is how button-less boards can get a reset pin (§ 7.3).
- ESP32 GPIO 34-39 have no internal pulls. A preset using them relies on the board's external
  pull (m5-paper buttons 37/38/39) and sets no pull flag.
- Not the D-FF power-latch clock (`pwr_pin_3`), unless the board's latch toggles only on press
  edges. That must be verified on the board: a hold must not toggle the power off.
- Every generated default is **untested** until checked on its board, and is listed in
  `docs/HARDWARE_VERIFICATION_CHECKLIST.md`.

## 3. Behaviour at boot

### 3.1 When it is sampled

Every boot, on every target, in this order:

1. Assert the power-latch hold (on boards with a latch), so that releasing the button cannot drop
   power.
2. Load the config (the reset pin is in it).
3. **Reset-pin check** (this section). It runs before the boot screen, BLE advertising and the
   display init, so that a stuck panel cannot prevent a factory reset. Then the power-cycle
   pattern (§ 3.4) is read from the boot history.
4. Resume an interrupted factory reset if a valid reset marker exists (§ 5.3); otherwise start one
   if the pin committed or the pattern completed. This runs even when there is no config.
5. The SEC2 boot decision (SECURITY_V2 § 10.1), then claim-mode entry (SECURITY_V2 § 7.1).
6. Only then start the radio and the display.

The check runs on every kind of device: unclaimed, claimed and **LOCKED**. LOCKED is the case it
exists for.

"Every boot" includes the wakes that reset the chip:
- ESP32 deep-sleep wake (timer or button);
- nRF System OFF wake;
- BG22 EM4 wake;
- power-latch power-on.

On a **button wake** the user simply keeps holding the button that woke the device. On a **timer
wake** the check costs one GPIO read (the pin is not asserted) and the boot continues with no
delay. The pin is **not** sampled at runtime: a long press while running is an ordinary button
event (0xB2 BUTTON_LONG actions).

A reset-pin hold is not a power-cycle and does not count towards the claim gesture or the
five-cycle pattern (SECURITY_V2 § 7.2). The software reset that ends the factory reset is not
counted either.

### 3.2 Hold, debounce, release

```
sample period            10 ms
debounce                 a level counts after 5 consecutive equal samples (50 ms)
first check              configure the pin with its pulls, wait 1 ms, read once:
                         not asserted -> return immediately (normal boot)
hold threshold           5000 ms continuously asserted (debounced)
commit                   released (debounced) after >= 5000 ms and < 30000 ms -> factory reset
too short                released before 5000 ms -> normal boot (merely delayed)
stuck                    still asserted at 30000 ms -> abort, log "reset pin stuck", ignore the
                         pin for this boot, normal boot
watchdog                 fed throughout (od_watchdog)
```

Committing on **release**, not at 5 s, gives the user a chance to see "release now". It also avoids
resetting a device whose button is jammed or pressed by its case.

### 3.3 Feedback (LED / buzzer), also used where there is no panel

The first configured LED (packet 0x21) and the passive buzzer (0x29), if present:

| Phase | LED | Buzzer |
|---|---|---|
| holding, < 5 s | blink 1 Hz (100 ms on) | — |
| threshold reached, "release now" | solid on | one short beep |
| erasing (after release) | blink 5 Hz | — |
| done, just before reboot | 3 long flashes | two beeps |
| stuck pin, aborted | 3 slow blinks | — |
| key erase failed → LOCKED (§ 5.2) | SOS pattern (3 short, 3 long, 3 short) once per boot | three long beeps |
| claim mode, waiting / code shown / claimed | as SECURITY_V2 § 7.7 | as SECURITY_V2 § 7.7 |

After the reset there is no config, so the fresh claim mode that follows knows no LED or buzzer
(SECURITY_V2 § 7.7). A device with neither a LED nor a buzzer relies on the RESET screen. With no
panel either, the reset is silent. Its only signs are the reboot, the changed advertisement (MSD
bit 3 cleared, bit 2 set for claim mode) and READ_IDENT CLAIM_MODE.

### 3.4 Five quick power-cycles

The second trigger, normative in SECURITY_V2 § 7.10: a counted (power-on) boot whose boot history
holds `short_boots ≥ 4`, i.e. four consecutive previous power-ons that each lived 2-10 s. It needs
no pin and no config, so it is the reset for button-less boards and for LOCKED devices. It runs
the same procedure (§ 5) and ends in the same fresh claim mode.

**On by default; off per device or per build** (revision 4, maintainer decision):
- every target ships with the pattern on;
- a config switches it off with 0x27 `flags` bit 6 `NO_RESET_PATTERN` (provisional). Clear = on,
  so every existing config keeps it. The reset pin and the two-cycle claim gesture are
  unaffected. On an unclaimed device the gesture still counts and a fifth quick boot is a claim
  mode boot, not a reset;
- a **LOCKED** device ignores the bit and resets: its stored config may be what locked it;
- a build removes the pattern with `OD_FR_PATTERN_SHORT_BOOTS 0` whatever the bit says (board
  profiles for energy-harvesting supplies SHOULD).

Consequences (SECURITY_V2 § 7.10.1): an owner with flaky power switches it off and no longer
risks an erase; on an unclaimed device anyone in range can switch it, which is acceptable
because they could claim the device as well; **a claimed device with the bit set and no enabled
reset pin has no lost-key recovery except a wired reflash (§ 7.3)**. Clients MUST warn and ask
for confirmation before writing that combination.

Feedback: k + 1 short LED blinks at 2 s of each counted boot; no e-paper refresh during the
pattern (the boot screen and the SETUP screen wait until 10 s); the RESET screen once, before the
erase. The count is written at 2 s even when a boot-screen refresh is blocking (measured on the
ESP32-S3 Sticky: the write lands at 2.9 s, so an on-period of about 3-10 s counts there).

**False triggers.** A false trigger needs five power-ons in a row whose first four on-periods each
last 2-10 s, with no other reset in between. The supplies that can produce it, and why nRF52
brown-outs count, are in SECURITY_V2 § 7.10. The result is an erased, FRESH device that anyone
nearby can claim until its owner does; nothing leaks, because the secrets are scrubbed first. A
device on such a supply should have the pattern switched off (bit 6) **and** a reset pin
configured, so it keeps a recovery path.

## 4. The RESET screen

### 4.1 Layout

The RESET screen is the **RESET kind of the setup screen family** of SECURITY_V2 § 7.8:
- drawn by the firmware through the boot-screen path in display mode 0, with `od_display_boot()`
  → `od_boot_screen_render()` on ESP32 and nordic-zephyr, and BG22's own renderer in
  `opendisplay_display.cpp`;
- a fixed layout with firmware text only. **No 0x2C manufacturer/model header and no footer**,
  because those are config-supplied.

```
full layout (>= 400x300 logical)          reduced layout (small panels)
"OpenDisplay"                             "RESET"
"FACTORY RESET"                           "OD1A2B3C"
"Settings and key erased"                 QR: setup URL
"ID:  OD1A2B3C"
"Scan to set up again"
QR: setup URL (k=2)
```

When text and QR do not both fit, the QR wins. The screen is drawn **before** the config is
erased (§ 5.1). E-paper keeps it afterwards. The device then reboots into fresh claim mode with no
config, so it cannot draw again. The RESET screen therefore stays visible during that claim mode
and serves as its SETUP screen; that is why both carry the same URL and differ only in `k`. The
next screen the device draws is the SETUP screen of the fresh-install flow, once a config has been
written. Alternatively, a factory-embedded config re-provisions the device at its next boot and
draws its SETUP screen directly (ESP32 `tryProvisionFactoryEmbed`, Nordic `factory_config.c`).

### 4.2 The setup URL

The URL is defined once, in SECURITY_V2 § 7.8.1 (grammar, byte budget, vectors, page rules):

```
https://opendisplay.org/l/?f=1&k=2&i=<id>&t=<target>&v=<fw>[&b=<driver>][&d=<display>][&p=<power>]
```

- The `b`, `d` and `p` indices come from the **old** config's 0x02 packet, read before the erase.
  That is what lets the website preselect the same board, display and power option again.
- The worst case is 83 bytes, inside QR version 6 ECC-M (106).
- It carries no secret.

Revision 1's `/r/` and revision 2's `/s/` never shipped and are replaced.

### 4.3 What the opendisplay.org `/l/` page shows for `k=2`

The page follows SECURITY_V2 § 7.8.1 page rules: strict parsing, no secrets on the page, the
preselection shown and confirmed, the genuine-domain note, no third-party requests. For `k=2` it
says (and then runs the same wizard as for `k=1`):

1. The device name `OD<i>` and the firmware version. *"This display was factory reset. Its
   settings, its security key and its stored images were erased. It is now waiting to be set up."*
2. If `b`/`d`/`p` are present: the board, display and power option names, looked up in
   `firmware/toolbox/simple-config-id-registry.json` (index → string id) and the presets file. A
   button **"Set up this display again"** opens the setup wizard with exactly these preselected
   (string ids), and the user confirms them first. If an index is unknown to this website version,
   the page says so and falls back to the manual picker.
3. If they are absent: *"Choose your board and display"* (the normal flow).
4. The next steps: *"Keep the display switched on. Setup mode ends 10 minutes after it was
   switched on; if that happens, switch it off and on again — a factory-reset display stays in
   setup mode at every power-on until it is set up or you skip setup."* (The FRESH state survives
   power loss; SECURITY_V2 § 7.1.) Then: write the configuration, check the display shows SETUP
   MODE, claim it, and enter the code shown on the display.

## 5. Erase procedure and power-fail ordering

### 5.1 The ordering problem

Once the config is erased, no drive packets (0xAE-0xB1, or 0xB3) are left to drive the panel. The
RESET screen **must therefore be drawn from the still-present config, before the config is
erased.** But drawing is the slowest and least reliable step:
- a 1-40 s refresh;
- BUSY timeouts;
- a panel that is absent or broken;
- a weak battery that browns out during the refresh.

The reset must complete in every one of those cases. A power failure at any point must not leave
a half-reset device, an open device with secrets, or a loop.

### 5.2 Stages

A small **reset marker** record (OS2R) makes the procedure resumable. It is stored through the
same secret-record HAL as K_dev (SECURITY_V2 § 4.1), separate from the config record:

```
"OS2R" (4) | version 0x02 | stage (1) | draw_attempts (1) | erase_attempts (1) | crc32 (4)   = 12 bytes
stage: 1 SCRUB, 2 DRAW, 3 ERASE_REST, 4 KEY_ERASE_FAILED
```

| Step | Action | Why here |
|---|---|---|
| R0 | trigger accepted (released between 5 and 30 s) | |
| R1 | write the marker {stage 1, draw 0, erase 0} | from here on, the reset finishes even across power loss |
| **R1b** | **scrub the secrets**: the SECURITY_V2 § 10.4 scrub procedure. One atomic config rewrite zeroes every 0x26 `password` and every 0x2D `eik` / `net_a_adv_key` / `advertisement_key`, and sets 0x27 to the unclaimed form (enabled 0, key 0, marker 0). The display, drive and 0x02 packets are kept for R3. ESP32: erase `nvs.net80211` | review A4. From here on, **every** interruption point leaves either a claimed device without secrets or an unclaimed device without secrets, even if the marker is later lost. With the marker lost after R1b and before R2, the boot decision sees a valid key and an unmarked 0x27, so the device is CLAIMED and the marker is repaired: it stays claimed, without secrets, and the user repeats the reset. This is the same code as SEC_KEY UNCLAIM |
| R2 | erase the **key record** OS2K (secure erase where the HAL supports it; best effort, SECURITY_V2 § 1.4 item 3), clear OS2C, and tear down any session. If the erase fails: increment `erase_attempts` in the marker and retry at the next boot. At 3 failures, write stage 4 KEY_ERASE_FAILED and stop: the device is **LOCKED** (§ 5.3) and shows the § 3.3 pattern; it never boot-loops | the key is not needed to draw |
| R3 | write the marker {stage 2, draw_attempts + 1}. If `draw_attempts` ≤ 2 **and** mode 0 is drawable from the config, draw the RESET screen. The result is ignored | the attempt is counted *before* drawing, so a brown-out or watchdog reset during the refresh counts |
| R4 | write the marker {stage 3} | drawing is over, whether it succeeded or not |
| R5a | erase the **image store**: all slots (`od_imgstore_erase_all()`, or each slot's header sector where that is much faster). Skipped if the store is not configured or does not mount | must come before the config erase: the store's SPI pins and geometry come from the config (0x2B, 0x20) |
| R5b | erase the **BLE bonds** (nordic-zephyr `bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY)`; BG22 `sl_bt_sm_delete_bondings()`, if the SM ever stored any; ESP32 persists none) and the target extras: BG22 FMDN / FindMy runtime NVM3 objects, if any exist outside the config | |
| R5c | erase the **config record** (`od_config_store_clear()`; on ESP32 via `od_hal_nvs_secure_erase()`) | the last data erase |
| R6 | write `OS2S {FRESH, MIGRATED, PURGE, short_boots 0, key_gen = previous key_gen + 1}`. The key_gen tombstone means an erased OS2K that a backend brings back is caught (SECURITY_V2 § 10.1 row 1). PURGE makes the next boot remove the superseded copies from the raw storage (SECURITY_V2 § 10.4a) | the device must come back fresh |
| R7 | delete the marker | |
| R8 | software reset, without OS2C → the boot sees FRESH → **fresh claim mode** (SECURITY_V2 § 7) | the user is present, and the device is ready to be set up |

### 5.3 Resume on boot

At every boot, after the reset-pin check (§ 3.1 step 4), if a valid marker exists:

| Marker stage | Resume at | Notes |
|---|---|---|
| 1 SCRUB | R1b | R1b and R2 are idempotent |
| 2 DRAW | R3 | if `draw_attempts` is already 2, R3 skips drawing, so a draw that browned out twice will not loop |
| 3 ERASE_REST | R5a | R5a/b/c are each idempotent. If the config is already gone, R5a had already completed (it comes before R5c), so skipping an unmountable store is correct |
| 4 KEY_ERASE_FAILED | — | **LOCKED**, overriding the boot decision. Gated commands get FE, SEC_* answer LOCKED, and the radio runs so that a client can read SEC_STATUS. Recovery: a full USB/SWD erase. A storage that cannot erase one record is a hardware fault |

A power failure between any two steps therefore repeats at most one step. The marker write itself
is atomic per the `od_hal_nvs` contract (whole old, whole new, or nothing):
- a lost R1 write means no reset happened yet, and the user holds the button again;
- a lost R7 makes the next boot run R5-R7 again, which is harmless.

If the marker itself is lost or corrupt (SECURITY_V2 § 10.1 cannot tell), R1b ensures no secrets
are left on an open device.

### 5.4 Panel absent, broken or slow

- If no display is configured, the config has no drawable mode 0 (`OD_DISPLAY_NO_DISPLAY` /
  `OD_DISPLAY_BAD_MODE`, NEEDS_MIGRATION, a missing 0xB3 on a native board), R3 does not draw.
  Only the LED/buzzer patterns are used.
- If the panel does not respond, the drive sequence's BUSY waits time out, `od_display_boot()`
  returns `OD_DISPLAY_PANEL`, and R3 continues to R4.
  - Implementations must make sure the whole draw is bounded: BUSY timeouts per wait, with a
    target total of ≤ 120 s.
  - The watchdog must be fed. If it fires anyway, the attempts counter ends the loop after two
    tries.
- The firmware cannot confirm the draw visually, so a wrong-looking screen is not detected.

## 6. What is erased

| Item | Factory reset | CONFIG_CLEAR 0x0045 | SEC_KEY UNCLAIM |
|---|---|---|---|
| K_dev (OS2K), claimed state | **erased** (R2) | kept | **erased** |
| OS2S (FRESH flag, gesture counter, key_gen) | **rewritten as FRESH**, key_gen tombstone (R6) | kept | key_gen tombstone |
| pending claim / session / OS2C | dropped | kept (session) | dropped |
| secrets in the config (SECURITY_V2 § 10.4: 0x26 `password`, 0x2D `eik` / `net_a_adv_key` / `advertisement_key`, the 0x27 key) | **scrubbed first** (R1b), then erased with the config | claimed: erased, but 0x27 kept at rest; unclaimed: erased | **scrubbed** (before the key is erased) |
| ESP32 `nvs.net80211` WiFi copy | **erased** (R1b) | erased | erased |
| config record: every packet (0x01/0x02/0x04 system, 0x20 display, 0x26 WiFi, 0x27, 0x2D FindMy, 0xAE-0xB3 drive data, 0xB2 actions) | **erased** (R5c) | **claimed device**: replaced by a config holding only 0x27 (claim evidence + sticky reset pin, SECURITY_V2 § 10.6); **unclaimed**: erased | kept, except the secrets |
| image store slots | **erased** (R5a) | kept | kept |
| BLE bonds | **erased** (R5b) | kept | kept |
| FindMy / FMDN runtime state outside the config (if a target keeps any) | **erased** (R5b) | kept | kept |
| firmware, bootloader, MCUboot secondary slot, factory data (UICR, factory-embedded config) | **kept** | kept | kept |

CONFIG_CLEAR stays a remote config wipe; on a claimed device it needs a session (SECURITY_V2 § 6).
It deliberately does **not** remove the key, the claim evidence or the reset pin. So a remote
clear can neither leave a claimed device open nor remove the owner's recovery path. It does not
draw a screen.

A factory reset is CONFIG_CLEAR plus the key, images and bonds, done physically, in the order
above, and it ends in fresh claim mode.

## 7. Fallbacks

### 7.1 No working panel

§ 5.4 covers it: the reset still completes, with LED/buzzer feedback only. The fresh claim mode
that follows is then set up with the blank-panel or no-panel path (SECURITY_V2 § 8.6), or with a
corrected preset.

### 7.2 No simple-config ids

With no 0x02 packet, or indices of 0, the URL omits `b`, `d` and `p`, for example
`https://opendisplay.org/l/?f=1&k=2&i=1A2B3C&t=1&v=3.0.0`. The page then offers the manual board
picker.

### 7.3 No reset pin (button-less boards, or a config without an enabled 0x27 pin)

There is no physical factory reset through firmware. For a lost key, the recovery is:
- a **full flash erase** over USB/SWD (ESP32 `esptool erase_flash`, then a reflash; nRF and BG22
  mass erase over SWD), which also proves physical access;
- or, where the board data names a free **pad or jumper**, a preset default that uses it as the
  reset pin (§ 2.1).

Since revision 3 the firmware-level path is **five quick power-cycles** (§ 3.4, SECURITY_V2
§ 7.10), adopted by maintainer decision, on by default. **If a claimed device has the pattern
switched off (0x27 bit 6) and no reset pin, this section's wired reflash is its only lost-key
recovery** (revision 4; clients warn before writing that combination). Its cost is stated there: a false trigger erases the
key, WiFi and FindMy settings and leaves the device fresh and claimable by anyone nearby, and
flaky supplies (supercap and solar boards, sagging batteries, nRF52's indistinguishable
brown-outs) can produce the pattern. Boards with such supplies should switch the pattern off
(0x27 bit 6 per device, or `OD_FR_PATTERN_SHORT_BOOTS 0` per build) and keep a pad or jumper reset
pin.

## 8. Per-target notes

| Target | GPIO for the check | Reset cause (also used by the claim gesture, SECURITY_V2 § 7.2) | Erase specifics |
|---|---|---|---|
| esp32-idf | `od_hal_gpio_config_input/read` (exists target-local, `targets/esp32-idf/hal/od_hal_gpio.h`) | `esp_reset_reason()` + `esp_sleep_get_wakeup_cause()`; retained RAM `RTC_NOINIT_ATTR` for OS2C | `od_hal_nvs_secure_erase()` for the config; OS2K/OS2S/OS2R in NVS namespace `od_sec`; `nvs.net80211` |
| nordic-zephyr | `gpio_pin_configure` / `gpio_pin_get` through the pin codec (`od_gpio.c`) | `hwinfo_get_reset_cause()` + `hwinfo_clear_reset_cause()` (RESETREAS accumulates); `__noinit` RAM | OS2* records in a separate NVS/settings area (not the 2 x 4 kB config NVS); `bt_unpair` |
| efr32bg22-slc | `GPIO_PinModeSet` / `GPIO_PinInGet` | `RMU_ResetCauseGet()` / EMU reset cause; BURAM retention registers | NVM3 objects; `sl_bt_sm_delete_bondings()`; the image store per `od_cmd_flash.c` |

The shared part, `od_factory_reset.c`, holds the sampling, the debounce, the stage machine and the
marker format. It uses only `shared/hal` interfaces, per the repo's one rule; the plan lists the
new HAL functions.

## 9. Tests

**Host tests** (no hardware):
- the debounce/hold state machine against scripted pin traces: glitches; release at 4.99 s,
  5.0 s, 29.99 s and 30 s; a stuck pin;
- the stage machine with a fake store that "loses power" after every individual write and erase.
  Every interruption point must converge to a fully erased, FRESH device within at most one more
  boot, with these checks:
  - **no interruption point may leave an unclaimed device with a secret in its config**, and
    likewise with the marker deleted at that point (the A4 property);
  - the draw runs at most twice;
- a key erase that always fails ends in stage 4 / LOCKED without a loop;
- the boot after R8 enters FRESH claim mode;
- URL builder vectors (SECURITY_V2 V13);
- RESET-screen renders for 152x152, 200x200, 128x296 and one 800x480 layout, with the QR decoded
  back to the URL, and with a config whose 0x2C strings are set, to prove they are not drawn.

**Hardware.** Run so far (revision 4), on one ESP32-S3 only: the five-cycle trigger on an
unclaimed, a claimed and a LOCKED device; an EN reset landing at R1b, R2, R5c and R6, each
resuming correctly; a raw NVS readback after the reset showing nothing of the owner. Everything
below is still **open** (each row goes into `docs/HARDWARE_VERIFICATION_CHECKLIST.md`), as is
every row on BG22 and Nordic:
- hold-to-reset on one board per target;
- on a latch board;
- on a LOCKED device;
- button wake from deep sleep on ESP32;
- power pulled during the refresh (it resumes and erases);
- power pulled right after R1b (the device is claimed and holds no secrets);
- panel unplugged (the reset completes);
- the QR scanned on a 1.54in panel;
- the device in fresh claim mode afterwards.
