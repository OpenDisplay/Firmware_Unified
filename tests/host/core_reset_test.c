/* core_reset_test.c -- od_core_reset(), the shared half of a teardown.
 *
 * WHAT IT MUST NOT DO carries as much weight as what it does. RX is not in it: Nordic's producer
 * is the BT thread and can push concurrently, and ESP32's connection policy has stricter ordering
 * around its own ring -- so a reset here would race one target and reorder the other. The call
 * sites drain RX themselves, on their own terms.
 *
 * WHAT IS NOT ASSERTED HERE, stated so a green run is not over-read: the ORDER of the five calls.
 * They run straight-line on the consumer context with nothing interleaved, so no test driving the
 * public API can distinguish one order from another -- swapping the cancel and the reset leaves
 * every assertion below unchanged, verified by mutation. The order is not load-bearing (od_core.c
 * says why); what these cases pin is that all five happen, which is.
 */

#include "od_core.h"

#include "od_nfc.h"

#include "od_config_read.h"
#include "od_cmd_test_ctx.h"
#include "od_reply.h"
#include "od_rxq.h"
#include "od_rxq_app.h"
#include "od_sec2.h"
#include "od_sec2_app.h"
#include "od_txq.h"
#include "od_xfer.h"
#include "od_xfer_app_test_stub.h"
#include "od_xfer_internal.h"
#include "sec2_client.h"
#include "session_fake.h"

#include <stdio.h>
#include <string.h>

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

/* ------------------------------------------------------------------------------ fake radio --- */

static unsigned g_sent_n;
static bool     g_radio_accepts = true;

/* The last frame the radio accepted, for the NFC probe below. */
static uint8_t  g_last[8];
static uint16_t g_last_len;

/* Every accepted frame, for the SEC2 client's handshake replies. */
#define CAP_MAX 8u
static uint8_t  g_cap[CAP_MAX][OD_TX_FRAME_MAX];
static uint16_t g_cap_len[CAP_MAX];
static unsigned g_cap_n;
static unsigned g_cap_pop;

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                    const uint8_t *frame, uint16_t len)
{
    (void)origin; (void)tag;
    if (!g_radio_accepts) {
        return OD_RADIO_RETRY;          /* hold entries in the queue so a reset has work to do */
    }
    /* The bytes are kept for the NFC probe below, which has to tell a refusal apart from an
     * acceptance whose reply merely failed to queue -- both are OD_CMD_NACK. */
    g_last_len = len > sizeof g_last ? (uint16_t)sizeof g_last : len;
    memcpy(g_last, frame, g_last_len);
    if (g_cap_n < CAP_MAX) {
        memcpy(g_cap[g_cap_n], frame, len);
        g_cap_len[g_cap_n] = len;
        ++g_cap_n;
    }
    ++g_sent_n;
    return OD_RADIO_SENT;
}

bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{
    (void)origin; (void)tag;
    return true;
}

bool sec2c_radio_pop(uint8_t *buf, uint16_t *len)
{
    if (g_cap_pop >= g_cap_n) {
        return false;
    }
    memcpy(buf, g_cap[g_cap_pop], g_cap_len[g_cap_pop]);
    *len = g_cap_len[g_cap_pop];
    ++g_cap_pop;
    return true;
}

void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    (void)rp; (void)len; (void)why;
}

/* Built at the default INFO level, where od_rxq.c's arrival block preprocesses away and neither
 * predicate is referenced. Defined anyway so this fixture keeps linking if the suite is ever
 * compiled at DEBUG. */
bool od_rxq_app_encryption_enabled(void) { return false; }
bool od_rxq_app_quiet(uint16_t cmd) { (void)cmd; return false; }

/* ------------------------------------------------------------------------- od_sec2_app hooks --- */

uint32_t od_sec2_app_now_ms(void) { return 1000u; }
void od_sec2_app_uid(uint8_t uid[8]) { memcpy(uid, UID, 8u); }
uint8_t od_sec2_app_dflags(void) { return OD_SEC2_DFLAG_HAS_PANEL; }
const struct SecurityConfig *od_sec2_app_security(void) { return &g_sec; }

/* --------------------------------------------------------------------------------- helpers --- */

static const od_reply_t BLE = { OD_ORIGIN_BLE, 9u };
static uint8_t g_blob[2048];
static struct sec2c C;

/* SEC_AUTH HELLO + FINISH on BLE; the handshake frames are then forgotten. */
static bool authenticate(void)
{
    const bool ok = sec2c_auth(&C, &BLE) == 0;

    g_cap_n = 0u;
    g_cap_pop = 0u;
    g_sent_n = 0u;
    return ok && od_sec2_session_live(&BLE);
}

/* A claimed device (2.x key, row 4) with a live session on BLE. */
static void setup(void)
{
    fake_reset();
    sec_init(0);
    od_config_read_cancel();
    od_xfer_reset();
    od_test_xfer_app_reset();
    od_txq_reset();
    g_sent_n = 0u;
    g_radio_accepts = true;
    memset(g_blob, 0x5Au, sizeof g_blob);
    (void)od_sec2_boot(&g_sec);
    sec2c_init(&C, MASTER, UID);
    g_cap_n = 0u;
    g_cap_pop = 0u;
    CHECK(authenticate());
}

static void start_an_owned_transfer(void)
{
    od_tx_reservation_t reservation = { 0 };
    od_cmd_ctx_t ctx = od_test_cmd_ctx(BLE, &reservation, 12u, false);

    od_test_xfer_app_set_panel_ready(true);
    CHECK(od_xfer_pipe_arm_full(&ctx, 32u, false) == OD_XFER_START_OK);
    CHECK(od_xfer_pipe_activate());
    CHECK(od_xfer_owns_hardware());
}

/* Start a config read and leave it mid-flight with entries stuck in the queue. */
static void start_a_stuck_read(void)
{
    od_tx_reservation_t r;

    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    g_radio_accepts = false;                    /* nothing drains: the producer stalls */
    CHECK(od_config_read_start(&BLE, &r, g_blob, sizeof g_blob) == OD_TXQ_OK);
    (void)od_config_read_pump();
    CHECK(od_config_read_active());
    CHECK(od_txq_depth() > 0u);
}

/* Arm a chunked NFC write and put one byte in it, so the reset has something to discard. */
static void stage_a_partial_nfc_assembly(void)
{
    static const uint8_t start[] = { NFC_SUB_WRITE_START, OD_NFC_REC_RAW_NDEF, 0u, 4u };
    static const uint8_t data[] = { NFC_SUB_WRITE_DATA, 0xA1u };
    od_tx_reservation_t r;

    CHECK(od_txq_reserve(2u, &r) == OD_TXQ_OK);
    {
        od_cmd_ctx_t ctx = od_test_cmd_ctx(BLE, &r, 6u, false);

        CHECK(od_nfc_frame(&ctx, od_span_make(start, sizeof start)) == OD_CMD_OK);
        CHECK(od_nfc_frame(&ctx, od_span_make(data, sizeof data)) == OD_CMD_OK);
    }
    od_txq_release(&r);
}

/* True when a DATA frame from the incumbent owner draws NFC_ERR_CHUNK_NO_START.
 *
 * ASSERTS THE ERROR BYTE, not the verdict. OD_CMD_NACK is ambiguous here -- an accepted chunk
 * whose ACK could not be queued returns it too -- so a verdict-only probe passes whether or not
 * the assembly survived, which is exactly the mistake this comment exists to stop being made
 * again. The queue is cleared and the radio opened first so the reply is certain to be observable
 * rather than lost to a full ring left over from the transfer above. */
static bool nfc_data_is_refused(void)
{
    static const uint8_t data[] = { NFC_SUB_WRITE_DATA, 0xA2u };
    od_tx_reservation_t r;

    od_txq_reset();
    g_radio_accepts = true;
    g_last_len = 0u;
    if (od_txq_reserve(1u, &r) != OD_TXQ_OK) {
        return false;
    }
    {
        od_cmd_ctx_t ctx = od_test_cmd_ctx(BLE, &r, 4u, false);

        (void)od_nfc_frame(&ctx, od_span_make(data, sizeof data));
    }
    od_txq_release(&r);
    (void)od_txq_process();
    return g_last_len == 4u && g_last[0] == RESP_NACK && g_last[1] == RESP_NFC_ENDPOINT
        && g_last[3] == NFC_ERR_CHUNK_NO_START;
}

/* ----------------------------------------------------------------------------------- cases --- */

static void test_reset_clears_all_five(void)
{
    CASE("a reset clears the NFC assembly, transfer, producer, queue and session");
    setup();
    start_an_owned_transfer();
    start_a_stuck_read();
    stage_a_partial_nfc_assembly();
    CHECK(od_sec2_session_live(&BLE));

    od_core_reset();

    CHECK(!od_config_read_active());
    CHECK(!od_xfer_frames_may_arrive());
    CHECK(od_test_xfer_app_abort_calls() == 1u);
    CHECK(od_txq_depth() == 0u);
    CHECK(od_txq_reserved() == 0u);
    CHECK(!od_sec2_session_live(&BLE));

    /* LAST, because proving this costs a frame. The assembly is gone in the only way a client can
     * observe it -- the DATA that would have completed it is answered as having no active START --
     * and that probe queues a NACK of its own, which would perturb the queue assertions above.
     * Asserting a zeroed struct instead would pin an implementation detail N5 leaves free. */
    CHECK(nfc_data_is_refused());
}

static void test_reset_releases_the_key_slot(void)
{
    CASE("the reset tears the session down, so both record-key slots are released");
    setup();
    CHECK(fake_slot_loaded(0u) && fake_slot_loaded(1u));
    {
        const unsigned clears_before = g_key_clear_calls;
        od_core_reset();
        /* A zeroed session struct would leave the record keys in the HAL with nothing naming
         * them. This is the assertion that says the reset is a teardown rather than a zeroing. */
        CHECK(g_key_clear_calls >= clears_before + 2u);
        CHECK(!fake_slot_loaded(0u));
        CHECK(!fake_slot_loaded(1u));
    }

    CASE("and the next handshake loads both slots again");
    CHECK(authenticate());
    CHECK(fake_slot_loaded(0u) && fake_slot_loaded(1u));
}

static void test_reset_does_not_touch_rx(void)
{
    uint8_t frame[4] = { 0x00u, 0x77u, 0x01u, 0x02u };

    CASE("RX is NOT the shared reset's -- its producer differs per target");
    setup();
    (void)od_rxq_reset();
    CHECK(od_rxq_push(frame, sizeof frame, 9u));
    CHECK(od_rxq_peek() != NULL);

    od_core_reset();

    CHECK(od_rxq_peek() != NULL);              /* still there, for the caller to drain */
    (void)od_rxq_reset();
}

static void test_reset_is_idempotent(void)
{
    CASE("resetting twice is the same as resetting once");
    setup();
    start_a_stuck_read();

    od_core_reset();
    od_core_reset();

    CHECK(!od_config_read_active());
    CHECK(!od_xfer_frames_may_arrive());
    CHECK(od_test_xfer_app_abort_calls() == 0u);
    CHECK(od_txq_depth() == 0u);
    CHECK(od_txq_reserved() == 0u);
    CHECK(!od_sec2_session_live(&BLE));
}

/* A CANCELLED PRODUCER STAYS CANCELLED. Not an ordering check -- see the header comment -- but the
 * property that matters at the call sites: after a disconnect teardown, pumping the producer again
 * (which both targets do, every pass) must not resurrect a chunk of a read the departed client
 * started. Without the cancel, this pump emits chunk N to whoever inherits the link. */
static void test_no_chunk_survives_the_reset(void)
{
    CASE("a cancelled producer emits nothing when the pump runs again");
    setup();
    start_a_stuck_read();

    od_core_reset();
    g_radio_accepts = true;
    g_sent_n = 0u;

    (void)od_config_read_pump();
    (void)od_txq_process();
    CHECK(g_sent_n == 0u);
    CHECK(od_txq_depth() == 0u);
}

int main(void)
{
    test_reset_clears_all_five();
    test_reset_releases_the_key_slot();
    test_reset_does_not_touch_rx();
    test_reset_is_idempotent();
    test_no_chunk_survives_the_reset();

    printf("core_reset: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
