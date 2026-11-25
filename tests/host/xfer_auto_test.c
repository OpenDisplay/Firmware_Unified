#include "od_color.h"
#include "od_inflate_app.h"
#include "od_reply.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "od_zlib_inflate.h"
#include "od_cmd_test_ctx.h"

#include <stdio.h>

static unsigned g_app_replies;
static unsigned g_writes;
static unsigned g_refreshes;
static uint8_t g_scratch[32];

od_txq_status_t od_reply(od_tx_reservation_t *r, const od_reply_t *rp,
                         const uint8_t *frame, uint16_t len)
{
    (void)r; (void)rp; (void)frame; (void)len;
    ++g_app_replies;
    return OD_TXQ_OK;
}
od_txq_status_t od_reply_plain(od_tx_reservation_t *r, const od_reply_t *rp,
                               const uint8_t *frame, uint16_t len)
{ (void)r; (void)rp; (void)frame; (void)len; return OD_TXQ_OK; }

void od_inflate_app_reset(uint32_t expected) { od_zlib_stream_reset(expected); }
od_zlib_status_t od_inflate_app_push(od_span_t input, bool final)
{ return od_zlib_stream_push(input.p, input.n, final); }
od_zlib_status_t od_inflate_app_poll(uint8_t *out, size_t cap, size_t *produced)
{ return od_zlib_stream_poll(out, cap, produced); }
const char *od_inflate_app_error(void) { return od_zlib_stream_error(); }
uint32_t od_inflate_app_output_count(void) { return od_zlib_stream_output_count(); }

bool od_xfer_app_panel_info(od_xfer_panel_info_t *out)
{
    if (out == NULL) return false;
    out->width = 16u;
    out->height = 2u;
    out->partial_enabled = true;
    return od_color_direct_geometry(OD_COLOR_SCHEME_MONO, out->width, out->height,
                                    &out->geometry) == OD_COLOR_OK;
}

/* No session in this fixture, so a refusal is the plaintext reply. */
od_txq_status_t od_reply_refusal(od_tx_reservation_t *r, const od_reply_t *rp,
                                 const uint8_t *frame, uint16_t len)
{
    return od_reply_plain(r, rp, frame, len);
}
void od_xfer_app_prepare_start(void) { }
bool od_xfer_app_begin_full(const od_color_geometry_t *g) { return g != NULL; }
bool od_xfer_app_begin_partial(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t p)
{ (void)x; (void)y; (void)w; (void)h; (void)p; return false; }
uint32_t od_xfer_app_write(uint32_t offset, od_span_t data)
{ (void)offset; ++g_writes; return (uint32_t)data.n; }
od_mut_span_t od_xfer_app_inflate_scratch(void)
{ return od_mut_span_make(g_scratch, sizeof g_scratch); }
void od_xfer_app_abort(od_xfer_abort_reason_t reason) { (void)reason; }
od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner)
{ (void)owner; return OD_XFER_BARRIER_PROCEED; }
void od_xfer_app_barrier_abort(const od_reply_t *owner) { (void)owner; }
bool od_xfer_app_refresh(uint8_t mode, bool *completed)
{ (void)mode; ++g_refreshes; *completed = true; return true; }
uint32_t od_xfer_app_displayed_etag(void) { return 0u; }
void od_xfer_app_set_displayed_etag(uint32_t etag) { (void)etag; }
uint32_t od_xfer_app_now_ms(void) { return 0u; }

int main(void)
{
    od_tx_reservation_t reservation = { 0u, 0u };
    od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 1u },
                                       &reservation, 2u, false);
    uint8_t data[6] = { 0u };

    od_xfer_reset();
    if (od_xfer_direct_start(&ctx, od_span_none()) != OD_CMD_OK
        || od_xfer_data(&ctx, od_span_make(data, sizeof data)) != OD_CMD_OK
        || od_xfer_active() || g_writes != 1u || g_refreshes != 1u
        || g_app_replies != 3u) {
        return 1;
    }
    printf("xfer_auto: raw full-frame auto-END passed\n");
    return 0;
}
