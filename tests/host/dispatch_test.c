/* dispatch_test.c -- the ordering in od_dispatch.h is the specification; these are its cases.
 *
 * The bugs this guards against are all ORDERING bugs, and each has a specific consequence:
 *   reserve after the gate      -> the gate's own FE/FF answer has no slot and vanishes
 *   reserve after the handler   -> a handler mutates state and then cannot say so, which to the
 *                                  host is indistinguishable from the command never running
 *   defer after record open     -> the replay window advanced, so the re-dispatched frame is
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
#include "od_sec2.h"
#include "od_sec2_app.h"
#include "od_xfer.h"
#include "sec2_client.h"
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
static unsigned g_pop;
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

bool sec2c_radio_pop(uint8_t *buf, uint16_t *len)
{
    if (g_pop >= g_sent_n) {
        return false;
    }
    memcpy(buf, g_sent[g_pop].data, g_sent[g_pop].len);
    *len = g_sent[g_pop].len;
    ++g_pop;
    return true;
}

/* ------------------------------------------------------------------------- od_sec2_app hooks --- */

static uint32_t g_now_ms = 1000u;

uint32_t od_sec2_app_now_ms(void) { return g_now_ms; }
void od_sec2_app_uid(uint8_t uid[8]) { memcpy(uid, UID, 8u); }
uint8_t od_sec2_app_dflags(void) { return OD_SEC2_DFLAG_HAS_PANEL; }
const struct SecurityConfig *od_sec2_app_security(void) { return &g_sec; }

/* --------------------------------------------------------------------------- fake handlers --- */

static unsigned        g_handler_calls;
static uint16_t        g_handler_cmd;
/* Wide enough for the largest UNENCRYPTED body the dispatcher can pass (its BLE ceiling minus the
 * two opcode bytes), not just the largest opened record; a ceiling mutation must fail an
 * assertion, not corrupt the harness. */
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
HOOK(od_cmd_app_flash_info,       CMD_FLASH_INFO)
HOOK(od_cmd_app_flash_list,       CMD_FLASH_LIST)
HOOK(od_cmd_app_flash_erase,      CMD_FLASH_ERASE)
HOOK(od_cmd_app_flash_preload,    CMD_FLASH_PRELOAD)
HOOK(od_cmd_app_flash_display,    CMD_FLASH_DISPLAY)
HOOK(od_cmd_app_action_info,      CMD_ACTION_INFO)
HOOK(od_cmd_app_action_fire,      CMD_ACTION_FIRE)
#endif

/* Same reason, for the config-driven drive path. This fixture asserts that a routed opcode
 * reaches its hook with the right budget, not what the hook does with the panel. */
#if OD_CAP_DRIVE_WRITE
HOOK(od_cmd_app_drive_write_start, CMD_DRIVE_WRITE_START)
#endif

#if OD_CAP_SPI_RAW
HOOK(od_cmd_app_spi_raw,           CMD_SPI_RAW)
#endif

#if OD_CAP_READ_IDENT
HOOK(od_cmd_app_read_ident,        CMD_READ_IDENT)
#endif

static bool g_mutates_config;
bool od_cmd_mutates_config(uint16_t cmd)
{
    (void)cmd;
    return g_mutates_config;
}

/* --------------------------------------------------------------------------------- helpers --- */

static const od_reply_t BLE = { OD_ORIGIN_BLE, 9u };
/* TLS-PSK LAN is authenticated and encrypted by the transport (K_lan is the PSK), so its frames
 * carry no SEC2 record and must not meet the record layer. Tag 0 is what wifi_service publishes
 * when the link owner is not OWNER_LAN, and od_hal_radio_tag_is_live treats it as live. */
static const od_reply_t LAN_TLS = { OD_ORIGIN_LAN_TLS, 0u };
static const od_reply_t LAN_PLAIN = { OD_ORIGIN_LAN_PLAIN, 0u };
static uint8_t g_blob[1024];
static struct sec2c C;

static void clear_sent(void)
{
    memset(g_sent, 0, sizeof g_sent);
    g_sent_n = 0u;
    g_pop = 0u;
}

/* `claimed` boots from a 2.x key (row 4); `open_session` then runs SEC_AUTH on BLE. */
static void setup(bool claimed, bool open_session)
{
    fake_reset();
    sec_init(0);
    od_config_read_cancel();
    od_txq_reset();
    clear_sent();
    g_tag_live = true;
    (void)od_sec2_boot(claimed ? &g_sec : NULL);
    sec2c_init(&C, MASTER, UID);
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
        CHECK(sec2c_auth(&C, &BLE) == 0);
        clear_sent();
    }
    logs_reset();
}

/* Build [cmd:2][ctr:4][C][T:12] as the wire carries it. */
static uint16_t make_encrypted(uint16_t cmd, const uint8_t *payload, uint16_t n, uint8_t *out)
{
    return sec2c_seal(&C, cmd, payload, n, out);
}

/* ----------------------------------------------------------------------------------- cases --- */

static void test_plaintext_path(void)
{
    uint8_t frame[6] = { 0x00u, 0x77u, 1u, 2u, 3u, 4u };

    CASE("unclaimed: the body reaches the handler unchanged");
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
    uint8_t wire[256];
    const uint8_t payload[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };
    uint16_t n;

    CASE("claimed with a live session: the handler sees the opened plaintext");
    setup(true, true);
    n = make_encrypted(0x0077u, payload, sizeof payload, wire);
    CHECK(n > 0u);
    CHECK(od_dispatch_frame(&BLE, od_span_make(wire, n)) == OD_FRAME_ACCEPTED);
    CHECK(g_handler_calls == 1u);
    CHECK(g_handler_body_len == sizeof payload);
    CHECK(g_handler_wire_len == n && g_handler_was_protected);
    CHECK(memcmp(g_handler_body, payload, sizeof payload) == 0);

    CASE("claimed with no session: AUTH_REQUIRED, and the handler never runs");
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
     * from a genuine session problem. */
    CASE("TLS-LAN bypasses the record layer: PLAINTEXT reaches the handler on a claimed device");
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
}

/* Inside a live session a plaintext frame is parsed as a record and refused: there is no way to
 * slip a cleartext command past the record layer and its replay window mid-session. */
static void test_plaintext_refused_inside_a_session(void)
{
    uint8_t plain[6] = { 0x00u, 0x41u, 1u, 2u, 3u, 4u };
    uint8_t wire[64];
    uint16_t n;

    CASE("with a LIVE session a plaintext CONFIG_WRITE is refused and never runs");
    setup(true, true);
    CHECK(od_dispatch_frame(&BLE, od_span_make(plain, sizeof plain)) == OD_FRAME_CRYPTO_FAILED);
    CHECK(g_handler_calls == 0u);

    CASE("and a SEALED one on that session opens normally");
    n = make_encrypted(0x0041u, plain + 2, 4u, wire);
    CHECK(n > 0u);
    CHECK(od_dispatch_frame(&BLE, od_span_make(wire, n)) == OD_FRAME_ACCEPTED);
    CHECK(g_handler_calls == 1u);
    CHECK(g_handler_body_len == 4u);
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
    uint8_t wire[256];
    uint8_t alternate[256];
    const uint8_t payload[2] = { 1u, 2u };
    uint16_t n;

    CASE("a config write during an active read is DEFERRED before the record is opened");
    /* Deferring after the open would commit the record counter, so the re-dispatched frame is
     * refused as a replay and the transfer stalls -- the reason this check is above the gate. */
    setup(true, true);
    g_mutates_config = true;
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_config_read_start(&BLE, &r, g_blob, sizeof g_blob) == OD_TXQ_OK);
    CHECK(od_config_read_active());

    n = make_encrypted(0x0041u, payload, sizeof payload, wire);
    CHECK(n > 0u);
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

    CASE("and the SAME frame dispatches once the read completes: its counter was never spent");
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

    CASE("AUTHENTICATE on a claimed device refuses with [00][50][05] and reaches no handler");
    setup(true, false);
    CHECK(od_dispatch_frame(&BLE, od_span_make(step1, sizeof step1)) == OD_FRAME_AUTH_CONTROL);
    CHECK(g_handler_calls == 0u);
    (void)od_txq_process();
    CHECK(g_sent_n == 1u && g_sent[0].len == 3u);
    CHECK(g_sent[0].data[0] == RESP_ACK && g_sent[0].data[1] == 0x50u &&
          g_sent[0].data[2] == AUTH_STATUS_V2_REQUIRED);
    CHECK(!od_sec2_session_live(&BLE));

    CASE("AUTHENTICATE on an unclaimed device answers [00][50][03]");
    setup(false, false);
    CHECK(od_dispatch_frame(&BLE, od_span_make(step1, sizeof step1)) == OD_FRAME_AUTH_CONTROL);
    (void)od_txq_process();
    CHECK(g_sent_n == 1u && g_sent[0].len == 3u);
    CHECK(g_sent[0].data[2] == AUTH_STATUS_NOT_CONFIG);

    CASE("FIRMWARE_VERSION bypasses the gate on a claimed device with no session");
    /* A client must be able to learn what it is talking to before it can authenticate, and a
     * device whose key the host lost would otherwise be unidentifiable. */
    setup(true, false);
    CHECK(od_dispatch_frame(&BLE, od_span_make(ver, sizeof ver)) == OD_FRAME_DISCOVERY);
    CHECK(g_handler_calls == 1u);
    CHECK(g_handler_cmd == CMD_FIRMWARE_VERSION);

    CASE("and its reply stays plaintext inside a live session");
    setup(true, true);
    CHECK(od_dispatch_frame(&BLE, od_span_make(ver, sizeof ver)) == OD_FRAME_DISCOVERY);
    (void)od_txq_process();
    CHECK(g_sent_n == 1u && g_sent[0].data[0] == RESP_ACK && g_sent[0].len == 4u);

    CASE("READ_MSD bypasses the gate too, and is DISCOVERY");
    {
        const uint8_t msd[2] = { 0x00u, (uint8_t)(CMD_READ_MSD & 0xFFu) };
        setup(true, false);
        CHECK(od_dispatch_frame(&BLE, od_span_make(msd, sizeof msd)) == OD_FRAME_DISCOVERY);
        CHECK(g_handler_calls == 1u);
        CHECK(g_handler_cmd == CMD_READ_MSD);
    }

    CASE("DISCOVERY is distinct from ACCEPTED, so a version poll cannot stamp activity");
    CHECK(OD_FRAME_DISCOVERY != OD_FRAME_ACCEPTED);

#if OD_CAP_READ_IDENT
    {
        const uint8_t ident[2] = { 0x00u, (uint8_t)(CMD_READ_IDENT & 0xFFu) };
        CASE("READ_IDENT bypasses the gate too, and is DISCOVERY");
        setup(true, false);
        CHECK(od_dispatch_frame(&BLE, od_span_make(ident, sizeof ident)) == OD_FRAME_DISCOVERY);
        CHECK(g_handler_calls == 1u);
        CHECK(g_handler_cmd == CMD_READ_IDENT);
    }
#endif
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

    CASE("neither crypto outcome touches the run -- od_sec2 already applied its own strike");
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
    test_plaintext_refused_inside_a_session();
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
