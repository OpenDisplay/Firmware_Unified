# `shared/core` + `shared/hal` API design

Concrete C interfaces for the target-agnostic core and the HAL it sits on. Grounded in the
four repos' implementations (see DIVERGENCE_MATRIX.md for the evidence and the resolutions
this design encodes). Everything here is **plain C, C99**, no C++ (`shared/` CI rejects
`.cpp`/`.hpp` and Arduino `String`), no vendor headers, no heap, no scheduler.

## The one hard constraint that shapes everything: the EFR32BG22

Every interface below must be satisfiable by the Silabs target, which is the floor:

- **No kernel.** Handlers run inline in the BGAPI event callback (`opendisplay_pipe.c:1271`),
  on the main stack. There is no thread to block, no `k_msleep`, no work queue.
- **~10.3 KB heap, ~21 KB static already committed** (`targets/efr32bg22-slc/README.md`). App
  code owns ~11.6 KB of statics today. `shared/core` must not *add* net RAM — it replaces the
  per-repo copies, and on BG22 it competes with a 3150-byte BT buffer pool for the heap.
- **No `malloc` in app code** (verified: zero `malloc`/`sl_malloc` in Silabs app sources). The
  core must run entirely on caller-provided or statically-sized buffers.
- **Handlers may not block.** The Silabs `wait_for_refresh` already blocks up to 60 s in the
  callback and drops the BLE link mid-refresh; that is a bug the shared model must not
  institutionalize. Long panel operations are driven by a **non-blocking pump**, not by
  blocking calls.

Consequences, stated once and applied throughout:

1. **No function-pointer HAL vtable is required** — targets are selected at *compile time*, so
   the HAL is a set of `extern` C functions the target links against (link-time binding), not a
   `struct` of pointers. This costs zero RAM and zero indirection, which the ESP32's
   two-existing-backends panel ladder already proves works. (A vtable is used in exactly one
   place — the panel ops — because a single target genuinely has 2-3 panel backends; see §3.)
   Link-time binding is also what makes the core **host-testable**: a test binary links stub
   HAL implementations and exercises every path without a board. Do not add a vtable "for
   testability" — that trade is already paid for. See ARCHITECTURE.md § "Everything on the wire
   is testable without hardware".
2. **The core owns all protocol state as file-static singletons**, sized by target macros.
   Single-connection is a given on all four repos (Silabs `MAX_CONNECTIONS=0`, others
   single-`connection`), so no per-connection context is needed.
3. **All parsing lives in the core; the HAL sees only bytes and geometry.** The NRF54 mistake
   of passing raw wire payloads into the panel driver (which then parses the 17-byte 0x76 BE
   header itself — `opendisplay_display.cpp:467-473`) is explicitly *not* reproduced.

## Feature parity across targets is **not** a goal

The BG22 constraint above bounds the *shape* of every interface — no heap, no blocking, no
kernel, caller-provided buffers. It does **not** bound the *feature set*. These are separate
axes and conflating them is the main way this design could go wrong:

- **Shape is universal.** Every signature must be callable from a bare-metal callback on a
  32 KB part. This is non-negotiable and applies to interfaces a given target will never call.
- **Coverage is per-target.** No target is required to implement every subsystem. This restates
  ARCHITECTURE.md's non-goal — *"Lowest-common-denominator features… capability differences are
  expressed through config and `#if`, not by removing features"* — at the level where the
  contract is actually written.

There is still a reference and a direction of travel: **`Firmware`'s ESP32 branch models the
feature set, and the other targets converge toward it** (DIVERGENCE_MATRIX.md § "Reference
implementation and direction of travel"). That governs *which shape to adopt* when two targets
do the same thing differently. It does **not** mean a target is behind because it lacks a
capability, and convergence is not one-way — NFC and Channel Sounding exist outside `Firmware`
and are not defects in it.

The divergence is real and permanent, not a migration backlog to burn down:

| Capability | Where it lives | Why it will not spread |
|---|---|---|
| PIPE sliding window `0x80`-`0x82` | ESP32 | 8.3 KB reorder queue; BG22 has ~10.3 KB of heap total |
| Partial region `0x76` | targets with a framebuffer | BG22 has no framebuffer to compose into |
| WiFi/LAN transport | ESP32 | no radio for it elsewhere |
| tinfl ROM inflate | ESP32 | ROM-provided; others use uzlib bit-serial |
| `CMD_NFC_ENDPOINT` (`0x83`) + NFC hardware | **EFR32BG22 and nRF54L15** | product hardware; ESP32 has no NFC part. *Corrected 2026-07-25 — this row said "EFR32BG22, one product's hardware", which was wrong: NRF54 implements it too (`Firmware_NRF54/src/opendisplay_nfc.c`, dispatch at `opendisplay_pipe.c:1325`; Silabs at `opendisplay_pipe.c:1165`). Two implementations is why the endpoint logic is promoted rather than left target-local — see § "NFC: standard packet, optional support" below.* |
| Channel Sounding | nRF54L15 | silicon feature |
| Parallel e-paper (FastEPD) | ESP32-S3 | needs the S3 LCD peripheral + PSRAM |

So `shared/core` is a **superset with compile-time subsetting**, never an intersection. Promote
a subsystem into `shared/` when *one* target needs it and the logic is target-agnostic — not
when all four agree to adopt it. The mechanism is § "Capability gating" below; the rule this
section states is *when to reach for it*, which is: routinely, and without treating a gap as a
defect.

Two obligations follow, and both are easy to skip:

1. **A disabled subsystem still answers.** It NACKs its opcode rather than dropping the frame,
   so a host can tell "this device does not do that" apart from "this device is broken."
   Silence must never mean *unsupported* — a timeout is a transport failure and a host is right
   to retry it. This is the prerequisite for capability discovery's secondary mechanism
   (ARCHITECTURE.md § "Capabilities are discovered by interrogation, not assumed").
2. **Capability differences the host must know about become host-visible**, not silent. The
   `MAX_CONFIG_SIZE` split (2048 on BG22, 4096 elsewhere — DIVERGENCE_MATRIX) was the
   motivating example: a config that fit nRF was truncated on BG22 with no way for the sender
   to know. Divergence is fine; *undiscoverable* divergence is the bug.

   (That example was resolved on 2026-07-25 the other way — by making
   the value uniform at 4096 rather than making it discoverable, DIVERGENCE_MATRIX §2.7. It is
   kept here because the *principle* is unchanged: what could not be unified would still have
   to be discoverable.)

   Concretely, `od_config.c` must **clamp capability fields on write** (settled 2026-07-25 —
   an earlier draft of this paragraph said clamp on read, which was overturned: clamp-on-read
   makes config export lossy and collapses "can't" into "don't"; see ARCHITECTURE.md § "The
   gap, and a proposed fix" and DESIGN_REVIEW_2026-07-25.md § "Proposal 1"). Mask
   `transmission_modes` and `communication_modes` against a compile-time mask derived from
   this target's `OD_*_ENABLE` set at `CONFIG_WRITE`, apply the same mask at NVS load (so
   blobs provisioned by pre-clamp firmware are corrected too), and report "accepted with
   modifications" via a bit in the config ack's reserved bytes. `CONFIG_READ` stays a
   faithful, byte-stable mirror of stored config. Clamp only bits the firmware is
   authoritative over — never `partial_update_support`, `panel_ic_type`, geometry, or pins,
   which describe the attached panel the firmware cannot interrogate and must not overrule.
   That is the primary discovery mechanism and it costs no wire surface.

## Layering

> **As built, 2026-08-16 (C8-C11).** The composition below was the design; what shipped differs in
> two named ways, and both are deliberate. There is no `od_core_rx()`/`od_core_process()` pair —
> ingress is `od_rxq_push()` and each target writes its own pump, because the surrounding work
> differs (Nordic's main thread also owns LED, buzzer, input and watchdog; ESP32's `loop()` also
> owns the LAN listener and the connection policy). Both pumps are bounded to one ring per pass and
> compose the same shared steps: `od_txq_process()` → `od_config_read_pump()` → stale discard →
> `od_dispatch_frame()` → `od_core_frame_done()` → consume → `od_txq_process()`. The only shared
> `od_core_*` calls are `od_core_frame_done()` (target-implemented policy) and `od_core_reset()`
> (the shared half of a teardown). Second: the transfer state machines are **not** in `shared/`
> — they are target-owned, and C11 left them so on purpose.

```
target BLE/LAN glue ──frame in──► od_core_rx() ──enqueue──► od_core_process()
                                                                 │ parse, decrypt, dispatch
                                                                 ▼
                        ┌───────── shared/core ─────────────────────────────┐
                        │ dispatch · config TLV · xfer state machines ·     │
                        │ session/CCM · advert build · (PIPE, compile-opt)  │
                        └───────────────┬───────────────────────────────────┘
                          calls ▼                       ▲ target supplies
                 ┌── shared/hal (extern C) ──┐   ┌── core/zlib inflate ┐
                 │ time gpio spi i2c nvs      │   │ od_zlib_stream_*    │
                 │ crypto radio log panel     │   │ (uzlib, 512 B win)  │
                 └───────────────────────────┘   └─────────────────────┘
```

The core calls *out* to the HAL and *up* to the target only via `od_hal_radio_send` (response
egress). Nothing in `shared/` includes a vendor header.

---

## `shared/hal` — interface signatures

Each interface is a header of `extern` C declarations. The target provides the definitions in
`targets/<t>/hal/`. Return convention: `int` returning `0` on success, negative `od_err_t` on
failure, unless noted. No interface may block beyond a bounded busy-wait; anything long is
split into start + `busy()` poll (the panel model, §3).

> ### Corrections from the first implementation (2026-08-04)
>
> **This section was written from *reading* the four repos. Five of its interfaces have now
> been implemented for real, in `targets/esp32-idf/hal/`, during phase C of the ESP32 port —
> and the sketch did not survive contact with the hardware in five places.** Each is corrected
> below at the interface it affects, and each is marked **CORRECTED** with what was found.
>
> The pattern is worth stating once, because it will repeat when the Nordic and Silabs targets
> are built: **the sketches are too narrow wherever a real driver needs the bus framing, the
> pin ordering, or the failure signal to be visible.** Reading a driver shows you what it does;
> implementing the interface underneath it shows you which distinctions it was relying on. Two
> of the five were latent hardware defects rather than design opinions — a probe that could
> never detect a device, and a register read that returned plausible garbage.
>
> **Do not treat the uncorrected interfaces here as verified.** `od_hal_crypto`, `od_hal_radio`
> and `od_hal_panel` have not been implemented against this document yet, and the base rate so
> far is five corrections in five interfaces.
>
> **UPDATE 2026-08-04: `od_hal_panel` HAS now been implemented** — `targets/esp32-idf/hal/`
> `od_hal_panel.{h,c}` plus `panel/od_panel_bbep.cpp` and `panel/od_panel_fastepd.cpp`. The
> base rate held: **five more corrections**, listed at the end of that section.
>
> **UPDATE 2026-08-15: `od_hal_crypto` HAS now been implemented** — `shared/hal/od_hal_crypto.h`
> plus both target backends. The base rate held again: the shipped interface differs from this
> document's sketch in **four** ways (enum instead of `int`, prepared key slots instead of a key
> per call, one combined `ciphertext||tag` buffer instead of two pointers, and no `tag_len`
> parameter). That section has been rewritten to the shipped contract. **`od_hal_radio` remains
> unverified** — it is the next one this warning applies to.

### `od_hal_time` — implemented

```c
uint32_t od_hal_uptime_ms(void);          /* monotonic; wrap-safe by subtraction */
void     od_hal_delay_us(uint32_t us);    /* busy-wait; does not yield */
```

The uptime result is milliseconds since boot, converted in the target's full native clock domain
and only then narrowed modulo `2^32`. Shared session, watchdog and TXQ policy retains its explicit
`now_ms` arguments; this ambient clock is not a reason to replace testable policy inputs with a
global dependency.

Bounded millisecond sleep is deliberately outside this interface. ESP32's private
`od_hal_delay_ms(uint32_t)` rounds positive delays up to whole ticks and treats zero as one tick;
Nordic's private `od_msleep(int32_t)` follows `k_msleep`, where non-positive values are already
expired. Naming, signedness, zero behavior and positive-delay rounding need a separate decision
before sleep can be promoted. Panel-refresh waits remain state-machine work regardless of that
future decision.

### `od_hal_gpio` — already exists in embryo

Promote `Firmware_NRF54/src/nrf54_gpio.h`. The critical portability decision: **`cfg` is an
opaque target-encoded pin byte**, decoded inside the HAL, never by the core. (nRF54 encodes
`(port<<4)|pin`; Silabs encodes `0xPN`; `opendisplay_structs.h` documents the field as
target-defined.) This is exactly why the same 66-panel config blob drives three different pin
encodings unchanged.

```c
#define OD_PIN_UNUSED 0xFFu
typedef void (*od_hal_gpio_irq_fn)(void);        /* ISR context: set a flag ONLY */
typedef void (*od_hal_gpio_irq_arg_fn)(void *);  /* same rule; arg is opaque to the HAL */
typedef enum { OD_GPIO_EDGE_RISING, OD_GPIO_EDGE_FALLING, OD_GPIO_EDGE_BOTH }
        od_hal_gpio_edge_t;

bool od_hal_gpio_decode(uint8_t cfg, uint8_t *port_out, uint8_t *pin_out);
void od_hal_gpio_config_output(uint8_t cfg, bool initial_high);
void od_hal_gpio_config_input(uint8_t cfg, bool pull_up, bool pull_down);
int  od_hal_gpio_config_irq(uint8_t cfg, od_hal_gpio_edge_t edge,
                            od_hal_gpio_irq_fn handler);               /* CORRECTED: edge */
int  od_hal_gpio_config_irq_arg(uint8_t cfg, od_hal_gpio_edge_t edge,
                                od_hal_gpio_irq_arg_fn handler, void *arg);   /* ADDED */
void od_hal_gpio_clear_irq(uint8_t cfg);                               /* ADDED */
void od_hal_gpio_irq_enable(uint8_t cfg);                              /* ADDED: mask, */
void od_hal_gpio_irq_disable(uint8_t cfg);                             /*   not detach */
void od_hal_gpio_set_mode_output(uint8_t cfg);                         /* ADDED: no drive */
void od_hal_gpio_write(uint8_t cfg, bool level_high);
int  od_hal_gpio_read(uint8_t cfg);
void od_hal_gpio_park(uint8_t cfg);        /* high-Z for deep sleep */
```

> **CORRECTED 2026-08-04 - four additions, each forced by a driver.** Implemented in
> `targets/esp32-idf/hal/od_hal_gpio.{h,c}` during phase C steps 3, 6 and 7.
>
> * **`config_irq` needs an EDGE.** The sketch specified edge-both with no mode argument. The
>   GT911 touch controller asserts INT active-low and must be attached FALLING; edge-both
>   raises a spurious event on every release.
> * **`config_irq_arg` - an opaque argument handed back to the handler** (Arduino's
>   `attachInterruptArg`). Without it, N buttons need N near-identical ISRs differing only in
>   an index constant - the shape `touch_input.cpp` is stuck with and `device_control.cpp`
>   avoids.
> * **`irq_enable`/`irq_disable` - MASK without detaching**, distinct from `clear_irq()`.
>   `device_control.cpp`'s button re-baselining disables every button interrupt, settles,
>   re-reads the pins, re-enables. Detaching instead discards each handler *and its argument*,
>   and re-attaching is where a wrong index lands one button's events on its neighbour.
> * **`set_mode_output` - make a pad an output WITHOUT driving it.** `config_output()` cannot
>   express this, and the GT911 hardware reset depends on the separation: it makes both INT and
>   RST outputs *before* driving either, and INT's level at RST's rising edge selects the
>   controller's I2C address (0x14 vs 0x5D). Collapsing the `pinMode`/`digitalWrite` pairs
>   reorders that sequence and changes which address the part answers on.
>
> **`decode()` and `park()` are still unimplemented** - no caller has needed either, so neither
> has been checked against hardware. `park()` is asserted by this document and by nothing else.
>
> **A gap this document does not yet cover.** Arduino's `noInterrupts()`/`interrupts()` appear
> at several call sites heading for `shared/core`. The ESP32 target implements them as
> `od_hal_gpio_irq_lock()`/`unlock()` over `portDISABLE_INTERRUPTS`, which is **per-core on
> ESP32 and therefore not the global disable those callers assumed**. DESIGN_REVIEW
> § "Big-picture soundness" already flags that `shared/core` has no global-disable primitive.
> It needs one, with stated semantics, before any of those call sites is promoted - the current
> spelling preserves existing behaviour rather than fixing it.

### `od_hal_spi`, `od_hal_i2c` — panel bus + sensors

The panel bus abstraction that already works cross-target is `bb_epaper`'s `*_io.inl` backends
(Silabs `silabs_efr32_io.inl`, ESP-IDF `esp_generic.inl`). Keep the panel SPI *inside*
`third_party/bb_epaper` (it can't satisfy the `shared/` rule) and expose only what the core's
non-panel code needs. I2C is used by sensors/PMIC, which are target drivers, so the core needs
almost nothing here — keep both minimal:

```c
/* SPI: panel bus is driven by bb_epaper's own backend. This is only for any
   core-level SPI need (none today) — declared for completeness, likely unused. */
int od_hal_spi_write(const uint8_t *data, uint32_t len);

/* I2C: sensor drivers live in targets/; the core does not call I2C directly. */
int od_hal_i2c_read (uint8_t addr, uint8_t reg, uint8_t *buf, uint16_t len);
int od_hal_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *buf, uint16_t len);
```

> **CORRECTED 2026-08-04 - the register-oriented shape cannot express either driver.**
> Implemented as `targets/esp32-idf/hal/od_hal_i2c.{h,c}` in phase C step 5, with primitives
> instead:
>
> ```c
> bool od_hal_i2c_init(uint8_t sda, uint8_t scl, uint32_t hz);   /* one bus handle per port */
> void od_hal_i2c_deinit(void);
> bool od_hal_i2c_is_up(void);
> void od_hal_i2c_set_clock(uint32_t hz);
> int  od_hal_i2c_probe(uint8_t addr);                           /* address only, no data */
> int  od_hal_i2c_write(uint8_t addr, const uint8_t *buf, uint16_t len);
> int  od_hal_i2c_read (uint8_t addr, uint8_t *buf, uint16_t len);
> int  od_hal_i2c_write_read(uint8_t addr, const uint8_t *tx, uint16_t tx_len,
>                            uint8_t *rx, uint16_t rx_len);      /* repeated START, no STOP */
> ```
>
> **Why the sketch does not work.** The two real drivers need opposite bus framing, and
> `read(addr, reg, ...)` picks one of them:
>
> * **SHT40**: write the command, **STOP**, wait ~9 ms for the conversion, then a bare read.
>   There is no register to name, and the delay must sit *between two separate transactions*.
> * **BQ27220**: write the selector then read with a **repeated START and no STOP** - a single
>   transaction. Split into STOP + fresh START the gauge answers as if unaddressed and returns
>   plausible garbage rather than an error.
>
> A register-shaped call gets the framing wrong for each, in opposite directions. The
> primitives express both; the doc's two calls are expressible on top of them, and the reverse
> is not.
>
> **Two further requirements, both learned from live defects:**
>
> * **A presence probe is its own primitive.** `beginTransmission(a); endTransmission();` with
>   no payload is the universal "is anything at this address?", and IDF's
>   `i2c_master_transmit()` **rejects a zero-length transfer outright** - nothing reaches the
>   bus, so no START, no address byte, no ACK to observe. A scan built on a zero-length write
>   reports every address absent whether or not hardware is present; a connected SHT40 was
>   undetectable. `i2c_master_probe()` is the address-only primitive and takes the *bus*
>   handle, so a scan needs no per-address device registration.
> * **Bus lifecycle belongs in the interface.** IDF permits exactly one
>   `i2c_new_master_bus()` per port, so whoever opens the bus owns it. This is not an ESP32
>   quirk to hide: any target where two drivers can independently open a bus has the same
>   problem, and it does not fail cleanly - it fails wherever the two disagree about who
>   configured the pins.
>
> **GT911 clones differ in which framing they accept**, so `touch_input.cpp` tries the
> repeated-START form and then the STOP-then-START form. That is a second, independent reason
> both must be expressible.

Note: `od_hal_i2c` should land on ESP-IDF's `driver/i2c_master.h` (IDF ≥ 5.2), never the
deprecated `driver/i2c.h` (TOOLCHAINS.md).

### `od_hal_nvs` — the config record, as bytes — **SHIPPED**

**Implemented 2026-08-24 (`shared/hal/od_hal_nvs.h`, step 1 of
`plans/PLAN_STORAGE_SEAM_2026-08-23.md`). The signatures below are what shipped; the whole-blob
sketch this section used to carry is superseded.** Per CLAUDE.md decision 14, read the header
over this doc where they differ.

```c
int od_hal_nvs_init (void);
int od_hal_nvs_size (uint32_t *len_out);                 /* stored size, or ENOENT */
int od_hal_nvs_read (uint32_t offset, void *buf, uint32_t len);
int od_hal_nvs_write(const void *record, uint32_t len);  /* ONE contiguous span */
int od_hal_nvs_erase(void);
```

The core owns the record framing (magic, version, inner CRC-32) so the HAL sees an opaque byte
range. **Read takes an offset** because BG22 reads the 16-byte header, validates it, and only
then reads the payload — `nvm3_readPartialData()` does that with no staging buffer at all, on a
part whose whole heap is ~10.5 KB, and a whole-record read would force it to find a second 2 KB
it does not have (MEMORY_CONSTRAINTS.md item 3).

Neither of the other two media can read at an offset, and that is a property of the media rather
than of the port: the Zephyr settings NVS backend registers no `csi_load_one`, so
`settings_load_one()` falls back to a full partition scan, and the only primitive under it,
`nvs_read()`, starts at byte 0; ESP-IDF's `nvs_get_blob()` likewise returns the whole blob or
nothing. Both therefore read the record once into a buffer of their own and serve offsets from
it. `od_hal_nvs_size()` is cheap on ESP32 (`nvs_get_blob` with a NULL destination resolves the
entry through the page index without touching a data page) and on BG22 (`nvm3_getObjectInfo`);
on Nordic it is answered from the same fill, because `settings_get_val_len()` would walk the
partition a second time for a number the fill already has.

**Write takes one contiguous span**, not a header pointer plus a payload pointer: BG22's single
`nvm3_writeData` works only because a union puts header and payload adjacent, and two unrelated
pointers would force exactly the copy the offset seam exists to avoid. The caller supplies the
workspace with the header at offset 0.

Failure semantics are part of the seam, not left to each implementation. A failed mutation must
leave a *whole* record — the complete old one, the complete new one, or nothing on a device that
had none — never a partial write, and an implementation that caches
must **invalidate on failure** rather than either keep or clear its copy: ESP-IDF's
`nvs_set_blob()` writes the new entry before erasing the previous, and Zephyr's settings delete
removes the name entry before the value entry, so in both cases the error arrives after the
medium may already have changed. Invalidating re-reads the medium, so a config that survived is
still served; clearing to empty is the failure the rule exists to prevent — a device reporting
itself unconfigured while the medium still holds its config, and coming back configured on the
next boot.

`od_hal_nvs_secure_erase()` is **not** here. One target defines it and one call site uses it, so
it lives in `targets/esp32-idf/hal/od_hal_nvs_secure.h`.

### `od_hal_crypto` — **SHIPPED**, and the header is now the contract

**Implemented 2026-08-15 (`shared/hal/od_hal_crypto.h`, C1 of the od_session plan). The
signatures below are what shipped; the one-shot sketch this section used to carry is superseded.**
Per CLAUDE.md decision 14, read the header over this doc where they differ.

Firmware used mbedTLS native CCM; NRF54 and Silabs hand-rolled CCM over PSA AES-ECB (RFC 3610) at
a cost of a key import/destroy **per 16-byte block**. The cause turned out to be one missing
Kconfig line rather than a missing capability: `CONFIG_PSA_WANT_ALG_CCM` was never set, while the
Oberon software driver and the CRACEN hardware driver both implement AES-CCM in this NCS tree.
Nordic now uses native `psa_aead_*`, ESP32 native `mbedtls_ccm_*`, and the hand-rolled code is
gone — preserved as `tests/host/session_ccm_reference.inc` for differential testing.

```c
enum od_hal_crypto_status {          /* four values, and the distinction is load-bearing */
    OD_HAL_CRYPTO_OK = 0,
    OD_HAL_CRYPTO_AUTH_FAILED,       /* decrypt only: tag mismatch, NEVER an engine fault */
    OD_HAL_CRYPTO_UNSUPPORTED,
    OD_HAL_CRYPTO_ERROR
};
#define OD_HAL_CRYPTO_TAG_LEN 12u
#ifndef OD_HAL_CRYPTO_KEY_SLOTS
#define OD_HAL_CRYPTO_KEY_SLOTS 1u
#endif
typedef uint8_t od_hal_crypto_slot_t;

/* PREPARED KEY SLOTS. key_set is idempotent: it releases whatever the slot held first. */
enum od_hal_crypto_status od_hal_crypto_key_set(od_hal_crypto_slot_t slot, const uint8_t key[16]);
void                      od_hal_crypto_key_clear(od_hal_crypto_slot_t slot);

/* AEAD over a prepared slot. ONE combined ciphertext||tag buffer; NO tag_len parameter. */
enum od_hal_crypto_status od_hal_crypto_ccm_encrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len, const uint8_t *aad, uint8_t aad_len,
        const uint8_t *plain, uint16_t plain_len,
        uint8_t *ct, uint16_t ct_cap, uint16_t *ct_len);
enum od_hal_crypto_status od_hal_crypto_ccm_decrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len, const uint8_t *aad, uint8_t aad_len,
        const uint8_t *ct, uint16_t ct_len,
        uint8_t *plain, uint16_t plain_cap, uint16_t *plain_len);

/* One-shot, key-per-call. ECB is KDF finalisation only, not a payload primitive. */
enum od_hal_crypto_status od_hal_crypto_cmac(const uint8_t key[16], const uint8_t *msg,
                                             uint32_t msg_len, uint8_t out[16]);
enum od_hal_crypto_status od_hal_crypto_aes_ecb(const uint8_t key[16], const uint8_t in[16],
                                                uint8_t out[16]);
enum od_hal_crypto_status od_hal_crypto_random(uint8_t *buf, uint16_t len);
```

**Four decisions worth not re-litigating.**

*An enum, not `int`.* A tag mismatch must count toward the session's 3-strike teardown and an
engine fault must not — counting an allocation failure as an attack turns a transient OOM into a
forced re-authentication. All three targets collapsed both into `false`, so the distinction never
existed; a shared strike counter is what makes it matter. Vendor status values stay in `targets/`,
where the implementation can log the raw `psa_status_t`/mbedTLS `ret` at the point of failure.

*Slots, not a key inside the caller's state.* An embedded `psa_key_id_t` would make
`memset(&session, 0, sizeof session)` — which is literally how the targets clear a session —
silently drop a live PSA handle. PSA slots are a finite pool, so that leaks a few hundred
re-authentications deep and presents months later as "auth stops working after a while". The slot
keeps one `uint8_t` in `shared/` and the vendor context entirely in `targets/`. It also removes
the last nested resource lifetime from `od_session.c`, which is what settles CLAUDE.md decision
1's C-vs-C++ revisit in favour of C.

*One combined `ciphertext||tag` buffer.* PSA consumes and emits them contiguously; separate
pointers would make adjacency an unwritten precondition the signature cannot enforce, and would
need a bounce buffer on every Nordic call.

*Tag length pinned at the contract.* PSA imports the prepared key with
`PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 12)`, and the key policy pins the algorithm
*including* tag length — so a per-call length would fail `NOT_PERMITTED` on PSA while succeeding
on mbedTLS. One HAL, two behaviours, findable only on hardware.

**`OD_CRYPTO_SOFT_CCM` is deferred, with a stated trigger.** Both targets have native CCM, so a
shared soft path would be a `shared/` source no consumer compiles. Native CCM measured **+2,320 B
flash, +0 B RAM** on nRF52840, nowhere near a level that would justify it. The trigger is a target
with no native CCM, or an unacceptable flash cost — and when it fires, that implementation needs
`od_hal_crypto_ecb_prepared()` so its per-block primitive reuses a prepared key rather than
reintroducing the per-block import this design removed.

### `od_hal_radio` — transport-agnostic framed egress + origin

The core must reply on the *originating* transport only, and must know a frame's origin to
apply/skip the CCM envelope (SECTION 9 rule 4: TLS-PSK LAN bypasses app-layer CCM). Firmware
already does this with a `g_commandOrigin` and `frameOwnsSession()`
(`communication.cpp:27-45, 2115`). Make origin an explicit parameter, not a global:

```c
typedef enum { OD_ORIGIN_BLE = 0, OD_ORIGIN_LAN_PLAIN = 1, OD_ORIGIN_LAN_TLS = 2 } od_origin_t;

/* target implements: send a fully-framed response on the given transport. */
int  od_hal_radio_send(od_origin_t origin, const uint8_t *frame, uint16_t len);
bool od_hal_radio_notify_ready(od_origin_t origin);   /* CCCD enabled / socket up */
```

### `od_hal_log`

Firmware already funnels logging through `writeSerial(String,bool)` — same shape, wrong
signature/type. Silabs uses RTT, NRF54 RTT/UART, ESP32 `esp_log`. One line-sink:

```c
void od_hal_log(const char *line);   /* NUL-terminated, one line, no String */
```

> **CORRECTED 2026-08-04 - a line sink alone is not implementable on both targets.**
> `targets/esp32-idf/hal/od_hal_log.{h,c}` (phase C step 1) exposes four calls, not one:
> `open()`, `is_open()`, `room()`, `write(buf,len)`, `flush()`.
>
> The reason is `room()`. On the nRF target, off-loop producers poll the port's free space and
> **discard** the record rather than block - a producer at a higher priority than `loop()`
> waiting on a stalled USB host is a priority inversion. That backoff needs a free-space
> query, which a `void od_hal_log(const char*)` cannot provide, and a target that answers "yes,
> always" turns the backoff into either a permanent stall or a permanent no-op.
>
> Narrowing to one line sink is a decision to take when the logger is promoted and both targets
> are in front of you - not something to force now by specifying a contract one target cannot
> honestly implement. The extra three calls all die inside `od_log.c`.
>
> Note also what the split buys structurally: the ESP32 port's `od_log` was handed a `Stream *`
> (an Arduino type) purely as its port. Moving the *port* behind `od_hal_log` is what let
> `od_log.h` stop including `<Arduino.h>` - a **port is not policy**, and that separation is
> the general shape for any interface currently taking a vendor object.

### `od_hal_adc` — MISSING from this document

> **ADDED 2026-08-04.** Not specified here at all, and it should have been - not for the core,
> which does not read analogue inputs, but because **two target drivers do** and IDF's oneshot
> driver has a single ADC1 unit handle they must share. Implemented as
> `targets/esp32-idf/hal/od_hal_adc.{h,c}` in phase C step 7:
>
> ```c
> bool od_hal_adc_pin_readable(uint8_t pin);
> void od_hal_adc_set_atten(uint8_t pin, od_hal_adc_atten_t atten);
> void od_hal_adc_set_resolution(uint8_t bits);
> int  od_hal_adc_read(uint8_t pin);            /* raw count, or 0 */
> ```
>
> **`pin_readable()` is the part worth copying to other targets.** A zero reading is also a
> legal reading, so a caller cannot infer failure from the value - and both callers here turn a
> reading into something a host believes. The battery path scales it into the MSD advert, so 0
> becomes a plausible **0.0 V** rather than the -1.0 "unknown" the same function returns when
> no sense pin is configured. The ADC button ladder classifies 0 into its catch-all bottom
> bucket, which reads as the last button permanently pressed. Both were live defects produced
> by a `return 0` stub. Any HAL call whose failure value is indistinguishable from a valid
> result needs a separate "can this work?" question.
>
> **ADC1 only, on ESP32.** ADC2 shares hardware with the WiFi radio on ESP32/S2/S3 and reads
> fail with `ESP_ERR_TIMEOUT` whenever WiFi is up. A pin that maps to ADC2 is reported
> unreadable rather than read unreliably - which is exactly what `pin_readable()` is for.

**Every call site is bound by the no-secrets rule** (ARCHITECTURE.md § "Secrets are never
logged verbatim"): presence and length, never content — for SSID/password (`0x26`),
`security_config` (`0x27`), session and derived keys, nonces, MACs, and raw config/TLV
payloads. It applies at *all* levels, including `debug`, because debug logging is exactly what
gets enabled in the field. `od_config.c` is the highest-risk unit here: a parser that dumps its
input has logged a credential.

### `od_hal_panel` — the display-specific interface

> **Superseded in 3.0.** `od_hal_panel`, `od_panel_ops`, the bb_epaper backend and
> `protocol_pending.h` are deleted. SPI panels are driven by `od_epd` through the
> `od_epd_cfg_bus_t` seam (`shared/core/od_epd_cfg.h`); ESP32's IT8951 / parallel panels are
> selected by the 0xB3 config packet (`targets/esp32-idf/src/od_controller_esp.cpp`). This
> section and its corrections are the design record of the 2.x-era interface.

This is the one interface worth real care, and `display_fastepd.h` (upstream) is the reference
shape. Its actual surface (verified):

```c
void opendisplay_fastepd_load_pins_from_display(const struct DisplayConfig*, const struct SystemConfig*, uint16_t panel_ic_type);
bool fastepd_init_failed(void);
void fastepd_prepare_hardware(void);   void fastepd_epaper_begin(void);
void fastepd_full_update(void);        bool fastepd_wait_refresh(int timeout_sec);
void fastepd_sleep_after_refresh(void);
void fastepd_direct_write_reset(void); void fastepd_direct_write_chunk(const uint8_t*, uint32_t);
void fastepd_direct_refresh(int mode /*0 FULL,1 FAST,2 PARTIAL*/); void fastepd_direct_sleep(void);
void fastepd_mark_hw_deinitialized(void);
void fastepd_partial_prepare(uint16_t x,uint16_t y,uint16_t w,uint16_t h);
bool fastepd_partial_write_chunk(const uint8_t*, uint32_t); bool fastepd_partial_refresh(int mode);
```

Five fixes turn it into a portable C panel HAL (gaps confirmed by the ESP32 analysis and by the
NRF54/Silabs `opendisplay_display.h` boundary):

1. **All C linkage.** `display_fastepd.h` wraps only 2 of 16 functions in `extern "C"`.
2. **A capability query**, so the core asks instead of scattering `getBitsPerPixel()` /
   `seeed_driver_used()` / `e1004_panel_used()` / `directWriteIsGray4()` across handlers — and
   so the core, not the panel driver, decides whether 0x76 is supported. Crucially this captures
   the **streaming vs framebuffer** split: bb_epaper streams to controller RAM with no
   framebuffer; FastEPD/Seeed buffer a full frame and blit. The core must not assume either.
3. **No blocking refresh in the call path.** Refresh is `start` + `busy()` poll, pumped (§ pump)
   — replacing both `fastepd_wait_refresh(timeout)` and Silabs' 60 s in-callback block.
4. **Symmetric error reporting.** `write_chunk` returns an int everywhere (FastEPD's full-frame
   path returns void and silently truncates; the partial path returns bool — pick one).
5. **Normalize plane order at the seam.** bb_epaper routes the first (old) plane to `PLANE_1`
   (`display_service.cpp:3207`); FastEPD routes plane 0 → `previousBuffer` (natural order). Left
   unnormalized, one backend inverts partial updates. The HAL contract fixes plane 0 = old.

Proposed interface. `od_pixfmt_t` and geometry are core types (from parsed config), never
vendor types. etag state stays in the **core** (it is protocol bookkeeping), not the panel:

```c
typedef enum { OD_PIX_1BPP=0, OD_PIX_2BPP, OD_PIX_4GRAY, OD_PIX_BWR, OD_PIX_BWY } od_pixfmt_t;
typedef enum { OD_REFRESH_FULL=0, OD_REFRESH_FAST=1, OD_REFRESH_PARTIAL=2 } od_refresh_t;

typedef struct {
    uint16_t width, height;
    od_pixfmt_t fmt;
    uint8_t  plane_count;         /* 1, or 2 for BWR/BWY/4gray */
    bool     needs_framebuffer;   /* false = streaming sink (bb_epaper), true = buffered (FastEPD) */
    bool     supports_partial;    /* 0x76 / PIPE-partial available on THIS panel */
} od_panel_caps_t;

int  od_hal_panel_init(const struct DisplayConfig *d, const struct SystemConfig *sys,
                       uint16_t panel_ic_type, od_panel_caps_t *caps_out);   /* <0 = init failed */
int  od_hal_panel_begin(void);                          /* full-frame session: open write window */
int  od_hal_panel_begin_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h); /* partial */
int  od_hal_panel_write(const uint8_t *bytes, uint32_t len);  /* sequential sink; plane 0 first */
int  od_hal_panel_refresh_start(od_refresh_t mode);     /* returns immediately */
bool od_hal_panel_refresh_busy(void);                   /* poll; false = done */
void od_hal_panel_sleep(void);
void od_hal_panel_abort(void);
void od_hal_panel_mark_deinitialized(void);             /* rail was cut behind our back */
```

### Corrections from the first implementation (2026-08-04)

Written against the interface above; these are what it got wrong, in the order they bit.

1. **`DisplayConfig` has no `width`/`height`.** The fields are `pixel_width` and `pixel_height`.
   The sketch's `caps.width/height` names are fine, but the mapping is not the identity it looks
   like.
2. **`supports_partial` needs BOTH halves, and the doc named neither.** `partial_update_support`
   is what the *config declares over the wire*; whether the panel has a partial init sequence
   compiled in is a separate fact (`bbep.pInitPart`). A config claiming partial on a panel that
   cannot do it makes the core offer `0x76` and the panel ignore it — silently, which is the
   exact failure this caps query exists to remove. The backend now requires both.
3. **The ops table needs a `claims()` predicate.** "Selected once at `od_hal_panel_init` from
   `panel_ic_type` and `display_technology`" reads as though the selector holds both backends'
   panel lists. It cannot: the FastEPD panel IDs live in `src/protocol_pending.h`, which is
   **C++** (it guards itself with a `static_assert`), while the selector must be plain C for
   `shared/hal`. Each backend now answers for its own panels and the selector just asks in
   order, with bb_epaper last as the fallback.
4. **Correction 3 (no blocking refresh) is NOT satisfiable against FastEPD today.** It exposes
   only `fastepd_wait_refresh(timeout_sec)`, which *blocks*; there is no "is it done" query to
   poll. The backend degrades to a 0-second call, which is a genuine non-blocking check on the
   IT8951 path but coarser than the contract promises. The real fix is exposing the ready-bit
   predicate `it8951WaitForReady()` already computes. **bb_epaper satisfies it natively** —
   `bbepRefresh()` issues the command and returns.
5. **Correction 4 (symmetric errors) is unmet on one path, and the `int` return hides it.**
   `fastepd_direct_write_chunk()` returns `void` and silently truncates a full frame buffer.
   The HAL signature returns `int`, so the seam *looks* closed while the information does not
   exist to fill it. Recorded at the call site rather than papered over.

Because a single target legitimately has 2-3 panel backends (ESP32: bb_epaper + FastEPD +
e1004; Silabs: bb_epaper only), this **one** interface is the place a function-pointer `struct
od_panel_ops *` earns its keep — selected once at `od_hal_panel_init` from `panel_ic_type` and
`display_technology`, exactly the predicate `fastepd_driver_used()` already computes. The
per-panel-controller quirks (EP397 Y-decrement, EP426 X-decrement, e1004 dual-CS) stay behind
the ops table, out of `shared/core`.

---

## `shared/core/od_zlib_inflate` — inflate

The portable engine is `shared/core/od_zlib_inflate.c`: a byte-resumable rewrite of uzlib/tinf,
plain C with no vendor headers or heap dependency. Its stable streaming API is:

```c
void             od_zlib_stream_reset(uint32_t expected_output_size);
od_zlib_status_t od_zlib_stream_push(const uint8_t *input, size_t len, bool final);
od_zlib_status_t od_zlib_stream_poll(uint8_t *output, size_t cap, size_t *produced);
const char      *od_zlib_stream_error(void);
uint32_t         od_zlib_stream_output_count(void);
/* status: NEEDS_INPUT=0, OUTPUT_READY=1, DONE=2, ERROR=-1 */
```

Window size is a **compile-time macro** (`OPENDISPLAY_ZLIB_WINDOW_BITS`, floor 9 = 512 B) backed
by one bounded static state object. The engine rejects a stream whose CMF declares a larger
window than the target allows (`od_zlib_inflate.c`) —
so this macro is a genuine wire contract, not just a buffer knob (see MEMORY_CONSTRAINTS.md).
The ESP32's ROM-tinfl adapter (`od_inflate_tinfl.*`) is a *build-level* alternative behind the
same four-symbol API (`#define` remap) — keep it optional and ESP32-only.

---

## `shared/core` — entry points

The whole command surface is driven by two functions plus init. This shape is dictated by the
Silabs no-kernel constraint: RX enqueues, a pump drains — never blocking the caller.

```c
/* one-time init: wire the parsed config in, init crypto, reset state. */
void od_core_init(void);

/* called from the transport's RX callback (BT thread / BGAPI callback / LAN task).
   Copies the frame into an internal ring and returns immediately — NO parsing,
   NO crypto, NO panel I/O here. `origin` selects the reply transport + CCM policy. */
void od_core_rx(od_origin_t origin, const uint8_t *frame, uint16_t len);

/* called from the main loop / superloop. Drains queued frames: parse → (decrypt) →
   dispatch → reply via od_hal_radio_send. Also advances any in-flight panel refresh
   via od_hal_panel_refresh_busy(). Returns quickly; never blocks on a refresh. */
void od_core_process(void);
```

**Superseded by what shipped.** Both promoted targets use one SPSC ring, `shared/core/od_rxq.c`
(peek/consume, every slot carrying its writer's identity), pushed from the transport callback and
drained by the target's own pump — not a shared `od_core_process()`. Depth derives from the
target's own `PIPE_MAX_W + 2` and is asserted where both constants are visible. Silabs still
dispatches inline from the BGAPI handler.

### Internal structure (one file per subsystem, all file-static state)

| Unit | Responsibility | Primary donor |
|---|---|---|
| `od_dispatch.c` | opcode BE parse, structural validation, tag liveness, producer conflict, response budget, reservation, the auth/decrypt gate, **and the `CMD_*` map** — which dispatches to the per-command target hooks in `od_cmd_app.h`. The ORDER is the specification; see the header. | NRF54 `dispatch()` (structure), Firmware (coverage) |
| `od_config.c` | TLV walk into `opendisplay_structs.h` types, chunked assembly, CRC, `od_hal_nvs` calls | NRF54 size-table parser (unknown-TLV safety) |
| `od_xfer_direct.c` | 0x70/0x71/0x72 state machine, streaming into `od_hal_panel_write`, inflate integration | Firmware (most complete) |
| `od_xfer_partial.c` | 0x76 header parse (17-byte BE), etag policy, two-plane routing | Firmware / NRF54 (parity) |
| `od_pipe.c` | 0x80-0x82 sliding window + reorder + SACK — **compile-gated `OD_PIPE_ENABLE`** | Firmware (only impl) |
| `od_session.c` | auth handshake, KDF, nonce/replay, CCM envelope over `od_hal_ccm_*` | any (byte-identical across all four) |
| `od_advert.c` | 16-byte MSD build (sensor encode, voltage/temp clamp, status bits) | any (identical) |

### The dispatcher does not grow new opcodes

`od_dispatch.c` implements the opcode set the canonical header already defines. **Promoting
logic into `shared/core` is not an occasion to add wire surface** — it is the moment the
temptation is strongest, because the refactor exposes every awkward corner at once, and a new
opcode looks like a cheap way to tidy one.

Reach for the cheaper options first — an existing field, a reserved/extension field, a new
error code in an existing namespace, or a config TLV packet type (versioned and skippable, so
old firmware degrades gracefully). Only then a new opcode.

**If a new opcode would genuinely simplify the structure, propose it** — with what it replaces,
why the cheaper options cannot carry it, and its cost on targets that will only ever NACK it.
The rule is *argue for it in the docs*, not *avoid it*. DIVERGENCE_MATRIX.md § "The opcode
space is conservative" holds the full ordering and two live candidates (capability/limits
discovery for the `MAX_CONFIG_SIZE` split; an explicit compressed-payload flag in place of the
current length inference). Adding one is blocked mechanically anyway while the canonical
headers are frozen — which is the right default for a decision this permanent.

### Capability gating — a target never pays for what it lacks

This is the mechanism for § "Feature parity across targets is **not** a goal"; read that first
for when to reach for it.

Config, dispatch, direct-write, session, and advert are unconditional (all targets implement
them). The optional subsystems are compile-time flags the target sets, and a disabled subsystem
**still NACKs its opcode** rather than dropping it silently (the Silabs fail-fast NACK for 0x76
is the correct model; its silent drop of 0x45/0x80-0x82 is not — DIVERGENCE_MATRIX §1):

```c
#define OD_PIPE_ENABLE      0   /* Silabs/NRF54: 0 — no 8.3 KB reorder queue in .bss */
#define OD_PARTIAL_ENABLE   0   /* Silabs: 0 (no framebuffer) */
#define OD_NFC_ENABLE       1   /* Silabs/NRF54: 1; Firmware: 0. Gates the 0x83 endpoint
                                 * and the hardware ONLY -- never the 0x2A config parse. */
#define OD_BUZZER_ENABLE    0   /* Silabs: 0 */
#define OD_LAN_ENABLE       0   /* ESP32-with-WiFi only */
```

`OD_PIPE_ENABLE=0` must exclude the reorder queue from `.bss` entirely (`#if` around the
`PipeReorderSlot[]`), because 8.3 KB — or even the 4.3 KB small-window form — does not fit BG22.

### NFC: standard packet, optional support

Decided 2026-07-25, resolving the `CMD_NFC_ENDPOINT` placement question that MEMORY_CONSTRAINTS
and TOOLCHAINS both carried as open. NFC is the worked example for every capability flag here,
because it is the one where "optional" means three different things and only two of them are
true:

| Sense of *optional* | NFC | Mechanism |
|---|---|---|
| Optional **in a config blob** — a config need not contain one | **yes** | schema: `@packet 0x2A @repeatable max=2`, unlike `system`/`manufacturer`/`power`, which are required singletons |
| Optional **to support** — a target need not drive NFC hardware or answer `0x83` | **yes** | `OD_CAP_NFC`. Since Phase 4 the opcode routes to `shared/core/od_nfc.c`; the capability-off arm returns `OD_CMD_UNKNOWN` and answers **nothing**, and a supporting target implements `od_nfc_app.h` |
| Optional **to parse** — a target may fail to step over the packet | **NO** | the size-table parser walks every canonical packet unconditionally |

So the placement is a three-way split, not a two-way one:

| Concern | Placement | Gate |
|---|---|---|
| `0x2A` TLV parse into `struct NfcConfig` | `shared/core` — `od_config.c` | **none. Never gated.** |
| `0x83` routing | `shared/core` — `od_dispatch.c`, unconditionally | none: every target defines the hook, so a missing one is a link error |
| the §5 NFC sub-protocol framing | `targets/<t>/` — Nordic's `od_cmd_nfc.c` | absent on a target that returns `OD_CMD_UNKNOWN` |
| TNB132M-over-I2C / SoC-NFCT driver | `targets/<t>/` | target-local backend |

> **Correction, 2026-08-16.** This section said a non-supporting target "NACKs the opcode". It does
> not, and should not: ESP32 answers `0x83` with **silence**, because the canonical header defines
> no "unsupported NFC" code and manufacturing one would be inventing a wire meaning unilaterally.
> `OD_CMD_UNKNOWN` also keeps the frame out of the activity stamp, so probing `0x83` cannot hold
> an exclusive link open. The one place a target does emit an unsupported NACK is `0x52`, where
> the header defines `OD_ERR_POWER_OFF_UNSUPPORTED` for exactly that. `OD_NFC_ENABLE` was never
> implemented; the gate is which hook the target links.

**The gate is on *applying and answering*, never *parsing*.** `OD_PKT_NFC = 0x2A` is a
first-class member of the canonical packet enum in `opendisplay_structs.h`, so a target that
cannot do NFC still walks the packet, stores it, and returns it byte-stable on `CONFIG_READ` —
it simply never acts on it. This is DIVERGENCE §2.1's rule stated for the case that motivated
it: *per-target `#if` only for applying a packet, never for parsing it.* A target is entitled
to ignore a packet; it is not entitled to lose the rest of the blob because of one.

Two consequences worth stating, because both are easy to get backwards:

- **The NRF54 size-table parser is a precondition, not a preference.** Only a parser that knows
  every canonical packet's size can step over one it does not act on. A skip-to-CRC parser
  cannot implement the middle row of the first table above.
- **`NfcConfig` is peripheral-descriptive, so it is not a clamp-on-write candidate.**
  `nfc_ic_type`, the data bus, and the field-detect GPIO describe attached hardware, which the
  firmware is explicitly *not* authoritative over (ARCHITECTURE.md § "A device should determine
  its own capabilities"). Effective capability is the AND of firmware and peripheral; the host
  learns the firmware half from the `0x83` NACK, not from a rewritten config.

The backend split is not hypothetical even within one target: nRF54 already drives an external
TNB132M *and* the SoC NFCT peripheral on the LM20 board (`opendisplay_nfc.c:161`). That is the
same shape as `od_hal_panel`'s ops table — a shared upper layer over a thin backend seam — and
it is an argument for promotion, not against it.

### Invariants the core must assert at compile time

Two numeric couplings are load-bearing and undocumented outside code comments:

```c
/* PIPE window is bounded by the AES-CCM replay window: in-flight ≤ W ≤ 32 ≤ ±32.
   Raising PIPE_MAX_W without raising the replay window silently breaks decryption
   under packet reorder (communication.cpp:740-745 ↔ encryption.cpp:128-136). */
_Static_assert(OD_PIPE_MAX_W <= OD_REPLAY_WINDOW_HALF, "PIPE window exceeds replay window");

/* reorder slot indexing seq % SLOTS is collision-free only if a live window fits. */
_Static_assert(OD_PIPE_REORDER_SLOTS == OD_PIPE_MAX_W + 1, "slot count must be W+1");
```

### Bugs to fix on promotion (not port verbatim) — from DIVERGENCE_MATRIX

- Config TLV: add a real per-record length, or at minimum keep NRF54's size-table so one
  unknown packet type does not abandon the rest of the config (Firmware/Silabs both do).
- Silabs: gate encryption on `sec_enabled()`, not `frame_len ≥ 31` (§1.5a security hole).
- Silabs: clear the session after a config save (key may have changed — §2.4).
- Session timeout: absolute from `session_start`, not idle (§6.2); reject exact-counter replay
  (`diff==0` currently accepted on NRF54/Silabs).
- Replay window is a 64-entry `uint64_t` linear-scan list (512 B) for a ±32 accept window —
  8× oversized; a 64-bit bitmap over the window is ~8 bytes and BG22 wants that RAM back.
