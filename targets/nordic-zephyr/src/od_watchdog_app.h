/* The target's owner of shared/core/od_watchdog.h.
 *
 * The shared controller holds no global state and does not log; something on each target has to
 * own the struct and turn the boot report into log lines. This is that, and it is deliberately
 * the whole of it -- no policy lives here. The ESP32 target carries the same five entry points
 * (targets/esp32-idf/src/od_watchdog_app.h); the logger and the clock are what differ, which is
 * exactly why this half could not be shared.
 *
 * THREADING. od_watchdog itself is lock-free and single-owner; this module is the owner and
 * serialises the callers on a spinlock, because unlike the reference -- one loop task doing
 * everything -- main() feeds here while the display work queue stamps breadcrumbs.
 *
 * od_watchdog_app_service() is still main()'s alone, and that is a rule, not a convention: a
 * feed must come from a site whose execution proves the program is alive. Never call it from a
 * work queue, timer or BLE callback.
 *
 * WHAT IS COVERED, AND WHAT IS NOT. main() is the only feeder, so what trips the watchdog is
 * main() ceasing to run. A wedge confined to the display work queue does NOT trip it -- main()
 * keeps feeding beside it -- and that is why the reference's feeds immediately before each
 * panel library call are deliberately NOT reproduced here: on this target those calls happen on a
 * thread that is not the feeder, so adding feeds there would only widen the blind spot. The
 * breadcrumb phases ARE reproduced, and they are what names the wedge after the fact. Covering
 * the display thread properly needs a second watchdog channel, which the Zephyr API supports
 * and this does not yet use.
 */
#ifndef OD_WATCHDOG_APP_H
#define OD_WATCHDOG_APP_H

#include <stdbool.h>
#include <stdint.h>

/* For enum od_wdt_phase, which od_watchdog_app_phase() takes. */
#include "od_watchdog.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decode the boot, run the strike counter, log what happened. Call once, early in main(),
 * before anything can touch the panel. */
void od_watchdog_app_boot(void);

/* Arm the hardware watchdog and log which of arm/inherit/disabled/unsupported happened. Call
 * once, after od_watchdog_app_boot() and before the boot panel path. */
void od_watchdog_app_arm(void);

/* Prove forward progress. Call from the main loop, and from any cooperative wait that returns. */
void od_watchdog_app_service(void);

/* True when this boot must skip ALL panel work to stay reachable over BLE and DFU.
 *
 * Callers must BRANCH on this, not merely log it: three consecutive watchdog resets mean the
 * panel path is what is wedging, and a fourth attempt at it is another reset. */
bool od_watchdog_app_safe_mode(void);

/* Record the phase about to be entered, so the boot after a watchdog reset can name the wait
 * that wedged. Takes an enum od_wdt_phase value. */
void od_watchdog_app_phase(uint8_t phase);

#ifdef __cplusplus
}
#endif

#endif /* OD_WATCHDOG_APP_H */
