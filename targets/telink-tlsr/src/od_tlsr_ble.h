/* BLE on the Telink stack: advertising, the GATT table, the OpenDisplay pipe and the updater
 * characteristic. Stack callbacks only copy and flag; od_tlsr_ble_loop() acts (dispatch,
 * od_core_reset, the updater), because those are consumer-context operations. */
#ifndef OD_TLSR_BLE_H
#define OD_TLSR_BLE_H

#include <stdbool.h>
#include <stdint.h>

void od_tlsr_ble_init(const uint8_t mac[6]);
void od_tlsr_ble_loop(uint32_t now_ms);
/* main() after a deep-retention wake, in place of od_tlsr_ble_init(). */
void od_tlsr_ble_retention_wake(void);
/* The BLE side has nothing queued or in progress. */
bool od_tlsr_ble_idle(void);
/* Once per loop pass: allow suspend between radio events only while both the BLE side and the
 * rest of the app (app_idle) have nothing to do. */
void od_tlsr_ble_power(bool app_idle);

/* Rebuild the MSD and the advertising payload (config change, state change). */
void od_tlsr_ble_refresh_adv(void);
void od_tlsr_ble_copy_msd(uint8_t out[16]);

/* Close the link if `tag` still owns it. */
void od_tlsr_ble_close(uint32_t tag);

#endif /* OD_TLSR_BLE_H */
