# Factory reset, reset pin and recovery screen — specification

Status: **DRAFT, 2026-11-11, not implemented.** Values marked provisional need adopting in the
protocol repo. Companion to [SECURITY_V2.md](SECURITY_V2.md); work breakdown in
[SECURITY_V2_IMPLEMENTATION_PLAN.md](SECURITY_V2_IMPLEMENTATION_PLAN.md).

## 1. What a factory reset is

A factory reset is the **physical** recovery path for a device whose key is lost, or that changes
owner: the user holds the configured reset button while the device boots; the device draws a
"factory reset" screen with a QR code to a recovery page, erases its key, configuration, stored
images and bonds, and reboots unclaimed and open with a fresh 10-minute claim window.

It is the only way to remove ownership without the key (SEC2 has no remote re-claim; SECURITY_V2
§ 10). Remote equivalents for the *owner* are SEC_KEY UNCLAIM + CONFIG_CLEAR + FLASH_ERASE_ALL.

Today only `targets/esp32-idf/src/encryption.cpp:96` `checkResetPin()` implements a reset pin
(called once from `config_parser.cpp:310` after config load: one 100 ms delay, one sample, no hold
time, erases the config, reboots). 3.0 replaces it with one shared implementation on every target.

## 2. Configuration (config packet 0x27, unchanged layout)

| Field | Meaning on 3.0 |
|---|---|
| `reset_pin` | the GPIO, in the **same encoding the target uses for binary-input pins** (packet 0x25 `inputpinN`): ESP32 GPIO number; nRF the packed/absolute port-pin byte decoded by `od_pin_codec`; BG22 the port/pin byte its config parser uses for buttons. `0xFF` = none |
| `flags` bit 2 `RESET_PIN_ENABLED` | must be set, otherwise no reset pin |
| `flags` bit 3 `RESET_PIN_POLARITY` | 0 = LOW asserts (button to GND, the common case), 1 = HIGH asserts |
| `flags` bit 4 / bit 5 | internal pull-up / pull-down while sampling |

The reset pin lives in the config, so it is the owner's (on a claimed device only the owner can
change it) and it disappears with the config: after a factory reset there is no reset pin until a
new config is written — the device is then open anyway.

### 2.1 Defaults in the board presets

The website's `simple-config-presets.json` and py-opendisplay's copy gain a `securityConfig`
default block for **every board that has a button**: `reset_pin` = the board's first button
(`buttons.inputpin1`), `RESET_PIN_ENABLED` = 1, polarity and pulls copied from how that board's
binary-input config already reads the same button. Encryption fields stay 0 (SECURITY_V2 § 9.2),
`session_timeout_seconds` 0 (= 3600 s).

Boards in `v3/website` presets with a `buttons` block (2026-11-11): en04, en05, ee02, ee04, ee05,
good-display-133c02, reterminal-e1001, -e1002, -e1003, -e1004, reterminal-sticky, m3-nrf,
m3-pro-silabs, opendisplay-426-mono-kit, opendisplay-73-color-kit, nm-epd-420-4c, m5-paper.
Boards without a button (the XIAO boards, m3-lite-nrf, m3-core-silabs, esp32-s3-wspp,
esp32-epd-driver, inkplate-5v2/-10, x4, m5-papers3, ee03, nrf54*-xiao) get no reset pin; see § 7.3.

Preset rules (each preset entry is **untested** until checked on the board, and must be listed in
`docs/HARDWARE_VERIFICATION_CHECKLIST.md`):

- **Never a strapping pin.** Holding a strapping pin during reset changes how the chip boots, so
  the firmware never runs and the reset never happens: ESP32 GPIO 0, 2, 5, 12, 15; ESP32-S3 GPIO 0,
  3, 45, 46; ESP32-C3 GPIO 2, 8, 9; ESP32-C6 GPIO 8, 9, 15; nRF52840 P0.18 when used as nRESET.
  Example: nm-epd-420-4c lists GPIO 45 and GPIO 0 as buttons — neither may be the reset pin; pick
  another button or none.
- ESP32 GPIO 34-39 have no internal pulls; a preset using them must rely on the board's external
  pull (m5-paper buttons 37/38/39) and set no pull flag.
- Not the D-FF power latch clock (`pwr_pin_3`) unless the board's latch toggles only on press
  edges (verify on the board; a hold must not toggle power off).

## 3. Behaviour at boot

### 3.1 When it is sampled

Every boot, on every target, in this order:

1. Power-latch hold asserted (boards with a latch) — so releasing the button cannot drop power.
2. Config loaded (the reset pin is in it).
3. **Reset-pin check** (this section) — before the boot screen, BLE advertising and the display
   init, so a stuck panel cannot prevent a factory reset.
4. Resume an interrupted factory reset if the reset marker exists (§ 5.3) — this runs even when
   there is no config.

Every boot includes wakes that reset the chip: ESP32 deep-sleep wake (timer or button), nRF
System OFF wake, BG22 EM4 wake, power-latch power-on. On a **button wake** the user simply keeps
holding the button that woke the device. On a **timer wake** the check costs one GPIO read (the
pin is not asserted) and the boot continues; no delay is added. The pin is **not** sampled at
runtime: a long press while running is an ordinary button event (0xB2 BUTTON_LONG actions).

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

Committing on **release** (not at 5 s) gives the user a chance to see "release now" and avoids
resetting a device whose button is jammed or pressed by its case.

### 3.3 Feedback (LED / buzzer), also used where there is no panel

Using the first configured LED (packet 0x21) and the passive buzzer (0x29) if present:

| Phase | LED | Buzzer |
|---|---|---|
| holding, < 5 s | blink 1 Hz (100 ms on) | — |
| threshold reached, "release now" | solid on | one short beep |
| erasing (after release) | blink 5 Hz | — |
| done, just before reboot | 3 long flashes | two beeps |
| stuck pin, aborted | 3 slow blinks | — |
| (SEC2) claim pending on a device without panel | double blink every 2 s | — |

Devices with neither a LED nor a buzzer rely on the recovery screen; with no panel either, the
reset is silent (the reboot, the changed advertisement — MSD bit 3 cleared — and the new open state
are the only signs).

## 4. Recovery screen

### 4.1 Layout — the boot screen, re-labelled

Rendered by the existing boot-screen path in **display mode 0**: `od_display_boot()` →
`od_boot_screen_render()` (ESP32 incl. its IT8951/parallel sinks via
`OD_DISPLAY_SHARED_BOOT_SINK=0`; nordic-zephyr) and BG22's own renderer in
`targets/efr32bg22-slc/opendisplay_display.cpp` (which also uses `od_boot_url_build` + the
vendored `qrcode`, QR version 6, ECC MEDIUM). The renderer gains a screen-kind parameter
(NORMAL / CLAIM_CODE / RECOVERY); RECOVERY changes only the middle text lines and the QR text:

```
full layout (>= 400x300 logical)          reduced layout (small panels)
header: manufacturer / model (as boot)    "RESET"
"FACTORY RESET"                           "OD1A2B3C"
"Settings and key erased"                 "Scan QR"
"Scan to set up again"
"ID:   OD1A2B3C"                          QR: recovery URL
"FW:   OD ver 3.0.0"
"URL:  OpenDisplay.org/r"
QR: recovery URL
footer: as boot (scheme, resolution, swatches, battery, temperature)
```

When text and QR do not both fit, the QR wins and the text falls back to the reduced lines.
E-paper keeps the image after the erase; the unconfigured device then draws nothing at later
boots, so the recovery screen stays until a new config is written (or until a factory-embedded
config re-provisions the device at the next boot and draws its normal boot screen — ESP32
`tryProvisionFactoryEmbed`, Nordic `factory_config.c`).

### 4.2 QR URL (provisional)

```
https://opendisplay.org/r/?i=<id>&v=<fw>&t=<target>[&b=<driver>][&d=<display>][&p=<power>]

i   6 upper-case hex digits: od_boot_app_device_id24() — the suffix of the advertised name "OD1A2B3C"
v   firmware major.minor.patch, decimal ("3.0.0")
t   READ_IDENT fw_target, decimal (1 FIRMWARE, 2 NRF54, 3 SILABS)
b   ManufacturerData.simple_config_driver_index   (decimal, omitted when 0 / no 0x02 packet)
d   ManufacturerData.simple_config_display_index  (omitted when 0)
p   ManufacturerData.simple_config_power_index    (omitted when 0)
```

Example: `https://opendisplay.org/r/?i=1A2B3C&v=3.0.0&t=3&b=16&d=10&p=2` (61 bytes). Worst case
with every field at its maximum (`b=65535&d=65535&p=65535`, `v=255.255.255`) is 79 bytes, inside
QR version 6 ECC-M byte capacity (106), which the renderer already uses — no renderer change for
size. On a 200x200 1.54in panel a version-6 symbol plus quiet zone (49 modules) renders at 4 px
per module (196 px); on 152x152, 3 px (147 px); on a 128-px-wide 2.9in, 2 px (98 px, ~22 mm).
Host render tests must confirm these three fit (plan WP5).

Rules: the URL carries **no secret** (the id is advertised; indices and version are public); query
form, not path form, because opendisplay.org is statically hosted and `/r/index.html` reads
`location.search`. Parameter order is fixed so the string is deterministic (test vectors).

### 4.3 What the opendisplay.org recovery page must show

A new static page `httpdocs/r/index.html` (website work, plan WP9):

1. The device name `OD<i>` and the firmware version; "This display was factory reset. Its
   settings, its security key and its stored images were erased. It is now unclaimed."
2. If `b`/`d`/`p` are present: the board, display and power option names, looked up in
   `firmware/toolbox/simple-config-id-registry.json` (index → string id) and the presets file,
   with a button **"Set up this display again"** that opens the toolbox with exactly these presets
   pre-selected (`/firmware/toolbox/?board=<id>&display=<id>&power=<id>`, string ids).
   If an index is unknown to this website version: say so and fall back to the manual picker.
3. If they are absent: "Choose your board and display" (the normal simple-config flow).
4. The next steps, in order: switch the display on (the claim window is open for 10 minutes
   after a power-on or a factory reset), write the configuration, then claim it from Home
   Assistant, py-opendisplay or the website, and enter the code shown on the display.
5. For `v` < 3: a note that this is a 2.x firmware screen (should not occur; 2.x has no
   recovery screen) and a link to the normal toolbox.
6. No analytics or third-party requests that would leak the id; the page works from the query
   string alone.

## 5. Erase procedure and power-fail ordering

### 5.1 The ordering problem

After the config is erased there are no drive packets (0xAE-0xB1) left to drive the panel, so the
recovery screen **must be drawn from the still-present config before the config is erased**. But
drawing is the slowest and least reliable step (1-40 s refresh, BUSY timeouts, a panel that is
absent or broken, a weak battery that browns out during the refresh). The reset must complete in
every one of those cases, and a power failure at any point must not leave a half-reset device or a
loop.

### 5.2 Stages

A small **reset marker** record makes the procedure resumable. It is stored through the same
secret-record HAL as K_dev (SECURITY_V2 § 9.1), separate from the config record:

```
"OS2R" (4) | version 0x01 | stage (1) | draw_attempts (1) | reserved 0x00 | crc32 (4)   = 12 bytes
stage: 1 ERASE_KEY, 2 DRAW, 3 ERASE_REST
```

| Step | Action | Why here |
|---|---|---|
| R0 | trigger accepted (release between 5 and 30 s) | |
| R1 | write marker {stage 1, attempts 0} | from here on, the reset finishes even across power loss |
| R2 | erase the **secret record** (K_dev; secure-erase where the HAL supports it) and the retained claim-window record; tear down any session | the security-relevant step first, before anything slow; the key is not needed to draw. From R2 to R5c the config may still hold secrets on a device without a key — acceptable only because the whole procedure (and its resume, § 5.3) runs **before the radio is started**; the BLE stack and advertising must not start until R6 is done |
| R3 | write marker {stage 2, attempts + 1}; if attempts <= 2 **and** a display is configured: draw the recovery screen (mode 0) from the config; the result is ignored | attempts is written *before* drawing, so a brown-out or watchdog reset during the refresh counts |
| R4 | write marker {stage 3} | drawing is over, success or not |
| R5a | erase the **image store** (all slots; `od_imgstore_erase_all()`, or each slot's header sector where that is much faster) — skipped if the store is not configured or does not mount | must precede the config erase: the store's SPI pins and geometry come from the config (0x2B, 0x20) |
| R5b | erase **BLE bonds** (nordic-zephyr `bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY)`; BG22 `sl_bt_sm_delete_bondings()` if the SM ever stored any; ESP32 none persisted) and target extras (ESP32: ESP-IDF WiFi NVS namespace if WiFi storage is not RAM; BG22 FMDN/FindMy runtime NVM3 objects if any exist outside the config) | |
| R5c | erase the **config record** (`od_config_store_clear()`; ESP32 via `od_hal_nvs_secure_erase()`) | last data erase |
| R6 | delete the marker | |
| R7 | write the retained record "fresh claim window" (SECURITY_V2 § 7.2); software reset | the user is physically present: the device reboots claimable for 10 minutes |

### 5.3 Resume on boot

At every boot, after the reset-pin check (§ 3.1, step 4), if a valid marker exists:

| Marker stage | Resume at | Notes |
|---|---|---|
| 1 ERASE_KEY | R2 | R2 is idempotent |
| 2 DRAW | R3 | if attempts already = 2, R3 skips drawing: a draw that browned out twice will not loop |
| 3 ERASE_REST | R5a | R5a/b/c are each idempotent; if the config is already gone, R5a was already complete (it precedes R5c), so skipping an unmountable store is correct |

A power failure between any two steps therefore repeats at most one step. The marker write itself
is atomic per the `od_hal_nvs` contract (whole old, whole new, or nothing): a lost R1 write means
no reset happened yet (the user holds the button again); a lost R6 makes the next boot run R5 again
(harmless) and then R6/R7.

### 5.4 Panel absent, broken or slow

- No display configured, or the config has no usable mode 0 (`OD_DISPLAY_NO_DISPLAY` /
  `OD_DISPLAY_BAD_MODE`), or NEEDS_MIGRATION: R3 does not draw; LED/buzzer patterns only.
- The panel does not respond: the drive sequence's BUSY waits time out and `od_display_boot()`
  returns `OD_DISPLAY_PANEL`; R3 continues to R4. Implementations must make sure the whole draw is
  bounded (BUSY timeouts per wait; total target <= 120 s) and that the watchdog is fed; if the
  watchdog fires anyway, the attempts counter ends the loop after two tries.
- The draw cannot be confirmed visually by firmware; a wrong-looking screen is not detected.

## 6. What is erased

| Item | Factory reset | CONFIG_CLEAR 0x0045 | SEC_KEY UNCLAIM |
|---|---|---|---|
| K_dev (secret record), claimed state | **erased** (R2) | kept | **erased** |
| pending claim / session | dropped | kept (session) | dropped |
| config record: every packet — 0x01/0x02/0x04 system, 0x20 display, 0x26 WiFi, 0x27 (incl. reset pin), 0x2D FindMy, 0xAE-0xB3 drive data, 0xB2 actions | **erased** (R5c) | **erased** | kept, except the secrets |
| secrets in the config (SECURITY_V2 § 9.3: 0x26 `password`, 0x2D `eik` / `net_a_adv_key` / `advertisement_key`, 0x27 key) | **erased** (with the config, R5c) | **erased** (with the config) | **scrubbed** (zeroed in place, before the key is erased) |
| image store slots | **erased** (R5a) | kept | kept |
| BLE bonds | **erased** (R5b) | kept | kept |
| FindMy / FMDN runtime state outside the config (if a target keeps any) | **erased** (R5b) | kept | kept |
| retained claim-window record | replaced by "fresh window" (R7) | — | — |
| firmware, bootloader, MCUboot secondary slot, factory data (UICR, factory-embedded config) | **kept** | kept | kept |

CONFIG_CLEAR stays what it is today (a remote config wipe; on a claimed device it needs a session,
SECURITY_V2 § 6). It deliberately does **not** remove the key, so a remote clear cannot leave a
claimed device open, and it does not draw the recovery screen. A factory reset is CONFIG_CLEAR
plus key, images and bonds, done physically and in the order above.

## 7. Fallbacks

### 7.1 No working panel

Covered by § 5.4: the reset still completes; LED/buzzer only.

### 7.2 No simple-config ids

No 0x02 packet, or indices 0: the URL omits `b`, `d`, `p` (`https://opendisplay.org/r/?i=1A2B3C&v=3.0.0&t=1`)
and the page offers the manual board picker.

### 7.3 No reset pin (button-less boards, or a config without 0x27)

No physical factory reset through firmware. Recovery for a lost key on such a device is, today:
a full flash erase over USB/SWD (ESP32 `esptool erase_flash` then reflash; nRF and BG22 mass erase
over SWD) — which also proves physical access. Nordic's REWRITE_ALLOWED flag, which used to be
the key-loss escape hatch, is removed in 3.0 (SECURITY_V2 § 9.2). A firmware-level alternative for
button-less boards — a "power-cycle the device N times within M seconds" counter that triggers the
same procedure — is a maintainer decision (plan Q7).

## 8. Per-target notes

| Target | GPIO for the check | Reset cause (claim window, § 5 R7) | Erase specifics |
|---|---|---|---|
| esp32-idf | `od_hal_gpio_config_input/read` (exists target-local, `targets/esp32-idf/hal/od_hal_gpio.h`) | `esp_reset_reason()` + `esp_sleep_get_wakeup_cause()`; retained RAM `RTC_NOINIT_ATTR` | `od_hal_nvs_secure_erase()` for config; secret record in NVS namespace `od_sec`; WiFi NVS |
| nordic-zephyr | `gpio_pin_configure`/`gpio_pin_get` through the pin codec (`od_gpio.c`) | `hwinfo_get_reset_cause()` (RESETREAS), System OFF wake by GPIO = button; `__noinit` RAM | settings/NVS IDs; `bt_unpair` |
| efr32bg22-slc | `GPIO_PinModeSet`/`GPIO_PinInGet` | `RMU_ResetCauseGet()` / EMU reset cause, EM4 wake; BURAM retention registers | NVM3 objects; `sl_bt_sm_delete_bondings()`; image store per `od_cmd_flash.c` |

The shared part (`od_factory_reset.c`: sampling, debounce, stage machine, marker format) uses only
`shared/hal` interfaces, per the repo's one rule; the plan lists the new HAL functions.

## 9. Tests

Host tests (no hardware): the debounce/hold state machine against scripted pin traces (glitches,
release at 4.99 s / 5.0 s / 29.99 s / 30 s, stuck pin); the stage machine with a fake store that
"loses power" after every individual write and erase (every interruption point must converge to a
fully erased device in at most one more boot, and the draw must run at most twice); URL builder
vectors (all fields, omitted fields, maxima, exact bytes); recovery-screen renders for 152x152,
200x200, 128x296 and one 800x480 layout with the QR decoded back to the URL.

Hardware (each row goes into `docs/HARDWARE_VERIFICATION_CHECKLIST.md`, all **open**): hold-to-reset
on one board per target; on a latch board; button wake from deep sleep on ESP32; power pulled
during the refresh (resumes, erases); panel unplugged (completes); QR scanned on a 1.54in panel.
