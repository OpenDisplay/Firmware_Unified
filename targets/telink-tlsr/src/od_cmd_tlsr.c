/* TLSR825x implementations of every shared command hook. The config path, reply-sealing rules and
 * unauthenticated-mutation policy are BG22's (targets/efr32bg22-slc/od_cmd_silabs.c); what differs
 * is what this target cannot do yet -- buzzer, power-off, deep sleep -- and each of
 * those answers an explicit NACK rather than a false ACK. */

#include "od_cmd_app.h"

#include "od_config_read.h"
#include "od_dispatch.h"
#include "od_led_tlsr.h"
#include "od_reply.h"
#include "od_session.h"
#include "od_session_app.h"
#include "od_tlsr.h"
#include "od_txq.h"
#include "opendisplay_protocol.h"
#include "tlsr_port.h"

#include <string.h>

static bool authenticated(void)
{
    return od_session_authenticated(od_session_app_state());
}

static od_txq_status_t reply(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    return od_reply(ctx->r, &ctx->rp, frame, len);
}

static od_txq_status_t reply_plain(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    return od_reply_plain(ctx->r, &ctx->rp, frame, len);
}

static od_txq_status_t config_ack(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    return authenticated() ? reply(ctx, frame, len) : reply_plain(ctx, frame, len);
}

static od_cmd_result_t nack(const od_cmd_ctx_t *ctx, uint8_t response, uint8_t code)
{
    uint8_t err[] = { RESP_NACK, response, code, 0u };
    (void)reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
}

static od_cmd_result_t persist_config(const od_cmd_ctx_t *ctx, uint8_t response,
                                      const uint8_t *data, uint32_t len)
{
    uint8_t ok[] = { RESP_ACK, response, 0u, 0u };
    od_txq_status_t qrc;

    /* Persistence is the commit point: a success response never gets ahead of it. */
    if (!od_tlsr_config_save(data, len)) {
        return nack(ctx, response, 0u);
    }
    /* Seal under the old key while it is still live, then load the new config and retire it. */
    qrc = config_ack(ctx, ok, sizeof(ok));
    od_tlsr_config_reload();
    od_session_clear(od_session_app_state());
    return qrc == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_firmware_version(const od_cmd_ctx_t *ctx, od_span_t body)
{
    static const char build[] = OD_TLSR_BUILD_ID;
    uint8_t rsp[46];
    uint8_t n = (uint8_t)(sizeof(build) - 1u);

    (void)body;
    if (n > 40u) {
        n = 40u;
    }
    rsp[0] = RESP_ACK;
    rsp[1] = RESP_FIRMWARE_VERSION;
    rsp[2] = (uint8_t)OD_TLSR_VERSION_MAJOR;
    rsp[3] = (uint8_t)OD_TLSR_VERSION_MINOR;
    rsp[4] = n;
    memcpy(&rsp[5], build, n);
    rsp[5u + n] = (uint8_t)OD_TLSR_VERSION_PATCH;
    (void)reply_plain(ctx, rsp, (uint16_t)(6u + n));
    return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_read_msd(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t rsp[18] = { RESP_ACK, RESP_MSD_READ };

    (void)body;
    od_tlsr_copy_msd(&rsp[2]);
    (void)reply(ctx, rsp, sizeof(rsp));
    return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_reboot(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)ctx;
    (void)body;
    tlsr_port_reboot();
    return OD_CMD_OK;
}

/* No separate bootloader: ENTER_DFU arms the Telink OTA service for this connection, and the
 * image is then streamed there (tools/ble_ota.py). Reaching this handler already passed the
 * command gate, so with a key configured only an authenticated central can arm it. */
od_cmd_result_t od_cmd_app_enter_dfu(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t ok[] = { RESP_ACK, RESP_ENTER_DFU };

    (void)body;
    tlsr_port_ota_arm(true);
    (void)reply(ctx, ok, sizeof(ok));
    return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_power_off(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return nack(ctx, RESP_POWER_OFF, OD_ERR_POWER_OFF_UNSUPPORTED);
}

od_cmd_result_t od_cmd_app_deep_sleep(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return nack(ctx, RESP_DEEP_SLEEP, OD_ERR_DEEP_SLEEP_UNSUPPORTED);
}

od_cmd_result_t od_cmd_app_config_read(const od_cmd_ctx_t *ctx, od_span_t body)
{
    struct od_config_asm *s = od_tlsr_config_assembler();
    uint32_t len = OD_CONFIG_MAX_SIZE;

    (void)body;
    /* The read streams out of the assembler's buffer; a chunked write in flight owns it. */
    if (s->active || !od_tlsr_config_load(s->buffer, &len)) {
        (void)od_config_read_start(&ctx->rp, ctx->r, NULL, 0u);
        return OD_CMD_NACK;
    }
    return od_config_read_start(&ctx->rp, ctx->r, s->buffer, len) == OD_TXQ_OK ? OD_CMD_OK
                                                                              : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_config_write(const od_cmd_ctx_t *ctx, od_span_t body)
{
    struct od_config_asm *s = od_tlsr_config_assembler();
    uint8_t ack[] = { RESP_ACK, RESP_CONFIG_WRITE, 0u, 0u };

    switch (od_config_asm_start(s, body)) {
    case OD_CONFIG_ASM_SINGLE:
        return persist_config(ctx, RESP_CONFIG_WRITE, body.p, (uint32_t)body.n);
    case OD_CONFIG_ASM_ACCEPTED:
        (void)config_ack(ctx, ack, sizeof(ack));
        return OD_CMD_OK;
    case OD_CONFIG_ASM_COMPLETE:
        return persist_config(ctx, RESP_CONFIG_WRITE, s->buffer, s->received);
    case OD_CONFIG_ASM_REJECTED:
    default:
        return nack(ctx, RESP_CONFIG_WRITE, 0u);
    }
}

od_cmd_result_t od_cmd_app_config_chunk(const od_cmd_ctx_t *ctx, od_span_t body)
{
    struct od_config_asm *s = od_tlsr_config_assembler();
    uint8_t ack[] = { RESP_ACK, RESP_CONFIG_CHUNK, 0u, 0u };

    switch (od_config_asm_chunk(s, body)) {
    case OD_CONFIG_ASM_ACCEPTED:
        (void)config_ack(ctx, ack, sizeof(ack));
        return OD_CMD_OK;
    case OD_CONFIG_ASM_COMPLETE:
        return persist_config(ctx, RESP_CONFIG_CHUNK, s->buffer, s->received);
    default:
        return nack(ctx, RESP_CONFIG_CHUNK, 0u);
    }
}

od_cmd_result_t od_cmd_app_config_clear(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t ok[] = { RESP_ACK, RESP_CONFIG_CLEAR, 0u, 0u };
    od_txq_status_t qrc;

    (void)body;
    if (!od_tlsr_config_clear()) {
        return nack(ctx, RESP_CONFIG_CLEAR, 0u);
    }
    od_config_asm_reset(od_tlsr_config_assembler());
    qrc = config_ack(ctx, ok, sizeof(ok));
    od_tlsr_config_reload();
    od_session_clear(od_session_app_state());
    return qrc == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

bool od_cmd_mutates_config(uint16_t cmd)
{
    return cmd == CMD_CONFIG_WRITE || cmd == CMD_CONFIG_CHUNK || cmd == CMD_CONFIG_CLEAR;
}

bool od_cmd_allow_unauthenticated(uint16_t cmd)
{
    /* No physical-presence or key-loss recovery path exists here, so no storage mutation is ever
     * allowed without a session -- the BG22 policy. */
    (void)cmd;
    return false;
}

/* LED replies as BG22's: NACK code 1 for a missing instance byte, 2 for an unknown instance or a
 * stop aimed at an instance that is not the one running. */
od_cmd_result_t od_cmd_app_led_activate(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t ok[] = { RESP_ACK, RESP_LED_ACTIVATE_ACK, 0u, 0u };

    if (body.n < 1u) {
        return nack(ctx, RESP_LED_ACTIVATE_ACK, 1u);
    }
    if (od_led_tlsr_activate(body.p[0], body.p + 1u, (uint16_t)(body.n - 1u)) != 0) {
        return nack(ctx, RESP_LED_ACTIVATE_ACK, 2u);
    }
    (void)reply(ctx, ok, sizeof(ok));
    return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_led_stop(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t ok[] = { RESP_ACK, RESP_LED_STOP_ACK, 0u, 0u };

    if ((body.n ? od_led_tlsr_stop(body.p[0], true) : od_led_tlsr_stop(0u, false)) != 0) {
        return nack(ctx, RESP_LED_STOP_ACK, 2u);
    }
    (void)reply(ctx, ok, sizeof(ok));
    return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_buzzer(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return nack(ctx, RESP_BUZZER_ACK, OD_ERR_PARTIAL_UNSUPPORTED);
}
