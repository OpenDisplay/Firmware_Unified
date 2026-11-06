/* od_cmd_flash -- image-store management handlers, and the preload arm they set. */
#ifndef OD_CMD_FLASH_H
#define OD_CMD_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#include "od_cmd.h"
#include "od_span.h"

#ifdef __cplusplus
extern "C" {
#endif

od_cmd_result_t od_cmd_app_flash_info(const od_cmd_ctx_t *ctx, od_span_t body);
od_cmd_result_t od_cmd_app_flash_list(const od_cmd_ctx_t *ctx, od_span_t body);
od_cmd_result_t od_cmd_app_flash_erase(const od_cmd_ctx_t *ctx, od_span_t body);
od_cmd_result_t od_cmd_app_flash_preload(const od_cmd_ctx_t *ctx, od_span_t body);
od_cmd_result_t od_cmd_app_flash_display(const od_cmd_ctx_t *ctx, od_span_t body);

/* Consume the preload arm, if one is set. Called by the transfer path at begin: true means
 * this upload is stored in *slot_out instead of being displayed. */
bool od_cmd_flash_take_arm(uint8_t *slot_out);
void od_cmd_flash_clear_arm(void);

/* Implemented in opendisplay_display.cpp: queue a stored slot for display. Returns false if
 * the slot is not VALID or its geometry does not match the live panel. */
bool od_display_from_slot_arm(uint8_t index);

#ifdef __cplusplus
}
#endif
#endif /* OD_CMD_FLASH_H */
