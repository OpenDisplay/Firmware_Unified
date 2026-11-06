/* od_cmd_app.h -- the per-command target seam. One link-time function per canonical opcode.
 *
 * MIGRATION SEAM, and named as one. The opcode map itself is shared (od_dispatch.c): a target
 * supplies the BEHAVIOUR of a command, never the routing of it. That split is the point -- two
 * targets each owning a copy of the switch is how one of them silently answers an opcode the other
 * treats as unknown, and "unknown" is a wire-visible property (od_frame_policy gives it no
 * activity stamp, so an unrecognised opcode cannot hold an exclusive link open).
 *
 * AUTHENTICATE (0x0050) is deliberately absent: od_gate owns the whole handshake and no target
 * sees it.
 *
 * EVERY TARGET IMPLEMENTS EVERY HOOK DECLARED HERE. A capability a target lacks still gets a
 * definition, which returns OD_CMD_UNKNOWN and answers nothing -- unless the protocol defines an
 * explicit unsupported response. Promoted subsystems leave this surface and route directly to
 * their shared state machine; capability policy then lives with that machine.
 *
 * `body` is PLAINTEXT: the frame has already met the session gate, so an encrypted frame arrives
 * decrypted and a plaintext one arrives as sent. A handler must complete -- dispatch has already
 * resolved capacity and producer conflicts, so there is nothing left to defer on -- and spends
 * response units from ctx->r via od_reply().
 *
 * STATIC LINK-TIME COMPOSITION. No registry, no vtable, no constructor, no heap.
 */

#ifndef OD_CMD_APP_H
#define OD_CMD_APP_H

#include "od_caps.h"
#include "od_cmd.h"
#include "od_span.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- device and lifecycle ------------------------------------------------------------------ */

od_cmd_result_t od_cmd_app_reboot(const od_cmd_ctx_t *ctx, od_span_t body);            /* 0x000F */
/* Exempt from the session gate on every target, and answered as DISCOVERY rather than ACCEPTED --
 * a client must be able to learn what it is talking to before it can authenticate. */
od_cmd_result_t od_cmd_app_firmware_version(const od_cmd_ctx_t *ctx, od_span_t body);  /* 0x0043 */
od_cmd_result_t od_cmd_app_read_msd(const od_cmd_ctx_t *ctx, od_span_t body);          /* 0x0044 */
od_cmd_result_t od_cmd_app_enter_dfu(const od_cmd_ctx_t *ctx, od_span_t body);         /* 0x0051 */
od_cmd_result_t od_cmd_app_power_off(const od_cmd_ctx_t *ctx, od_span_t body);         /* 0x0052 */
od_cmd_result_t od_cmd_app_deep_sleep(const od_cmd_ctx_t *ctx, od_span_t body);        /* 0x0053 */

/* --- configuration ------------------------------------------------------------------------- */

od_cmd_result_t od_cmd_app_config_read(const od_cmd_ctx_t *ctx, od_span_t body);       /* 0x0040 */
od_cmd_result_t od_cmd_app_config_write(const od_cmd_ctx_t *ctx, od_span_t body);      /* 0x0041 */
od_cmd_result_t od_cmd_app_config_chunk(const od_cmd_ctx_t *ctx, od_span_t body);      /* 0x0042 */
od_cmd_result_t od_cmd_app_config_clear(const od_cmd_ctx_t *ctx, od_span_t body);      /* 0x0045 */

/* --- peripherals --------------------------------------------------------------------------- */

od_cmd_result_t od_cmd_app_led_activate(const od_cmd_ctx_t *ctx, od_span_t body);      /* 0x0073 */
od_cmd_result_t od_cmd_app_led_stop(const od_cmd_ctx_t *ctx, od_span_t body);          /* 0x0075 */
od_cmd_result_t od_cmd_app_buzzer(const od_cmd_ctx_t *ctx, od_span_t body);            /* 0x0077 */

/* --- image store and actions, EXPERIMENTAL -------------------------------------------------- */

/* Declared here rather than beside the target that implements them, because od_dispatch_ops.h
 * names them from SHARED code and shared/ never includes a target header. Gated on the same
 * capability as the dispatch rows, so only a build that routes these opcodes owes definitions.
 * The values, and why they are not yet in shared/protocol/, are in od_dispatch_ops.h. */
#if OD_CAP_IMGSTORE
od_cmd_result_t od_cmd_app_flash_info(const od_cmd_ctx_t *ctx, od_span_t body);        /* 0x0047 */
od_cmd_result_t od_cmd_app_flash_list(const od_cmd_ctx_t *ctx, od_span_t body);        /* 0x0048 */
od_cmd_result_t od_cmd_app_flash_erase(const od_cmd_ctx_t *ctx, od_span_t body);       /* 0x0049 */
od_cmd_result_t od_cmd_app_flash_preload(const od_cmd_ctx_t *ctx, od_span_t body);     /* 0x004A */
od_cmd_result_t od_cmd_app_flash_display(const od_cmd_ctx_t *ctx, od_span_t body);     /* 0x004B */
od_cmd_result_t od_cmd_app_action_info(const od_cmd_ctx_t *ctx, od_span_t body);       /* 0x004C */
od_cmd_result_t od_cmd_app_action_fire(const od_cmd_ctx_t *ctx, od_span_t body);       /* 0x004D */
#endif

#ifdef __cplusplus
}
#endif

#endif /* OD_CMD_APP_H */
