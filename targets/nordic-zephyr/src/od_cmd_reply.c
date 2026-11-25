/* od_cmd_reply.c -- see od_cmd_reply.h. */

#include "od_cmd_reply.h"

#include "od_reply.h"

#include "od_watchdog_app.h"

#include <zephyr/kernel.h>

#include <stddef.h>

od_txq_status_t od_cmd_reply(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
  if (ctx == NULL || frame == NULL || len == 0u) {
    return OD_TXQ_INVARIANT;
  }
  return od_reply(ctx->r, &ctx->rp, frame, len);
}

od_txq_status_t od_cmd_reply_plain(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
  if (ctx == NULL || frame == NULL || len == 0u) {
    return OD_TXQ_INVARIANT;
  }
  return od_reply_plain(ctx->r, &ctx->rp, frame, len);
}

od_txq_status_t od_cmd_reply_refusal(const od_cmd_ctx_t *ctx,
                                     const uint8_t *frame, uint16_t len)
{
  if (ctx == NULL || frame == NULL || len == 0u) {
    return OD_TXQ_INVARIANT;
  }
  return od_reply_refusal(ctx->r, &ctx->rp, frame, len);
}

/* Half the host's 500 ms tail-flush read -- see od_cmd_reply.h. */
#define OD_CMD_REFRESH_BARRIER_MS 250u
#define OD_CMD_REFRESH_BARRIER_POLL_MS 5u

void od_cmd_flush_before_refresh(void)
{
  /* Wrap is fine: od_txq_flush() compares (int32_t)(now - deadline), so a deadline past
   * UINT32_MAX stays correct as long as both stamps come from this one clock. */
  const uint32_t deadline = k_uptime_get_32() + OD_CMD_REFRESH_BARRIER_MS;

  for (;;) {
    if (od_txq_flush(k_uptime_get_32(), deadline) != OD_TXQ_BUSY) {
      return;                       /* OK: drained. TIMEOUT: left queued, deliberately. */
    }
    /* The refresh that follows can hold this thread for a minute, but the barrier itself must not
     * look like a wedge to the watchdog either -- same thread, same timer. */
    od_watchdog_app_service();
    k_msleep(OD_CMD_REFRESH_BARRIER_POLL_MS);
  }
}
