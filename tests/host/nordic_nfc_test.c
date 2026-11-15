/* Nordic CMD_NFC_ENDPOINT (0x0083) length-bound regression, against the production handler.
 *
 * The inline-write arm takes a peer-supplied 16-bit length and compares it to the body it arrived
 * in. Evaluated in 16 bits that sum wraps, so a declared length near UINT16_MAX passes a bound it
 * exceeds by ~65 KB -- and the record builder downstream copies the unwrapped value. This suite
 * pins the widened comparison by driving the real handler and asserting the tag sink is never
 * reached, which is the part a reply byte alone cannot show.
 *
 * THE REAL NDEF ENCODER IS DELIBERATELY NOT LINKED. Executing it with a wrapped length is the
 * overflow itself; the fake sink records what the handler forwarded instead.
 */

#include "fake_nordic.h"
#include "od_cmd_app.h"
#include "od_nfc.h"
#include "od_cmd_test_ctx.h"
#include "od_hal_radio.h"
#include "od_reply.h"
#include "od_sec2.h"
#include "od_sec2_app.h"
#include "sec2_client.h"
#include "od_txq.h"
#include "opendisplay_protocol.h"
#include "session_fake.h"

#include <stdio.h>
#include <string.h>

void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{ (void)rp; (void)len; (void)why; }

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond); \
        }                                                                      \
    } while (0)
#define CASE(name) (g_case = (name))

#define SENT_MAX 16
static struct { uint16_t len; uint8_t data[OD_TX_FRAME_MAX]; } g_sent[SENT_MAX];
static unsigned g_sent_n;
static unsigned g_pop;

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                    const uint8_t *frame, uint16_t len)
{
    (void)origin; (void)tag;
    if (g_pop == g_sent_n) {              /* everything captured so far was consumed */
        g_pop = 0u;
        g_sent_n = 0u;
    }
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
    return true;
}

/* The SEC2 client reads its handshake replies from the capture. */
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

/* Unclaimed unless a reference row asks for a session: the bound under test is evaluated before
 * anything session-dependent, and an unclaimed device takes every command in plaintext. */
uint32_t od_sec2_app_now_ms(void) { return 1000u; }
void od_sec2_app_uid(uint8_t uid[8]) { memcpy(uid, UID, 8u); }
uint8_t od_sec2_app_dflags(void) { return OD_SEC2_DFLAG_HAS_PANEL; }
const struct SecurityConfig *od_sec2_app_security(void) { return &g_sec; }

static struct sec2c g_ref_client;

static void setup(void)
{
    fake_nordic_reset();
    fake_reset();
    sec_init(0u);
    (void)od_sec2_boot(NULL);
    od_txq_reset();
    od_nfc_reset();
    memset(g_sent, 0, sizeof g_sent);
    g_sent_n = 0u;
    g_pop = 0u;
}

/* Drive one 0x0083 frame through the production handler and drain the queue into g_sent. */
static od_cmd_result_t submit_as_origin(const uint8_t *body, uint16_t n, od_origin_t origin,
                                        uint32_t tag)
{
    od_tx_reservation_t r;

    if (od_txq_reserve(1u, &r) != OD_TXQ_OK) {
        return OD_CMD_UNKNOWN;
    }
    {
        od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ origin, tag }, &r,
                                           (uint16_t)(2u + n), false);
        const od_cmd_result_t rc = od_nfc_frame(&ctx, od_span_make(body, n));

        od_txq_release(&r);
        (void)od_txq_process();
        return rc;
    }
}

static od_cmd_result_t submit(const uint8_t *body, uint16_t n)
{ return submit_as_origin(body, n, OD_ORIGIN_BLE, 1u); }

static bool nacked_malformed(void)
{
    return g_sent_n == 1u && g_sent[0].len == 4u
        && g_sent[0].data[0] == 0xFFu && g_sent[0].data[1] == RESP_NFC_ENDPOINT
        && g_sent[0].data[2] == 0xFFu && g_sent[0].data[3] == NFC_ERR_MALFORMED;
}

/* The four declared lengths whose 16-bit sum with the 4-byte header wraps below any real body. */
static void test_inline_write_length_wrap(void)
{
    static const uint16_t wrapping[] = { 0xFFFCu, 0xFFFDu, 0xFFFEu, 0xFFFFu };

    for (unsigned i = 0; i < sizeof wrapping / sizeof wrapping[0]; ++i) {
        const uint16_t declared = wrapping[i];
        /* rec_type 0 is OD_NFC_REC_TEXT -- a VALID type, so the record-type gate cannot be what
         * refuses this frame. The length check is the only thing standing in front of the sink. */
        const uint8_t body[] = { NFC_SUB_WRITE, OD_NFC_REC_TEXT,
                                 (uint8_t)(declared >> 8), (uint8_t)declared };

        CASE("a wrapping declared length is refused and reaches no tag sink");
        setup();
        CHECK(submit(body, sizeof body) == OD_CMD_NACK);
        CHECK(nacked_malformed());
        CHECK(fake_nfc_write_calls == 0u);

        /* HISTORICAL FIXTURE, asserted as arithmetic rather than executed: the pre-fix bound was
         * (uint16_t)(4 + declared) > payload_len, which these inputs pass -- so the handler went
         * on to forward `declared` itself to a 512-byte staging buffer. */
        CHECK((uint16_t)(4u + declared) <= (uint16_t)sizeof body);
        CHECK(declared > (uint16_t)sizeof body);
    }
}

static void test_inline_write_exact_and_over(void)
{
    uint8_t body[8];

    CASE("a declared length that exactly fills the body is forwarded whole");
    setup();
    body[0] = NFC_SUB_WRITE; body[1] = OD_NFC_REC_TEXT; body[2] = 0u; body[3] = 4u;
    memset(&body[4], 0x11, 4u);
    CHECK(submit(body, 8u) == OD_CMD_OK);
    CHECK(fake_nfc_write_calls == 1u);
    CHECK(fake_nfc_write_len == 4u);
    CHECK(fake_nfc_write_rec_type == OD_NFC_REC_TEXT);

    CASE("one byte beyond the body is refused before the sink");
    setup();
    body[3] = 5u;
    CHECK(submit(body, 8u) == OD_CMD_NACK);
    CHECK(nacked_malformed());
    CHECK(fake_nfc_write_calls == 0u);

    CASE("trailing bytes beyond the declared length are still accepted");
    setup();
    body[3] = 3u;
    CHECK(submit(body, 8u) == OD_CMD_OK);
    CHECK(fake_nfc_write_calls == 1u);
    CHECK(fake_nfc_write_len == 3u);
}

/* The chunked arm's accumulator is bounded by the same rule. It cannot wrap at today's frame
 * sizes; it is widened and pinned so a larger transport cannot make it wrap later. */
static void test_chunk_accumulator_bound(void)
{
    uint8_t start[4];
    uint8_t data[64];

    CASE("a chunk that would exceed total_len clears the assembler and reaches no sink");
    setup();
    start[0] = NFC_SUB_WRITE_START; start[1] = OD_NFC_REC_RAW_NDEF; start[2] = 0u; start[3] = 8u;
    CHECK(submit(start, 4u) == OD_CMD_OK);
    g_sent_n = 0u;
    data[0] = NFC_SUB_WRITE_DATA;
    memset(&data[1], 0x22, 16u);
    CHECK(submit(data, 17u) == OD_CMD_NACK);
    CHECK(g_sent_n == 1u && g_sent[0].data[3] == NFC_ERR_CHUNK_OVERFLOW);
    CHECK(fake_nfc_write_calls == 0u);

    CASE("a later END on the cleared assembler is refused as having no active START");
    g_sent_n = 0u;
    data[0] = NFC_SUB_WRITE_END;
    CHECK(submit(data, 1u) == OD_CMD_NACK);
    CHECK(g_sent_n == 1u && g_sent[0].data[3] == NFC_ERR_CHUNK_NO_START);
    CHECK(fake_nfc_write_calls == 0u);
}

/* ------------------------------------------------------- frozen 0x0083 reference behaviour --- */

#define REF_TARGET_MASK REF_NORDIC

static void ref_setup(void) { setup(); }
static void ref_knob_read_ok(bool ok)      { fake_nfc_read_ok = ok; }
static void ref_knob_read_len(uint16_t n)  { fake_nfc_read_len = n; }
static void ref_knob_write_ok(bool ok)     { fake_nfc_write_ok = ok; }
/* The assembler is the shared machine's, so the reset that clears it is od_nfc_reset() -- which
 * od_core_reset() calls as part of a teardown. Called directly here to keep this suite about the
 * machine; core_reset_test.c proves the composition. */
static void ref_reset_transport(void)      { od_nfc_reset(); }
static unsigned ref_reply_count(void)      { return g_sent_n; }
static uint16_t ref_reply_len(unsigned i)  { return g_sent[i].len; }
static const uint8_t *ref_reply_data(unsigned i) { return g_sent[i].data; }
static unsigned ref_sink_calls(void)       { return fake_nfc_write_calls; }
static uint16_t ref_sink_len(void)         { return fake_nfc_write_len; }
static uint8_t  ref_sink_rec_type(void)    { return fake_nfc_write_rec_type; }
static const uint8_t *ref_sink_data(void)  { return fake_nfc_write_data; }

/* A spent reservation: od_txq_commit refuses on remaining == 0, which is the reachable reply
 * failure once reservation has already guaranteed capacity. */
static od_cmd_result_t submit_unqueueable(const uint8_t *body, uint16_t n, od_origin_t origin,
                                          uint32_t tag)
{
    od_tx_reservation_t spent = { 0u, 0u };
    od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ origin, tag }, &spent,
                                       (uint16_t)(2u + n), false);
    const od_cmd_result_t rc = od_nfc_frame(&ctx, od_span_make(body, n));

    (void)od_txq_process();
    return rc;
}

static od_cmd_result_t ref_submit(const uint8_t *body, uint16_t n, bool foreign_tag,
                                  bool foreign_origin, bool reply_fails)
{
    const od_origin_t origin = foreign_origin ? OD_ORIGIN_LAN_PLAIN : OD_ORIGIN_BLE;
    const uint32_t tag = foreign_tag ? 2u : 1u;

    g_sent_n = 0u;
    if (reply_fails) {
        return submit_unqueueable(body, n, origin, tag);
    }
    return submit_as_origin(body, n, origin, tag);
}

static void ref_authenticate(void)
{
    const od_reply_t ble = { OD_ORIGIN_BLE, 1u };

    (void)od_sec2_boot(&g_sec);
    sec2c_init(&g_ref_client, MASTER, UID);
    CHECK(sec2c_auth(&g_ref_client, &ble) == 0);
    CHECK(od_sec2_session_live(&ble));
    g_sent_n = 0u;
    g_pop = 0u;
}

static bool ref_open_reply(const uint8_t *frame, uint16_t n, uint8_t *out, uint16_t *out_n)
{
    return sec2c_open(&g_ref_client, frame, n, out, out_n);
}

#include "nfc_reference_cases.inc"
#include "nfc_reference_runner.inc"

int main(void)
{
    test_inline_write_length_wrap();
    test_inline_write_exact_and_over();
    test_chunk_accumulator_bound();
    ref_run_table();

    printf("nordic_nfc: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
