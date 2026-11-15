#include "od_color.h"
#include "od_inflate_app.h"
#include "od_sec2.h"
#include "od_sec2_app.h"
#include "od_cmd_test_ctx.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "od_zlib_inflate.h"
#include "sec2_client.h"
#include "session_fake.h"

#include <stdio.h>
#include <string.h>

static struct sec2c g_client;
static bool g_panel_ok;
static bool g_refresh_completed;
static uint8_t g_scratch[32];
static uint8_t g_sent[8][OD_TX_FRAME_MAX];
static uint16_t g_sent_len[8];
static unsigned g_sent_n;
static unsigned g_pop;

uint32_t od_sec2_app_now_ms(void) { return 1000u; }
void od_sec2_app_uid(uint8_t uid[8]) { memcpy(uid, UID, 8u); }
uint8_t od_sec2_app_dflags(void) { return OD_SEC2_DFLAG_HAS_PANEL; }
const struct SecurityConfig *od_sec2_app_security(void) { return &g_sec; }

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                    const uint8_t *frame, uint16_t len)
{
    (void)origin; (void)tag;
    if (g_sent_n < sizeof g_sent / sizeof g_sent[0]) {
        g_sent_len[g_sent_n] = len;
        memcpy(g_sent[g_sent_n], frame, len);
    }
    ++g_sent_n;
    return OD_RADIO_SENT;
}
bool sec2c_radio_pop(uint8_t *buf, uint16_t *len)
{
    if (g_pop >= g_sent_n || g_pop >= sizeof g_sent / sizeof g_sent[0]) return false;
    memcpy(buf, g_sent[g_pop], g_sent_len[g_pop]);
    *len = g_sent_len[g_pop];
    ++g_pop;
    return true;
}
bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{ (void)origin; (void)tag; return true; }
void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{ (void)rp; (void)len; (void)why; }

void od_inflate_app_reset(uint32_t expected) { od_zlib_stream_reset(expected); }
od_zlib_status_t od_inflate_app_push(od_span_t input, bool final)
{ return od_zlib_stream_push(input.p, input.n, final); }
od_zlib_status_t od_inflate_app_poll(uint8_t *out, size_t cap, size_t *produced)
{ return od_zlib_stream_poll(out, cap, produced); }
const char *od_inflate_app_error(void) { return od_zlib_stream_error(); }
uint32_t od_inflate_app_output_count(void) { return od_zlib_stream_output_count(); }

bool od_xfer_app_panel_info(od_xfer_panel_info_t *out)
{
    if (!g_panel_ok || out == NULL) return false;
    out->width = 16u;
    out->height = 2u;
    out->partial_enabled = true;
    return od_color_direct_geometry(OD_COLOR_SCHEME_MONO, out->width, out->height,
                                    &out->geometry) == OD_COLOR_OK;
}
void od_xfer_app_prepare_start(void) { }
bool od_xfer_app_begin_full(const od_color_geometry_t *g) { return g != NULL; }
bool od_xfer_app_begin_partial(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t p)
{ (void)x; (void)y; (void)w; (void)h; (void)p; return false; }
uint32_t od_xfer_app_write(uint32_t offset, od_span_t data)
{ (void)offset; return (uint32_t)data.n; }
od_mut_span_t od_xfer_app_inflate_scratch(void)
{ return od_mut_span_make(g_scratch, sizeof g_scratch); }
void od_xfer_app_abort(od_xfer_abort_reason_t reason) { (void)reason; }
od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner)
{ (void)owner; return OD_XFER_BARRIER_PROCEED; }
void od_xfer_app_barrier_abort(const od_reply_t *owner) { (void)owner; }
bool od_xfer_app_refresh(uint8_t mode, bool *completed)
{ (void)mode; *completed = g_refresh_completed; return true; }
uint32_t od_xfer_app_displayed_etag(void) { return 0u; }
void od_xfer_app_set_displayed_etag(uint32_t etag) { (void)etag; }
uint32_t od_xfer_app_now_ms(void) { return 1000u; }

static void clear_sent(void)
{
    memset(g_sent, 0, sizeof g_sent);
    memset(g_sent_len, 0, sizeof g_sent_len);
    g_sent_n = 0u;
    g_pop = 0u;
}

static int drain_all(void)
{
    unsigned guard = 16u;
    while (od_txq_depth() != 0u && guard-- != 0u) {
        (void)od_txq_process();
    }
    return od_txq_depth() == 0u;
}

static int run_start(bool valid_panel)
{
    od_tx_reservation_t reservation;
    od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 7u },
                                        &reservation, 2u, false);

    clear_sent();
    g_panel_ok = valid_panel;
    if (od_txq_reserve(1u, &reservation) != OD_TXQ_OK) return 0;
    if (od_xfer_direct_start(&ctx, od_span_none())
        != (valid_panel ? OD_CMD_OK : OD_CMD_NACK)) return 0;
    od_txq_release(&reservation);
    return drain_all();
}

static int run_complete(bool completed)
{
    od_tx_reservation_t reservation;
    od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 7u },
                                        &reservation, 2u, false);
    uint8_t data[4] = { 1u, 2u, 3u, 4u };

    clear_sent();
    g_panel_ok = true;
    g_refresh_completed = completed;

    if (od_txq_reserve(1u, &reservation) != OD_TXQ_OK) return 0;
    if (od_xfer_direct_start(&ctx, od_span_none()) != OD_CMD_OK) return 0;
    od_txq_release(&reservation);
    if (!drain_all()) return 0;

    if (od_txq_reserve(2u, &reservation) != OD_TXQ_OK) return 0;
    ctx.r = &reservation;
    if (od_xfer_data(&ctx, od_span_make(data, sizeof data)) != OD_CMD_OK) return 0;
    od_txq_release(&reservation);
    if (!drain_all()) return 0;

    if (od_txq_reserve(2u, &reservation) != OD_TXQ_OK) return 0;
    ctx.r = &reservation;
    if (od_xfer_end(&ctx, od_span_none()) != OD_CMD_OK) return 0;
    od_txq_release(&reservation);
    return drain_all();
}

/* Sealed: every frame opens under the live session, in order. Plain: none is a sealed frame. */
static int is_sealed(unsigned i)
{
    uint8_t plain[OD_TX_FRAME_MAX];
    uint16_t plain_len = 0u;
    return g_sent[i][0] == OD_SEC2_SEALED_STATUS &&
           sec2c_open(&g_client, g_sent[i], g_sent_len[i], plain, &plain_len);
}

static int all_application_replies_are(bool sealed)
{
    unsigned i;
    if (g_sent_n != 4u) return 0;
    for (i = 0u; i < g_sent_n; ++i) {
        if (sealed ? !is_sealed(i) : (g_sent[i][0] == OD_SEC2_SEALED_STATUS)) return 0;
    }
    return 1;
}

int main(void)
{
    static const od_reply_t ble = { OD_ORIGIN_BLE, 7u };

    fake_reset();
    sec_init(0u);
    od_txq_reset();
    (void)od_sec2_boot(&g_sec);
    sec2c_init(&g_client, MASTER, UID);
    if (sec2c_auth(&g_client, &ble) != 0) {
        return 1;
    }
    od_xfer_reset();
    g_refresh_completed = true;

    if (!run_start(true) || g_sent_n != 1u || !is_sealed(0u)) {
        return 1;
    }
    od_xfer_reset();
    if (!run_start(false) || g_sent_n != 1u || g_sent_len[0] != 2u
        || g_sent[0][0] != RESP_NACK || g_sent[0][1] != RESP_DIRECT_WRITE_START_ACK) {
        return 1;
    }
    if (!run_complete(true) || !all_application_replies_are(true)) return 1;
    if (!run_complete(false) || !all_application_replies_are(true)) return 1;

    /* Unclaimed: no key record in the store, so the same replies go out in plaintext. */
    fake_reset();
    (void)od_sec2_boot(NULL);
    od_txq_reset();
    od_xfer_reset();
    if (!run_complete(true) || !all_application_replies_are(false)) return 1;
    if (!run_complete(false) || !all_application_replies_are(false)) return 1;

    printf("xfer_reply_session: START/DATA/END/success/timeout sealing policy passed\n");
    return 0;
}
