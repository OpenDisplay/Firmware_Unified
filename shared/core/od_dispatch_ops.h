/* od_dispatch_ops.h -- one metadata row per ordinary routed opcode.
 *
 * The dispatcher expands this table into both its route and its response budget. Keeping those
 * two properties in one row makes it impossible to add a multi-reply hook while accidentally
 * leaving the one-unit default budget in place.
 *
 * The plaintext set (FIRMWARE_VERSION, READ_MSD, READ_IDENT, AUTHENTICATE, SEC_STATUS, SEC_CLAIM,
 * SEC_AUTH) is answered before the gate and stays outside this table, at one unit each.
 */

#ifndef OD_DISPATCH_OPS_H
#define OD_DISPATCH_OPS_H

#include "od_caps.h"

/* Image-store and action opcodes: canonical at protocol 2.4 (CMD_FLASH_* / CMD_ACTION_* in
 * shared/protocol/opendisplay_protocol.h). The CMD_FLASH_* rows need an image store and the
 * CMD_ACTION_* rows the action runner; a target without OD_CAP_IMGSTORE / OD_CAP_ACTION leaves
 * them unrouted, which is SILENCE rather than a NACK (od_dispatch's OD_FRAME_UNKNOWN_OPCODE logs
 * and emits nothing). */
#if OD_CAP_IMGSTORE
#define OD_DISPATCH_IMGSTORE_ROWS(X)                                                 \
    X(CMD_FLASH_INFO,       od_cmd_app_flash_info,      1u)                       \
    X(CMD_FLASH_LIST,       od_cmd_app_flash_list,      1u)                       \
    X(CMD_FLASH_ERASE,      od_cmd_app_flash_erase,     1u)                       \
    X(CMD_FLASH_PRELOAD,    od_cmd_app_flash_preload,   1u)                       \
    X(CMD_FLASH_DISPLAY,    od_cmd_app_flash_display,   1u)
#else
#define OD_DISPATCH_IMGSTORE_ROWS(X)
#endif

#if OD_CAP_ACTION
#define OD_DISPATCH_ACTION_ROWS(X)                                                   \
    X(CMD_ACTION_INFO,      od_cmd_app_action_info,     1u)                       \
    X(CMD_ACTION_FIRE,      od_cmd_app_action_fire,     1u)
#else
#define OD_DISPATCH_ACTION_ROWS(X)
#endif

/* CMD_DRIVE_WRITE_START is specified in shared/protocol/opendisplay_protocol.h at 2.3 with its own OD_ERR_DRIVE_* namespace. Only the
 * routing is gated, because a target whose display path cannot run a config-declared mode must
 * answer unknown-opcode rather than accept and fail. */
#if OD_CAP_DRIVE_WRITE
#define OD_DISPATCH_DRIVE_ROWS(X)                                                    \
    X(CMD_DRIVE_WRITE_START,   od_cmd_app_drive_write_start, 1u)
#else
#define OD_DISPATCH_DRIVE_ROWS(X)
#endif

/* CMD_SPI_RAW is canonical at 2.3 and Silabs-only by specification; routed here on every target. Its DATA_NOACK sub-opcode
 * answers nothing at all, so the row's budget covers the replying sub-opcodes; a handler that
 * sends no response simply does not spend its unit. */
#if OD_CAP_SPI_RAW
#define OD_DISPATCH_SPI_RAW_ROWS(X)                                                  \
    X(CMD_SPI_RAW,             od_cmd_app_spi_raw,          1u)
#else
#define OD_DISPATCH_SPI_RAW_ROWS(X)
#endif

#define OD_DISPATCH_OPCODE_ROWS(X)                                                   \
    X(CMD_REBOOT,              od_cmd_app_reboot,          1u)                       \
    X(CMD_CONFIG_READ,         od_cmd_app_config_read,     1u)                       \
    X(CMD_CONFIG_WRITE,        od_cmd_app_config_write,    1u)                       \
    X(CMD_CONFIG_CHUNK,        od_cmd_app_config_chunk,    1u)                       \
    X(CMD_CONFIG_CLEAR,        od_cmd_app_config_clear,    1u)                       \
    X(CMD_ENTER_DFU,           od_cmd_app_enter_dfu,       1u)                       \
    X(CMD_POWER_OFF,           od_cmd_app_power_off,       1u)                       \
    X(CMD_DEEP_SLEEP,          od_cmd_app_deep_sleep,      1u)                       \
    X(CMD_SEC_KEY,             od_sec2_key_cmd,            1u)                       \
    X(CMD_DIRECT_WRITE_START,  od_xfer_direct_start,       1u)                       \
    X(CMD_DIRECT_WRITE_DATA,   od_xfer_data,               2u)                       \
    X(CMD_DIRECT_WRITE_END,    od_xfer_end,                2u)                       \
    X(CMD_LED_ACTIVATE,        od_cmd_app_led_activate,    1u)                       \
    X(CMD_LED_STOP,            od_cmd_app_led_stop,        1u)                       \
    X(CMD_PARTIAL_WRITE_START, od_xfer_partial_start,      1u)                       \
    X(CMD_BUZZER,              od_cmd_app_buzzer,          1u)                       \
    X(CMD_PIPE_WRITE_START,    od_pipe_start,              1u)                       \
    X(CMD_PIPE_WRITE_DATA,     od_pipe_data,               (OD_CAP_PIPE ? 3u : 1u))  \
    X(CMD_PIPE_WRITE_END,      od_pipe_end,                (OD_CAP_PIPE ? 3u : 1u))  \
    X(CMD_NFC_ENDPOINT,        od_nfc_frame,               1u)                       \
    OD_DISPATCH_SPI_RAW_ROWS(X)                                                      \
    OD_DISPATCH_DRIVE_ROWS(X)                                                        \
    OD_DISPATCH_IMGSTORE_ROWS(X)                                                     \
    OD_DISPATCH_ACTION_ROWS(X)

#endif /* OD_DISPATCH_OPS_H */
