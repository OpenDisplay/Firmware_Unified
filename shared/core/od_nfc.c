/* od_nfc.c -- see od_nfc.h.
 *
 * THE ORDER OF THE CHECKS IS THE CONTRACT. On the inline write the length is tested before the
 * record type, and as its own arm: both donors do it that way, and the alternative -- BG22's,
 * which tested the type first and folded the length test into the tag call -- makes a malformed
 * length answer TAG_WRITE_FAILED, which tells a client its tag is broken when its frame was.
 *
 * EVERY LENGTH BOUND IS EVALUATED IN 32 BITS. The declared length is a peer-supplied 16-bit field;
 * a 16-bit sum with its header wraps and admits a length the body cannot hold.
 */

#include "od_nfc.h"

#include "od_log.h"
#include "od_nfc_app.h"
#include "od_reply.h"

#include <string.h>

/* Both derived constants are asserted HERE rather than in the header -- see od_nfc.h for why. */
OD_STATIC_ASSERT(OD_NFC_READ_MAX == 218u, "NFC read cap is wire-visible");
OD_STATIC_ASSERT(OD_NFC_ASSEMBLY_MAX == 512u, "NFC assembly limit is the wire's, not a target's");

void od_nfc_log_event(enum od_nfc_log_event event, int detail)
{
#if OD_CAP_NFC
    switch (event) {
    case OD_NFC_LOG_PAYLOAD_SET_FAILED:
        od_log_error("NFC payload setup failed: %d", detail);
        break;
    case OD_NFC_LOG_EMULATION_START_FAILED:
        od_log_error("NFC emulation start failed: %d", detail);
        break;
    case OD_NFC_LOG_CONFIG_ABSENT:
        od_log_info("No NFC configuration; SoC NFCT is idle");
        break;
    case OD_NFC_LOG_CONFIG_DISABLED:
        od_log_info("NFC configurations are present but none are enabled");
        break;
    case OD_NFC_LOG_IC_UNSUPPORTED:
        od_log_warn("Unsupported NFC IC type %u; expected auto or SoC NFCT",
                    (unsigned)detail);
        break;
    case OD_NFC_LOG_T2T_SETUP_FAILED:
        od_log_error("NFC Type 2 Tag setup failed: %d", detail);
        break;
    case OD_NFC_LOG_T2T_ACTIVE:
        od_log_info("SoC NFCT Type 2 Tag is active; advertising byte %u",
                    (unsigned)detail);
        break;
    default:
        break;
    }
#else
    (void)event;
    (void)detail;
#endif
}

#if OD_CAP_NFC

struct od_nfc {
    od_reply_t owner;        /* the reply identity entire, per N1 -- od_txq.h */
    uint16_t   total_len;
    uint16_t   received_len;
    uint8_t    rec_type;
    bool       active;
    uint8_t    data[OD_NFC_ASSEMBLY_MAX];
};

static struct od_nfc s_nfc;

void od_nfc_reset(void)
{
    memset(&s_nfc, 0, sizeof s_nfc);
}

static bool rec_type_valid(uint8_t t)
{
    return t == OD_NFC_REC_TEXT || t == OD_NFC_REC_URI || t == OD_NFC_REC_WELL_KNOWN_RAW
        || t == OD_NFC_REC_MIME || t == OD_NFC_REC_RAW_NDEF;
}

/* A refusal is sealed while a session is live and plaintext otherwise (od_reply_refusal). The
 * verdict is NACK so the frame does not stamp activity. */
static od_cmd_result_t nack(const od_cmd_ctx_t *ctx, uint8_t err)
{
    uint8_t frame[4] = { RESP_NACK, RESP_NFC_ENDPOINT, 0xFFu, err };

    (void)od_reply_refusal(ctx->r, &ctx->rp, frame, sizeof frame);
    return OD_CMD_NACK;
}

/* An ACK whose meaning is "done": the tag has already been touched, so a queue failure is
 * reported in the verdict and cannot revert it (N6). */
static od_cmd_result_t ack_committed(const od_cmd_ctx_t *ctx, uint8_t status)
{
    uint8_t frame[3] = { RESP_ACK, RESP_NFC_ENDPOINT, status };

    return od_reply(ctx->r, &ctx->rp, frame, sizeof frame) == OD_TXQ_OK
        ? OD_CMD_OK : OD_CMD_NACK;
}

/* An ACK whose meaning is "armed". If it cannot be queued the client never learns its frame
 * landed, and its only recovery is a fresh START -- so the staged bytes go with it, or the next
 * DATA draws an overflow and the next END a mismatch on a transfer the client believes never
 * began. A deliberate divergence from the donors, which ignore the send result (N6). */
static od_cmd_result_t ack_armed(const od_cmd_ctx_t *ctx)
{
    uint8_t frame[3] = { RESP_ACK, RESP_NFC_ENDPOINT, NFC_STATUS_CHUNK_ACCEPTED };

    if (od_reply(ctx->r, &ctx->rp, frame, sizeof frame) != OD_TXQ_OK) {
        od_nfc_reset();
        return OD_CMD_NACK;
    }
    return OD_CMD_OK;
}

static bool owned_by(const od_cmd_ctx_t *ctx)
{
    return s_nfc.active && od_reply_same(&ctx->rp, &s_nfc.owner);
}

static od_cmd_result_t handle_read(const od_cmd_ctx_t *ctx)
{
    /* Sized exactly, and on the stack: this outlives nothing, and a static would be the largest
     * NFC object after the assembler for no reason (N7). */
    uint8_t rsp[6u + OD_NFC_READ_MAX];
    uint16_t len = 0u;
    uint8_t type = 0u;

    if (!od_nfc_app_read(&type, &rsp[6], &len, OD_NFC_READ_MAX) || len > OD_NFC_READ_MAX) {
        return nack(ctx, NFC_ERR_READ_FAILED);
    }
    rsp[0] = RESP_ACK;
    rsp[1] = RESP_NFC_ENDPOINT;
    rsp[2] = NFC_STATUS_READ_DATA;
    rsp[3] = type;
    rsp[4] = (uint8_t)(len >> 8);
    rsp[5] = (uint8_t)len;
    return od_reply(ctx->r, &ctx->rp, rsp, (uint16_t)(6u + len)) == OD_TXQ_OK
        ? OD_CMD_OK : OD_CMD_NACK;
}

static od_cmd_result_t handle_inline_write(const od_cmd_ctx_t *ctx, od_span_t body)
{
    od_span_t header;
    od_span_t rest;
    od_span_t payload;
    od_span_t trailing;
    uint16_t declared;
    uint8_t type;

    if (!od_span_split(body, 4u, &header, &rest)) {
        return nack(ctx, NFC_ERR_MALFORMED);
    }
    type = header.p[1];
    declared = (uint16_t)(((uint16_t)header.p[2] << 8) | header.p[3]);
    /* THE BOUND IS ITS OWN ARM, evaluated in 32 bits, and it runs BEFORE the record-type gate.
     * The split below would also refuse an over-long cut, so against a valid record type the two
     * are indistinguishable -- both answer MALFORMED and neither touches the tag. What makes this
     * arm observable, and therefore testable, is its POSITION: with an invalid record type and a
     * wrapping declared length, the widened comparison answers MALFORMED while a 16-bit one falls
     * through to the type gate and answers INVALID_REC_TYPE. That input is the oracle for this
     * line; the split is defense in depth behind it, not a substitute. */
    if ((uint32_t)declared + 4u > (uint32_t)body.n) {
        return nack(ctx, NFC_ERR_MALFORMED);
    }
    if (!rec_type_valid(type)) {
        return nack(ctx, NFC_ERR_INVALID_REC_TYPE);
    }
    /* Trailing bytes past the declared length are accepted and ignored, which is what both donors
     * do; the cut is checked rather than assumed. */
    if (!od_span_split(rest, declared, &payload, &trailing)) {
        return nack(ctx, NFC_ERR_MALFORMED);
    }
    if (!od_nfc_app_write(type, payload.p, (uint16_t)payload.n)) {
        return nack(ctx, NFC_ERR_TAG_WRITE_FAILED);
    }
    return ack_committed(ctx, NFC_STATUS_WRITE_COMMITTED);
}

static od_cmd_result_t handle_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
    od_span_t header;
    od_span_t rest;
    uint16_t total;
    uint8_t type;

    if (!od_span_split(body, 4u, &header, &rest)) {
        return nack(ctx, NFC_ERR_MALFORMED);
    }
    type = header.p[1];
    total = (uint16_t)(((uint16_t)header.p[2] << 8) | header.p[3]);
    if (!rec_type_valid(type)) {
        return nack(ctx, NFC_ERR_INVALID_REC_TYPE);
    }
    if (total == 0u || (uint32_t)total > (uint32_t)OD_NFC_ASSEMBLY_MAX) {
        return nack(ctx, NFC_ERR_BAD_TOTAL_LEN);
    }
    /* A replacement START displaces whatever was staged and binds the new owner, exactly as the
     * donors do: START carries no ownership test, only DATA and END do. */
    od_nfc_reset();
    s_nfc.owner = ctx->rp;
    s_nfc.rec_type = type;
    s_nfc.total_len = total;
    s_nfc.active = true;
    return ack_armed(ctx);
}

static od_cmd_result_t handle_data(const od_cmd_ctx_t *ctx, od_span_t body)
{
    od_span_t sub;
    od_span_t chunk;

    /* Ownership before shape: a foreign frame must not learn whether its guess was well-formed,
     * and must mutate nothing either way (N1). */
    if (!owned_by(ctx)) {
        return nack(ctx, NFC_ERR_CHUNK_NO_START);
    }
    if (!od_span_split(body, 1u, &sub, &chunk) || od_span_is_empty(chunk)) {
        return nack(ctx, NFC_ERR_MALFORMED);
    }
    if ((uint32_t)s_nfc.received_len + (uint32_t)chunk.n > (uint32_t)s_nfc.total_len) {
        od_nfc_reset();
        return nack(ctx, NFC_ERR_CHUNK_OVERFLOW);
    }
    memcpy(&s_nfc.data[s_nfc.received_len], chunk.p, chunk.n);
    s_nfc.received_len = (uint16_t)(s_nfc.received_len + chunk.n);
    return ack_armed(ctx);
}

static od_cmd_result_t handle_end(const od_cmd_ctx_t *ctx)
{
    if (!owned_by(ctx)) {
        return nack(ctx, NFC_ERR_CHUNK_NO_START);
    }
    /* RETRYABLE, and deliberately so: the assembly survives a short END so a client that lost a
     * DATA can send the rest and commit. Deployed behaviour on both targets and both donors. */
    if (s_nfc.received_len != s_nfc.total_len) {
        return nack(ctx, NFC_ERR_END_LEN_MISMATCH);
    }
    if (!od_nfc_app_write(s_nfc.rec_type, s_nfc.data, s_nfc.total_len)) {
        od_nfc_reset();
        return nack(ctx, NFC_ERR_TAG_WRITE_FAILED);
    }
    od_nfc_reset();
    return ack_committed(ctx, NFC_STATUS_WRITE_COMMITTED);
}

od_cmd_result_t od_nfc_frame(const od_cmd_ctx_t *ctx, od_span_t body)
{
    if (ctx == NULL) {
        return OD_CMD_NACK;
    }
    if (body.p == NULL || body.n < 1u) {
        return nack(ctx, NFC_ERR_MALFORMED);
    }
    switch (body.p[0]) {
    case NFC_SUB_READ:        return handle_read(ctx);
    case NFC_SUB_WRITE:       return handle_inline_write(ctx, body);
    case NFC_SUB_WRITE_START: return handle_start(ctx, body);
    case NFC_SUB_WRITE_DATA:  return handle_data(ctx, body);
    case NFC_SUB_WRITE_END:   return handle_end(ctx);
    default:                  return nack(ctx, NFC_ERR_UNKNOWN_SUBCMD);
    }
}

#else /* !OD_CAP_NFC */

/* SILENT, AND UNKNOWN. This target implements no NFC, and manufacturing an "unsupported" error
 * would invent a wire meaning the client cannot distinguish from firmware older than the command
 * -- which is exactly how py-opendisplay detects absence. UNKNOWN also keeps the frame out of the
 * activity stamp, so probing 0x0083 cannot hold the exclusive link open.
 *
 * Both symbols still exist: dispatch and od_core_reset() name them. No state, no seam. */
od_cmd_result_t od_nfc_frame(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)ctx;
    (void)body;
    return OD_CMD_UNKNOWN;
}

void od_nfc_reset(void)
{
}

#endif /* OD_CAP_NFC */
