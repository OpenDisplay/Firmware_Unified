/* Image store on the XIAO nRF52840 module's flash: wake/sleep around each use, and the preload arm. */
#ifndef OD_CMD_FLASH_H
#define OD_CMD_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mount for the configured panel, then put the flash back into deep power-down. */
void od_flash_store_mount(void);

/* Consume the preload arm: true means this upload is stored in *slot_out, not displayed. */
bool od_cmd_flash_take_arm(uint8_t *slot_out);
void od_cmd_flash_clear_arm(void);

/* The flash is awake from a successful begin until finish or abort. */
bool od_flash_store_begin(uint8_t slot, uint16_t w, uint16_t h, uint8_t scheme, uint32_t len);
bool od_flash_store_finish(void);
void od_flash_store_abort(void);

void od_flash_store_wake(void);
void od_flash_store_sleep(void);

/* Implemented in opendisplay_display.c; opendisplay_display_service() runs it from the loop, so
 * the ACK leaves before the refresh. */
bool od_display_from_slot_arm(uint8_t index);

#ifdef __cplusplus
}
#endif

#endif /* OD_CMD_FLASH_H */
