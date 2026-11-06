/* dispatch_test.c -- the ordering in od_dispatch.h is the specification; these are its cases.
 *
 * The bugs this guards against are all ORDERING bugs, and each has a specific consequence:
 *   reserve after the gate      -> the gate's own FE/FF answer has no slot and vanishes
 *   reserve after the handler   -> a handler mutates state and then cannot say so, which to the
 *                                  host is indistinguishable from the command never running
 *   defer after decrypt         -> the replay window advanced, so the re-dispatched frame is
 *                                  refused the second time and the transfer stalls
 *   producer conflict too late  -> a config write lands between two read chunks and splices two
 *                                  configs into one CRC-valid read-back
 */

#include "od_dispatch.h"

#include "od_caps.h"
#include "od_cmd_app.h"
#include "od_dispatch_ops.h"   /* the experimental opcode values the silabs fixture routes */
#include "od_log.h"
#include "od_pipe.h"
#include "od_config_read.h"
#include "od_reply.h"
#include "od_session.h"
#include "od_session_app.h"
#include "od_xfer.h"
#include "session_fake.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define LOG_CAP 32u
static struct {
    int level;
    char text[160];
} g_logs[LOG_CAP];
static unsigned g_log_count;

void _od_log(int level, const char *fmt, ...)
{
    va_list ap;

    if (g_log_count >= LOG_CAP) {
        return;
    }
    g_logs[g_log_count].level = level;
    va_start(ap, fmt);
    (void)vsnprintf(g_logs[g_log_count].text, sizeof g_logs[g_log_count].text, fmt, ap);
    va_end(ap);
    ++g_log_count;
}

static void logs_reset(void)
{
    memset(g_logs, 0, sizeof g_logs);
    g_log_count = 0u;
}

#if OD_CAP_LOG
static unsigned log_count_exact(int level, const char *text)
{
    unsigned count = 0u;
    unsigned i;

    for (i = 0u; i < g_log_count; ++i) {
        if (g_logs[i].level == level && strcmp(g_logs[i].text, text) == 0) {
            ++count;
        }
    }
    return count;
}
#endif

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond); \
        }                                                                      \
    } while (0)

#define CASE(name) (g_case = (name))

/* ------------------------------------------------------------------------------ fake radio --- */

#define SENT_MAX 64u
static struct { uint16_t len; uint8_t data[OD_TX_FRAME_MAX]; } g_sent[SENT_MAX];
static unsigned g_sent_n;
static bool     g_tag_live;

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                    const uint8_t *frame, uint16_t len)
{
    (void)origin; (void)tag;
    if (g_sent_n < SENT_MAX) {
        g_sent[g_sent_n].len = len;
        memcpy(g_sent[g_sent_n].data, frame, len);
        ++g_sent_n;
    }
    return OD_RADIO_SENT;
}

bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{
    (void)origin; (void)tag;
    return g_tag_live;
}

void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    (void)rp; (void)len; (void)why;
}

/* ------------------------------------------------------------------ fake od_session_app seam --- */

static struct od_session g_app_session;
static bool     g_security_on;
static uint32_t g_now_ms = 1000u;

struct od_session *od_session_app_state(void) { return &g_app_session; }
const struct SecurityConfig *od_session_app_security(void)
{
    return g_security_on ? &g_sec : NULL;
}
uint32_t od_session_app_now_ms(void) { return g_now_ms; }
void od_session_app_device_id(uint8_t out[OD_SESSION_DEVICE_ID_LEN])
{
    memcpy(out, DEVICE_ID, OD_SESSION_DEVICE_ID_LEN);
}
void od_session_app_report(enum od_session_app_op op, int result, uint16_t cmd,
                           const struct od_session_report *report)
{
    (void)op; (void)result; (void)cmd; (void)report;
}

/* --------------------------------------------------------------------------- fake handlers --- */

static unsigned        g_handler_calls;
static uint16_t        g_handler_cmd;
/* Wide enough for the largest UNENCRYPTED body the dispatcher can pass (its BLE ceiling minus the
 * two opcode bytes), not just the largest decrypted one. Sized to OD_SESSION_PLAIN_MAX, a mutation
 * that raises the ceiling overflowed this and corrupted the harness -- the defect was detected,
 * but by memory corruption rather than by an assertion. */
static uint8_t         g_handler_body[OD_TX_FRAME_MAX];
static uint16_t        g_handler_body_len;
static uint16_t        g_handler_wire_len;
static bool            g_handler_was_protected;
static od_cmd_result_t g_handler_result;
static uint8_t         g_handler_replies;   /* how many frames the fake handler emits */

/* ONE RECORDER BEHIND EVERY HOOK. This suite is about the ORDERING, not the map -- which opcode
 * reaches which hook is dispatch_route_test.c's question -- so every od_cmd_app hook lands here
 * and reports the opcode it stands for. The opcode is a constant per hook rather than a parameter,
 * because after C11 the hook IS the routing decision: a fake that took the opcode as an argument
 * would agree with itself however the shared switch were wired. */
static od_cmd_result_t handler(const od_cmd_ctx_t *ctx, uint16_t cmd, od_span_t body)
{
    const od_reply_t *rp = &ctx->rp;
    od_tx_reservation_t *r = ctx->r;
    uint8_t k;

    ++g_handler_calls;
    g_handler_cmd = cmd;
    g_handler_body_len = (uint16_t)body.n;
    g_handler_wire_len = ctx->wire_len;
    g_handler_was_protected = ctx->was_protected;
    if (body.n > 0u && body.p != NULL) {
        const size_t n = (body.n < sizeof g_handler_body) ? body.n : sizeof g_handler_body;
        memcpy(g_handler_body, body.p, n);
    }
    for (k = 0; k < g_handler_replies; ++k) {
        uint8_t frame[4];
        frame[0] = RESP_ACK;
        frame[1] = (uint8_t)(cmd & 0xFFu);
        frame[2] = k;
        frame[3] = 0x00u;
        (void)od_reply(r, rp, frame, sizeof frame);
    }
    return g_handler_result;
}

#define HOOK(fn, cmd)                                                    \
    od_cmd_result_t fn(const od_cmd_ctx_t *ctx, od_span_t body)          \
    { return handler(ctx, (cmd), body); }

HOOK(od_cmd_app_reboot,           CMD_REBOOT)
HOOK(od_cmd_app_firmware_version, CMD_FIRMWARE_VERSION)
HOOK(od_cmd_app_read_msd,         CMD_READ_MSD)
HOOK(od_cmd_app_enter_dfu,        CMD_ENTER_DFU)
HOOK(od_cmd_app_power_off,        CMD_POWER_OFF)
HOOK(od_cmd_app_deep_sleep,       CMD_DEEP_SLEEP)
HOOK(od_cmd_app_config_read,      CMD_CONFIG_READ)
HOOK(od_cmd_app_config_write,     CMD_CONFIG_WRITE)
HOOK(od_cmd_app_config_chunk,     CMD_CONFIG_CHUNK)
HOOK(od_cmd_app_config_clear,     CMD_CONFIG_CLEAR)
HOOK(od_xfer_direct_start,        CMD_DIRECT_WRITE_START)
HOOK(od_xfer_data,                CMD_DIRECT_WRITE_DATA)
HOOK(od_xfer_end,                 CMD_DIRECT_WRITE_END)
HOOK(od_xfer_partial_start,       CMD_PARTIAL_WRITE_START)
HOOK(od_pipe_start,               CMD_PIPE_WRITE_START)
HOOK(od_pipe_data,                CMD_PIPE_WRITE_DATA)
HOOK(od_pipe_end,                 CMD_PIPE_WRITE_END)
HOOK(od_cmd_app_led_activate,     CMD_LED_ACTIVATE)
HOOK(od_cmd_app_led_stop,         CMD_LED_STOP)
HOOK(od_cmd_app_buzzer,           CMD_BUZZER)
HOOK(od_nfc_frame,                CMD_NFC_ENDPOINT)

/* The silabs fixture routes the experimental image-store and action opcodes, so this
 * executable owes them definitions as much as any other row in the table. Gated on the same
 * capability as the dispatch rows, leaving the portable fixture unchanged. */
#if OD_CAP_IMGSTORE
HOOK(od_cmd_app_flash_info,       OD_CMD_FLASH_INFO)
HOOK(od_cmd_app_flash_list,       OD_CMD_FLASH_LIST)
HOOK(od_cmd_app_flash_erase,      OD_CMD_FLASH_ERASE)
HOOK(od_cmd_app_flash_preload,    OD_CMD_FLASH_PRELOAD)
HOOK(od_cmd_app_flash_display,    OD_CMD_FLASH_DISPLAY)
HOOK(od_cmd_app_action_info,      OD_CMD_ACTION_INFO)
HOOK(od_cmd_app_action_fire,      OD_CMD_ACTION_FIRE)
#endif

/* Same reason, for the config-driven drive path. This fixture asserts that a routed opcode
 * reaches its hook with the right budget, not what the hook does with the panel. */
#if OD_CAP_DRIVE_WRITE
HOOK(od_cmd_app_drive_write_start, CMD_DRIVE_WRITE_START)
#endif

static bool g_mutates_config;
bool od_cmd_mutates_config(uint16_t cmd)
{
    (void)cmd;
    return g_mutates_config;
}

/* The key-loss exemption, off unless a case turns it on. Defaulting it OFF is deliberate: every
 * existing case asserts the gate's behaviour, and a fake that exempted by default would quietly
 * make them assert nothing. */
static bool g_allow_unauthenticated;

bool od_cmd_allow_unauthenticated(uint16_t cmd)
{
    return g_allow_unauthenticated && cmd == CMD_CONFIG_WRITE;
}

/* --------------------------------------------------------------------------------- helpers --- */

static const od_reply_t BLE = { OD_ORIGIN_BLE, 9u };
/* SECTION 9 rule 4: TLS-PSK LAN is authenticated and encrypted by the transport, so its frames
 * carry no CCM envelope and must not meet the session gate. Tag 0 is what wifi_service publishes
 * when the link owner is not OWNER_LAN, and od_hal_radio_tag_is_live treats it as live. */
static const od_reply_t LAN_TLS = { OD_ORIGIN_LAN_TLS, 0u };
static const od_reply_t LAN_PLAIN = { OD_ORIGIN_LAN_PLAIN, 0u };
static uint8_t g_blob[1024];

static void setup(bool security_on, bool open_session)
{
    uint8_t server_nonce[16];

    fake_reset();
    sec_init(0);
    memset(&g_app_session, 0, sizeof g_app_session);
    od_session_init(&g_app_session, 0);
    od_config_read_cancel();
    od_txq_reset();
    memset(g_sent, 0, sizeof g_sent);
    g_sent_n = 0u;
    g_tag_live = true;
    g_security_on = security_on;
    g_handler_calls = 0u;
    g_handler_cmd = 0u;
    g_handler_body_len = 0u;
    g_handler_wire_len = 0u;
    g_handler_was_protected = false;
    g_handler_result = OD_CMD_OK;
    g_handler_replies = 1u;
    g_mutates_config = false;
    memset(g_blob, 0x5Au, sizeof g_blob);

    if (open_session) {
        CHECK(handshake(&g_app_session, g_now_ms, server_nonce, false)
              == OD_SESSION_AUTH_ESTABLISHED);
    }
    logs_reset();
}

/* Build [cmd:2][sealed envelope] as the wire carries it. */
static uint16_t make_encrypted(uint16_t cmd, const uint8_t *payload, uint16_t n, uint8_t *out)
{
    uint8_t plain[OD_SESSION_PLAIN_FRAME_MAX];
    uint16_t sealed_len = 0u;

    plain[0] = (uint8_t)((cmd >> 8) & 0xFFu);
    plain[1] = (uint8_t)(cmd & 0xFFu);
    memcpy(plain + 2, payload, n);
    if (od_session_seal(&g_app_session, od_span_make(plain, (uint16_t)(n + 2u)),
                        out, OD_SESSION_SEALED_MAX, &sealed_len, g_now_ms, NULL)
        != OD_SESSION_SEAL_OK) {
        return 0u;
    }
    return sealed_len;
}

/* ----------------------------------------------------------------------------------- cases --- */

static void test_plaintext_path(void)
{
    uint8_t frame[6] = { 0x00u, 0x77u, 1u, 2u, 3u, 4u };

    CASE("security off: the body reaches the handler unchanged");
    setup(false, false);
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_ACCEPTED);
    CHECK(g_handler_calls == 1u);
    CHECK(g_handler_cmd == 0x0077u);
    CHECK(g_handler_body_len == 4u);
    CHECK(g_handler_wire_len == sizeof frame && !g_handler_was_protected);
    CHECK(memcmp(g_handler_body, frame + 2, 4u) == 0);
    CHECK(od_txq_reserved() == 0u);        /* the reservation was released either way */
}

static void test_encrypted_path(void)
{
    uint8_t wire[OD_SESSION_SEALED_MAX];
    const uint8_t payload[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };
    uint16_t n;

    CASE("security on: the handler sees decrypted plaintext");
    setup(true, true);
    n = make_encrypted(0x0077u, payload, sizeof payload, wire);
    CHECK(n > 0u);
    CHECK(od_dispatch_frame(&BLE, od_span_make(wire, n)) == OD_FRAME_ACCEPTED);
    CHECK(g_handler_calls == 1u);
    CHECK(g_handler_body_len == sizeof payload);
    CHECK(g_handler_wire_len == n && g_handler_was_protected);
    CHECK(memcmp(g_handler_body, payload, sizeof payload) == 0);

    CASE("security on with no session: AUTH_REQUIRED, and the handler never runs");
    setup(true, false);
    {
        uint8_t junk[40];
        memset(junk, 0xAAu, sizeof junk);
        junk[0] = 0x00u; junk[1] = 0x77u;
        CHECK(od_dispatch_frame(&BLE, od_span_make(junk, sizeof junk)) == OD_FRAME_AUTH_REQUIRED);
        CHECK(g_handler_calls == 0u);
    }

    /* THE TLS BYPASS, pinned in both directions. Losing it is silent in exactly the wrong way:
     * the device still answers, so it looks alive, and every ordinary command inside an
     * authenticated TLS session comes back AUTH_REQUIRED with no way for the host to tell that
     * from a genuine session problem. It also makes the TLS branch of the target's config-write
     * authorization unreachable. */
    CASE("TLS-LAN bypasses the session gate: PLAINTEXT reaches the handler with security on");
    setup(true, false);
    {
        uint8_t plain[6] = { 0x00u, 0x77u, 1u, 2u, 3u, 4u };
        CHECK(od_dispatch_frame(&LAN_TLS, od_span_make(plain, sizeof plain)) == OD_FRAME_ACCEPTED);
        CHECK(g_handler_calls == 1u);
        CHECK(g_handler_body_len == 4u);
        CHECK(g_handler_wire_len == sizeof plain && !g_handler_was_protected);
        CHECK(memcmp(g_handler_body, plain + 2, 4u) == 0);
    }

    CASE("and the bypass is TLS-ONLY: plaintext LAN still meets the gate");
    setup(true, false);
    {
        uint8_t plain[6] = { 0x00u, 0x77u, 1u, 2u, 3u, 4u };
        CHECK(od_dispatch_frame(&LAN_PLAIN, od_span_make(plain, sizeof plain)) ==
              OD_FRAME_AUTH_REQUIRED);
        CHECK(g_handler_calls == 0u);
    }
    g_allow_unauthenticated = false;
}

/* KEY-LOSS RECOVERY. A device whose host lost the session key answers AUTH_REQUIRED to everything
 * and is otherwise reachable only physically, so a target may exempt a config REWRITE. The whole
 * safety of that rests on the two boundaries below, and neither is obvious from the call site. */
static void test_unauthenticated_exemption(void)
{
    uint8_t plain[6] = { 0x00u, 0x41u, 1u, 2u, 3u, 4u };
    uint8_t other[6] = { 0x00u, 0x77u, 1u, 2u, 3u, 4u };
    uint8_t server_nonce[16];
    uint8_t wire[64];
    uint16_t n;

    CASE("an exempt opcode reaches the handler unauthenticated, with its PLAINTEXT body");
    setup(true, false);
    g_allow_unauthenticated = true;
    CHECK(od_dispatch_frame(&BLE, od_span_make(plain, sizeof plain)) == OD_FRAME_ACCEPTED);
    CHECK(g_handler_calls == 1u);
    CHECK(g_handler_body_len == 4u);
    CHECK(memcmp(g_handler_body, plain + 2, 4u) == 0);

    CASE("a NON-exempt opcode is still refused, so the exemption is per-opcode not per-device");
    setup(true, false);
    g_allow_unauthenticated = true;
    CHECK(od_dispatch_frame(&BLE, od_span_make(other, sizeof other)) == OD_FRAME_AUTH_REQUIRED);
    CHECK(g_handler_calls == 0u);

    /* THE BOUNDARY THAT MATTERS. With a session live the exemption must not be consulted at all,
     * or a peer could send the exempt opcode in the CLEAR mid-session and skip both the envelope
     * and the replay window -- turning a recovery path into an authentication bypass. */
    CASE("with a LIVE session the exemption is not consulted: plaintext is still refused");
    setup(true, false);
    g_allow_unauthenticated = true;
    CHECK(handshake(od_session_app_state(), g_now_ms, server_nonce, false) ==
          OD_SESSION_AUTH_ESTABLISHED);
    CHECK(od_dispatch_frame(&BLE, od_span_make(plain, sizeof plain)) != OD_FRAME_ACCEPTED);
    CHECK(g_handler_calls == 0u);

    CASE("and a SEALED exempt frame on that live session still decrypts normally");
    n = make_encrypted(0x0041u, plain + 2, 4u, wire);
    CHECK(n > 0u);
    CHECK(od_dispatch_frame(&BLE, od_span_make(wire, n)) == OD_FRAME_ACCEPTED);
    CHECK(g_handler_calls == 1u);
    CHECK(g_handler_body_len == 4u);

    g_allow_unauthenticated = false;
}

static void test_structural_and_liveness(void)
{
    uint8_t one[1] = { 0x00u };
    static uint8_t big[OD_BLE_MAX_FRAME + 8];
    uint8_t frame[4] = { 0x00u, 0x77u, 1u, 2u };

    memset(big, 0x11u, sizeof big);
    big[0] = 0x00u; big[1] = 0x77u;

    CASE("a frame with no opcode is rejected and answers nothing");
    setup(false, false);
    CHECK(od_dispatch_frame(&BLE, od_span_make(one, 1u)) == OD_FRAME_REJECTED_FRAME);
#if OD_CAP_LOG
    CHECK(log_count_exact(OD_LOG_WARN, "Command frame is too short (1 B)") == 1u);
    CHECK(od_dispatch_frame(&BLE, od_span_make(one, 1u)) == OD_FRAME_REJECTED_FRAME);
    CHECK(log_count_exact(OD_LOG_WARN, "Command frame is too short (1 B)") == 1u);
    g_now_ms += 5000u;
    CHECK(od_dispatch_frame(&BLE, od_span_make(one, 1u)) == OD_FRAME_REJECTED_FRAME);
    CHECK(log_count_exact(OD_LOG_WARN, "Command frame is too short (1 B)") == 2u);
#endif
    CHECK(g_handler_calls == 0u);
    (void)od_txq_process();
    CHECK(g_sent_n == 0u);                 /* nothing to echo, so nothing is said */

    /* ALL THREE BYTES, because checking only one is how the shape drifted. This case used to
     * assert byte 2 alone and passed while the dispatcher answered [00][cmd][FF] -- the ACK-family
     * shape, and the same three bytes a DECRYPT FAILURE produces. Both donors ship
     * [FF][cmd_lo][FE] and the corpus records it. */
    CASE("a BLE frame above 244 is refused by the DISPATCHER with the donors' [FF][cmd][FE]");
    setup(false, false);
    g_now_ms = 0u;
    CHECK(od_dispatch_frame(&BLE, od_span_make(big, 245u)) == OD_FRAME_REJECTED_FRAME);
#if OD_CAP_LOG
    CHECK(log_count_exact(OD_LOG_WARN,
                          "Command 0x0077 exceeds dispatch limit (245 > 244 B)") == 1u);
#endif
    CHECK(g_handler_calls == 0u);
    (void)od_txq_process();
    CHECK(g_sent_n == 1u);
    CHECK(g_sent[0].len == 3u);
    CHECK(g_sent[0].data[0] == RESP_NACK);
    CHECK(g_sent[0].data[1] == 0x77u);          /* the opcode echoed, low byte */
    CHECK(g_sent[0].data[2] == 0xFEu);
    /* And it must NOT be the decrypt-failure frame, which is the collision this shape avoids. */
    CHECK(!(g_sent[0].data[0] == RESP_ACK && g_sent[0].data[2] == RESP_NACK));
#if OD_CAP_LOG
    CHECK(od_dispatch_frame(&BLE, od_span_make(big, 245u)) == OD_FRAME_REJECTED_FRAME);
    CHECK(log_count_exact(OD_LOG_WARN,
                          "Command 0x0077 exceeds dispatch limit (245 > 244 B)") == 1u);
    g_now_ms = 5000u;
    CHECK(od_dispatch_frame(&BLE, od_span_make(big, 245u)) == OD_FRAME_REJECTED_FRAME);
    CHECK(log_count_exact(OD_LOG_WARN,
                          "Command 0x0077 exceeds dispatch limit (245 > 244 B)") == 2u);
#endif
    CASE("a dead tag is STALE_TAG and answers nothing");
    setup(false, false);
    g_tag_live = false;
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_STALE_TAG);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    CHECK(log_count_exact(OD_LOG_DEBUG,
                          "Command 0x0077 ignored because its reply target is stale") == 1u);
#endif
    CHECK(g_handler_calls == 0u);
    (void)od_txq_process();
    CHECK(g_sent_n == 0u);

    CASE("a missing reply target is an internal frame error");
    setup(false, false);
    CHECK(od_dispatch_frame(NULL, od_span_make(frame, sizeof frame)) == OD_FRAME_REJECTED_FRAME);
#if OD_CAP_LOG
    CHECK(log_count_exact(OD_LOG_ERROR, "Command frame has no reply target") == 1u);
#endif
}

static void test_reserve_precedes_the_handler(void)
{
    od_tx_reservation_t hog;
    uint8_t frame[4] = { 0x00u, 0x77u, 1u, 2u };
    uint8_t other[4] = { 0x00u, 0x78u, 1u, 2u };

    CASE("a full queue DEFERS and the handler never runs");
    /* This is the ordering property: a handler that mutates state and then cannot answer looks to
     * the host exactly like a command that never ran. DEFERRED, not refused -- the frame is good
     * and must be re-offered unchanged. */
    setup(false, false);
    CHECK(od_txq_reserve((uint8_t)(OD_TXQ_SLOTS - 1u), &hog) == OD_TXQ_OK);
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_DEFERRED);
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_DEFERRED);
    CHECK(od_dispatch_frame(&BLE, od_span_make(other, sizeof other)) == OD_FRAME_DEFERRED);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    CHECK(log_count_exact(OD_LOG_DEBUG,
                          "Command 0x0077 deferred for response capacity (budget=1)") == 1u);
#endif
    CHECK(g_handler_calls == 0u);
    od_txq_release(&hog);

    CASE("and once capacity returns the same frame dispatches normally");
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_ACCEPTED);
    CHECK(g_handler_calls == 1u);
}

static void test_budget_covers_the_worst_case(void)
{
    uint8_t pipe[4] = { 0x00u, 0x81u, 1u, 2u };
    uint8_t other[4] = { 0x00u, 0x77u, 1u, 2u };

    CASE("0x81 reservation follows the target PIPE capability");
    setup(false, false);
#if OD_CAP_PIPE
    g_handler_replies = 3u;
#else
    g_handler_replies = 1u;
#endif
    CHECK(od_dispatch_frame(&BLE, od_span_make(pipe, sizeof pipe)) == OD_FRAME_ACCEPTED);
    (void)od_txq_process();
    CHECK(g_sent_n == g_handler_replies);  /* none of them dropped for want of a slot */

    CASE("an ordinary opcode reserves one, and a second reply is refused rather than borrowed");
    setup(false, false);
    g_handler_replies = 2u;
    CHECK(od_dispatch_frame(&BLE, od_span_make(other, sizeof other)) == OD_FRAME_ACCEPTED);
    (void)od_txq_process();
    CHECK(g_sent_n == 1u);                 /* the overrun did not steal another dispatch's unit */

    CASE("EVERY multi-reply opcode is pinned, not just 0x81");
    /* The earlier version of this case tested 0x81 and one ordinary opcode and the plan claimed
     * the table was "already asserted per opcode". It was not -- and 0x82 was wrong: an explicit
     * PIPE END emits the tail SACK, the END ACK and the refresh status, which is three. At two the
     * last one was dropped after the panel had already refreshed. */
    {
        static const struct { uint16_t cmd; uint8_t replies; } BUDGETS[] = {
#if OD_CAP_PIPE
            { CMD_PIPE_WRITE_DATA,   3u },
            { CMD_PIPE_WRITE_END,    3u },
#else
            { CMD_PIPE_WRITE_DATA,   1u },
            { CMD_PIPE_WRITE_END,    1u },
#endif
            { CMD_DIRECT_WRITE_DATA, 2u },
            { CMD_DIRECT_WRITE_END,  2u },
            { CMD_BUZZER,            1u }
        };
        unsigned i;
        for (i = 0; i < sizeof BUDGETS / sizeof BUDGETS[0]; ++i) {
            uint8_t f[4];
            f[0] = (uint8_t)((BUDGETS[i].cmd >> 8) & 0xFFu);
            f[1] = (uint8_t)(BUDGETS[i].cmd & 0xFFu);
            f[2] = 1u;
            f[3] = 2u;
            setup(false, false);
            g_handler_replies = BUDGETS[i].replies;      /* exactly its worst case */
            CHECK(od_dispatch_frame(&BLE, od_span_make(f, sizeof f)) == OD_FRAME_ACCEPTED);
            (void)od_txq_process();
            CHECK(g_sent_n == BUDGETS[i].replies);       /* every one of them reached the radio */
        }
    }
}

static void test_producer_conflict_defers_before_decrypt(void)
{
    od_tx_reservation_t r;
    uint8_t wire[OD_SESSION_SEALED_MAX];
    uint8_t alternate[OD_SESSION_SEALED_MAX];
    const uint8_t payload[2] = { 1u, 2u };
    uint16_t n;
    uint64_t rx_before;

    CASE("a config write during an active read is DEFERRED, and the replay window does NOT move");
    /* Deferring after decrypt would advance rx_last, so the re-dispatched frame is refused the
     * second time and the transfer stalls -- the reason this check is above the gate. */
    setup(true, true);
    g_mutates_config = true;
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_config_read_start(&BLE, &r, g_blob, sizeof g_blob) == OD_TXQ_OK);
    CHECK(od_config_read_active());

    n = make_encrypted(0x0041u, payload, sizeof payload, wire);
    CHECK(n > 0u);
    rx_before = g_app_session.rx_last;
    CHECK(od_dispatch_frame(&BLE, od_span_make(wire, n)) == OD_FRAME_DEFERRED);
    CHECK(od_dispatch_frame(&BLE, od_span_make(wire, n)) == OD_FRAME_DEFERRED);
    memcpy(alternate, wire, n);
    alternate[1] ^= 0x01u;
    CHECK(od_dispatch_frame(&BLE, od_span_make(alternate, n)) == OD_FRAME_DEFERRED);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    CHECK(log_count_exact(OD_LOG_DEBUG,
                          "Command 0x0041 deferred while configuration read is active") == 1u);
#endif
    CHECK(g_handler_calls == 0u);
    CHECK(g_app_session.rx_last == rx_before);      /* untouched: it never reached the cipher */

    CASE("and the SAME frame dispatches once the read completes");
    od_config_read_cancel();
    g_mutates_config = true;
    CHECK(od_dispatch_frame(&BLE, od_span_make(wire, n)) == OD_FRAME_ACCEPTED);
    CHECK(g_handler_calls == 1u);

    CASE("a second CONFIG_READ during an active read is also deferred");
    setup(false, false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_config_read_start(&BLE, &r, g_blob, sizeof g_blob) == OD_TXQ_OK);
    {
        uint8_t rd[2];
        rd[0] = (uint8_t)((CMD_CONFIG_READ >> 8) & 0xFFu);
        rd[1] = (uint8_t)(CMD_CONFIG_READ & 0xFFu);
        CHECK(od_dispatch_frame(&BLE, od_span_make(rd, sizeof rd)) == OD_FRAME_DEFERRED);
        CHECK(g_handler_calls == 0u);
    }

    CASE("but an UNRELATED command is not blocked by a live read");
    {
        uint8_t other[4] = { 0x00u, 0x77u, 1u, 2u };
        CHECK(od_dispatch_frame(&BLE, od_span_make(other, sizeof other)) == OD_FRAME_ACCEPTED);
        CHECK(g_handler_calls == 1u);
    }
}

static void test_control_opcodes(void)
{
    uint8_t step1[3] = { 0x00u, 0x50u, 0x00u };
    uint8_t ver[2];

    ver[0] = (uint8_t)((CMD_FIRMWARE_VERSION >> 8) & 0xFFu);
    ver[1] = (uint8_t)(CMD_FIRMWARE_VERSION & 0xFFu);

    CASE("AUTHENTICATE is answered by the gate and never reaches a handler");
    setup(true, false);
    CHECK(od_dispatch_frame(&BLE, od_span_make(step1, sizeof step1)) == OD_FRAME_AUTH_CONTROL);
    CHECK(g_handler_calls == 0u);
    (void)od_txq_process();
    CHECK(g_sent[0].len == OD_SESSION_STEP1_REPLY_LEN);

    CASE("FIRMWARE_VERSION bypasses the session gate even with security on and no session");
    /* A client must be able to learn what it is talking to before it can authenticate, and a
     * device whose key the host lost would otherwise be unidentifiable. */
    setup(true, false);
    CHECK(od_dispatch_frame(&BLE, od_span_make(ver, sizeof ver)) == OD_FRAME_DISCOVERY);
    CHECK(g_handler_calls == 1u);
    CHECK(g_handler_cmd == CMD_FIRMWARE_VERSION);

    CASE("DISCOVERY is distinct from ACCEPTED, so a version poll cannot stamp activity");
    CHECK(OD_FRAME_DISCOVERY != OD_FRAME_ACCEPTED);
}

static void test_handler_results_map(void)
{
    uint8_t frame[4] = { 0x00u, 0x77u, 1u, 2u };

    CASE("a handler NACK is HANDLER_NACK, which still counts as activity");
    setup(false, false);
    g_handler_result = OD_CMD_NACK;
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_HANDLER_NACK);

    CASE("an unknown opcode is UNKNOWN_OPCODE, and therefore NOT activity");
    /* Folded into NACK it would stamp the owner clock, and unknown-command traffic could hold the
     * exclusive link indefinitely. The policy table gives UNKNOWN_OPCODE no stamp and no abuse
     * movement, so the two halves have to agree. */
    setup(false, false);
    g_handler_result = OD_CMD_UNKNOWN;
    g_now_ms = UINT32_MAX - 2u;
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_UNKNOWN_OPCODE);
#if OD_CAP_LOG
    CHECK(log_count_exact(OD_LOG_WARN, "Unknown command 0x0077") == 1u);
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_UNKNOWN_OPCODE);
    CHECK(log_count_exact(OD_LOG_WARN, "Unknown command 0x0077") == 1u);
    g_now_ms = 1u;
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_UNKNOWN_OPCODE);
    CHECK(log_count_exact(OD_LOG_WARN, "Unknown command 0x0077") == 1u);
    g_now_ms = 4997u;
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_UNKNOWN_OPCODE);
    CHECK(log_count_exact(OD_LOG_WARN, "Unknown command 0x0077") == 2u);
#endif
    {
        const od_frame_policy_t p = od_frame_policy(OD_FRAME_UNKNOWN_OPCODE);
        CHECK(!p.stamp_activity);
        CHECK(!p.reset_abuse);
        CHECK(!p.increment_abuse);
        CHECK(p.consume_rx);
    }

    CASE("a handler auth rejection is AUTH_REQUIRED, not a NACK");
    /* Without the distinction a TLS client's refused CONFIG_WRITE stamps activity and holds the
     * exclusive link forever. */
    setup(false, false);
    g_handler_result = OD_CMD_AUTH_REJECTED;
    CHECK(od_dispatch_frame(&BLE, od_span_make(frame, sizeof frame)) == OD_FRAME_AUTH_REQUIRED);
}

/* The section 5 policy table, row by row. Each row is a decision with a named consequence, and the
 * two targets got different rows wrong before it was written down -- so asserting the table is
 * asserting the decisions, not restating the code. */
static void test_outcome_policy_table(void)
{
    od_frame_policy_t p;

    CASE("ACCEPTED and HANDLER_NACK both stamp activity and clear the refusal run");
    /* A NACK is still a client talking correctly and getting a real answer. Not stamping would age
     * out a peer that works fine but happens to send commands this device rejects. */
    p = od_frame_policy(OD_FRAME_ACCEPTED);
    CHECK(p.stamp_activity && p.reset_abuse && !p.increment_abuse && p.consume_rx);
    p = od_frame_policy(OD_FRAME_HANDLER_NACK);
    CHECK(p.stamp_activity && p.reset_abuse && !p.increment_abuse && p.consume_rx);

    CASE("AUTH_ESTABLISHED clears the run but does NOT stamp");
    p = od_frame_policy(OD_FRAME_AUTH_ESTABLISHED);
    CHECK(!p.stamp_activity);
    CHECK(p.reset_abuse);
    CHECK(p.consume_rx);

    CASE("AUTH_CONTROL and DISCOVERY touch nothing -- a poll cannot hold the link");
    p = od_frame_policy(OD_FRAME_AUTH_CONTROL);
    CHECK(!p.stamp_activity && !p.reset_abuse && !p.increment_abuse && p.consume_rx);
    p = od_frame_policy(OD_FRAME_DISCOVERY);
    CHECK(!p.stamp_activity && !p.reset_abuse && !p.increment_abuse && p.consume_rx);

    CASE("AUTH_REQUIRED is the ONLY outcome that advances the run");
    p = od_frame_policy(OD_FRAME_AUTH_REQUIRED);
    CHECK(p.increment_abuse);
    CHECK(!p.stamp_activity && !p.reset_abuse);
    {
        /* Enumerated rather than asserted in prose: exactly one row may increment. */
        const od_frame_outcome_t ALL[] = {
            OD_FRAME_ACCEPTED, OD_FRAME_HANDLER_NACK, OD_FRAME_AUTH_CONTROL,
            OD_FRAME_AUTH_ESTABLISHED, OD_FRAME_DISCOVERY, OD_FRAME_AUTH_REQUIRED,
            OD_FRAME_CRYPTO_FAILED, OD_FRAME_CRYPTO_DROPPED, OD_FRAME_REJECTED_FRAME,
            OD_FRAME_UNKNOWN_OPCODE, OD_FRAME_STALE_TAG, OD_FRAME_DEFERRED
        };
        unsigned i, inc = 0u, no_consume = 0u;
        for (i = 0; i < sizeof ALL / sizeof ALL[0]; ++i) {
            const od_frame_policy_t q = od_frame_policy(ALL[i]);
            if (q.increment_abuse) { ++inc; }
            if (!q.consume_rx) { ++no_consume; }
        }
        CHECK(inc == 1u);
        CHECK(no_consume == 1u);
    }

    CASE("neither crypto outcome touches the run -- od_session already applied its own strike");
    p = od_frame_policy(OD_FRAME_CRYPTO_FAILED);
    CHECK(!p.increment_abuse && !p.reset_abuse && !p.stamp_activity);
    p = od_frame_policy(OD_FRAME_CRYPTO_DROPPED);
    CHECK(!p.increment_abuse && !p.reset_abuse && !p.stamp_activity);

    CASE("DEFERRED is the one outcome that does not consume the frame");
    p = od_frame_policy(OD_FRAME_DEFERRED);
    CHECK(!p.consume_rx);
    CHECK(!p.stamp_activity && !p.reset_abuse && !p.increment_abuse);
}

int main(void)
{
    test_plaintext_path();
    test_encrypted_path();
    test_unauthenticated_exemption();
    test_structural_and_liveness();
    test_reserve_precedes_the_handler();
    test_budget_covers_the_worst_case();
    test_producer_conflict_defers_before_decrypt();
    test_control_opcodes();
    test_handler_results_map();
    test_outcome_policy_table();

    printf("dispatch: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
