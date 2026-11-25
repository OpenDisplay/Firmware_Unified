#include "od_caps.h"
#include "od_inflate_app.h"
#include "od_reply.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "od_zlib_inflate.h"
#include "od_cmd_test_ctx.h"

#include <stdio.h>

typedef char partial_must_be_disabled[(OD_CAP_PARTIAL == 0) ? 1 : -1];

static unsigned g_plain;

void od_inflate_app_reset(uint32_t expected) { od_zlib_stream_reset(expected); }
od_zlib_status_t od_inflate_app_push(od_span_t input, bool final)
{ return od_zlib_stream_push(input.p, input.n, final); }
od_zlib_status_t od_inflate_app_poll(uint8_t *out, size_t cap, size_t *produced)
{ return od_zlib_stream_poll(out, cap, produced); }
const char *od_inflate_app_error(void) { return od_zlib_stream_error(); }
uint32_t od_inflate_app_output_count(void) { return od_zlib_stream_output_count(); }

od_txq_status_t od_reply(od_tx_reservation_t *r, const od_reply_t *rp,
                         const uint8_t *frame, uint16_t len)
{
    (void)r; (void)rp; (void)frame; (void)len;
    return OD_TXQ_OK;
}

od_txq_status_t od_reply_plain(od_tx_reservation_t *r, const od_reply_t *rp,
                               const uint8_t *frame, uint16_t len)
{
    (void)r; (void)rp;
    if (len == 4u && frame[0] == 0xFFu && frame[1] == 0x76u
        && frame[2] == OD_ERR_PARTIAL_UNSUPPORTED && frame[3] == 0u) {
        ++g_plain;
    }
    return OD_TXQ_OK;
}

/* No session in this fixture, so a refusal is the plaintext reply. */
od_txq_status_t od_reply_refusal(od_tx_reservation_t *r, const od_reply_t *rp,
                                 const uint8_t *frame, uint16_t len)
{
    return od_reply_plain(r, rp, frame, len);
}

/* Unused by the capability-off entry point, but required if another transfer object is linked. */
bool od_xfer_app_panel_info(od_xfer_panel_info_t *out) { (void)out; return false; }
void od_xfer_app_prepare_start(void) { }
bool od_xfer_app_begin_full(const od_color_geometry_t *g) { (void)g; return false; }
uint32_t od_xfer_app_write(uint32_t o, od_span_t d) { (void)o; (void)d; return 0u; }
od_mut_span_t od_xfer_app_inflate_scratch(void) { return od_mut_span_none(); }
void od_xfer_app_abort(od_xfer_abort_reason_t reason) { (void)reason; }
od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *o)
{ (void)o; return OD_XFER_BARRIER_ABORT; }
void od_xfer_app_barrier_abort(const od_reply_t *o) { (void)o; }
bool od_xfer_app_refresh(uint8_t m, bool *c) { (void)m; (void)c; return false; }
uint32_t od_xfer_app_now_ms(void) { return 0u; }
static uint32_t g_displayed_etag;
uint32_t od_xfer_app_displayed_etag(void) { return g_displayed_etag; }
void od_xfer_app_set_displayed_etag(uint32_t etag) { g_displayed_etag = etag; }

int main(void)
{
    od_tx_reservation_t reservation = { 0u, 0u };
    od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 1u },
                                       &reservation, 19u, false);
    uint8_t body[17] = { 0u };
    if (od_xfer_partial_start(&ctx, od_span_make(body, sizeof body)) != OD_CMD_NACK
        || g_plain != 1u || od_xfer_active()) {
        return 1;
    }
    printf("xfer_silabs: capability-off reply passed\n");
    return 0;
}
