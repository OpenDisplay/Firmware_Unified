/* The shared command hooks (od_cmd_app.h) and the config adapter: flash storage is
 * od_tlsr_store.c behind od_hal_nvs; parsing, framing and the SEC2 policy are shared/core. */
#include "od_tlsr_ble.h"
#include "od_tlsr_ota.h"
#include "od_tlsr_target.h"

#include "od_cfg_index.h"
#include "od_cfg_policy.h"
#include "od_claim_mode.h"
#include "od_cmd_app.h"
#include "od_config.h"
#include "od_config_asm.h"
#include "od_config_read.h"
#include "od_config_store.h"
#include "od_hal_reset.h"
#include "od_ident_app.h"
#include "od_reply.h"
#include "od_sec2.h"
#include "od_span.h"
#include "od_txq.h"
#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"

#include <string.h>

/* One object is the chunked-write reassembly state and the save workspace, as on BG22
 * (opendisplay_config_storage.c there has the ordering argument): the save writes the record
 * header over the assembler's state words, so the assembler is reset after every save. */
typedef union {
    struct od_config_asm assembler;
    uint8_t              record[OD_CONFIG_STORE_MAX_RECORD];
} od_config_work_t;
OD_STATIC_ASSERT(sizeof(struct od_config_asm) == OD_CONFIG_STORE_MAX_RECORD,
                 "the assembler and the record must be the same object");

static od_config_work_t s_work;
static struct od_config s_cfg;
static bool s_cfg_loaded;

bool od_tlsr_config_load(void)
{
    uint32_t len = OD_CONFIG_MAX_SIZE;
    struct od_config_report report;

    od_config_reset(&s_cfg);
    s_cfg_loaded = false;
    if (od_config_store_init() != OD_CONFIG_STORE_OK ||
        od_config_store_load(s_work.assembler.buffer, &len) != OD_CONFIG_STORE_OK) {
        return false;
    }
    s_cfg_loaded = od_config_parse(&s_cfg, od_span_make(s_work.assembler.buffer, len), &report) ==
                   OD_CFG_TLV_OK;
    od_config_asm_reset(&s_work.assembler);
    return s_cfg_loaded;
}

const struct od_config *od_tlsr_config(void)
{
    return &s_cfg;
}

bool od_tlsr_config_loaded(void)
{
    return s_cfg_loaded;
}

void od_tlsr_config_reload(void)
{
    (void)od_tlsr_config_load();
    (void)od_cfg_index_build();
    od_tlsr_display_init();
    od_tlsr_display_request_boot();
    od_tlsr_ble_refresh_adv();
}

void od_tlsr_cmd_reset(void)
{
    od_config_asm_reset(&s_work.assembler);
}

uint8_t *od_tlsr_config_workspace(uint32_t *cap)
{
    if (cap != NULL) {
        *cap = sizeof s_work.record;
    }
    return s_work.record;
}

bool od_tlsr_config_assembling(void)
{
    return s_work.assembler.active;
}

const struct od_config *od_ident_app_config(void)
{
    return s_cfg_loaded ? &s_cfg : NULL;
}

static od_txq_status_t reply(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    return od_reply(ctx->r, &ctx->rp, frame, len);
}

static od_txq_status_t reply_plain(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    return od_reply_plain(ctx->r, &ctx->rp, frame, len);
}

static od_cmd_result_t nack(const od_cmd_ctx_t *ctx, uint8_t resp, uint8_t err)
{
    uint8_t f[] = { RESP_NACK, resp, err, 0u };
    (void)reply_plain(ctx, f, sizeof f);
    return OD_CMD_NACK;
}

static bool save_config(const uint8_t *data, uint32_t len)
{
    enum od_config_store_result rc;

    /* Refusals that leave an assembly intact come before the header write consumes it. */
    if (data == NULL || len > OD_CONFIG_MAX_SIZE || od_config_store_init() != OD_CONFIG_STORE_OK) {
        return false;
    }
    rc = od_cfg_policy_save(s_work.record, sizeof s_work.record, data, len);
    od_config_asm_reset(&s_work.assembler);
    return rc == OD_CONFIG_STORE_OK;
}

static od_cmd_result_t persist_config(const od_cmd_ctx_t *ctx, uint8_t response,
                                      const uint8_t *data, uint32_t len)
{
    uint8_t ok[] = { RESP_ACK, response, 0u, 0u };
    od_txq_status_t qrc;

    if (!save_config(data, len)) {
        return nack(ctx, response, od_config_store_save_error());
    }
    qrc = reply(ctx, ok, sizeof ok);
    od_tlsr_config_reload();
    return qrc == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_firmware_version(const od_cmd_ctx_t *ctx, od_span_t body)
{
    static const char build[] = OD_BUILD_ID;
    uint8_t rsp[6u + sizeof build];
    uint8_t n = (uint8_t)(sizeof build - 1u);

    (void)body;
    rsp[0] = RESP_ACK;
    rsp[1] = RESP_FIRMWARE_VERSION;
    rsp[2] = (uint8_t)OD_FW_MAJOR;
    rsp[3] = (uint8_t)OD_FW_MINOR;
    rsp[4] = n;
    memcpy(&rsp[5], build, n);
    rsp[5u + n] = (uint8_t)OD_FW_PATCH;
    (void)reply_plain(ctx, rsp, (uint16_t)(6u + n));
    return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_read_msd(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t rsp[18] = { RESP_ACK, RESP_MSD_READ };

    (void)body;
    od_tlsr_ble_copy_msd(&rsp[2]);
    (void)reply(ctx, rsp, sizeof rsp);
    return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_reboot(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)ctx;
    (void)body;
    (void)od_claim_mode_before_reboot();
    od_hal_reset_reboot();
    return OD_CMD_OK;
}

/* No separate DFU mode: ENTER_DFU opens the updater's SEC2 gate (od_tlsr_ota.h) for this link
 * and this session. The dispatcher has already required a session where the device needs one. */
static od_reply_t s_dfu_rp;

od_cmd_result_t od_cmd_app_enter_dfu(const od_cmd_ctx_t *ctx, od_span_t body)
{
    const uint8_t ok[] = { RESP_ACK, RESP_ENTER_DFU };

    (void)body;
    s_dfu_rp = ctx->rp;
    od_tlsr_ota_unlock();
    (void)od_reply(ctx->r, &ctx->rp, ok, sizeof ok);
    return OD_CMD_OK;
}

bool od_tlsr_ota_app_needs_unlock(void)
{
    return od_sec2_dfu_needs_unlock();
}

bool od_tlsr_ota_app_unlock_live(void)
{
    return od_sec2_session_live(&s_dfu_rp);
}

od_cmd_result_t od_cmd_app_power_off(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return nack(ctx, RESP_POWER_OFF, OD_ERR_POWER_OFF_UNSUPPORTED);
}

/* CMD_DEEP_SLEEP is not implemented on this target: refused. */
od_cmd_result_t od_cmd_app_deep_sleep(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return nack(ctx, RESP_DEEP_SLEEP, OD_ERR_DEEP_SLEEP_UNSUPPORTED);
}

od_cmd_result_t od_cmd_app_config_read(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t *buf = s_work.assembler.buffer;
    uint32_t len = OD_CONFIG_MAX_SIZE;
    od_txq_status_t rc;

    (void)body;
    if (s_work.assembler.active || od_config_store_init() != OD_CONFIG_STORE_OK ||
        od_config_store_load(buf, &len) != OD_CONFIG_STORE_OK) {
        (void)od_config_read_start(&ctx->rp, ctx->r, NULL, 0u);
        return OD_CMD_NACK;
    }
    rc = od_config_read_start(&ctx->rp, ctx->r, buf, len);
    return rc == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_config_write(const od_cmd_ctx_t *ctx, od_span_t body)
{
    struct od_config_asm *a = &s_work.assembler;
    uint8_t ack[] = { RESP_ACK, RESP_CONFIG_WRITE, 0u, 0u };

    switch (od_config_asm_start(a, body)) {
    case OD_CONFIG_ASM_SINGLE:
        return persist_config(ctx, RESP_CONFIG_WRITE, body.p, (uint32_t)body.n);
    case OD_CONFIG_ASM_ACCEPTED:
        (void)reply(ctx, ack, sizeof ack);
        return OD_CMD_OK;
    case OD_CONFIG_ASM_COMPLETE:
        return persist_config(ctx, RESP_CONFIG_WRITE, a->buffer, a->received);
    case OD_CONFIG_ASM_REJECTED:
    default:
        return nack(ctx, RESP_CONFIG_WRITE, 0u);
    }
}

od_cmd_result_t od_cmd_app_config_chunk(const od_cmd_ctx_t *ctx, od_span_t body)
{
    struct od_config_asm *a = &s_work.assembler;
    uint8_t ack[] = { RESP_ACK, RESP_CONFIG_CHUNK, 0u, 0u };
    enum od_config_asm_result r = od_config_asm_chunk(a, body);

    if (r == OD_CONFIG_ASM_ACCEPTED) {
        (void)reply(ctx, ack, sizeof ack);
        return OD_CMD_OK;
    }
    if (r == OD_CONFIG_ASM_COMPLETE) {
        return persist_config(ctx, RESP_CONFIG_CHUNK, a->buffer, a->received);
    }
    return nack(ctx, RESP_CONFIG_CHUNK, 0u);
}

od_cmd_result_t od_cmd_app_config_clear(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t ok[] = { RESP_ACK, RESP_CONFIG_CLEAR, 0u, 0u };
    od_txq_status_t qrc;

    (void)body;
    if (od_cfg_policy_clear_store() != OD_CONFIG_STORE_OK) {
        return nack(ctx, RESP_CONFIG_CLEAR, 0u);
    }
    od_config_asm_reset(&s_work.assembler);
    qrc = reply(ctx, ok, sizeof ok);
    od_tlsr_config_reload();
    return qrc == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

bool od_cmd_mutates_config(uint16_t cmd)
{
    return cmd == CMD_CONFIG_WRITE || cmd == CMD_CONFIG_CHUNK || cmd == CMD_CONFIG_CLEAR;
}

/* LED and buzzer patterns arrive with the board pin map; refused until then. */
od_cmd_result_t od_cmd_app_led_activate(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return nack(ctx, RESP_LED_ACTIVATE_ACK, 2u);
}

od_cmd_result_t od_cmd_app_led_stop(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return nack(ctx, RESP_LED_STOP_ACK, 2u);
}

od_cmd_result_t od_cmd_app_buzzer(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return nack(ctx, RESP_BUZZER_ACK, OD_ERR_PARTIAL_UNSUPPORTED);
}
