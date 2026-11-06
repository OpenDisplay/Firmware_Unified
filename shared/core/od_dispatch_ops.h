/* od_dispatch_ops.h -- one metadata row per ordinary routed opcode.
 *
 * The dispatcher expands this table into both its route and its response budget. Keeping those
 * two properties in one row makes it impossible to add a multi-reply hook while accidentally
 * leaving the one-unit default budget in place.
 *
 * AUTHENTICATE belongs to od_gate and FIRMWARE_VERSION is the explicit pre-gate discovery path;
 * both remain outside this ordinary-route table and have one-unit assertions in od_dispatch.c.
 */

#ifndef OD_DISPATCH_OPS_H
#define OD_DISPATCH_OPS_H

#include "od_caps.h"

/* Image-store opcodes are EXPERIMENTAL and target-local: the values live in each target's
 * protocol_pending.h, not in shared/protocol/, because an opcode is real when the canonical
 * repo says so. A target without OD_CAP_IMGSTORE expands these to nothing and answers the
 * ordinary unknown-opcode NACK, which is the honest reply from a build with no flash. */
#if OD_CAP_IMGSTORE
/* EXPERIMENTAL OPCODES, DEFINED HERE RATHER THAN IN shared/protocol/.
 *
 * That directory is a byte-for-byte mirror of ../opendisplay-protocol and must not grow values
 * the canonical repo has not assigned. These are not canonical yet: they are implemented and
 * exercised on BG22 first, and a host that does not know them gets the ordinary unknown-opcode
 * NACK. Landing them upstream -- and only then moving them into the mirror -- is required
 * follow-up, not optional cleanup.
 *
 * The targets' own protocol_pending.h is the usual home for this kind of debt, but a value the
 * SHARED dispatch table has to see cannot live in a target header: shared/ never includes one.
 *
 * 0x0047-0x004F is free between CMD_READ_IDENT (0x0046) and the power family at 0x0050. */
#define OD_CMD_FLASH_INFO     0x0047u   /* chip identity + store geometry */
#define OD_CMD_FLASH_LIST     0x0048u   /* per-slot state and CRC-32 digest */
#define OD_CMD_FLASH_ERASE    0x0049u   /* one slot, or all when index is 0xFF */
#define OD_CMD_FLASH_PRELOAD  0x004Au   /* store the NEXT upload instead of displaying it */
#define OD_CMD_FLASH_DISPLAY  0x004Bu   /* display a stored slot */
#define OD_CMD_ACTION_INFO    0x004Cu   /* action table: entries/active/inactive/rejected */
#define OD_CMD_ACTION_FIRE    0x004Du   /* fire OD_TRIG_HOST, for testing a table remotely */

#define OD_DISPATCH_IMGSTORE_ROWS(X)                                                 \
    X(OD_CMD_FLASH_INFO,       od_cmd_app_flash_info,      1u)                       \
    X(OD_CMD_FLASH_LIST,       od_cmd_app_flash_list,      1u)                       \
    X(OD_CMD_FLASH_ERASE,      od_cmd_app_flash_erase,     1u)                       \
    X(OD_CMD_FLASH_PRELOAD,    od_cmd_app_flash_preload,   1u)                       \
    X(OD_CMD_FLASH_DISPLAY,    od_cmd_app_flash_display,   1u)                       \
    X(OD_CMD_ACTION_INFO,      od_cmd_app_action_info,     1u)                       \
    X(OD_CMD_ACTION_FIRE,      od_cmd_app_action_fire,     1u)
#else
#define OD_DISPATCH_IMGSTORE_ROWS(X)
#endif

/* CMD_DRIVE_WRITE_START is CANONICAL, unlike the experimental block above: it is specified in
 * shared/protocol/opendisplay_protocol.h at 2.3 with its own OD_ERR_DRIVE_* namespace. Only the
 * routing is gated, because a target whose display path cannot run a config-declared mode must
 * answer unknown-opcode rather than accept and fail. */
#if OD_CAP_DRIVE_WRITE
#define OD_DISPATCH_DRIVE_ROWS(X)                                                    \
    X(CMD_DRIVE_WRITE_START,   od_cmd_app_drive_write_start, 1u)
#else
#define OD_DISPATCH_DRIVE_ROWS(X)
#endif

#define OD_DISPATCH_OPCODE_ROWS(X)                                                   \
    X(CMD_REBOOT,              od_cmd_app_reboot,          1u)                       \
    X(CMD_CONFIG_READ,         od_cmd_app_config_read,     1u)                       \
    X(CMD_CONFIG_WRITE,        od_cmd_app_config_write,    1u)                       \
    X(CMD_CONFIG_CHUNK,        od_cmd_app_config_chunk,    1u)                       \
    X(CMD_READ_MSD,            od_cmd_app_read_msd,        1u)                       \
    X(CMD_CONFIG_CLEAR,        od_cmd_app_config_clear,    1u)                       \
    X(CMD_ENTER_DFU,           od_cmd_app_enter_dfu,       1u)                       \
    X(CMD_POWER_OFF,           od_cmd_app_power_off,       1u)                       \
    X(CMD_DEEP_SLEEP,          od_cmd_app_deep_sleep,      1u)                       \
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
    OD_DISPATCH_DRIVE_ROWS(X)                                                        \
    OD_DISPATCH_IMGSTORE_ROWS(X)

#endif /* OD_DISPATCH_OPS_H */
