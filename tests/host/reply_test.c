/* reply_test.c -- shared/core/od_reply.c: the seal-or-plain decision.
 *
 * The property under test is not "does it encrypt" but "is confidentiality chosen by the CALL".
 * Both targets previously inferred it from the response bytes, and Nordic's version sealed its own
 * rejection frames -- which py-opendisplay then decrypted and validated as an ACK for a command
 * the device had refused. So the cases that matter are the ones where the bytes would mislead.
 */

#include "od_reply.h"

#include "od_log.h"
#include "od_sec2.h"
#include "od_sec2_app.h"
#include "sec2_client.h"
#include "session_fake.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define LOG_CAP 16u
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

#define SENT_MAX 16u

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

/* The SEC2 client reads the handshake replies from here. */
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

uint32_t od_sec2_app_now_ms(void) { return 1000u; }
void od_sec2_app_uid(uint8_t uid[8]) { memcpy(uid, UID, 8u); }
uint8_t od_sec2_app_dflags(void) { return OD_SEC2_DFLAG_HAS_PANEL; }
const struct SecurityConfig *od_sec2_app_security(void) { return &g_sec; }

/* od_txq's drop seam. Counted so a test can assert that a discarded entry was REPORTED, not just
 * that it vanished -- the difference between a diagnosable failure and a silent one. */
static unsigned g_dropped;
void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    (void)rp; (void)len; (void)why;
    ++g_dropped;
}

/* --------------------------------------------------------------------------------- helpers --- */

static const od_reply_t BLE  = { OD_ORIGIN_BLE, 7u };
static const od_reply_t TLS  = { OD_ORIGIN_LAN_TLS, 7u };

static struct sec2c C;

/* `claimed` boots from a 2.x key (row 4); `open_session` then runs SEC_AUTH HELLO + FINISH. */
static void setup(bool claimed, bool open_session)
{
    fake_reset();
    sec_init(0);
    od_txq_reset();
    memset(g_sent, 0, sizeof g_sent);
    g_sent_n = 0u;
    g_pop = 0u;
    g_tag_live = true;
    (void)od_sec2_boot(claimed ? &g_sec : NULL);
    sec2c_init(&C, MASTER, UID);

    if (open_session) {
        CHECK(sec2c_auth(&C, &BLE) == 0);
    }
    g_sent_n = 0u;
    g_pop = 0u;
    logs_reset();
}

/* [cmd:2][payload] -- what a handler produces. */
static const uint8_t FRAME[6] = { 0x00u, 0x71u, 0xDEu, 0xADu, 0xBEu, 0xEFu };

/* ----------------------------------------------------------------------------------- cases --- */

static void test_plain_when_security_is_off(void)
{
    od_tx_reservation_t r;

    CASE("unclaimed device: the response goes out verbatim");
    setup(false, false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &BLE, FRAME, sizeof FRAME) == OD_TXQ_OK);
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent_n == 1u);
    CHECK(g_sent[0].len == sizeof FRAME);
    CHECK(memcmp(g_sent[0].data, FRAME, sizeof FRAME) == 0);
}

static void test_sealed_when_session_is_live(void)
{
    od_tx_reservation_t r;
    uint8_t plain[256];
    uint16_t plain_len = 0u;

    CASE("live session: the response is sealed, and round-trips back to the original");
    setup(true, true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &BLE, FRAME, sizeof FRAME) == OD_TXQ_OK);
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent_n == 1u);
    /* [E5][ctr:4][C][T:12]: sealing adds exactly 17 bytes and hides the opcode too. */
    CHECK(g_sent[0].len == sizeof FRAME + OD_SEC2_D2H_OVERHEAD);
    CHECK(g_sent[0].data[0] == OD_SEC2_SEALED_STATUS);
    CHECK(memcmp(g_sent[0].data, FRAME, sizeof FRAME) != 0);   /* it is NOT the plaintext */

    CASE("and what was sealed is exactly what the handler produced");
    CHECK(sec2c_open(&C, g_sent[0].data, g_sent[0].len, plain, &plain_len));
    CHECK(plain_len == sizeof FRAME);
    CHECK(memcmp(plain, FRAME, plain_len) == 0);
}

static void test_plain_scope_never_seals(void)
{
    od_tx_reservation_t r;

    CASE("inside the plaintext scope od_reply leaves the frame alone with a live session");
    setup(true, true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    od_reply_plain_scope(true);
    CHECK(od_reply(&r, &BLE, FRAME, sizeof FRAME) == OD_TXQ_OK);
    od_reply_plain_scope(false);
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent[0].len == sizeof FRAME);
    CHECK(memcmp(g_sent[0].data, FRAME, sizeof FRAME) == 0);
}

static void test_reply_plain_never_seals(void)
{
    od_tx_reservation_t r;

    CASE("od_reply_plain leaves a control frame alone even with a live session");
    setup(true, true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply_plain(&r, &BLE, FRAME, sizeof FRAME) == OD_TXQ_OK);
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent[0].len == sizeof FRAME);
    CHECK(memcmp(g_sent[0].data, FRAME, sizeof FRAME) == 0);

    CASE("THE REGRESSION THIS EXISTS FOR: a rejection frame stays readable");
    /* {0x00, cmd, 0xFF} has its marker in byte 2 and 0x00 in byte 0. Nordic's old predicate read
     * byte 0, so it sealed this -- and py-opendisplay decrypts any 31+ byte response and returns
     * before its raw[2] guard, so the host validated a REFUSED command as an ACK. Choosing at the
     * call site removes the class, not just this instance. */
    setup(true, true);
    {
        const uint8_t nack[3] = { 0x00u, 0x71u, 0xFFu };
        CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
        CHECK(od_reply_plain(&r, &BLE, nack, sizeof nack) == OD_TXQ_OK);
        CHECK(od_txq_process() == 1u);
        CHECK(g_sent[0].len == 3u);                     /* NOT sealed */
        CHECK(g_sent[0].data[2] == 0xFFu);              /* the host can still see the marker */
    }
}

static void test_tls_lan_is_never_double_wrapped(void)
{
    od_tx_reservation_t r;

    CASE("TLS-LAN goes plain at the application layer even through od_reply");
    setup(true, true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &TLS, FRAME, sizeof FRAME) == OD_TXQ_OK);
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent[0].len == sizeof FRAME);
    CHECK(memcmp(g_sent[0].data, FRAME, sizeof FRAME) == 0);
}

static void test_no_session_substitutes_a_nack(void)
{
    od_tx_reservation_t r;

    CASE("claimed but no session: the payload must NOT go out in the clear");
    setup(true, false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &BLE, FRAME, sizeof FRAME) == OD_TXQ_SEAL_FAILED);
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent_n == 1u);
    /* A hard NACK, not the response. Sending the response plaintext would hand an unauthenticated
     * peer exactly the payload the session exists to hide. */
    CHECK(g_sent[0].len == 3u);
    CHECK(g_sent[0].data[0] == RESP_NACK);
    CHECK(g_sent[0].data[1] == 0x71u);            /* the opcode is echoed so a client can match it */
    CHECK(memcmp(g_sent[0].data, FRAME, sizeof FRAME) != 0);
}

static void test_oversized_becomes_a_nack(void)
{
    od_tx_reservation_t r;
    static uint8_t big[OD_SEC2_PLAIN_RESP_MAX + 8];

    memset(big, 0x5Au, sizeof big);
    big[0] = 0x00u;
    big[1] = 0x71u;

    CASE("exactly OD_SEC2_PLAIN_RESP_MAX seals to the 253-byte ceiling");
    setup(true, true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &BLE, big, OD_SEC2_PLAIN_RESP_MAX) == OD_TXQ_OK);
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent[0].len == OD_SEC2_SEALED_MAX);

    CASE("one byte more is refused and becomes a NACK, with no second reply");
    setup(true, true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &BLE, big, OD_SEC2_PLAIN_RESP_MAX + 1u) == OD_TXQ_TOO_LARGE);
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent_n == 1u);                        /* exactly one frame, the NACK */
    CHECK(g_sent[0].len == 3u);
    CHECK(g_sent[0].data[0] == RESP_NACK);
    CHECK(r.remaining == 0u);                     /* the unit went to the NACK */
}

static void test_torn_down_session_substitutes_a_nack(void)
{
    od_tx_reservation_t r;

    CASE("a session torn down mid-exchange: the response becomes a NACK, never cleartext");
    setup(true, true);
    od_sec2_teardown();
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &BLE, FRAME, sizeof FRAME) == OD_TXQ_SEAL_FAILED);
    CHECK(od_txq_process() == 1u);
    CHECK(g_sent[0].len == 3u);
    CHECK(g_sent[0].data[0] == RESP_NACK);
}

static void test_argument_and_accounting(void)
{
    od_tx_reservation_t r;

    CASE("bad arguments spend nothing and queue nothing");
    setup(true, true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(NULL, &BLE, FRAME, sizeof FRAME) == OD_TXQ_INVARIANT);
    CHECK(od_reply(&r, NULL, FRAME, sizeof FRAME) == OD_TXQ_INVARIANT);
    CHECK(od_reply(&r, &BLE, NULL, 4u) == OD_TXQ_INVARIANT);
    CHECK(od_reply(&r, &BLE, FRAME, 0u) == OD_TXQ_INVARIANT);
    CHECK(log_count_exact(OD_LOG_ERROR,
          "Application response arguments are invalid (reservation=0, target=1, frame=1, len=6)")
          == 1u);
    CHECK(log_count_exact(OD_LOG_ERROR,
          "Application response arguments are invalid (reservation=1, target=0, frame=1, len=6)")
          == 1u);
    CHECK(log_count_exact(OD_LOG_ERROR,
          "Application response arguments are invalid (reservation=1, target=1, frame=0, len=4)")
          == 1u);
    CHECK(log_count_exact(OD_LOG_ERROR,
          "Application response arguments are invalid (reservation=1, target=1, frame=1, len=0)")
          == 1u);
    CHECK(r.remaining == 1u);
    CHECK(od_txq_depth() == 0u);

    CASE("a frame too short to carry an opcode emits nothing at all");
    /* Under two bytes there is no opcode to echo, so no honest NACK can be built -- the caller's
     * release() reclaims the unit instead. */
    setup(true, true);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &BLE, FRAME, 1u) == OD_TXQ_INVARIANT);
    CHECK(od_txq_depth() == 0u);
    CHECK(g_sent_n == 0u);
}

static void test_nack_substitution_failure_is_visible(void)
{
    od_tx_reservation_t r;

    CASE("a seal-failure NACK that cannot spend its reservation is diagnosed");
    setup(true, false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    od_txq_release(&r);
    CHECK(od_reply(&r, &BLE, FRAME, sizeof FRAME) == OD_TXQ_INVARIANT);
    CHECK(log_count_exact(OD_LOG_ERROR,
                          "Response NACK could not be queued (cmd=0x71, rc=7)") == 1u);

    CASE("a seal-failure NACK for a departed target is an informational loss");
    setup(true, false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    g_tag_live = false;
    CHECK(od_reply(&r, &BLE, FRAME, sizeof FRAME) == OD_TXQ_GONE);
    CHECK(log_count_exact(OD_LOG_INFO,
          "Response NACK was not queued because the reply target is gone (cmd=0x71)") == 1u);
}

static void test_queue_failure_is_visible(void)
{
    od_tx_reservation_t r;

    CASE("an application response without reservation capacity is diagnosed");
    setup(false, false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    od_txq_release(&r);
    CHECK(od_reply(&r, &BLE, FRAME, sizeof FRAME) == OD_TXQ_INVARIANT);
    CHECK(log_count_exact(OD_LOG_ERROR,
                          "Response could not be queued (cmd=0x0071, rc=7)") == 1u);

    CASE("a plain response for a departed target is an informational loss");
    setup(false, false);
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    g_tag_live = false;
    CHECK(od_reply_plain(&r, &BLE, FRAME, sizeof FRAME) == OD_TXQ_GONE);
    CHECK(log_count_exact(OD_LOG_INFO,
          "Response was not queued because the reply target is gone (cmd=0x0071)") == 1u);
}

int main(void)
{
    test_plain_when_security_is_off();
    test_sealed_when_session_is_live();
    test_reply_plain_never_seals();
    test_plain_scope_never_seals();
    test_tls_lan_is_never_double_wrapped();
    test_no_session_substitutes_a_nack();
    test_oversized_becomes_a_nack();
    test_torn_down_session_substitutes_a_nack();
    test_argument_and_accounting();
    test_nack_substitution_failure_is_visible();
    test_queue_failure_is_visible();

    printf("reply: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
