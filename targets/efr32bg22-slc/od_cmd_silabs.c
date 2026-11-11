/* BG22 implementations of every shared command hook. */

#include "od_cmd_app.h"

#include "od_config_read.h"
#include "od_dispatch.h"
#include "od_ident_app.h"
#include "od_reply.h"
#include "od_txq.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "od_config_store.h"
#include "opendisplay_config_storage.h"
#include "opendisplay_constants.h"
#include "opendisplay_led.h"
#include "opendisplay_protocol.h"

#include "em_device.h"

#include <stdio.h>
#include <string.h>

#ifndef OPENDISPLAY_BUILD_ID
#define OPENDISPLAY_BUILD_ID "bg22-dev"
#endif

static od_txq_status_t reply(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
  return od_reply(ctx->r, &ctx->rp, frame, len);
}

static od_txq_status_t reply_plain(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
  return od_reply_plain(ctx->r, &ctx->rp, frame, len);
}

static od_cmd_result_t persist_config(const od_cmd_ctx_t *ctx, uint8_t response,
                                      const uint8_t *data, uint32_t len)
{
  uint8_t ok[] = { RESP_ACK, response, 0u, 0u };
  uint8_t err[] = { RESP_NACK, response, 0u, 0u };
  od_txq_status_t qrc;

  /* Persistence is the commit point. A success response must never get ahead of it. */
  if (!saveConfig((uint8_t *)(uintptr_t)data, len)) {
    err[2] = od_config_store_save_error();
    (void)reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  /* Queued (and sealed, on a claimed device) before the reload. A config save does not end the
   * session: the key is not in the config record (SECURITY_V2 9.6). */
  qrc = reply(ctx, ok, sizeof(ok));
  opendisplay_ble_reload_config_from_nvm();
  return qrc == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_firmware_version(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t rsp[46];
  const char *build = OPENDISPLAY_BUILD_ID;
  uint8_t n = (uint8_t)strlen(build);
  uint16_t version = opendisplay_ble_get_app_version();

  (void)body;
  if (n > 40u) n = 40u;
  rsp[0] = RESP_ACK;
  rsp[1] = RESP_FIRMWARE_VERSION;
  rsp[2] = (uint8_t)(version >> 8);
  rsp[3] = (uint8_t)version;
  rsp[4] = n;
  memcpy(&rsp[5], build, n);
  rsp[5u + n] = opendisplay_ble_get_app_version_patch();
  (void)reply_plain(ctx, rsp, (uint16_t)(6u + n));
  return OD_CMD_OK;
}

/* CMD_READ_IDENT 0x0046 is shared (od_cmd_ident.c); this is its config accessor. */
const struct od_config *od_ident_app_config(void)
{
  return opendisplay_get_global_config();
}

od_cmd_result_t od_cmd_app_read_msd(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t rsp[18] = { RESP_ACK, RESP_MSD_READ };
  (void)body;
  opendisplay_ble_copy_msd_bytes(&rsp[2]);
  (void)reply(ctx, rsp, sizeof(rsp));
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_reboot(const od_cmd_ctx_t *ctx, od_span_t body)
{
  (void)ctx; (void)body;
  NVIC_SystemReset();
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_enter_dfu(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { RESP_ACK, RESP_ENTER_DFU };
  (void)body;
  (void)reply(ctx, ok, sizeof(ok));
  opendisplay_ble_schedule_dfu();
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_power_off(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t err[] = { RESP_NACK, RESP_POWER_OFF, OD_ERR_POWER_OFF_UNSUPPORTED, 0u };
  (void)body;
  (void)reply_plain(ctx, err, sizeof(err));
  return OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_deep_sleep(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { RESP_ACK, RESP_DEEP_SLEEP };
  (void)body;
  (void)reply(ctx, ok, sizeof(ok));
  opendisplay_ble_schedule_deep_sleep();
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_config_read(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t *buf = opendisplay_config_assembler()->buffer;
  uint32_t len = OD_CONFIG_MAX_SIZE;
  od_txq_status_t rc;

  (void)body;
  if (opendisplay_config_assembler()->active) {
    (void)od_config_read_start(&ctx->rp, ctx->r, NULL, 0u);
    return OD_CMD_NACK;
  }
  if (!initConfigStorage() || !loadConfig(buf, &len)) {
    (void)od_config_read_start(&ctx->rp, ctx->r, NULL, 0u);
    return OD_CMD_NACK;
  }
  rc = od_config_read_start(&ctx->rp, ctx->r, buf, len);
  return rc == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_config_write(const od_cmd_ctx_t *ctx, od_span_t body)
{
  struct od_config_asm *s = opendisplay_config_assembler();
  enum od_config_asm_result result = od_config_asm_start(s, body);
  uint8_t ack[] = { RESP_ACK, RESP_CONFIG_WRITE, 0u, 0u };
  uint8_t err[] = { RESP_NACK, RESP_CONFIG_WRITE, 0u, 0u };

  switch (result) {
  case OD_CONFIG_ASM_SINGLE:
    return persist_config(ctx, RESP_CONFIG_WRITE, body.p, (uint32_t)body.n);
  case OD_CONFIG_ASM_ACCEPTED:
    (void)reply(ctx, ack, sizeof(ack));
    return OD_CMD_OK;
  case OD_CONFIG_ASM_COMPLETE:
    return persist_config(ctx, RESP_CONFIG_WRITE, s->buffer, s->received);
  case OD_CONFIG_ASM_REJECTED:
    (void)reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  return OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_config_chunk(const od_cmd_ctx_t *ctx, od_span_t body)
{
  struct od_config_asm *s = opendisplay_config_assembler();
  enum od_config_asm_result result = od_config_asm_chunk(s, body);
  uint8_t ack[] = { RESP_ACK, RESP_CONFIG_CHUNK, 0u, 0u };
  uint8_t err[] = { RESP_NACK, RESP_CONFIG_CHUNK, 0u, 0u };

  if (result == OD_CONFIG_ASM_ACCEPTED) {
    (void)reply(ctx, ack, sizeof(ack));
    return OD_CMD_OK;
  }
  if (result == OD_CONFIG_ASM_COMPLETE) {
    return persist_config(ctx, RESP_CONFIG_CHUNK, s->buffer, s->received);
  }
  (void)reply_plain(ctx, err, sizeof(err));
  return OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_config_clear(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { RESP_ACK, RESP_CONFIG_CLEAR, 0u, 0u };
  uint8_t err[] = { RESP_NACK, RESP_CONFIG_CLEAR, 0u, 0u };
  od_txq_status_t qrc;

  (void)body;
  if (!clearStoredConfig()) {
    (void)reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  od_config_asm_reset(opendisplay_config_assembler());
  qrc = reply(ctx, ok, sizeof(ok));
  opendisplay_ble_reload_config_from_nvm();
  return qrc == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

bool od_cmd_mutates_config(uint16_t cmd)
{
  return cmd == CMD_CONFIG_WRITE || cmd == CMD_CONFIG_CHUNK || cmd == CMD_CONFIG_CLEAR;
}

od_cmd_result_t od_cmd_app_led_activate(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { RESP_ACK, RESP_LED_ACTIVATE_ACK, 0u, 0u };
  uint8_t err[] = { RESP_NACK, RESP_LED_ACTIVATE_ACK, 2u, 0u };
  if (body.n < 1u || opendisplay_led_activate(body.p[0], body.p + 1u,
                                               (uint16_t)(body.n - 1u)) != 0) {
    if (body.n < 1u) err[2] = 1u;
    (void)reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  (void)reply(ctx, ok, sizeof(ok));
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_led_stop(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { RESP_ACK, RESP_LED_STOP_ACK, 0u, 0u };
  uint8_t err[] = { RESP_NACK, RESP_LED_STOP_ACK, 2u, 0u };
  int rc = body.n ? opendisplay_led_stop(body.p[0], true) : opendisplay_led_stop(0u, false);
  if (rc != 0) {
    (void)reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  (void)reply(ctx, ok, sizeof(ok));
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_buzzer(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t err[] = { RESP_NACK, RESP_BUZZER_ACK, OD_ERR_PARTIAL_UNSUPPORTED, 0u };
  (void)body;
  (void)reply_plain(ctx, err, sizeof(err));
  return OD_CMD_NACK;
}

void od_cmd_silabs_reset(void)
{
  od_config_asm_reset(opendisplay_config_assembler());
}
