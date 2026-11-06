/* od_dispatch.c -- see od_dispatch.h; the ordering comment there is the specification. */

#include "od_dispatch.h"

#include "od_cmd_app.h"
#include "od_dispatch_ops.h"
#include "od_config_read.h"
#include "od_gate.h"
#include "od_log.h"
#include "od_log_budget.h"
#include "od_nfc.h"
#include "od_pipe.h"
#include "od_reply.h"
#include "od_session.h"
#include "od_session_app.h"
#include "od_xfer.h"

#include <string.h>

/* THE DECRYPT SCRATCH, owned here. One buffer for the whole command path: the gate decrypts into
 * it and the body handed to a handler points into it, so it must outlive the handler and must not
 * be shared with anything that runs inside one. */
static uint8_t s_plain[OD_SESSION_PLAIN_MAX];

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
/* A deferred frame is re-offered on every loop pass. One flag per cause keeps the first command
 * visible without turning an episode -- including alternating opcodes -- into per-frame output. */
static bool s_config_defer_logged;
static bool s_capacity_defer_logged;

static void log_config_deferred(uint16_t cmd)
{
    if (!s_config_defer_logged) {
        od_log_debug("Command 0x%04X deferred while configuration read is active",
                     (unsigned)cmd);
        s_config_defer_logged = true;
    }
}

static void log_capacity_deferred(uint16_t cmd, uint8_t budget)
{
    if (!s_capacity_defer_logged) {
        od_log_debug("Command 0x%04X deferred for response capacity (budget=%u)",
                     (unsigned)cmd, (unsigned)budget);
        s_capacity_defer_logged = true;
    }
}

static void clear_config_deferred(void) { s_config_defer_logged = false; }
static void clear_capacity_deferred(void) { s_capacity_defer_logged = false; }
#else
#define log_config_deferred(cmd_)          ((void)(cmd_))
#define log_capacity_deferred(cmd_, budget_) ((void)(cmd_), (void)(budget_))
#define clear_config_deferred()            ((void)0)
#define clear_capacity_deferred()          ((void)0)
#endif

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_WARN
#define OD_DISPATCH_WARN_INTERVAL_MS 5000u

/* The peer controls all three malformed-input paths. Separate budgets prevent one class from
 * silencing another; `armed` keeps uptime zero from looking like an unused timestamp. */
static od_log_budget_t s_short_budget;
static od_log_budget_t s_oversize_budget;
static od_log_budget_t s_unknown_budget;

static bool warning_allowed(od_log_budget_t *budget)
{
    return od_log_budget_allows(budget, od_session_app_now_ms(),
                                OD_DISPATCH_WARN_INTERVAL_MS);
}

static void log_short_frame(unsigned len)
{
    if (warning_allowed(&s_short_budget)) {
        od_log_warn("Command frame is too short (%u B)", (unsigned)len);
    }
}

static void log_oversize_frame(uint16_t cmd, unsigned len, uint16_t ceiling)
{
    if (warning_allowed(&s_oversize_budget)) {
        od_log_warn("Command 0x%04X exceeds dispatch limit (%u > %u B)", (unsigned)cmd,
                    (unsigned)len, (unsigned)ceiling);
    }
}

static void log_unknown_command(uint16_t cmd)
{
    if (warning_allowed(&s_unknown_budget)) {
        od_log_warn("Unknown command 0x%04X", (unsigned)cmd);
    }
}
#else
#define log_short_frame(len_)                    ((void)(len_))
#define log_oversize_frame(cmd_, len_, ceiling_) \
    ((void)(cmd_), (void)(len_), (void)(ceiling_))
#define log_unknown_command(cmd_)                ((void)(cmd_))
#endif

/* Dispatcher ceilings, per origin. BLE's is below the 253-byte value admission so the
 * 245..253 band is a dispatcher NACK rather than a silent transport drop -- a host can discover
 * the first and cannot discover the second. */
#define OD_DISPATCH_MAX_BLE  244u
#define OD_DISPATCH_MAX_LAN  4094u

/* How many response slots one opcode may need, worst case. Under-reserving is the bug that matters:
 * a handler that mutates state and then cannot answer looks to the host exactly like a command
 * that never ran.
 *
 * 0x81 is 3 because od_pipe_data can emit the data ACK, the END ack AND the refresh status
 * in one call. 0x71 is 2 because it emits the data ack OR calls END, never both. The END opcodes
 * are 2: their own ack, then the post-refresh status -- and that reservation is HELD ACROSS THE
 * REFRESH, which is free here because capacity is a counter rather than a parked slot. */
uint8_t od_dispatch_budget(uint16_t cmd)
{
    switch (cmd) {
#define OD_BUDGET_CASE(cmd_, hook_, budget_) case cmd_: return (uint8_t)(budget_);
        OD_DISPATCH_OPCODE_ROWS(OD_BUDGET_CASE)
#undef OD_BUDGET_CASE
    case CMD_AUTHENTICATE:
    case CMD_FIRMWARE_VERSION:
    case CMD_READ_IDENT:
    default:
        return 1u;
    }
}

/* Per-row capacity checks, generated from the same source as route and budget. A named typedef per
 * hook is C99-safe and unique even under the canonical header's pre-C11 static-assert fallback. */
#define OD_ASSERT_ROW_FITS(cmd_, hook_, budget_)                                      \
    typedef char od_dispatch_budget_fits_##hook_[                                     \
        ((budget_) <= (OD_TXQ_SLOTS - 1u)) ? 1 : -1];
OD_DISPATCH_OPCODE_ROWS(OD_ASSERT_ROW_FITS)
#undef OD_ASSERT_ROW_FITS
typedef char od_dispatch_budget_fits_authenticate[(1u <= (OD_TXQ_SLOTS - 1u)) ? 1 : -1];
typedef char od_dispatch_budget_fits_firmware_version[(1u <= (OD_TXQ_SLOTS - 1u)) ? 1 : -1];

/* KEY-LOSS RECOVERY, and the ORDER of these two tests is the whole safety argument.
 *
 * The liveness question comes FIRST, so the target is asked only about a frame that has no session
 * behind it. On a live session every command still meets the gate, so this can never be used to
 * skip decryption or to slip a plaintext command past the replay window mid-session. It is
 * reachable exactly when the device would otherwise be permanently unreachable.
 *
 * od_session_alive() rather than od_session_authenticated(): the MUTATING form, which tears down an
 * expired session as it answers. The pure predicate would leave an expired-but-uncleared session
 * looking authenticated, and the exemption would then be judged against a session already dead. */
static bool unauthenticated_exempt(uint16_t cmd)
{
    if (od_session_alive(od_session_app_state(), od_session_app_now_ms(), NULL)) {
        return false;
    }
    return od_cmd_allow_unauthenticated(cmd);
}

static uint16_t origin_ceiling(od_origin_t origin)
{
    return (origin == OD_ORIGIN_BLE) ? OD_DISPATCH_MAX_BLE : OD_DISPATCH_MAX_LAN;
}

/* THE OVERSIZE REFUSAL, and its bytes are the two donors' rather than this dispatcher's invention:
 * [0xFF][cmd_lo][0xFE]. Firmware_NRF54 (opendisplay_pipe.c:1366) and Firmware_Silabs
 * (opendisplay_pipe.c:1235) both ship exactly that, and DIVERGENCE 1.3 records it.
 *
 * THE SHAPE IS LOAD-BEARING TWICE OVER. A leading 0xFF is the hard-NACK family; a leading 0x00 is
 * the "status carried as data" ACK family (DIVERGENCE 1.1/1.2). And [00][cmd][FF] is ALREADY
 * taken -- it is the decrypt-failure answer -- so emitting it here would leave a host unable to
 * tell "your frame was too big" from "your frame failed to authenticate". Two meanings, one byte
 * string, on a path a host reaches by sending 245..253 bytes.
 *
 * Reserves for itself: this refuses before any budget was claimed, where a one-slot reserve is the
 * whole cost. */
static void refuse_oversize(const od_reply_t *rp, uint16_t cmd)
{
    od_tx_reservation_t r;
    uint8_t frame[3];

    if (od_txq_reserve(1u, &r) != OD_TXQ_OK) {
        return;                       /* nothing to say it with; the host times out */
    }
    frame[0] = RESP_NACK;
    frame[1] = (uint8_t)(cmd & 0xFFu);
    /* 0xFE, and deliberately NOT spelled RESP_AUTH_REQUIRED even though the value matches: this is
     * an error byte inside a hard NACK, not the [00][cmd][FE] auth-required frame. The canonical
     * header names no constant for "frame too long" and is frozen, so the literal stays with this
     * note rather than borrowing a name that means something else. */
    frame[2] = 0xFEu;
    (void)od_reply_plain(&r, rp, frame, sizeof frame);
    od_txq_release(&r);
}

/* THE OPCODE MAP, and it is here rather than in each target for a reason that is not tidiness.
 * Two copies of this switch is how one target answers an opcode the other treats as unknown -- and
 * "unknown" is wire-visible: od_frame_policy() gives OD_FRAME_UNKNOWN_OPCODE no activity stamp, so
 * a recognised opcode holds an exclusive link open where an unrecognised one does not. A target
 * supplies target-specific command behaviour through od_cmd_app.h and hardware behaviour through
 * subsystem seams such as od_xfer_app.h; it does not get to invent the routing.
 *
 * Target-owned rows require every target to define their hook, so an incomplete addition is a
 * link error. Promoted rows name their shared state machine directly; capability-off behaviour is
 * compiled into that shared implementation.
 *
 * AUTHENTICATE and FIRMWARE_VERSION are absent: both are routed by od_dispatch_frame() before this
 * point, the first into od_gate and the second pre-gate. */
static od_cmd_result_t dispatch_plain(const od_cmd_ctx_t *ctx, uint16_t cmd, od_span_t body)
{
    switch (cmd) {
#define OD_ROUTE_CASE(cmd_, hook_, budget_) case cmd_: return hook_(ctx, body);
        OD_DISPATCH_OPCODE_ROWS(OD_ROUTE_CASE)
#undef OD_ROUTE_CASE
    default:
        return OD_CMD_UNKNOWN;
    }
}

/* Turn a handler's verdict into the dispatcher's conclusion. One place, because the mapping is
 * policy: UNKNOWN is NOT folded into NACK -- an unrecognised opcode must not stamp activity, or
 * unknown-command traffic keeps an exclusive link alive. */
static od_frame_outcome_t outcome_of(uint16_t cmd, od_cmd_result_t rc)
{
    switch (rc) {
    case OD_CMD_OK:            return OD_FRAME_ACCEPTED;
    case OD_CMD_NACK:          return OD_FRAME_HANDLER_NACK;
    case OD_CMD_AUTH_REJECTED: return OD_FRAME_AUTH_REQUIRED;
    case OD_CMD_UNKNOWN:
        log_unknown_command(cmd);
        return OD_FRAME_UNKNOWN_OPCODE;
    }
    /* No default above, so -Wswitch fails the build on a new od_cmd_result_t. Unreachable. */
    return OD_FRAME_HANDLER_NACK;
}

od_frame_outcome_t od_dispatch_frame(const od_reply_t *rp, od_span_t frame)
{
    od_tx_reservation_t r;
    od_frame_outcome_t outcome;
    od_span_t body;
    bool was_protected = false;
    uint16_t ceiling;
    uint16_t cmd;

    if (rp == NULL) {
        od_log_error("Command frame has no reply target");
        return OD_FRAME_REJECTED_FRAME;
    }
    if (!od_span_has(frame, 2u)) {
        log_short_frame((unsigned)frame.n);
        return OD_FRAME_REJECTED_FRAME;   /* no opcode: nothing to echo, nothing to answer */
    }
    cmd = (uint16_t)(((uint16_t)frame.p[0] << 8) | frame.p[1]);
    body = od_span_drop(frame, 2u);
    ceiling = origin_ceiling(rp->origin);

    /* ---- structural ---- */
    if (frame.n > ceiling) {
        /* Refused by the DISPATCHER, deliberately, rather than dropped by the transport: the band
         * between this and the 253-byte value admission tells a host its frame was too big. */
        log_oversize_frame(cmd, (unsigned)frame.n, ceiling);
        refuse_oversize(rp, cmd);
        return OD_FRAME_REJECTED_FRAME;
    }

    /* ---- tag liveness ---- */
    if (!od_hal_radio_tag_is_live(rp->origin, rp->tag)) {
        /* The peer that sent this is gone. Answering would deliver to whoever inherited the slot. */
        od_log_debug("Command 0x%04X ignored because its reply target is stale", (unsigned)cmd);
        return OD_FRAME_STALE_TAG;
    }

    /* ---- producer conflict, BEFORE reserve and BEFORE decrypt ----
     * A live CONFIG_READ owns the config scratch, and a write reloads through the same buffer;
     * letting one land between two chunks splices two configs into one CRC-valid read-back.
     * Deferring here leaves the replay window and the caller's bytes untouched, which is the whole
     * reason this check cannot move below the gate. */
    if (od_config_read_active() &&
        (cmd == CMD_CONFIG_READ || od_cmd_mutates_config(cmd))) {
        log_config_deferred(cmd);
        return OD_FRAME_DEFERRED;
    }
    if (!od_config_read_active()) {
        clear_config_deferred();
    }

    /* ---- budget and reserve ---- */
    {
        const uint8_t budget = od_dispatch_budget(cmd);

        if (od_txq_reserve(budget, &r) != OD_TXQ_OK) {
            /* No capacity to answer, so the handler must not run: it would mutate state and then
             * be unable to say so. Deferred rather than refused -- the frame is still good. */
            log_capacity_deferred(cmd, budget);
            return OD_FRAME_DEFERRED;
        }
    }
    clear_capacity_deferred();

    /* ---- gate ----
     * Everything below has a slot, including the gate's own FE/FF answers. */
    if (cmd == CMD_AUTHENTICATE) {
        outcome = od_gate_authenticate(&r, rp, body);
        od_txq_release(&r);
        return outcome;
    }

    /* FIRMWARE_VERSION IS ALWAYS PLAINTEXT-READABLE, exempt from the session gate on every target
     * (Firmware communication.cpp:488). A client must be able to learn what it is talking to
     * before it can authenticate -- and a device whose key the host has lost would otherwise be
     * unidentifiable. DISCOVERY rather than ACCEPTED, so a version poll cannot stamp activity and
     * hold the exclusive link indefinitely. */
    if (cmd == CMD_FIRMWARE_VERSION) {
        const od_cmd_ctx_t ctx = { *rp, &r, (uint16_t)frame.n, false };
        const od_cmd_result_t rc = od_cmd_app_firmware_version(&ctx, body);
        od_txq_release(&r);
        return (rc == OD_CMD_OK) ? OD_FRAME_DISCOVERY : outcome_of(cmd, rc);
    }

#if OD_CAP_READ_IDENT
    /* READ_IDENT is pre-gate for the same reason (protocol 2.3: "never auth-gated"): a host
     * picks the panel-database entry from it before it knows which key applies. It carries
     * only what DisplayConfig already exposes -- panel type, geometry, colour scheme. */
    if (cmd == CMD_READ_IDENT) {
        const od_cmd_ctx_t ctx = { *rp, &r, (uint16_t)frame.n, false };
        const od_cmd_result_t rc = od_cmd_app_read_ident(&ctx, body);
        od_txq_release(&r);
        return (rc == OD_CMD_OK) ? OD_FRAME_DISCOVERY : outcome_of(cmd, rc);
    }
#endif

    /* ORIGIN-GATED DECRYPT, SECTION 9 rule 4. A frame on the TLS-PSK LAN channel is already
     * confidential and authenticated by TLS and carries NO CCM envelope, so it must not meet this
     * gate: od_session_open() would read its plaintext as a nonce and refuse it, and every ordinary
     * command inside an authenticated TLS session would answer AUTH_REQUIRED.
     *
     * The decision belongs here and cannot be pushed into od_session, which deliberately takes no
     * origin -- see od_session.h. Applying the session to a TLS frame would also advance the replay
     * counter on traffic the window never authenticated. */
    if (rp->origin != OD_ORIGIN_LAN_TLS &&
        od_session_security_enabled(od_session_app_security()) &&
        !unauthenticated_exempt(cmd)) {
        const od_gate_result_t g = od_gate_open(&r, rp, cmd, body, s_plain, sizeof s_plain);
        if (g.outcome != OD_FRAME_ACCEPTED) {
            od_txq_release(&r);
            return g.outcome;
        }
        body = g.body;
        was_protected = true;
    }

    /* ---- handler ---- */
    {
        const od_cmd_ctx_t ctx = { *rp, &r, (uint16_t)frame.n, was_protected };
        const od_cmd_result_t rc = dispatch_plain(&ctx, cmd, body);
        od_txq_release(&r);
        return outcome_of(cmd, rc);
    }
}
