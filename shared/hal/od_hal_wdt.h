/* od_hal_wdt.h -- the watchdog HAL every target implements.
 *
 * Link-time C functions, not a vtable: shared/ is plain C and binds to the target
 * implementation at link time (CLAUDE.md § "The one rule"). The panel bus
 * (od_epd_cfg_bus_t / od_epd_io_t) is the single deliberate function-pointer exception in this
 * tree, and this is not it.
 *
 * THE DIVISION OF LABOUR. The target owns the peripheral, the retained-memory backing store,
 * and the chip's reset-cause register. The shared controller (od_watchdog.h) owns the POLICY
 * built on top of them: strike counting, safe mode, the retained-byte layout, and breadcrumb
 * change detection. Nothing here decides policy; nothing in od_watchdog.c names a register.
 *
 * MODELLED ON A WORKING IMPLEMENTATION, NOT DESIGNED FROM SCRATCH. Every function below exists
 * because Firmware/src/watchdog_nrf.cpp needed it on real hardware. That file is 413 lines, of
 * which roughly a third is portable policy that had no home; this HAL is the seam that lets the
 * policy move here and the register work stay in targets/.
 *
 * CONTEXT. Every function here is called from the application loop or from boot, never from an
 * ISR. See od_watchdog_feed()'s contract: an interrupt-fed watchdog proves the interrupt
 * controller is running, not the program, which is the classic way to build a watchdog that
 * never fires.
 */
#ifndef OD_HAL_WDT_H
#define OD_HAL_WDT_H

#include <stdbool.h>
#include <stdint.h>

/* shared/ is plain C, but not every target is: the ESP32 implements its HALs from C++
 * translation units. Without this guard those definitions get C++ linkage and fail to match
 * the C caller in shared/core. The host tests are C-only and structurally cannot catch it.
 * Same reasoning as od_hal_adv.h, and the same bug if it is omitted. */
#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------- reset cause --- */

/* Why the chip last reset, as a BITMASK -- several causes can be latched at once.
 *
 * A bitmask rather than an enum because that is what the hardware gives: nRF52840's RESETREAS
 * latches every cause independently, and an implementation that reported only the first match
 * would hide a watchdog reset behind a coincident pin reset. Firmware/src/watchdog_nrf.cpp
 * learned this the expensive way -- an earlier revision omitted three causes and printed them
 * all as POWERON, because its fallback keyed on "nothing matched" instead of on r == 0.
 *
 * POWER_ON IS AN EXPLICIT BIT, not the zero value. On nRF a cold start latches nothing, so the
 * target maps 0 to this bit; ESP-IDF reports ESP_RST_POWERON directly. Making it explicit means
 * OD_HAL_WDT_RESET_UNKNOWN keeps its own distinct meaning -- "the target could not tell" -- and
 * a caller never has to guess which of the two an all-zero word meant.
 */
#define OD_HAL_WDT_RESET_UNKNOWN   0x00u
#define OD_HAL_WDT_RESET_POWER_ON  0x01u
#define OD_HAL_WDT_RESET_WATCHDOG  0x02u  /* the one the strike counter keys on */
#define OD_HAL_WDT_RESET_PIN       0x04u
#define OD_HAL_WDT_RESET_SOFTWARE  0x08u  /* NVIC_SystemReset, esp_restart, sys_reboot */
#define OD_HAL_WDT_RESET_LOCKUP    0x10u
#define OD_HAL_WDT_RESET_BROWNOUT  0x20u
#define OD_HAL_WDT_RESET_OTHER     0x40u  /* recognised by the target, not modelled here */

/* Report the cause of the CURRENT boot. Called once, from od_watchdog_boot_init().
 *
 * IMPLEMENTATION TRAP, worth stating here because it silently destroys the whole feature: some
 * frameworks read and clear the hardware register before application code runs. The Arduino
 * nRF5 core does exactly this (cores/nRF5/wiring.c) and exposes the saved word through
 * readResetReason(); a target that reads the peripheral itself gets zero on every boot and
 * reports every watchdog reset as a power-on. Read whatever your framework saved, not the
 * register, unless you have established the framework leaves it alone.
 */
uint32_t od_hal_wdt_reset_reason(void);

/* ----------------------------------------------------------------------- retained storage --- */

/* One byte that survives a watchdog, lockup or software reset, and is lost on power-on.
 *
 * ONE BYTE IS THE CONTRACT, and it is the lowest common denominator on purpose. nRF52840 offers
 * GPREGRET2 -- 8 bits, and GPREGRET (id 0) is already spoken for by the DFU handshake, so there
 * is no second byte to be had there. Targets with more retained space (ESP32 RTC slow memory,
 * Zephyr retained_mem) must not widen this: the layout in od_watchdog.h packs a validity tag,
 * a strike count and a breadcrumb phase into these 8 bits, and a target-specific widening would
 * make the retained state mean different things on different boards.
 *
 * Both accessors report failure rather than swallowing it. A silently dead breadcrumb is worse
 * than no breadcrumb, because the boot log still prints a phase -- a stale or zero one -- and
 * invites a confident wrong conclusion about where the firmware wedged.
 *
 * A target with no retained memory at all returns false from both. The policy layer degrades to
 * "no breadcrumb, no strike counting" and says so, rather than pretending.
 */
bool od_hal_wdt_retained_read(uint8_t *out);
bool od_hal_wdt_retained_write(uint8_t value);

/* -------------------------------------------------------------------------- the watchdog --- */

enum od_hal_wdt_arm_result {
    OD_HAL_WDT_ARM_OK = 0,      /* armed by this call */
    OD_HAL_WDT_ARM_INHERITED,   /* one was ALREADY running; adopted and fed, not started */
    OD_HAL_WDT_ARM_DISABLED,    /* build-time disabled (timeout 0); nothing armed */
    OD_HAL_WDT_ARM_UNSUPPORTED, /* this target has no hardware watchdog */
    OD_HAL_WDT_ARM_ERROR
};

/* Arm the hardware watchdog for timeout_s seconds. Called ONCE, after od_watchdog_boot_init().
 *
 * IRREVERSIBLE ON SOME TARGETS -- there is deliberately no disarm in this HAL. The nRF52840 WDT
 * cannot be stopped once started: its register block has no TASKS_STOP and no ENABLE, and only
 * a system reset clears it. Exposing a stop() would advertise a capability one target cannot
 * honour, so none is offered.
 *
 * THE TIMEOUT IS A PARAMETER BUT NOT A RUNTIME KNOB. nRF latches CRV at the start task, so it
 * must be known before arming and can never change afterwards. Passing it here rather than
 * reading a per-target macro keeps the value in one place (OD_WDT_TIMEOUT_S, od_watchdog.h);
 * it does not mean a caller may re-arm with a different value.
 *
 * INHERITED IS NOT AN ERROR, and handling it is not optional. If a running watchdog survives a
 * non-power-on reset -- unestablished on nRF52840, and consequential either way -- then a build
 * with the watchdog disabled, reached by DFU or a reflash from a build that had it enabled,
 * would inherit a live watchdog it never feeds and reset forever until someone physically
 * removes power. That is a brick. An implementation must detect a running watchdog and feed it
 * regardless of its own configuration, which is correct under both possibilities.
 */
enum od_hal_wdt_arm_result od_hal_wdt_arm(uint32_t timeout_s);

/* Reload the counter. A no-op when nothing is armed or inherited.
 *
 * Must be cheap: this sits on tight poll loops. On nRF that means writing every ENABLED reload
 * register, because RREN is an AND rather than an OR -- feeding only RR0 would never reload an
 * inherited watchdog that had a different subset enabled.
 */
void od_hal_wdt_feed(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_HAL_WDT_H */
