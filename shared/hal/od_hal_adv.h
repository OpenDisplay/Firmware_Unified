/* od_hal_adv.h -- the advertising HAL every target implements.
 *
 * Three link-time C functions, not a vtable: shared/ is plain C and binds to the target
 * implementation at link time (CLAUDE.md § "The one rule"). The single deliberate
 * function-pointer exception in this tree is the panel bus (od_epd_cfg_bus_t / od_epd_io_t),
 * and this is not it.
 *
 * THE DIVISION OF LABOUR. The target owns AD-record packing and the stack calls, because
 * PDU layout and update APIs differ across NimBLE, Bluefruit, Zephyr and BGAPI. The shared
 * controller (od_adv_control.h) owns *when* those calls happen. Nothing here decides policy.
 *
 * CONTEXT. Every one of these is called from the application loop, never from a stack
 * callback. That is the whole point of F4: callbacks publish facts, the loop acts on them.
 */
#ifndef OD_HAL_ADV_H
#define OD_HAL_ADV_H

#include <stdint.h>

/* shared/ is plain C, but not every target is: the ESP32 implements this HAL inside a C++
 * translation unit (ble/od_ble_nimble.cpp), because that is where the NimBLE state it needs
 * lives. Without this guard those definitions get C++ linkage and fail to match the C caller
 * in shared/core. The host tests are C-only and structurally cannot catch it. */
#ifdef __cplusplus
extern "C" {
#endif

/* Result of one HAL operation.
 *
 * ALREADY and NOT_ACTIVE are IDEMPOTENT SUCCESS, not faults -- a stop on something already
 * stopped got the outcome it wanted. Treating them as errors is how a controller and a stack
 * end up disagreeing about state after a race, which is the class of bug this design exists
 * to remove.
 *
 * RETRY is temporary backpressure. It must leave controller state UNCHANGED so the next pass
 * tries again; a RETRY that advanced state would be a state lie.
 *
 * ERROR is a hard failure, surfaced to the application and latched so a hot loop cannot flood
 * the log with the same failure every pass.
 */
enum od_hal_adv_result {
    OD_HAL_ADV_OK = 0,
    OD_HAL_ADV_ALREADY,     /* start: already advertising */
    OD_HAL_ADV_NOT_ACTIVE,  /* stop: was not advertising */
    OD_HAL_ADV_RETRY,       /* temporary; state unchanged, try next pass */
    OD_HAL_ADV_ERROR
};

/* Install a COMPLETE, IMMUTABLE advertisement payload.
 *
 * msd is the canonical 16-byte MsdAdvertisement body (the bytes after the company id). The
 * target builds both the advertising and scan-response records from it using target-local
 * rules -- on ESP32 today that means flags+name+MSD in ADV and the 128-bit service UUID in
 * the scan response.
 *
 * The controller calls this ONLY while it believes advertising is inactive, so an
 * implementation never has to handle a live-update case it cannot express.
 */
enum od_hal_adv_result od_hal_adv_program(const uint8_t msd[16]);

/* Begin advertising with the last programmed payload. */
enum od_hal_adv_result od_hal_adv_start(void);

/* Stop advertising.
 *
 * A successful stop means the target accepts that it is SAFE TO PROGRAM before any delayed
 * completion callback is delivered. A target that cannot promise that must return RETRY until
 * it can.
 */
enum od_hal_adv_result od_hal_adv_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_HAL_ADV_H */
