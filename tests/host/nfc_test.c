/* The shared CMD_NFC_ENDPOINT (0x0083) machine, at OD_CAP_NFC=1, over the programmable tag.
 *
 * WHAT THE FAKE IS FOR. Every "this refusal touched no hardware" claim here rests on its call
 * counters, which is an absence reply bytes cannot show; every assembly claim rests on it keeping
 * the forwarded BYTES, so a record assembled out of order fails rather than passing on a correct
 * length. Above the read cap it models both deployed outcomes -- refuse and truncate -- because
 * the machine is required to report whichever came back rather than normalise them (N2b).
 *
 * od_core_reset()'s composition is proven in core_reset_test.c, which drives the whole teardown.
 * Here the reset is called directly, so this suite stays about the machine.
 */

#include "fake_nfc_tag.h"
#include "od_hal_radio.h"
#include "od_log.h"
#include "od_nfc.h"
#include "od_nfc_app.h"
#include "od_cmd_test_ctx.h"
#include "od_reply.h"
#include "od_sec2.h"
#include "od_sec2_app.h"
#include "sec2_client.h"
#include "od_txq.h"
#include "session_fake.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define LOG_MAX 8u
static struct {
    int level;
    char text[128];
} g_logs[LOG_MAX];
static unsigned g_log_n;

void _od_log(int level, const char *fmt, ...)
{
    va_list ap;

    if (g_log_n >= LOG_MAX) {
        return;
    }
    g_logs[g_log_n].level = level;
    va_start(ap, fmt);
    (void)vsnprintf(g_logs[g_log_n].text, sizeof g_logs[g_log_n].text, fmt, ap);
    va_end(ap);
    ++g_log_n;
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

/* ------------------------------------------------------------------------------- the link --- */

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

void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{ (void)rp; (void)len; (void)why; }

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

uint32_t od_sec2_app_now_ms(void) { return 5000u; }
void od_sec2_app_uid(uint8_t uid[8]) { memcpy(uid, UID, 8u); }
uint8_t od_sec2_app_dflags(void) { return OD_SEC2_DFLAG_HAS_PANEL; }
const struct SecurityConfig *od_sec2_app_security(void) { return &g_sec; }

static const od_reply_t OWNER = { OD_ORIGIN_BLE, 7u };
static const od_reply_t OTHER_TAG = { OD_ORIGIN_BLE, 8u };
static const od_reply_t OTHER_LINK = { OD_ORIGIN_LAN_PLAIN, 7u };

static void setup(void)
{
    fake_nfc_tag_reset();
    od_txq_reset();
    fake_reset();
    sec_init(0u);
    (void)od_sec2_boot(NULL);
    od_nfc_reset();
    g_sent_n = 0u;
    g_pop = 0u;
}

/* ------------------------------------------------------------------------------ submitting --- */

static od_cmd_result_t submit_from(od_reply_t who, const uint8_t *body, uint16_t n)
{
    od_tx_reservation_t r;

    g_sent_n = 0u;
    g_pop = 0u;
    if (od_txq_reserve(1u, &r) != OD_TXQ_OK) {
        return OD_CMD_UNKNOWN;
    }
    {
        od_cmd_ctx_t ctx = od_test_cmd_ctx(who, &r, (uint16_t)(2u + n), false);
        const od_cmd_result_t rc = od_nfc_frame(&ctx, od_span_make(body, n));

        od_txq_release(&r);
        (void)od_txq_process();
        return rc;
    }
}

static od_cmd_result_t submit(const uint8_t *body, uint16_t n)
{ return submit_from(OWNER, body, n); }

/* A spent reservation: od_txq refuses the frame the handler tries to queue. Reservation makes a
 * full queue impossible at the call site, so this is the reachable shape of a reply failure. */
static od_cmd_result_t submit_unqueueable(const uint8_t *body, uint16_t n)
{
    od_tx_reservation_t spent = { 0u, 0u };
    od_cmd_ctx_t ctx = od_test_cmd_ctx(OWNER, &spent, (uint16_t)(2u + n), false);
    od_cmd_result_t rc;

    g_sent_n = 0u;
    rc = od_nfc_frame(&ctx, od_span_make(body, n));
    (void)od_txq_process();
    return rc;
}

static bool nacked(uint8_t err)
{
    return g_sent_n == 1u && g_sent[0].len == 4u && g_sent[0].data[0] == RESP_NACK
        && g_sent[0].data[1] == RESP_NFC_ENDPOINT && g_sent[0].data[2] == 0xFFu
        && g_sent[0].data[3] == err;
}

static bool acked(uint8_t status)
{
    return g_sent_n == 1u && g_sent[0].len == 3u && g_sent[0].data[0] == RESP_ACK
        && g_sent[0].data[1] == RESP_NFC_ENDPOINT && g_sent[0].data[2] == status;
}

/* Frame builders. Bodies are built so a row can carry 512 payload bytes without a literal, and so
 * the filler is seed+offset -- which is what makes assembly ORDER checkable. */
static uint8_t g_body[600];

static uint16_t hdr_frame(uint8_t sub, uint8_t type, uint16_t declared,
                          uint16_t fill, uint8_t seed)
{
    uint16_t n = 0u;

    g_body[n++] = sub;
    g_body[n++] = type;
    g_body[n++] = (uint8_t)(declared >> 8);
    g_body[n++] = (uint8_t)declared;
    for (uint16_t i = 0; i < fill; ++i) {
        g_body[n++] = (uint8_t)(seed + i);
    }
    return n;
}

static uint16_t data_frame(uint16_t fill, uint8_t seed)
{
    uint16_t n = 0u;

    g_body[n++] = NFC_SUB_WRITE_DATA;
    for (uint16_t i = 0; i < fill; ++i) {
        g_body[n++] = (uint8_t)(seed + i);
    }
    return n;
}

static bool sink_holds(uint16_t len, uint8_t seed)
{
    if (fake_nfc_write_len != len) {
        return false;
    }
    for (uint16_t i = 0; i < len; ++i) {
        if (fake_nfc_write_data[i] != (uint8_t)(seed + i)) {
            return false;
        }
    }
    return true;
}

/* --------------------------------------------------------------------- sub-commands, errors --- */

static void test_dispatch_and_unknowns(void)
{
    static const uint8_t empty[1] = { 0u };

    CASE("an empty body is malformed");
    setup();
    CHECK(submit(empty, 0u) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_MALFORMED));
    CHECK(fake_nfc_read_calls == 0u && fake_nfc_write_calls == 0u);

    CASE("unknown sub-commands are refused without touching the tag");
    for (unsigned i = 0; i < 3u; ++i) {
        static const uint8_t subs[3] = { 0x02u, 0x13u, 0xFFu };
        const uint8_t body[1] = { subs[i] };

        setup();
        CHECK(submit(body, 1u) == OD_CMD_NACK);
        CHECK(nacked(NFC_ERR_UNKNOWN_SUBCMD));
        CHECK(fake_nfc_read_calls == 0u && fake_nfc_write_calls == 0u);
    }
}

/* ------------------------------------------------------------------------------------ read --- */

static void test_read_lengths(void)
{
    static const uint16_t lens[] = { 0u, 1u, 120u, 121u, 218u };
    static const uint8_t read[1] = { NFC_SUB_READ };

    for (unsigned i = 0; i < sizeof lens / sizeof lens[0]; ++i) {
        const uint16_t n = lens[i];

        CASE("a read is answered whole, header and payload");
        setup();
        fake_nfc_read_len = n;
        CHECK(submit(read, 1u) == OD_CMD_OK);
        CHECK(fake_nfc_read_calls == 1u);
        /* The cap the machine requests is wire-visible and must not drift. */
        CHECK(fake_nfc_read_cap_seen == OD_NFC_READ_MAX);
        CHECK(g_sent_n == 1u && g_sent[0].len == (uint16_t)(6u + n));
        CHECK(g_sent[0].data[0] == RESP_ACK && g_sent[0].data[1] == RESP_NFC_ENDPOINT);
        CHECK(g_sent[0].data[2] == NFC_STATUS_READ_DATA);
        CHECK(g_sent[0].data[3] == fake_nfc_read_type);
        CHECK(g_sent[0].data[4] == (uint8_t)(n >> 8) && g_sent[0].data[5] == (uint8_t)n);
        for (uint16_t b = 0; b < n; ++b) {
            if (g_sent[0].data[6u + b] != fake_nfc_read_fill) {
                CHECK(false);
                break;
            }
        }
    }
}

static void test_read_above_the_cap(void)
{
    static const uint8_t read[1] = { NFC_SUB_READ };

    /* N2b: the machine reports what the adapter did. Neither arm is normalised into the other. */
    CASE("a refusing adapter above the cap yields READ_FAILED");
    setup();
    fake_nfc_read_len = 219u;
    fake_nfc_over_cap = FAKE_NFC_OVER_CAP_REFUSE;
    CHECK(submit(read, 1u) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_READ_FAILED));

    CASE("a truncating adapter above the cap yields a short answer at its true length");
    setup();
    fake_nfc_read_len = 219u;
    fake_nfc_over_cap = FAKE_NFC_OVER_CAP_TRUNCATE;
    CHECK(submit(read, 1u) == OD_CMD_OK);
    CHECK(g_sent_n == 1u && g_sent[0].len == 224u);
    CHECK(g_sent[0].data[5] == 218u);

    /* 512 and 513 are the client's assembly sizes arriving on the READ path, where the cap is
     * 218: nothing about them is special to the machine, which is the point -- they must be
     * handled by the same cap rather than by a size-specific arm. */
    CASE("reads far above the cap follow the adapter, not the size");
    for (unsigned i = 0; i < 2u; ++i) {
        const uint16_t big = i == 0u ? 512u : 513u;

        setup();
        fake_nfc_read_len = big;
        fake_nfc_over_cap = FAKE_NFC_OVER_CAP_REFUSE;
        CHECK(submit(read, 1u) == OD_CMD_NACK);
        CHECK(nacked(NFC_ERR_READ_FAILED));

        setup();
        fake_nfc_read_len = big;
        fake_nfc_over_cap = FAKE_NFC_OVER_CAP_TRUNCATE;
        CHECK(submit(read, 1u) == OD_CMD_OK);
        CHECK(g_sent_n == 1u && g_sent[0].len == 224u);
        CHECK(g_sent[0].data[5] == 218u);
    }

    CASE("a failing tag read yields READ_FAILED");
    setup();
    fake_nfc_read_ok = false;
    CHECK(submit(read, 1u) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_READ_FAILED));
}

static void test_read_sealed(void)
{
    static const uint8_t read[1] = { NFC_SUB_READ };
    struct sec2c c;
    uint8_t expected[OD_SEC2_PLAIN_RESP_MAX];
    uint16_t expected_len;
    uint8_t plain[256];
    uint16_t plain_len = 0u;

    /* THE PLAINTEXT ANSWER IS THE ORACLE, captured rather than described. Spot-checking a status
     * byte and a length byte would pass on a frame carrying the wrong command, the wrong record
     * type, the wrong high length byte or the wrong payload -- so the same read is run unsealed
     * first and the sealed one is compared against it in full. */
    CASE("an unsealed read of the cap gives the reference frame");
    setup();
    fake_nfc_read_len = 218u;
    CHECK(submit(read, 1u) == OD_CMD_OK);
    CHECK(g_sent_n == 1u && g_sent[0].len == 224u);
    expected_len = g_sent[0].len;
    memcpy(expected, g_sent[0].data, expected_len);

    /* The cap is applied in BOTH modes on purpose, so the answer's size does not depend on whether
     * the session happens to be encrypted. */
    CASE("the same read in a protected session decodes to the identical frame");
    setup();
    (void)od_sec2_boot(&g_sec);
    sec2c_init(&c, MASTER, UID);
    CHECK(sec2c_auth(&c, &OWNER) == 0);
    fake_nfc_read_len = 218u;
    CHECK(submit(read, 1u) == OD_CMD_OK);
    CHECK(g_sent_n == 1u && g_sent[0].len == 224u + OD_SEC2_D2H_OVERHEAD);
    CHECK(sec2c_open(&c, g_sent[0].data, g_sent[0].len, plain, &plain_len));
    CHECK(plain_len == expected_len);
    CHECK(plain_len == expected_len && memcmp(plain, expected, expected_len) == 0);
}

/* ---------------------------------------------------------------------------- inline write --- */

static void test_inline_write(void)
{
    uint16_t n;

    CASE("an inline write forwards its payload whole, with the record type verbatim");
    setup();
    n = hdr_frame(NFC_SUB_WRITE, OD_NFC_REC_MIME, 120u, 120u, 0x40u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    CHECK(acked(NFC_STATUS_WRITE_COMMITTED));
    CHECK(fake_nfc_write_calls == 1u && fake_nfc_write_type == OD_NFC_REC_MIME);
    CHECK(sink_holds(120u, 0x40u));

    CASE("trailing bytes past the declared length are accepted and ignored");
    setup();
    n = hdr_frame(NFC_SUB_WRITE, OD_NFC_REC_TEXT, 2u, 4u, 0x51u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    CHECK(sink_holds(2u, 0x51u));

    CASE("a short inline frame is malformed");
    setup();
    g_body[0] = NFC_SUB_WRITE; g_body[1] = OD_NFC_REC_TEXT;
    CHECK(submit(g_body, 2u) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_MALFORMED));
    CHECK(fake_nfc_write_calls == 0u);

    CASE("a failing tag write yields TAG_WRITE_FAILED");
    setup();
    fake_nfc_write_ok = false;
    n = hdr_frame(NFC_SUB_WRITE, OD_NFC_REC_TEXT, 4u, 4u, 0u);
    CHECK(submit(g_body, n) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_TAG_WRITE_FAILED));
    CHECK(fake_nfc_write_calls == 1u);
}

static void test_record_types(void)
{
    static const uint8_t valid[5] = { OD_NFC_REC_TEXT, OD_NFC_REC_URI, OD_NFC_REC_WELL_KNOWN_RAW,
                                      OD_NFC_REC_MIME, OD_NFC_REC_RAW_NDEF };
    static const uint8_t bad[4] = { 5u, 6u, 0x80u, 0xFFu };
    uint16_t n;

    CASE("every valid record type reaches the tag unchanged");
    for (unsigned i = 0; i < 5u; ++i) {
        setup();
        n = hdr_frame(NFC_SUB_WRITE, valid[i], 4u, 4u, 0xB0u);
        CHECK(submit(g_body, n) == OD_CMD_OK);
        CHECK(fake_nfc_write_calls == 1u);
        CHECK(fake_nfc_write_type == valid[i]);
    }

    CASE("record types outside the five are refused before the tag");
    for (unsigned i = 0; i < 4u; ++i) {
        setup();
        n = hdr_frame(NFC_SUB_WRITE, bad[i], 4u, 4u, 0u);
        CHECK(submit(g_body, n) == OD_CMD_NACK);
        CHECK(nacked(NFC_ERR_INVALID_REC_TYPE));
        CHECK(fake_nfc_write_calls == 0u);

        setup();
        n = hdr_frame(NFC_SUB_WRITE_START, bad[i], 8u, 0u, 0u);
        CHECK(submit(g_body, n) == OD_CMD_NACK);
        CHECK(nacked(NFC_ERR_INVALID_REC_TYPE));
    }
}

/* THE OVERFLOW CLASS, AND THE ONE INPUT THAT ISOLATES THE WIDENED ARM.
 *
 * od_span_split() refuses an over-long cut on its own, so against a VALID record type the widened
 * 32-bit comparison and a 16-bit one are indistinguishable -- both answer MALFORMED, neither
 * reaches the tag. The arm is observable only through its POSITION ahead of the record-type gate:
 * with an INVALID type and a wrapping length, the widened form still answers MALFORMED while a
 * 16-bit one falls through and answers INVALID_REC_TYPE. Without the second loop below, reverting
 * the widening leaves this suite green. */
static void test_wrapping_lengths(void)
{
    static const uint16_t wrapping[4] = { 0xFFFCu, 0xFFFDu, 0xFFFEu, 0xFFFFu };
    uint16_t n;

    CASE("a wrapping declared length is refused and reaches no tag");
    for (unsigned i = 0; i < 4u; ++i) {
        setup();
        n = hdr_frame(NFC_SUB_WRITE, OD_NFC_REC_TEXT, wrapping[i], 0u, 0u);
        CHECK(submit(g_body, n) == OD_CMD_NACK);
        CHECK(nacked(NFC_ERR_MALFORMED));
        CHECK(fake_nfc_write_calls == 0u);
    }

    CASE("a wrapping length is refused BEFORE the record-type gate, not by it");
    for (unsigned i = 0; i < 4u; ++i) {
        setup();
        n = hdr_frame(NFC_SUB_WRITE, 0xFFu, wrapping[i], 0u, 0u);
        CHECK(submit(g_body, n) == OD_CMD_NACK);
        /* MALFORMED, not INVALID_REC_TYPE: the length arm ran first. This is the oracle. */
        CHECK(nacked(NFC_ERR_MALFORMED));
        CHECK(fake_nfc_write_calls == 0u);
    }

    CASE("a declared length one past the body is refused; exactly filling it is accepted");
    setup();
    n = hdr_frame(NFC_SUB_WRITE, OD_NFC_REC_TEXT, 5u, 4u, 0u);
    CHECK(submit(g_body, n) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_MALFORMED));
    setup();
    n = hdr_frame(NFC_SUB_WRITE, OD_NFC_REC_TEXT, 4u, 4u, 0x20u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    CHECK(sink_holds(4u, 0x20u));
}

/* --------------------------------------------------------------------------------- assembly --- */

static void arm(uint16_t total)
{
    const uint16_t n = hdr_frame(NFC_SUB_WRITE_START, OD_NFC_REC_RAW_NDEF, total, 0u, 0u);

    CHECK(submit(g_body, n) == OD_CMD_OK);
    CHECK(acked(NFC_STATUS_CHUNK_ACCEPTED));
}

static void test_start_bounds(void)
{
    uint16_t n;

    CASE("START at the 512-byte wire limit is accepted, one past it is not");
    setup();
    arm(512u);
    setup();
    n = hdr_frame(NFC_SUB_WRITE_START, OD_NFC_REC_RAW_NDEF, 513u, 0u, 0u);
    CHECK(submit(g_body, n) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_BAD_TOTAL_LEN));

    CASE("START declaring zero bytes is refused");
    setup();
    n = hdr_frame(NFC_SUB_WRITE_START, OD_NFC_REC_RAW_NDEF, 0u, 0u, 0u);
    CHECK(submit(g_body, n) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_BAD_TOTAL_LEN));

    CASE("a short START is malformed");
    setup();
    g_body[0] = NFC_SUB_WRITE_START; g_body[1] = OD_NFC_REC_RAW_NDEF;
    CHECK(submit(g_body, 2u) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_MALFORMED));
}

static void test_assembled_lengths(void)
{
    static const uint16_t totals[] = { 1u, 120u, 121u, 218u, 219u, 512u };
    static const uint8_t end[1] = { NFC_SUB_WRITE_END };

    for (unsigned i = 0; i < sizeof totals / sizeof totals[0]; ++i) {
        const uint16_t total = totals[i];
        uint16_t sent = 0u;

        CASE("an assembled write commits in order at every client-reachable length");
        setup();
        arm(total);
        while (sent < total) {
            const uint16_t left = (uint16_t)(total - sent);
            const uint16_t chunk = left > 120u ? 120u : left;
            const uint16_t n = data_frame(chunk, (uint8_t)sent);

            CHECK(submit(g_body, n) == OD_CMD_OK);
            sent = (uint16_t)(sent + chunk);
        }
        CHECK(submit(end, 1u) == OD_CMD_OK);
        CHECK(acked(NFC_STATUS_WRITE_COMMITTED));
        CHECK(fake_nfc_write_calls == 1u);
        CHECK(fake_nfc_write_type == OD_NFC_REC_RAW_NDEF);
        /* Seed 0 with chunk seeds equal to their offset: the whole record reads back as its own
         * offsets, so a chunk landing in the wrong place changes bytes rather than only counts. */
        CHECK(sink_holds(total, 0u));
    }
}

static void test_assembly_edges(void)
{
    static const uint8_t end[1] = { NFC_SUB_WRITE_END };
    static const uint8_t bare_data[1] = { NFC_SUB_WRITE_DATA };
    uint16_t n;

    CASE("a zero-length DATA is malformed and leaves the assembly intact");
    setup();
    arm(4u);
    CHECK(submit(bare_data, 1u) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_MALFORMED));
    n = data_frame(4u, 0x61u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    CHECK(submit(end, 1u) == OD_CMD_OK);
    CHECK(sink_holds(4u, 0x61u));

    CASE("one byte past total_len is refused and clears the assembly");
    setup();
    arm(4u);
    n = data_frame(5u, 0u);
    CHECK(submit(g_body, n) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_CHUNK_OVERFLOW));
    n = data_frame(1u, 0u);
    CHECK(submit(g_body, n) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_CHUNK_NO_START));
    CHECK(fake_nfc_write_calls == 0u);

    CASE("a short END is retryable: the assembly survives and a later END commits in order");
    setup();
    arm(4u);
    n = data_frame(2u, 0x70u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    CHECK(submit(end, 1u) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_END_LEN_MISMATCH));
    CHECK(fake_nfc_write_calls == 0u);
    n = data_frame(2u, 0x72u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    CHECK(submit(end, 1u) == OD_CMD_OK);
    CHECK(sink_holds(4u, 0x70u));

    CASE("a replacement START discards a partial assembly and re-arms");
    setup();
    arm(4u);
    n = data_frame(2u, 0u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    arm(2u);
    CHECK(submit(end, 1u) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_END_LEN_MISMATCH));
    n = data_frame(2u, 0x90u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    CHECK(submit(end, 1u) == OD_CMD_OK);
    CHECK(sink_holds(2u, 0x90u));

    CASE("a failing tag write on END clears the assembly");
    setup();
    arm(2u);
    n = data_frame(2u, 0u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    fake_nfc_write_ok = false;
    CHECK(submit(end, 1u) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_TAG_WRITE_FAILED));
    CHECK(submit(end, 1u) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_CHUNK_NO_START));

    CASE("DATA and END with nothing armed are refused");
    setup();
    n = data_frame(2u, 0u);
    CHECK(submit(g_body, n) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_CHUNK_NO_START));
    CHECK(submit(end, 1u) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_CHUNK_NO_START));
}

/* -------------------------------------------------------------------------------- ownership --- */

static void test_ownership(void)
{
    static const uint8_t end[1] = { NFC_SUB_WRITE_END };
    uint16_t n;

    CASE("DATA from another tag is refused and mutates nothing");
    setup();
    arm(4u);
    n = data_frame(2u, 0x10u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    n = data_frame(2u, 0xEEu);
    CHECK(submit_from(OTHER_TAG, g_body, n) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_CHUNK_NO_START));
    CHECK(fake_nfc_write_calls == 0u);
    /* The incumbent completes with ITS OWN bytes: the foreign chunk was not staged. */
    n = data_frame(2u, 0x12u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    CHECK(submit(end, 1u) == OD_CMD_OK);
    CHECK(sink_holds(4u, 0x10u));

    CASE("DATA from another link is refused, even at the owner's tag");
    setup();
    arm(4u);
    n = data_frame(2u, 0x20u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    n = data_frame(2u, 0xEEu);
    CHECK(submit_from(OTHER_LINK, g_body, n) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_CHUNK_NO_START));
    n = data_frame(2u, 0x22u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    CHECK(submit(end, 1u) == OD_CMD_OK);
    CHECK(sink_holds(4u, 0x20u));

    CASE("END from a foreign owner commits nothing");
    setup();
    arm(2u);
    n = data_frame(2u, 0x30u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    CHECK(submit_from(OTHER_TAG, end, 1u) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_CHUNK_NO_START));
    CHECK(fake_nfc_write_calls == 0u);
    CHECK(submit(end, 1u) == OD_CMD_OK);
    CHECK(fake_nfc_write_calls == 1u);

    CASE("END from another link commits nothing");
    setup();
    arm(2u);
    n = data_frame(2u, 0x36u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    CHECK(submit_from(OTHER_LINK, end, 1u) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_CHUNK_NO_START));
    CHECK(fake_nfc_write_calls == 0u);
    CHECK(submit(end, 1u) == OD_CMD_OK);
    CHECK(sink_holds(2u, 0x36u));

    /* A TAG IS A TRANSPORT HANDLE AND GETS REUSED. If the owner's connection drops and the next
     * one is handed the same tag, its frames must not inherit the dead owner's assembly -- the
     * teardown is what ends the transfer, not the tag value. */
    CASE("a recycled tag does not inherit the dead owner's assembly");
    setup();
    arm(4u);
    n = data_frame(2u, 0x38u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    od_nfc_reset();                      /* the owner's link went away */
    n = data_frame(2u, 0x3Au);           /* a new peer, same tag */
    CHECK(submit(g_body, n) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_CHUNK_NO_START));
    CHECK(submit(end, 1u) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_CHUNK_NO_START));
    CHECK(fake_nfc_write_calls == 0u);

    CASE("a replacement START from a foreign owner takes ownership");
    setup();
    arm(4u);
    n = hdr_frame(NFC_SUB_WRITE_START, OD_NFC_REC_RAW_NDEF, 2u, 0u, 0u);
    CHECK(submit_from(OTHER_TAG, g_body, n) == OD_CMD_OK);
    CHECK(acked(NFC_STATUS_CHUNK_ACCEPTED));
    /* The old owner is now the foreign one. */
    n = data_frame(2u, 0x40u);
    CHECK(submit(g_body, n) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_CHUNK_NO_START));
    CHECK(submit_from(OTHER_TAG, g_body, n) == OD_CMD_OK);
}

/* --------------------------------------------------------------------------- reply failure --- */

static void test_reply_failure(void)
{
    static const uint8_t read[1] = { NFC_SUB_READ };
    static const uint8_t end[1] = { NFC_SUB_WRITE_END };
    uint16_t n;

    /* N6: a verdict is never OK after a reply that could not be queued, which is what keeps a
     * failed frame out of the session activity stamp. */
    CASE("a READ whose reply cannot be queued reports failure; the tag was only read");
    setup();
    CHECK(submit_unqueueable(read, 1u) == OD_CMD_NACK);
    CHECK(g_sent_n == 0u);
    CHECK(fake_nfc_read_calls == 1u);

    CASE("an inline WRITE whose reply cannot be queued reports failure; the tag is not reverted");
    setup();
    n = hdr_frame(NFC_SUB_WRITE, OD_NFC_REC_TEXT, 4u, 4u, 0u);
    CHECK(submit_unqueueable(g_body, n) == OD_CMD_NACK);
    CHECK(g_sent_n == 0u);
    CHECK(fake_nfc_write_calls == 1u);

    /* START and DATA are the opposite: their ACK means "armed", so a failure the client never saw
     * must not leave bytes staged, or the next frame draws an overflow or mismatch on a transfer
     * it believes never began. */
    CASE("a START whose ACK cannot be queued leaves nothing armed");
    setup();
    n = hdr_frame(NFC_SUB_WRITE_START, OD_NFC_REC_RAW_NDEF, 4u, 0u, 0u);
    CHECK(submit_unqueueable(g_body, n) == OD_CMD_NACK);
    n = data_frame(2u, 0u);
    CHECK(submit(g_body, n) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_CHUNK_NO_START));

    CASE("a DATA whose ACK cannot be queued discards the whole assembly");
    setup();
    arm(4u);
    n = data_frame(2u, 0u);
    CHECK(submit_unqueueable(g_body, n) == OD_CMD_NACK);
    CHECK(submit(end, 1u) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_CHUNK_NO_START));

    CASE("an END whose ACK cannot be queued has still committed to the tag");
    setup();
    arm(2u);
    n = data_frame(2u, 0x50u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    CHECK(submit_unqueueable(end, 1u) == OD_CMD_NACK);
    CHECK(g_sent_n == 0u);
    CHECK(fake_nfc_write_calls == 1u);
    CHECK(sink_holds(2u, 0x50u));
}

static void test_reset(void)
{
    static const uint8_t end[1] = { NFC_SUB_WRITE_END };
    uint16_t n;

    CASE("a reset mid-assembly discards it");
    setup();
    arm(4u);
    n = data_frame(2u, 0u);
    CHECK(submit(g_body, n) == OD_CMD_OK);
    od_nfc_reset();
    CHECK(submit(g_body, n) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_CHUNK_NO_START));
    CHECK(submit(end, 1u) == OD_CMD_NACK);
    CHECK(nacked(NFC_ERR_CHUNK_NO_START));
    CHECK(fake_nfc_write_calls == 0u);
}

static void test_lifecycle_logging(void)
{
    static const struct {
        enum od_nfc_log_event event;
        int detail;
        int level;
        const char *text;
    } cases[] = {
        { OD_NFC_LOG_PAYLOAD_SET_FAILED, -5, OD_LOG_ERROR,
          "NFC payload setup failed: -5" },
        { OD_NFC_LOG_EMULATION_START_FAILED, -6, OD_LOG_ERROR,
          "NFC emulation start failed: -6" },
        { OD_NFC_LOG_CONFIG_ABSENT, 0, OD_LOG_INFO,
          "No NFC configuration; SoC NFCT is idle" },
        { OD_NFC_LOG_CONFIG_DISABLED, 0, OD_LOG_INFO,
          "NFC configurations are present but none are enabled" },
        { OD_NFC_LOG_IC_UNSUPPORTED, 7, OD_LOG_WARN,
          "Unsupported NFC IC type 7; expected auto or SoC NFCT" },
        { OD_NFC_LOG_T2T_SETUP_FAILED, -8, OD_LOG_ERROR,
          "NFC Type 2 Tag setup failed: -8" },
        { OD_NFC_LOG_T2T_ACTIVE, 4, OD_LOG_INFO,
          "SoC NFCT Type 2 Tag is active; advertising byte 4" },
    };
    unsigned i;

    CASE("NFC lifecycle records have shared wording and severity");
    memset(g_logs, 0, sizeof g_logs);
    g_log_n = 0u;
    for (i = 0u; i < sizeof cases / sizeof cases[0]; ++i) {
        od_nfc_log_event(cases[i].event, cases[i].detail);
    }
    CHECK(g_log_n == sizeof cases / sizeof cases[0]);
    for (i = 0u; i < g_log_n; ++i) {
        CHECK(g_logs[i].level == cases[i].level);
        CHECK(strcmp(g_logs[i].text, cases[i].text) == 0);
    }
}

int main(void)
{
    test_dispatch_and_unknowns();
    test_read_lengths();
    test_read_above_the_cap();
    test_read_sealed();
    test_inline_write();
    test_record_types();
    test_wrapping_lengths();
    test_start_bounds();
    test_assembled_lengths();
    test_assembly_edges();
    test_ownership();
    test_reply_failure();
    test_reset();
    test_lifecycle_logging();

    printf("nfc: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
