/* Fault injection around the production BG22 command and transport sources. */

#include "fake_silabs.h"
#include "od_cmd_app.h"
#include "od_cmd_test_ctx.h"
#include "od_config.h"
#include "od_config_read.h"
#include "od_nfc.h"
#include "od_reply.h"
#include "od_sec2.h"
#include "od_sec2_app.h"
#include "od_sec2_wire.h"
#include "od_txq.h"
#include "od_xfer.h"
#include "opendisplay_display.h"
#include "opendisplay_pipe.h"
#include "sec2_client.h"
#include "session_fake.h"
#include "sl_bt_api.h"

#include <stdio.h>
#include <string.h>

bool sl_bt_can_process_event(uint32_t len);

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

static void exchange_mtu(uint8_t connection, uint16_t mtu);

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond); \
        }                                                                      \
    } while (0)
#define CASE(name) (g_case = (name))

uint32_t od_sec2_app_now_ms(void) { return fake_silabs_now_ms; }
void od_sec2_app_uid(uint8_t uid[8]) { memcpy(uid, UID, 8u); }
uint8_t od_sec2_app_dflags(void) { return OD_SEC2_DFLAG_HAS_PANEL; }
const struct SecurityConfig *od_sec2_app_security(void) { return &g_sec; }

/* The SEC2 client reads its handshake replies from the vendor-notify capture. */
static unsigned g_pop;
bool sec2c_radio_pop(uint8_t *buf, uint16_t *len)
{
    if (g_pop >= fake_silabs_sent_n || g_pop >= FAKE_SILABS_SENT_MAX) {
        return false;
    }
    memcpy(buf, fake_silabs_sent[g_pop].data, fake_silabs_sent[g_pop].len);
    *len = fake_silabs_sent[g_pop].len;
    ++g_pop;
    return true;
}

static struct sec2c C;

static void open_connection(uint8_t connection)
{
    sl_bt_msg_t evt;
    memset(&evt, 0, sizeof evt);
    evt.header = sl_bt_evt_connection_opened_id;
    evt.data.evt_connection_opened.connection = connection;
    opendisplay_pipe_handle_gatt_event(&evt);
}

static void set_notifications(bool enabled)
{
    sl_bt_msg_t evt;
    memset(&evt, 0, sizeof evt);
    evt.header = sl_bt_evt_gatt_server_characteristic_status_id;
    evt.data.evt_gatt_server_characteristic_status.characteristic =
        opendisplay_pipe_characteristic();
    evt.data.evt_gatt_server_characteristic_status.status_flags =
        sl_bt_gatt_server_client_config;
    evt.data.evt_gatt_server_characteristic_status.client_config_flags =
        enabled ? sl_bt_gatt_server_notification : 0u;
    opendisplay_pipe_handle_gatt_event(&evt);
}

static void setup(void)
{
    fake_reset();
    fake_silabs_reset();
    fake_silabs_bgapi_reset();
    sec_init(0u);
    (void)od_sec2_boot(NULL);
    /* Boot-contract results are static and survive a characteristic reset, so restate them here
     * rather than leaving cases to undo each other's degradation. Deliberately NO MTU exchange:
     * every large-frame case below therefore runs on a connection that never negotiated, which is
     * the state an application-side MTU mirror would get wrong. */
    opendisplay_pipe_set_tx_report_available(true);
    opendisplay_pipe_set_characteristic(0x1234u);
    open_connection(7u);
    set_notifications(true);
    /* Initialisation deliberately resets/aborts once; cases count only subsequent actions. */
    fake_silabs_aborts = 0u;
    fake_silabs_resets = 0u;
}

/* Claim the device (a 2.x key migrated at boot) and run SEC_AUTH on the pipe's connection, so
 * every later reply on that tag is sealed. */
static void authenticate(void)
{
    const od_reply_t rp = { OD_ORIGIN_BLE, opendisplay_pipe_connection_tag() };

    (void)od_sec2_boot(&g_sec);
    sec2c_init(&C, MASTER, UID);
    fake_silabs_sent_n = 0u;
    g_pop = 0u;
    CHECK(sec2c_auth(&C, &rp) == 0);
    CHECK(od_sec2_session_live(&rp));
    fake_silabs_sent_n = 0u;
    g_pop = 0u;
}

static void test_config_persist_before_queue(void)
{
    const uint8_t config[] = { 0x55u };
    od_tx_reservation_t r;
    od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 0u },
                                        &r, 0u, false);

    CASE("config success persists before queue, then reloads with the session kept");
    setup();
    authenticate();
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE,
                                         opendisplay_pipe_connection_tag() },
                          &r, 2u, false);
    CHECK(od_cmd_app_config_write(&ctx, od_span_make(config, sizeof config)) == OD_CMD_OK);
    od_txq_release(&r);
    CHECK(fake_silabs_store_attempts == 1u);
    CHECK(fake_silabs_store_saves == 1u);
    CHECK(fake_silabs_save_saw_queue_empty);
    CHECK(fake_silabs_store_len == sizeof config);
    CHECK(memcmp(fake_silabs_store_blob, config, sizeof config) == 0);
    CHECK(fake_silabs_store_reloads == 1u);
    CHECK(fake_silabs_reload_saw_queued);
    CHECK(fake_silabs_reload_saw_authenticated);
    /* The key is not in the config record (SECURITY_V2 9.6), so a config write ends no session. */
    CHECK(od_sec2_session_live(NULL));
    CHECK(od_txq_depth() == 1u);
    CHECK(od_txq_process() == 1u);
    CHECK(fake_silabs_sent_n == 1u);
    {
        /* The success ACK left sealed, and carries the 4-byte ACK. */
        uint8_t plain[256];
        uint16_t plain_n = 0u;

        CHECK(fake_silabs_sent[0].data[0] == OD_SEC2_SEALED_STATUS);
        CHECK(sec2c_open(&C, fake_silabs_sent[0].data, fake_silabs_sent[0].len, plain, &plain_n));
        CHECK(plain_n == 4u);
        CHECK(plain[0] == RESP_ACK && plain[1] == RESP_CONFIG_WRITE);
    }

    CASE("config persistence failure cannot emit success, reload, or alter old storage");
    setup();
    authenticate();
    fake_silabs_store_blob[0] = 0xa5u;
    fake_silabs_store_len = 1u;
    fake_silabs_store_ok = false;
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE,
                                         opendisplay_pipe_connection_tag() },
                          &r, 2u, false);
    CHECK(od_cmd_app_config_write(&ctx, od_span_make(config, sizeof config)) == OD_CMD_NACK);
    od_txq_release(&r);
    CHECK(fake_silabs_store_attempts == 1u);
    CHECK(fake_silabs_store_saves == 0u);
    CHECK(fake_silabs_store_len == 1u);
    CHECK(fake_silabs_store_blob[0] == 0xa5u);
    CHECK(fake_silabs_store_reloads == 0u);
    CHECK(od_sec2_session_live(NULL));
    CHECK(od_txq_process() == 1u);
    CHECK(fake_silabs_sent_n == 1u);
    CHECK(fake_silabs_sent[0].len == 4u);
    CHECK(fake_silabs_sent[0].data[0] == RESP_NACK);
    CHECK(fake_silabs_sent[0].data[1] == RESP_CONFIG_WRITE);

    CASE("CONFIG_CLEAR uses the same persist/queue/reload ordering and keeps the session");
    setup();
    authenticate();
    fake_silabs_store_blob[0] = 0x5au;
    fake_silabs_store_len = 1u;
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE,
                                         opendisplay_pipe_connection_tag() },
                          &r, 2u, false);
    CHECK(od_cmd_app_config_clear(&ctx, od_span_make(NULL, 0u)) == OD_CMD_OK);
    od_txq_release(&r);
    CHECK(fake_silabs_store_clears == 1u);
    CHECK(fake_silabs_store_len == 0u);
    CHECK(fake_silabs_reload_saw_queued);
    CHECK(fake_silabs_reload_saw_authenticated);
    CHECK(od_sec2_session_live(NULL));

    CASE("CONFIG_CLEAR persistence failure preserves both stored config and the live session");
    setup();
    authenticate();
    fake_silabs_store_blob[0] = 0x5au;
    fake_silabs_store_len = 1u;
    fake_silabs_store_ok = false;
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE,
                                         opendisplay_pipe_connection_tag() },
                          &r, 2u, false);
    CHECK(od_cmd_app_config_clear(&ctx, od_span_make(NULL, 0u)) == OD_CMD_NACK);
    od_txq_release(&r);
    CHECK(fake_silabs_store_clears == 0u);
    CHECK(fake_silabs_store_len == 1u);
    CHECK(fake_silabs_store_blob[0] == 0x5au);
    CHECK(fake_silabs_store_reloads == 0u);
    CHECK(od_sec2_session_live(NULL));
    CHECK(od_txq_process() == 1u);
    CHECK(fake_silabs_sent_n == 1u);
    CHECK(fake_silabs_sent[0].len == 4u);
    CHECK(fake_silabs_sent[0].data[0] == RESP_NACK);
}

static void test_nfc_exact_limit(void)
{
    const uint8_t read[] = { NFC_SUB_READ };
    od_tx_reservation_t r;
    od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 0u },
                                        &r, 0u, false);

    CASE("an NFC record of exactly 218 bytes produces the 224-byte plaintext frame");
    setup();
    fake_silabs_nfc_read_len = 218u;
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE,
                                         opendisplay_pipe_connection_tag() },
                          &r, 2u, false);
    CHECK(od_nfc_frame(&ctx, od_span_make(read, sizeof read)) == OD_CMD_OK);
    od_txq_release(&r);
    CHECK(od_txq_process() == 1u);
    CHECK(fake_silabs_sent_n == 1u);
    CHECK(fake_silabs_sent[0].len == (OD_NFC_READ_MAX + 6u));
    CHECK(fake_silabs_sent[0].data[0] == RESP_ACK);
    CHECK(fake_silabs_sent[0].data[1] == RESP_NFC_ENDPOINT);

    CASE("219-byte NFC data is refused instead of truncated to a successful reply");
    setup();
    fake_silabs_nfc_read_len = 219u;
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE,
                                         opendisplay_pipe_connection_tag() },
                          &r, 2u, false);
    CHECK(od_nfc_frame(&ctx, od_span_make(read, sizeof read)) == OD_CMD_NACK);
    od_txq_release(&r);
    CHECK(od_txq_process() == 1u);
    CHECK(fake_silabs_sent_n == 1u);
    CHECK(fake_silabs_sent[0].len == 4u);
    CHECK(fake_silabs_sent[0].data[0] == RESP_NACK);
    CHECK(fake_silabs_sent[0].data[3] == NFC_ERR_READ_FAILED);
}

/* ------------------------------------------------------- frozen 0x0083 reference behaviour --- */

#define REF_TARGET_MASK REF_BG22

/* Declared where its only production caller declares it (opendisplay_pipe.c:45); BG22 exports no
 * header for it. */
void od_cmd_silabs_reset(void);

static od_cmd_result_t ref_submit_as(const uint8_t *body, uint16_t n, od_origin_t origin,
                                      uint32_t tag)
{
    od_tx_reservation_t r;

    fake_silabs_sent_n = 0u;
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

static void ref_setup(void) { setup(); }
static void ref_knob_read_ok(bool ok)      { fake_silabs_nfc_read_ok = ok; }
static void ref_knob_read_len(uint16_t n)  { fake_silabs_nfc_read_len = n; }
static void ref_knob_write_ok(bool ok)     { fake_silabs_nfc_write_ok = ok; }
/* The assembler is the shared machine's now, so the teardown that clears it is od_nfc_reset() --
 * which od_core_reset() calls. od_cmd_silabs_reset() is down to the config assembler. */
static void ref_reset_transport(void)      { od_cmd_silabs_reset(); od_nfc_reset(); }
static unsigned ref_reply_count(void)      { return fake_silabs_sent_n; }
static uint16_t ref_reply_len(unsigned i)  { return fake_silabs_sent[i].len; }
static const uint8_t *ref_reply_data(unsigned i) { return fake_silabs_sent[i].data; }
static unsigned ref_sink_calls(void)       { return fake_silabs_nfc_write_calls; }
static uint16_t ref_sink_len(void)         { return fake_silabs_nfc_write_len; }
static uint8_t  ref_sink_rec_type(void)    { return fake_silabs_nfc_write_rec_type; }
static const uint8_t *ref_sink_data(void)  { return fake_silabs_nfc_write_data; }

/* See the Nordic driver: a spent reservation is the reachable reply failure. */
static od_cmd_result_t ref_submit_unqueueable(const uint8_t *body, uint16_t n,
                                               od_origin_t origin, uint32_t tag)
{
    od_tx_reservation_t spent = { 0u, 0u };
    od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ origin, tag }, &spent,
                                        (uint16_t)(2u + n), false);
    const od_cmd_result_t rc = od_nfc_frame(&ctx, od_span_make(body, n));

    (void)od_txq_process();
    return rc;
}

/* A foreign owner is a different connection tag on the same link; a foreign origin is a different
 * link. The production tag comes from the pipe, so neither foreign value is it. */
static od_cmd_result_t ref_submit(const uint8_t *body, uint16_t n, bool foreign_tag,
                                  bool foreign_origin, bool reply_fails)
{
    const uint32_t tag = opendisplay_pipe_connection_tag();
    const od_origin_t origin = foreign_origin ? OD_ORIGIN_LAN_PLAIN : OD_ORIGIN_BLE;
    const uint32_t use_tag = foreign_tag ? tag + 1u : tag;

    fake_silabs_sent_n = 0u;
    if (reply_fails) {
        return ref_submit_unqueueable(body, n, origin, use_tag);
    }
    return ref_submit_as(body, n, origin, use_tag);
}

static void ref_authenticate(void) { authenticate(); fake_silabs_sent_n = 0u; }
static bool ref_open_reply(const uint8_t *f, uint16_t n, uint8_t *out, uint16_t *out_n)
{
    return sec2c_open(&C, f, n, out, out_n);
}

#include "nfc_reference_cases.inc"
#include "nfc_reference_runner.inc"

static void arm_direct_end(od_cmd_ctx_t *ctx, od_tx_reservation_t *r)
{
    static const uint8_t image[4096];
    od_tx_reservation_t setup_reservation;

    CHECK(od_txq_reserve(1u, &setup_reservation) == OD_TXQ_OK);
    *ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE,
                                          opendisplay_pipe_connection_tag() },
                           &setup_reservation, 2u, false);
    CHECK(od_xfer_direct_start(ctx, od_span_none()) == OD_CMD_OK);
    od_txq_release(&setup_reservation);
    od_txq_reset();
    CHECK(od_txq_reserve(2u, &setup_reservation) == OD_TXQ_OK);
    *ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE,
                                          opendisplay_pipe_connection_tag() },
                           &setup_reservation, 2u, false);
    CHECK(od_xfer_data(ctx, od_span_make(image, sizeof image)) == OD_CMD_OK);
    od_txq_release(&setup_reservation);
    od_txq_reset();
    CHECK(od_txq_reserve(2u, r) == OD_TXQ_OK);
    *ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE,
                                          opendisplay_pipe_connection_tag() },
                           r, 2u, false);
}

static void run_direct_end(od_cmd_ctx_t *ctx, od_tx_reservation_t *r,
                           od_cmd_result_t expected)
{
    const uint8_t full[] = { 0u };
    CHECK(od_xfer_end(ctx, od_span_make(full, sizeof full)) == expected);
    od_txq_release(r);
    (void)od_txq_process();
}

static void replace_connection_on_run(void)
{
    fake_silabs_run_hook = NULL;
    open_connection(8u);
}

static void test_direct_end_barrier(void)
{
    od_tx_reservation_t r;
    od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 0u },
                                        &r, 0u, false);

    CASE("stack acceptance alone is insufficient; refresh waits for zero pending packets");
    setup();
    arm_direct_end(&ctx, &r);
    fake_silabs_resource_script[0] =
        (struct fake_silabs_resource_result){ SL_STATUS_OK, 0u, 1u, 3u };
    fake_silabs_resource_script[1] =
        (struct fake_silabs_resource_result){ SL_STATUS_OK, 0u, 0u, 0u };
    fake_silabs_resource_script_n = 2u;
    run_direct_end(&ctx, &r, OD_CMD_OK);
    CHECK(fake_silabs_resource_calls == 2u);
    CHECK(fake_silabs_run_calls == 1u);
    CHECK(fake_silabs_refreshes == 1u);
    CHECK(fake_silabs_aborts == 0u);
    CHECK(fake_silabs_sent_n == 2u);
    CHECK(fake_silabs_sent[0].data[1] == RESP_DIRECT_WRITE_END_ACK);
    CHECK(fake_silabs_sent[1].data[1] == RESP_DIRECT_WRITE_REFRESH_SUCCESS);

    CASE("resource-report API failure prevents refresh and closes the issuing connection");
    setup();
    arm_direct_end(&ctx, &r);
    fake_silabs_resource_script[0] =
        (struct fake_silabs_resource_result){ SL_STATUS_FAIL, 0u, 1u, 3u };
    fake_silabs_resource_script_n = 1u;
    run_direct_end(&ctx, &r, OD_CMD_NACK);
    CHECK(fake_silabs_refreshes == 0u);
    CHECK(fake_silabs_aborts == 1u);
    CHECK(fake_silabs_close_calls == 1u);
    CHECK(fake_silabs_closed_connection == 7u);
    CHECK(fake_silabs_sent_n == 1u);
    CHECK(fake_silabs_sent[0].data[0] == RESP_ACK);
    CHECK(fake_silabs_sent[0].data[1] == RESP_DIRECT_WRITE_END_ACK);
    CHECK(!od_xfer_active());
    CHECK(od_txq_depth() == 0u);

    CASE("resource-report overflow/corruption flags fail closed before refresh");
    setup();
    arm_direct_end(&ctx, &r);
    fake_silabs_resource_script[0] = (struct fake_silabs_resource_result){
        SL_STATUS_OK, SL_BT_RESOURCE_CONNECTION_TX_FLAGS_ERROR_PACKET_OVERFLOW |
                      SL_BT_RESOURCE_CONNECTION_TX_FLAGS_ERROR_CORRUPT,
        1u, 3u };
    fake_silabs_resource_script_n = 1u;
    run_direct_end(&ctx, &r, OD_CMD_NACK);
    CHECK(fake_silabs_refreshes == 0u);
    CHECK(fake_silabs_aborts == 1u);
    CHECK(fake_silabs_close_calls == 1u);

    CASE("a permanently pending packet reaches the shared two-second deadline and fails closed");
    setup();
    arm_direct_end(&ctx, &r);
    fake_silabs_run_advance_ms = 1000u;
    fake_silabs_resource_script[0] =
        (struct fake_silabs_resource_result){ SL_STATUS_OK, 0u, 1u, 3u };
    fake_silabs_resource_script_n = 1u;
    run_direct_end(&ctx, &r, OD_CMD_NACK);
    CHECK(fake_silabs_run_calls == 2u);
    CHECK(fake_silabs_refreshes == 0u);
    CHECK(fake_silabs_aborts == 1u);
    CHECK(fake_silabs_close_calls == 1u);

    CASE("connection-instance replacement prevents refresh and never closes the replacement");
    setup();
    arm_direct_end(&ctx, &r);
    fake_silabs_resource_script[0] =
        (struct fake_silabs_resource_result){ SL_STATUS_OK, 0u, 1u, 3u };
    fake_silabs_resource_script_n = 1u;
    fake_silabs_run_hook = replace_connection_on_run;
    run_direct_end(&ctx, &r, OD_CMD_NACK);
    CHECK(fake_silabs_refreshes == 0u);
    CHECK(fake_silabs_aborts == 1u);
    CHECK(opendisplay_pipe_connection() == 8u);
    CHECK(fake_silabs_close_calls == 0u);
}

static void queue_marker(void)
{
    const uint8_t marker[] = { RESP_ACK, 0x42u };
    od_tx_reservation_t r;
    od_reply_t rp = { OD_ORIGIN_BLE, opendisplay_pipe_connection_tag() };
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_reply(&r, &rp, marker, sizeof marker) == OD_TXQ_OK);
    od_txq_release(&r);
}

static void test_transport_deadlines(void)
{
    static uint8_t blob[1000];
    od_tx_reservation_t r;
    od_reply_t rp;

    CASE("a RETRY head is retained byte-identically, then the two-second outer hold resets it");
    setup();
    authenticate();
    queue_marker();
    fake_silabs_notify_status = SL_STATUS_NO_MORE_RESOURCE;
    opendisplay_pipe_process();
    CHECK(od_txq_depth() == 1u);
    CHECK(!sl_bt_can_process_event(8u));
    fake_silabs_now_ms += 1999u;
    opendisplay_pipe_process();
    CHECK(od_txq_depth() == 1u);
    fake_silabs_now_ms += 1u;
    opendisplay_pipe_process();
    CHECK(fake_silabs_sent_n >= 2u);
    for (unsigned i = 1u; i < fake_silabs_sent_n; ++i) {
        CHECK(fake_silabs_sent[i].len == fake_silabs_sent[0].len);
        CHECK(memcmp(fake_silabs_sent[i].data, fake_silabs_sent[0].data,
                     fake_silabs_sent[0].len) == 0);
    }
    CHECK(od_txq_depth() == 0u);
    CHECK(!od_sec2_session_live(NULL));
    CHECK(fake_silabs_aborts == 1u);
    CHECK(sl_bt_can_process_event(8u));

    CASE("CONFIG_READ's 1500 ms producer deadline fires before the 2000 ms outer hold");
    setup();
    authenticate();
    memset(blob, 0x3cu, sizeof blob);
    rp.origin = OD_ORIGIN_BLE;
    rp.tag = opendisplay_pipe_connection_tag();
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_config_read_start(&rp, &r, blob, sizeof blob) == OD_TXQ_OK);
    fake_silabs_notify_status = SL_STATUS_NO_MORE_RESOURCE;
    opendisplay_pipe_process();
    CHECK(od_config_read_active());
    fake_silabs_now_ms += 1499u;
    opendisplay_pipe_process();
    CHECK(od_config_read_active());
    fake_silabs_now_ms += 1u;
    opendisplay_pipe_process();
    CHECK(!od_config_read_active());
    CHECK(od_txq_depth() == 0u);
    CHECK(!od_sec2_session_live(NULL));
    CHECK(fake_silabs_aborts == 1u);
}

static void test_transport_pressure_policy(void)
{
    static uint8_t blob[2048];
    od_tx_reservation_t r;
    od_reply_t rp;
    unsigned passes = 0u;

    CASE("a 2048-byte read emits exactly 22 ordered chunks through the two-slot arena");
    setup();
    memset(blob, 0x6du, sizeof blob);
    rp.origin = OD_ORIGIN_BLE;
    rp.tag = opendisplay_pipe_connection_tag();
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    CHECK(od_config_read_start(&rp, &r, blob, sizeof blob) == OD_TXQ_OK);
    CHECK(!sl_bt_can_process_event(8u));
    while ((od_config_read_active() || od_txq_depth() != 0u) && passes < 100u) {
        opendisplay_pipe_process();
        if (od_config_read_active()) CHECK(!sl_bt_can_process_event(8u));
        ++passes;
    }
    CHECK(!od_config_read_active());
    CHECK(od_txq_depth() == 0u);
    CHECK(fake_silabs_sent_n == 22u);
    for (unsigned i = 0u; i < fake_silabs_sent_n; ++i) {
        CHECK(fake_silabs_sent[i].data[0] == RESP_ACK);
        CHECK(fake_silabs_sent[i].data[1] == RESP_CONFIG_READ);
        CHECK((uint16_t)(fake_silabs_sent[i].data[2] |
                         ((uint16_t)fake_silabs_sent[i].data[3] << 8)) == i);
    }
    CHECK(sl_bt_can_process_event(8u));

    CASE("write before subscription is a permanent ERROR and cannot retain the event gate");
    setup();
    set_notifications(false);
    queue_marker();
    CHECK(od_txq_depth() == 1u);
    opendisplay_pipe_process();
    CHECK(od_txq_depth() == 0u);
    CHECK(fake_silabs_sent_n == 0u); /* vendor notify was never called without a subscription */
    CHECK(sl_bt_can_process_event(8u));
}

static void exchange_mtu(uint8_t connection, uint16_t mtu)
{
    sl_bt_msg_t evt;
    memset(&evt, 0, sizeof evt);
    evt.header = sl_bt_evt_gatt_mtu_exchanged_id;
    evt.data.evt_gatt_mtu_exchanged.connection = connection;
    evt.data.evt_gatt_mtu_exchanged.mtu = mtu;
    opendisplay_pipe_handle_gatt_event(&evt);
}

/* A successful but smaller boot selection and a smaller per-connection exchange are degraded
 * operating modes. A failure of the boot API itself is handled in opendisplay_ble_on_boot() by
 * entering Gecko AppLoader and therefore never reaches this transport setter. */
static void test_degraded_boot_contracts(void)
{
    od_tx_reservation_t r;
    od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 0u },
                                        &r, 0u, false);

    CASE("no MTU exchange ever happens, and the stack still carries a 224-byte reply");
    /* THE REGRESSION GUARD. The application keeps no MTU mirror, so it cannot refuse on a stale
     * one. A mirror would sit at the ATT default of 23 here -- the exchange event is a gatt-CLIENT
     * class event and this device is only a server -- and every reply above 20 bytes, sealed ones
     * included, would be dropped on a link that carries 253. */
    setup();
    fake_silabs_nfc_read_len = 218u;
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE,
                                         opendisplay_pipe_connection_tag() },
                          &r, 2u, false);
    {
        const uint8_t read[] = { NFC_SUB_READ };
        CHECK(od_nfc_frame(&ctx, od_span_make(read, sizeof read)) == OD_CMD_OK);
    }
    od_txq_release(&r);
    CHECK(od_txq_process() == 1u);
    CHECK(fake_silabs_sent_n == 1u);
    CHECK(fake_silabs_sent[0].len == (OD_NFC_READ_MAX + 6u));
    CHECK(fake_silabs_delivered_n == 1u);     /* accepted, not merely offered */
    CHECK(od_txq_depth() == 0u);

    CASE("an over-MTU reply is handed to the stack and dropped on its refusal, never retried");
    /* The stack owns the bound; the application learns the verdict. COMMAND_TOO_LONG must be
     * permanent -- RETRY would hold the BGAPI event gate against an MTU that only a new
     * connection can raise, the same deadlock shape as !s_notify. */
    setup();
    fake_silabs_att_mtu = 30u;          /* value max 27 */
    fake_silabs_nfc_read_len = 100u;
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE,
                                         opendisplay_pipe_connection_tag() },
                          &r, 2u, false);
    {
        const uint8_t read[] = { NFC_SUB_READ };
        CHECK(od_nfc_frame(&ctx, od_span_make(read, sizeof read)) == OD_CMD_OK);
    }
    od_txq_release(&r);
    CHECK(od_txq_depth() == 1u);
    CHECK(od_txq_process() == 1u);
    CHECK(fake_silabs_sent_n == 1u);          /* the frame WAS offered, not pre-refused */
    CHECK(fake_silabs_sent[0].len > 27u);
    CHECK(fake_silabs_delivered_n == 0u);     /* and the stack refused it */
    CHECK(od_txq_depth() == 0u);              /* dropped, not retained */
    CHECK(sl_bt_can_process_event(8u));       /* and the event gate is open again */

    CASE("the negotiated-MTU event is diagnostic only and cannot cause a refusal");
    /* A small exchanged value must not become an admission rule: the stack here still carries 253,
     * so the large reply goes out despite the event reporting 30. */
    setup();
    exchange_mtu(7u, 30u);
    fake_silabs_nfc_read_len = 218u;
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE,
                                         opendisplay_pipe_connection_tag() },
                          &r, 2u, false);
    {
        const uint8_t read[] = { NFC_SUB_READ };
        CHECK(od_nfc_frame(&ctx, od_span_make(read, sizeof read)) == OD_CMD_OK);
    }
    od_txq_release(&r);
    CHECK(od_txq_process() == 1u);
    CHECK(fake_silabs_sent_n == 1u);
    CHECK(fake_silabs_sent[0].len == (OD_NFC_READ_MAX + 6u));
    CHECK(od_txq_depth() == 0u);

    CASE("without TX completion reporting, END refuses instead of refreshing on an unproven ACK");
    setup();
    opendisplay_pipe_set_tx_report_available(false);
    arm_direct_end(&ctx, &r);
    run_direct_end(&ctx, &r, OD_CMD_NACK);
    CHECK(fake_silabs_refreshes == 0u);
    CHECK(fake_silabs_aborts == 1u);
    CHECK(fake_silabs_resource_calls == 0u);  /* fails closed without polling a dead API */
    CHECK(fake_silabs_close_calls == 1u);
    CHECK(od_txq_depth() == 0u);

    CASE("the defensive missing-report state does not poison unrelated commands");
    /* Boot enable failure enters AppLoader before commands are exposed. This separately pins the
     * transport's fail-closed defense if the capability is invalidated through this seam. */
    setup();
    opendisplay_pipe_set_tx_report_available(false);
    authenticate();
    {
        const uint8_t config[] = { 0x55u };
        CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE,
                                         opendisplay_pipe_connection_tag() },
                          &r, 2u, false);
        CHECK(od_cmd_app_config_write(&ctx, od_span_make(config, sizeof config)) == OD_CMD_OK);
        od_txq_release(&r);
        CHECK(fake_silabs_store_saves == 1u);
    }
}

static void test_read_ident(void)
{
    od_tx_reservation_t r;
    od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 0u }, &r, 0u, false);
    const uint8_t *f;

    CASE("READ_IDENT with no loaded config answers display_count 0, not an error");
    setup();
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, opendisplay_pipe_connection_tag() },
                          &r, 2u, false);
    CHECK(od_cmd_app_read_ident(&ctx, od_span_make(NULL, 0u)) == OD_CMD_OK);
    od_txq_release(&r);
    CHECK(od_txq_process() == 1u);
    CHECK(fake_silabs_sent_n == 1u);
    f = fake_silabs_sent[0].data;
    CHECK(fake_silabs_sent[0].len == 11u);
    CHECK(f[0] == RESP_ACK && f[1] == RESP_READ_IDENT);
    CHECK(f[2] == (uint8_t)OD_PROTOCOL_VERSION_MINOR && f[3] == OD_IDENT_TARGET_SILABS);
    CHECK(f[4] == 0u);
    /* config-driven firmware; no display, so nothing to migrate; SEC_V2 (unclaimed, normal mode) */
    CHECK(f[5] == (0x01u | OD_IDENT_FLAG_SEC_V2));
    /* The tail: BG22's limits, so a host needs no table. 3120 B config, 128 steps, no
     * WINDOW/FILL compiled in. */
    CHECK(f[6] == (3120u & 0xFFu) && f[7] == (3120u >> 8));
    CHECK(f[8] == 128u && f[9] == 0u);
    CHECK(f[10] == 0u);

    CASE("READ_IDENT reports each display's ic type, geometry and colour scheme, LE, PLAINTEXT");
    setup();
    authenticate();   /* still plaintext: identity is answered before any key is known */
    fake_silabs_global_config.loaded = true;
    fake_silabs_global_config.display_count = 1u;
    fake_silabs_global_config.displays[0].panel_ic_type = 0x001Eu;
    fake_silabs_global_config.displays[0].pixel_width = 168u;
    fake_silabs_global_config.displays[0].pixel_height = 384u;
    fake_silabs_global_config.displays[0].color_scheme = 3u;
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, opendisplay_pipe_connection_tag() },
                          &r, 2u, false);
    CHECK(od_cmd_app_read_ident(&ctx, od_span_make(NULL, 0u)) == OD_CMD_OK);
    od_txq_release(&r);
    CHECK(od_txq_process() == 1u);
    f = fake_silabs_sent[0].data;
    CHECK(fake_silabs_sent[0].len == 18u);
    CHECK(f[4] == 1u);
    CHECK(f[5] == 0x1Eu && f[6] == 0x00u);
    CHECK(f[7] == 168u && f[8] == 0u);
    CHECK(f[9] == 0x80u && f[10] == 0x01u);
    CHECK(f[11] == 3u);
    /* no drive packets stored: needs migration; claimed (authenticate() above) */
    CHECK(f[12] == (0x03u | OD_IDENT_FLAG_SEC_V2 | OD_IDENT_FLAG_CLAIMED));
}

int main(void)
{
    test_read_ident();
    test_config_persist_before_queue();
    test_nfc_exact_limit();
    ref_run_table();
    test_direct_end_barrier();
    test_transport_deadlines();
    test_transport_pressure_policy();
    test_degraded_boot_contracts();
    printf("silabs_fault: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
