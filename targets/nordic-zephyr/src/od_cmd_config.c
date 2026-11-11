/* od_cmd_config.c -- the configuration commands and the state a chunked write carries.
 *
 * It also answers the dispatcher's od_cmd_mutates_config(), a statement about THIS target's
 * configuration handlers, kept beside the handlers it describes.
 */

#include "od_cmd_config.h"

#include "od_cmd_app.h"
#include "od_cmd_reply.h"
#include "od_config_read.h"
#include "od_dispatch.h"   /* od_cmd_mutates_config */
#include "od_log.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "od_config_store.h"
#include "opendisplay_config_storage.h"
#include "opendisplay_constants.h"
#include "opendisplay_protocol.h"
#include "od_sec2_trace.h"

#include <string.h>

typedef struct {
  bool active;
  uint32_t total_size;
  uint16_t expected_chunks;
  uint16_t received_chunks;
  uint32_t received_size;
  uint8_t buffer[OD_CONFIG_MAX_SIZE];
} od_chunked_config_t;

static od_chunked_config_t s_cfg_chunk;

/* Test-trace builds: name every config ACK and whether it was queued, so a lost 0x0041/0x0042
 * acknowledgement on the host can be told apart from one the device never sent. */
static void cfg_ack(const od_cmd_ctx_t *ctx, const uint8_t *ack, uint16_t len, const char *where)
{
  const od_txq_status_t q = od_cmd_reply(ctx, ack, len);

  OD_SEC2_TRACE("config ACK %02x%02x %s: queued rc=%d (chunks %u/%u, %u/%u B)",
                (unsigned)ack[0], (unsigned)ack[1], where, (int)q,
                (unsigned)s_cfg_chunk.received_chunks, (unsigned)s_cfg_chunk.expected_chunks,
                (unsigned)s_cfg_chunk.received_size, (unsigned)s_cfg_chunk.total_size);
  (void)q;
}

void od_cmd_config_reset(void)
{
  memset(&s_cfg_chunk, 0, sizeof(s_cfg_chunk));
}

od_cmd_result_t od_cmd_app_config_clear(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { 0x00u, RESP_CONFIG_CLEAR, 0x00u, 0x00u };
  uint8_t err[] = { 0xFFu, RESP_CONFIG_CLEAR, 0x00u, 0x00u };

  (void)body;
  if (!clearStoredConfig()) {
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  /* SEALED BEFORE THE RELOAD, same rule as the config-write acks. Reloading an erased config
   * turns security OFF, after which od_reply takes its plaintext shortcut -- so replying second
   * silently downgrades an ack that the session which sent the command could still protect. */
  (void)od_cmd_reply(ctx, ok, sizeof(ok));
  opendisplay_ble_reload_config_from_nvm();
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_config_read(const od_cmd_ctx_t *ctx, od_span_t body)
{
  /* File-static: the producer reads from this across many pump passes, which is exactly why
   * od_dispatch DEFERS every config-mutating opcode while a read is active -- a write reloading
   * through the same buffer mid-read would splice two configs into one CRC-valid read-back. */
  static uint8_t config_data[OD_CONFIG_MAX_SIZE];
  uint32_t config_len = OD_CONFIG_MAX_SIZE;

  (void)body;
  if (!initConfigStorage()) {
    uint8_t err[] = { 0xFFu, RESP_CONFIG_READ, 0x00u, 0x00u };
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }

  if (!loadConfig(config_data, &config_len)) {
    /* NO CONFIG STORED answers the 4-byte ERROR frame, not a zero-length ACK, and the difference
     * is one the host acts on. py-opendisplay tests for {FF,40,..} explicitly and raises "Device
     * has no stored configuration"; its comment names the exact failure the test prevents --
     * without it, the {00,00} of an error frame reads as a zero-length config "instead of no
     * config". This target used to answer a valid read of nothing, so an UNPROVISIONED device
     * reported as provisioned-with-nothing, and a corrupt config did too.
     *
     * The canonical header's prose does not settle this and should not be read as if it did: it
     * describes an EMPTY config (stored, length zero) and reserves 0xFF for storage-init failure,
     * and neither sentence covers "nothing stored" -- which is the case that actually occurs.
     * Firmware and the host agree on the answer; FOLLOWUPS.md carries the header wording.
     *
     * Passing NULL rather than hand-building the frame: od_config_read_start() emits exactly this
     * shape for a NULL blob, so both targets now get it from one place. */
    const od_txq_status_t rc = od_config_read_start(&ctx->rp, ctx->r, NULL, 0u);
    (void)rc;
    return OD_CMD_NACK;
  }

  /* STARTS a read; it does not perform one. Chunk 0 goes out here and od_config_read_pump() emits
   * the rest, one slot per pass. The synchronous loop this replaces queued up to 44 frames in a
   * single call against a one-slot reservation -- at the cutover chunk 0 (which declares the total
   * length to the host) would have gone out and every later chunk failed silently on an exhausted
   * token, leaving the host waiting for a config it had been promised.
   *
   * The reservation is TRANSFERRED; od_dispatch's release afterwards is a no-op because the token
   * is already spent. */
  {
    const od_txq_status_t rc =
      od_config_read_start(&ctx->rp, ctx->r, config_data, config_len);
    return (rc == OD_TXQ_OK) ? OD_CMD_OK : OD_CMD_NACK;
  }
}

od_cmd_result_t od_cmd_app_config_write(const od_cmd_ctx_t *ctx, od_span_t body)
{
  const uint8_t *data = body.p;
  const uint16_t len = (uint16_t)body.n;
  od_cmd_result_t rc = OD_CMD_NACK;
  uint8_t ack[] = { 0x00u, RESP_CONFIG_WRITE, 0x00u, 0x00u };
  uint8_t err[] = { 0xFFu, RESP_CONFIG_WRITE, 0x00u, 0x00u };

  if (len == 0u) {
    /* Answers NOTHING, which is the shipped behaviour and is preserved here. It is still a
     * REFUSAL: the frame was malformed and no config was written, so it must not be reported as
     * an accepted command. */
    return OD_CMD_NACK;
  }

  if (len > CONFIG_CHUNK_SIZE) {
    od_cmd_config_reset();
    s_cfg_chunk.active = true;
    s_cfg_chunk.received_chunks = 1;

    if (len >= CONFIG_CHUNK_SIZE_WITH_PREFIX) {
      s_cfg_chunk.total_size = (uint32_t)data[0] | ((uint32_t)data[1] << 8);
      if (s_cfg_chunk.total_size > OD_CONFIG_MAX_SIZE || s_cfg_chunk.total_size == 0u) {
        od_cmd_config_reset();
        (void)od_cmd_reply_plain(ctx, err, sizeof(err));
        return OD_CMD_NACK;
      }
      {
        uint16_t chunk_data_size = (uint16_t)(len - 2u);
        if (chunk_data_size > CONFIG_CHUNK_SIZE) {
          chunk_data_size = CONFIG_CHUNK_SIZE;
        }
        if ((uint32_t)chunk_data_size > s_cfg_chunk.total_size) {
          chunk_data_size = (uint16_t)s_cfg_chunk.total_size;
        }
        memcpy(s_cfg_chunk.buffer, data + 2, chunk_data_size);
        s_cfg_chunk.received_size = chunk_data_size;
      }
    } else {
      s_cfg_chunk.total_size = len;
      if (s_cfg_chunk.total_size > OD_CONFIG_MAX_SIZE) {
        od_cmd_config_reset();
        (void)od_cmd_reply_plain(ctx, err, sizeof(err));
        return OD_CMD_NACK;
      }
      {
        uint16_t chunk_size = (len < CONFIG_CHUNK_SIZE) ? len : CONFIG_CHUNK_SIZE;
        memcpy(s_cfg_chunk.buffer, data, chunk_size);
        s_cfg_chunk.received_size = chunk_size;
      }
    }

    if (s_cfg_chunk.received_size >= s_cfg_chunk.total_size) {
      if (saveConfig(s_cfg_chunk.buffer, s_cfg_chunk.received_size)) {
        /* The ack is queued before the reload, so it is final before the config changes. */
        cfg_ack(ctx, ack, sizeof(ack), "write");
        opendisplay_ble_reload_config_from_nvm();
        rc = OD_CMD_OK;
      } else {
        err[2] = od_config_store_save_error();
        (void)od_cmd_reply_plain(ctx, err, sizeof(err));
        rc = OD_CMD_NACK;
      }
      od_cmd_config_reset();
      /* The verdict is CARRIED, not re-derived: both branches answer, and which one ran is the
       * only thing that distinguishes an accepted write from a refused one by the time control
       * reaches here. */
      return rc;
    }

    {
      uint32_t rem = s_cfg_chunk.total_size - s_cfg_chunk.received_size;
      s_cfg_chunk.expected_chunks =
        (uint16_t)(1u + (rem + CONFIG_CHUNK_SIZE - 1u) / CONFIG_CHUNK_SIZE);
    }

    cfg_ack(ctx, ack, sizeof(ack), "write");
    return OD_CMD_OK;
  }

  if (saveConfig((uint8_t *)(void *)data, len)) {
    /* Sealed and queued before the reload -- see the note on the chunked branch above. */
    cfg_ack(ctx, ack, sizeof(ack), "write");
    opendisplay_ble_reload_config_from_nvm();
  } else {
    err[2] = od_config_store_save_error();
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_config_chunk(const od_cmd_ctx_t *ctx, od_span_t body)
{
  const uint8_t *data = body.p;
  const uint16_t len = (uint16_t)body.n;
  od_cmd_result_t rc = OD_CMD_NACK;
  uint8_t ack[] = { 0x00u, RESP_CONFIG_CHUNK, 0x00u, 0x00u };
  uint8_t err[] = { 0xFFu, RESP_CONFIG_CHUNK, 0x00u, 0x00u };

  if (!s_cfg_chunk.active) {
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  if (len == 0u) {
    /* Silent, as shipped -- but a refusal, not an acceptance. */
    return OD_CMD_NACK;
  }
  if (len > CONFIG_CHUNK_SIZE) {
    od_cmd_config_reset();
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  if (s_cfg_chunk.received_size + len > s_cfg_chunk.total_size) {
    od_cmd_config_reset();
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  if (s_cfg_chunk.received_size + len > OD_CONFIG_MAX_SIZE) {
    od_cmd_config_reset();
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }
  if (s_cfg_chunk.received_chunks >= MAX_CONFIG_CHUNKS) {
    od_cmd_config_reset();
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
    return OD_CMD_NACK;
  }

  memcpy(s_cfg_chunk.buffer + s_cfg_chunk.received_size, data, len);
  s_cfg_chunk.received_size += len;
  s_cfg_chunk.received_chunks++;

  if (s_cfg_chunk.received_chunks >= s_cfg_chunk.expected_chunks) {
    if (s_cfg_chunk.received_size != s_cfg_chunk.total_size) {
      od_cmd_config_reset();
      (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      return OD_CMD_NACK;
    }
    if (saveConfig(s_cfg_chunk.buffer, s_cfg_chunk.received_size)) {
      /* Sealed and queued before the reload -- see the note on od_cmd_app_config_write(). */
      cfg_ack(ctx, ack, sizeof(ack), "chunk");
      opendisplay_ble_reload_config_from_nvm();
      rc = OD_CMD_OK;
    } else {
      err[2] = od_config_store_save_error();
      (void)od_cmd_reply_plain(ctx, err, sizeof(err));
      rc = OD_CMD_NACK;
    }
    od_cmd_config_reset();
    return rc;
  }
  /* An intermediate chunk: accepted, and the transfer continues. */
  cfg_ack(ctx, ack, sizeof(ack), "chunk");
  return OD_CMD_OK;
}

/* IMPLEMENTED FOR shared/core/od_dispatch.h. The opcodes a live CONFIG_READ must exclude, because
 * the producer reads the same stored blob a write reloads through: letting one land between two
 * chunks splices two configs into one CRC-valid read-back.
 *
 * Target-side because the set is a statement about THIS firmware's handlers, not about the wire --
 * Nordic has NFC, Silabs has no PIPE. CONFIG_READ itself is absent deliberately: the dispatcher
 * tests for it separately, since a second read is excluded for a different reason (it would
 * restart a producer that has already promised the host a chunk count). */
bool od_cmd_mutates_config(uint16_t cmd)
{
  switch (cmd) {
  case CMD_CONFIG_WRITE:   /* saveConfig + reload */
  case CMD_CONFIG_CHUNK:   /* same, on completion */
  case CMD_CONFIG_CLEAR:   /* clearStoredConfig */
    return true;
  default:
    return false;
  }
}

