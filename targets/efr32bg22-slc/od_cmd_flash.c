/* od_cmd_flash -- image-store management over BLE.
 *
 * Opcodes 0x0047-0x004D are canonical at protocol 2.4.
 *
 * PRELOAD ARMS THE EXISTING UPLOAD PATH rather than adding a parallel one. Chunking,
 * compression, inflate, acknowledgement and abort are identical whether the bytes end at the
 * panel or at the flash; a second copy of that machinery would be a second place for transfer
 * bugs to live, and only one of the two copies would get exercised.
 */
#include "od_cmd_flash.h"

#include <string.h>

#include "od_action.h"
#include "od_imgstore.h"
#include "od_log.h"
#include "od_reply.h"
#include "opendisplay_runtime.h"
#include "od_dispatch_ops.h"


/* The target's own reply shape: od_reply takes the reservation and the reply descriptor out
 * of the context, as od_cmd_silabs.c does. */
static od_txq_status_t reply(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    return od_reply(ctx->r, &ctx->rp, frame, len);
}

static bool s_armed;
static uint8_t s_armed_slot;

bool od_cmd_flash_take_arm(uint8_t *slot_out)
{
    if (!s_armed) {
        return false;
    }
    /* Consumed, not sticky. An arm that survived its upload would silently divert the NEXT
     * image away from the panel, which looks like a display that stopped working. */
    s_armed = false;
    if (slot_out != NULL) {
        *slot_out = s_armed_slot;
    }
    return true;
}

void od_cmd_flash_clear_arm(void) { s_armed = false; }

static od_cmd_result_t nack(const od_cmd_ctx_t *ctx, uint8_t op)
{
    uint8_t r[2] = { RESP_NACK, (uint8_t)op };
    (void)reply(ctx, r, sizeof r);
    return OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_flash_info(const od_cmd_ctx_t *ctx, od_span_t body)
{
    const od_imgstore_info_t *st = od_imgstore_status();
    uint8_t r[12];

    (void)body;
    memset(r, 0, sizeof r);
    r[0] = RESP_ACK;
    r[1] = (uint8_t)CMD_FLASH_INFO;
    /* Geometry only while mounted: after an unmount, or a mount that failed part-way, the
     * status block still carries stride/capacity, and a host would size uploads against a
     * store that is not there. */
    if (st->mounted) {
        r[2] = 1u;
        r[3] = st->slot_count;
        r[4] = (uint8_t)st->slot_stride;
        r[5] = (uint8_t)(st->slot_stride >> 8);
        r[6] = (uint8_t)(st->slot_stride >> 16);
        r[7] = (uint8_t)(st->slot_stride >> 24);
        r[8] = (uint8_t)st->capacity;
        r[9] = (uint8_t)(st->capacity >> 8);
        r[10] = (uint8_t)(st->capacity >> 16);
        r[11] = (uint8_t)(st->capacity >> 24);
    }
    return reply(ctx, r, sizeof r) == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_flash_list(const od_cmd_ctx_t *ctx, od_span_t body)
{
    const od_imgstore_info_t *st = od_imgstore_status();
    /* 3 header bytes + 6 per slot. Bounded by OD_IMGSTORE_MAX_SLOTS so the frame cannot grow
     * with the part size. */
    uint8_t r[3u + (OD_IMGSTORE_MAX_SLOTS * 6u)];
    uint8_t n = 0u;

    (void)body;
    if (!st->mounted) {
        return nack(ctx, (uint8_t)CMD_FLASH_LIST);
    }
    r[0] = RESP_ACK;
    r[1] = (uint8_t)CMD_FLASH_LIST;

    for (uint8_t i = 0; i < st->slot_count && i < OD_IMGSTORE_MAX_SLOTS; i++) {
        od_imgstore_slot_t s;
        if (!od_imgstore_slot(i, &s)) {
            break;
        }
        uint8_t *e = &r[3u + ((uint32_t)n * 6u)];
        e[0] = i;
        e[1] = s.state;
        e[2] = (uint8_t)s.digest;
        e[3] = (uint8_t)(s.digest >> 8);
        e[4] = (uint8_t)(s.digest >> 16);
        e[5] = (uint8_t)(s.digest >> 24);
        n++;
    }
    r[2] = n;
    return reply(ctx, r, (uint16_t)(3u + ((uint32_t)n * 6u))) == OD_TXQ_OK
           ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_flash_erase(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t r[3];
    bool ok;

    if (!od_span_valid(body) || body.n < 1u) {
        return nack(ctx, (uint8_t)CMD_FLASH_ERASE);
    }
    const uint8_t idx = body.p[0];
    ok = (idx == 0xFFu) ? od_imgstore_erase_all() : od_imgstore_erase(idx);

    r[0] = ok ? RESP_ACK : RESP_NACK;
    r[1] = (uint8_t)CMD_FLASH_ERASE;
    r[2] = idx;
    (void)reply(ctx, r, sizeof r);
    return ok ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_flash_preload(const od_cmd_ctx_t *ctx, od_span_t body)
{
    const od_imgstore_info_t *st = od_imgstore_status();
    uint8_t r[3];

    if (!od_span_valid(body) || body.n < 1u || !st->mounted) {
        return nack(ctx, (uint8_t)CMD_FLASH_PRELOAD);
    }
    const uint8_t idx = body.p[0];
    if (idx >= st->slot_count) {
        return nack(ctx, (uint8_t)CMD_FLASH_PRELOAD);
    }
    s_armed = true;
    s_armed_slot = idx;
    od_log_info("flash: next upload -> slot %u (not displayed)", (unsigned)idx);

    r[0] = RESP_ACK;
    r[1] = (uint8_t)CMD_FLASH_PRELOAD;
    r[2] = idx;
    return reply(ctx, r, sizeof r) == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_flash_display(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t r[3];
    bool ok;

    if (!od_span_valid(body) || body.n < 1u) {
        return nack(ctx, (uint8_t)CMD_FLASH_DISPLAY);
    }
    const uint8_t idx = body.p[0];

    /* ACK BEFORE THE REFRESH. A full refresh is 10-20 s on this panel, far past the host's
     * command timeout, so holding the reply until the panel settles would report a successful
     * display as a timeout -- the same defect the config-write path already hit once. */
    ok = od_display_from_slot_arm(idx);
    r[0] = ok ? RESP_ACK : RESP_NACK;
    r[1] = (uint8_t)CMD_FLASH_DISPLAY;
    r[2] = idx;
    (void)reply(ctx, r, sizeof r);
    return ok ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_action_info(const od_cmd_ctx_t *ctx, od_span_t body)
{
    const od_action_stats_t *st = od_action_stats();
    /* r[9] `tables` extends the 9-byte frame of protocol 2.4: the number of 0xB2 packets in
     * the config, of which only the first runs. Appended so a reader of the 9-byte form is
     * unaffected; the canonical ActionInfoResponse does not carry it yet. */
    uint8_t r[10];

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
    return reply(ctx, r, sizeof r) == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_action_fire(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t r[3];
    const uint8_t arg = (od_span_valid(body) && body.n >= 1u) ? body.p[0] : 0u;

    /* OD_TRIG_HOST exists so a table can be exercised without the physical event
     * it is really for. The runner picks it up on the next service pass -- firing
     * it here would run effects inside a command handler. */
    od_action_trigger(OD_TRIG_HOST, arg);
    r[0] = RESP_ACK;
    r[1] = (uint8_t)CMD_ACTION_FIRE;
    r[2] = arg;
    return reply(ctx, r, sizeof r) == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}
