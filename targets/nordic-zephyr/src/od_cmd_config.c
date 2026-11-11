/* od_cmd_config.c -- the configuration commands and the state a chunked write carries.
 *
 * It also answers the two dispatcher predicates, because both are statements about THIS target's
 * configuration handlers: which opcodes mutate stored config, and which may run unauthenticated
 * under key-loss recovery. Keeping them beside the handlers they describe is the point -- they
 * were a copy of this file's opcode set living somewhere else.
 */

#include "od_cmd_config.h"

#include "od_cmd_app.h"
#include "od_cmd_reply.h"
#include "od_config_read.h"
#include "od_dispatch.h"   /* od_cmd_mutates_config / od_cmd_allow_unauthenticated */
#include "od_log.h"
#include "od_session.h"
#include "od_session_app.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "od_config_store.h"
#include "opendisplay_config_storage.h"
#include "opendisplay_constants.h"
#include "opendisplay_protocol.h"

#include <string.h>

typedef struct {
  bool active;
  uint32_t total_size;
  uint16_t expected_chunks;
  uint16_t received_chunks;
  uint32_t received_size;
  /* Whether this transfer OPENED unauthenticated, under the key-loss exemption. Every continuation
   * must match, so a transfer cannot be started with a session and finished without one. */
  bool unauth;
  uint8_t buffer[OD_CONFIG_MAX_SIZE];
} od_chunked_config_t;

static od_chunked_config_t s_cfg_chunk;

void od_cmd_config_reset(void)
{
  memset(&s_cfg_chunk, 0, sizeof(s_cfg_chunk));
}

static bool authenticated(void)
{
  return od_session_authenticated(od_session_app_state());
}

/* An unauthenticated rewrite has NO session to seal with -- that is the whole point of the
 * exemption -- so its ack MUST go plaintext. Sending it through od_cmd_reply() instead makes
 * od_reply substitute a plaintext hard NACK, and the host is told the write FAILED after the new
 * config has already been persisted: recovery appears broken while actually having worked.
 *
 * This is not the byte-2 inference returning. The choice is made from whether a session EXISTS,
 * which is a fact about what is possible, never from the response bytes.
 */
static od_txq_status_t config_ack(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
  if (!authenticated()) {
    return od_cmd_reply_plain(ctx, frame, len);
  }
  return od_cmd_reply(ctx, frame, len);
}

/* The new config may carry a new key, so the old session cannot outlive it. od_session_clear()
 * rather than memset: it also releases the crypto slot and preserves the slot index. */
static void clear_session(void)
{
  od_session_clear(od_session_app_state());
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
  (void)config_ack(ctx, ok, sizeof(ok));
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

/* The erase that pays for the key-loss exemption, and the half od_dispatch deliberately does not
 * own -- what "erase" means is storage policy.
 *
 * Runs only when this write actually arrived unauthenticated, so an ordinary authenticated write
 * never touches it. Dropping the stored config drops the OLD KEY with it, which is what stops the
 * exemption being used to layer a new configuration over a key the peer never proved it had. The
 * reference erases on the single-shot path and again on the first chunk of a chunked one, for the
 * same reason: a chunked write starts as CONFIG_WRITE and continues as CONFIG_CHUNK, and a session
 * that expires mid-transfer must not land a new config over a retained key. */
static void erase_before_unauthenticated_rewrite(uint16_t cmd)
{
  if (!od_cmd_allow_unauthenticated(cmd)) {
    return;
  }
  if (authenticated()) {
    return;                       /* an authenticated write: nothing to pay for */
  }
  (void)clearStoredConfig();
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

  /* AFTER the length checks, never before: erasing and then rejecting a malformed frame destroys
   * a working configuration on a frame the device does not even accept. */
  erase_before_unauthenticated_rewrite(CMD_CONFIG_WRITE);

  if (len > CONFIG_CHUNK_SIZE) {
    od_cmd_config_reset();
    s_cfg_chunk.active = true;
    s_cfg_chunk.unauth = !authenticated();
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
        /* THE ACK IS SEALED AND QUEUED BEFORE THE RELOAD, and the order is load-bearing.
         * clear_session() drops the session -- the new config may carry a new key -- so a reply
         * attempted after it finds none, and od_reply substitutes a plaintext hard NACK: the host
         * is told a write FAILED that has already been persisted, and cannot tell that from a real
         * failure. od_reply seals at commit, so queueing first makes the bytes final while the
         * session that sent the write is still the one answering it. */
        (void)config_ack(ctx, ack, sizeof(ack));
        opendisplay_ble_reload_config_from_nvm();
        clear_session();
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

    (void)config_ack(ctx, ack, sizeof(ack));
    return OD_CMD_OK;
  }

  if (saveConfig((uint8_t *)(void *)data, len)) {
    /* Sealed and queued before the reload -- see the note on the chunked branch above. */
    (void)config_ack(ctx, ack, sizeof(ack));
    opendisplay_ble_reload_config_from_nvm();
    clear_session();
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
  /* A TRANSFER MAY NOT CHANGE AUTHENTICATION STATE MID-FLIGHT. The opening CONFIG_WRITE recorded
   * whether it arrived unauthenticated, and every continuation must match: otherwise a transfer
   * begun under a live session could be COMPLETED unauthenticated after that session expired,
   * finishing without the erase the exemption is paid for -- landing a new config while the old
   * key survived. Refused rather than erased here, because erasing mid-stream would discard the
   * chunks already collected and turn a downgrade attempt into data loss. */
  if (s_cfg_chunk.unauth != !authenticated()) {
    od_cmd_config_reset();
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
      (void)config_ack(ctx, ack, sizeof(ack));
      opendisplay_ble_reload_config_from_nvm();
      clear_session();
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
  (void)config_ack(ctx, ack, sizeof(ack));
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

/* IMPLEMENTED FOR shared/core/od_dispatch.h -- KEY-LOSS RECOVERY, and the only exemption this
 * target grants.
 *
 * A device provisioned with a session key whose host has lost that key is otherwise bricked for
 * configuration: every command answers AUTH_REQUIRED and the only route back is physical. When
 * SecurityConfig flags bit 0 is set, the owner has said -- in the device's own stored
 * configuration -- that an unauthenticated config REWRITE is acceptable, so the gate is skipped for
 * the two write opcodes, and only when no session is live, which od_dispatch establishes before
 * asking.
 *
 * READS ARE NEVER EXEMPT. CONFIG_READ is absent deliberately: this path exists to replace a
 * configuration, not to disclose one, and exempting the read would hand the stored config -- and
 * the key inside it -- to any unauthenticated peer. The handlers erase the stored config before
 * accepting such a write, so the old key cannot survive into the new one either. */
bool od_cmd_allow_unauthenticated(uint16_t cmd)
{
  const struct SecurityConfig *sec = od_get_parsed_security();

  /* SECURITY MUST BE ON. With no key configured there is nothing to recover from and no gate to
   * be exempt from -- but the flag byte can still be set, and reading it alone made every ordinary
   * CONFIG_WRITE on an unsecured device erase its own storage first. The flag is documented as
   * applying "while encryption is enabled"; this is where that is enforced. */
  if (!od_session_security_enabled(sec)) {
    return false;
  }
  if ((sec->flags & 0x01u) == 0u) {
    return false;
  }
  return cmd == CMD_CONFIG_WRITE || cmd == CMD_CONFIG_CHUNK;
}
