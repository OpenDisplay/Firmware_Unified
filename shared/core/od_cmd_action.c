/* od_cmd_action -- CMD_ACTION_INFO (0x004C) and CMD_ACTION_FIRE (0x004D) over od_action.
 *
 * Both handlers are pure functions of od_action's state, so every target that runs the action
 * table answers them the same way. Routed when OD_CAP_ACTION is set (od_dispatch_ops.h).
 */
#include "od_caps.h"

#if OD_CAP_ACTION

#include "od_action.h"
#include "od_cmd_app.h"
#include "od_reply.h"
#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"

/* [ACK][0x4C] + ActionInfoResponse. */
_Static_assert(sizeof(struct ActionInfoResponse) == 8u, "ACTION_INFO data is 8 bytes");

od_cmd_result_t od_cmd_app_action_info(const od_cmd_ctx_t *ctx, od_span_t body)
{
    const od_action_stats_t *st = od_action_stats();
    uint8_t r[2u + sizeof(struct ActionInfoResponse)];

    (void)body;
    r[0] = RESP_ACK;
    r[1] = (uint8_t)CMD_ACTION_INFO;
    r[2] = st->entries;
    r[3] = st->active;
    r[4] = st->inactive;
    r[5] = st->rejected;
    r[6] = st->last_trigger;
    r[7] = st->fire_count;
    r[8] = (uint8_t)st->last_temp_c;
    r[9] = st->tables;
    return od_reply(ctx->r, &ctx->rp, r, sizeof r) == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_action_fire(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t r[3];
    const uint8_t arg = (od_span_valid(body) && body.n >= 1u) ? body.p[0] : 0u;

    /* Recorded, not run: the runner starts the entry on its next service pass, so no effect
     * executes inside a command handler. */
    od_action_trigger(OD_TRIG_HOST, arg);
    r[0] = RESP_ACK;
    r[1] = (uint8_t)CMD_ACTION_FIRE;
    r[2] = arg;
    return od_reply(ctx->r, &ctx->rp, r, sizeof r) == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

#endif /* OD_CAP_ACTION */
