/* od_gate.c -- see od_gate.h. */

#include "od_gate.h"

#include "od_reply.h"
#include "od_sec2.h"
#include "od_sec2_app.h"

/* [00][cmd_lo][FE] SESSION_REQUIRED. Always plaintext: the peer has no session to read a sealed
 * one with. */
static od_gate_result_t refuse_session(od_tx_reservation_t *r, const od_reply_t *rp, uint16_t cmd)
{
    od_gate_result_t g;
    uint8_t f[3];

    f[0] = RESP_ACK;
    f[1] = (uint8_t)cmd;
    f[2] = RESP_AUTH_REQUIRED;
    (void)od_reply_plain(r, rp, f, sizeof f);
    g.outcome = OD_FRAME_AUTH_REQUIRED;
    g.body = od_span_none();
    g.sealed = false;
    return g;
}

/* [FF][cmd_lo][code] through od_reply: sealed when a session carries the connection, plaintext on
 * an unclaimed device. Used for FD and the record-layer NACKs F1-F3 / 00. */
static od_gate_result_t refuse(od_tx_reservation_t *r, const od_reply_t *rp, uint16_t cmd,
                               uint8_t code, od_frame_outcome_t outcome)
{
    od_gate_result_t g;
    uint8_t f[3];

    f[0] = RESP_NACK;
    f[1] = (uint8_t)cmd;
    f[2] = code;
    (void)od_reply(r, rp, f, sizeof f);
    g.outcome = outcome;
    g.body = od_span_none();
    g.sealed = false;
    return g;
}

static od_gate_result_t silent(void)
{
    od_gate_result_t g;

    g.outcome = OD_FRAME_CRYPTO_DROPPED;
    g.body = od_span_none();
    g.sealed = false;
    return g;
}

bool od_gate_claim_mode_allows(uint16_t cmd, bool fresh, bool config_locked, bool claimed)
{
    switch (cmd) {
    case CMD_REBOOT:
    case CMD_CONFIG_READ:
    case CMD_FIRMWARE_VERSION:
    case CMD_READ_MSD:
    case CMD_READ_IDENT:
    case CMD_AUTHENTICATE:
    case CMD_SEC_STATUS:
    case CMD_SEC_CLAIM:
        return true;
    case CMD_CONFIG_WRITE:
    case CMD_CONFIG_CHUNK:
    case CMD_CONFIG_CLEAR:
        return claimed || (fresh && !config_locked);
    default:
        return false;
    }
}

bool od_gate_no_ack(uint16_t cmd)
{
    return cmd == CMD_PIPE_WRITE_DATA || cmd == CMD_DEEP_SLEEP;
}

/* Claim-mode refusal (FD) or admission of an already opened / plaintext body. */
static od_gate_result_t admit(od_tx_reservation_t *r, const od_reply_t *rp, uint16_t cmd,
                              od_span_t body, bool sealed)
{
    struct od_sec2_claim_mode cm;
    od_gate_result_t g;

    od_sec2_app_claim_mode(&cm);
    if (cm.active && !od_gate_claim_mode_allows(cmd, cm.fresh, cm.config_locked,
                                                sealed && od_sec2_state() == OD_SEC2_CLAIMED)) {
        return refuse(r, rp, cmd, OD_GATE_CLAIM_MODE, OD_FRAME_HANDLER_NACK);
    }
    g.outcome = OD_FRAME_ACCEPTED;
    g.body = body;
    g.sealed = sealed;
    return g;
}

od_gate_result_t od_gate_admit(od_tx_reservation_t *r, const od_reply_t *rp, uint16_t cmd,
                               od_span_t body, uint8_t *scratch, size_t scratch_cap)
{
    const enum od_sec2_state st = od_sec2_state();
    const bool quiet = od_gate_no_ack(cmd);
    uint16_t n = 0u;
    enum od_sec2_open rc;

    if (st == OD_SEC2_LOCKED) {
        return refuse_session(r, rp, cmd);
    }
    if (st == OD_SEC2_UNCLAIMED) {
        return admit(r, rp, cmd, body, false);
    }
    if (rp->origin == OD_ORIGIN_LAN_TLS) {
        return admit(r, rp, cmd, body, false);
    }
    if (rp->origin != OD_ORIGIN_BLE || !od_sec2_session_live(rp)) {
        return refuse_session(r, rp, cmd);
    }

    rc = od_sec2_open(cmd, body, scratch, scratch_cap, &n);
    switch (rc) {
    case OD_SEC2_OPEN_OK:
        return admit(r, rp, cmd, od_span_make(scratch, n), true);
    case OD_SEC2_OPEN_NO_SESSION:
        return refuse_session(r, rp, cmd);
    case OD_SEC2_OPEN_RECORD:
        return quiet ? silent() : refuse(r, rp, cmd, OD_SEC_NACK_RECORD, OD_FRAME_CRYPTO_FAILED);
    case OD_SEC2_OPEN_REPLAY:
        return quiet ? silent() : refuse(r, rp, cmd, OD_SEC_NACK_REPLAY, OD_FRAME_CRYPTO_FAILED);
    case OD_SEC2_OPEN_BAD_TAG:
        return quiet ? silent() : refuse(r, rp, cmd, OD_SEC_NACK_INTEGRITY, OD_FRAME_CRYPTO_FAILED);
    case OD_SEC2_OPEN_BAD_TAG_TEARDOWN: {
        /* The third strike: its NACK still goes out under the session, then the session ends. */
        const od_gate_result_t g = quiet ? silent()
            : refuse(r, rp, cmd, OD_SEC_NACK_INTEGRITY, OD_FRAME_CRYPTO_FAILED);
        od_sec2_teardown();
        return g;
    }
    case OD_SEC2_OPEN_CRYPTO_ERROR:
    case OD_SEC2_OPEN_NO_ROOM:
        return quiet ? silent() : refuse(r, rp, cmd, 0x00u, OD_FRAME_CRYPTO_FAILED);
    }
    return refuse_session(r, rp, cmd);   /* no default above; unreachable */
}
