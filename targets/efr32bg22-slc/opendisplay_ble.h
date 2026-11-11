#ifndef OPENDISPLAY_BLE_H
#define OPENDISPLAY_BLE_H

#include "sl_bt_api.h"
#include "opendisplay_runtime.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void opendisplay_ble_on_boot(uint8_t advertising_set_handle);

const struct GlobalConfig *opendisplay_get_global_config(void);

void opendisplay_ble_reload_config_from_nvm(void);

/* The stored config was rewritten by the SEC2 policy: reload it at the next loop pass (or, at
 * boot, right after od_security_boot). */
void opendisplay_ble_config_changed(void);

/* Erase every image-store slot, mounting the store first if needed (factory reset R5a). */
bool opendisplay_ble_erase_images(void);

void opendisplay_ble_on_event(sl_bt_msg_t *evt);
void opendisplay_ble_process(void);
void opendisplay_ble_schedule_dfu(void);
void opendisplay_ble_schedule_deep_sleep(void);

uint16_t opendisplay_ble_get_app_version(void);
uint8_t opendisplay_ble_get_app_version_patch(void);

void opendisplay_ble_copy_msd_bytes(uint8_t out[16]);

/* Tag I/O is shared/core/od_nfc_app.h's, implemented in opendisplay_ble.c. */

#ifdef __cplusplus
}
#endif

#endif
