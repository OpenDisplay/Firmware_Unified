/* od_cmd_reply.cpp -- see od_cmd_reply.h. */

#include "od_cmd_reply.h"

#include "od_hal_time.h"
#include "od_hal_sleep.h"
#include "od_reply.h"
#include "od_watchdog_app.h"

extern "C" od_txq_status_t od_cmd_reply(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    if (ctx == nullptr || frame == nullptr || len == 0u) {
        return OD_TXQ_INVARIANT;
    }
    return od_reply(ctx->r, &ctx->rp, frame, len);
}

extern "C" od_txq_status_t od_cmd_reply_plain(const od_cmd_ctx_t *ctx,
                                              const uint8_t *frame, uint16_t len)
{
    if (ctx == nullptr || frame == nullptr || len == 0u) {
        return OD_TXQ_INVARIANT;
    }
    return od_reply_plain(ctx->r, &ctx->rp, frame, len);
}

extern "C" od_txq_status_t od_cmd_reply_refusal(const od_cmd_ctx_t *ctx,
                                                const uint8_t *frame, uint16_t len)
{
    if (ctx == nullptr || frame == nullptr || len == 0u) {
        return OD_TXQ_INVARIANT;
    }
    return od_reply_refusal(ctx->r, &ctx->rp, frame, len);
}

/* Half the host's 500 ms tail-flush read -- see od_cmd_reply.h. */
#define OD_CMD_REFRESH_BARRIER_MS 250u

/* Long enough not to spin the CPU while the controller drains, short enough that the barrier is
 * not quantised to a few coarse attempts: the queue is at most 3 deep and one notification goes
 * out per connection interval. */
#define OD_CMD_REFRESH_BARRIER_POLL_MS 5u

extern "C" void od_cmd_flush_before_refresh(void)
{
    /* Wrap is fine: od_txq_flush() compares (int32_t)(now - deadline), so a deadline that wraps
     * past UINT32_MAX stays correct as long as both stamps come from this one clock. */
    const uint32_t deadline = od_hal_uptime_ms() + OD_CMD_REFRESH_BARRIER_MS;

    for (;;) {
        if (od_txq_flush(od_hal_uptime_ms(), deadline) != OD_TXQ_BUSY) {
            return;                       /* OK: drained. TIMEOUT: left queued, deliberately. */
        }
        /* The refresh that follows may hold this task for minutes, but the barrier itself must not
         * look like a wedge to the watchdog either -- it is the same task and the same timer. */
        od_watchdog_app_service();
        od_hal_delay_ms(OD_CMD_REFRESH_BARRIER_POLL_MS);
    }
}
