#include "fuzz_pipe_support.h"

#include "od_inflate_app.h"
#include "od_pipe.h"
#include "od_reply.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "od_zlib_inflate.h"

#include <stdarg.h>
#include <string.h>

static uint8_t s_scratch[64];
static uint32_t s_written;

od_txq_status_t od_reply(od_tx_reservation_t *r, const od_reply_t *rp,
                         const uint8_t *frame, uint16_t len)
{
    (void)r; (void)rp; (void)frame; (void)len;
    return OD_TXQ_OK;
}

od_txq_status_t od_reply_plain(od_tx_reservation_t *r, const od_reply_t *rp,
                               const uint8_t *frame, uint16_t len)
{
    (void)r; (void)rp; (void)frame; (void)len;
    return OD_TXQ_OK;
}

/* No session in this fixture, so a refusal is the plaintext reply. */
od_txq_status_t od_reply_refusal(od_tx_reservation_t *r, const od_reply_t *rp,
                                 const uint8_t *frame, uint16_t len)
{
    return od_reply_plain(r, rp, frame, len);
}

void od_inflate_app_reset(uint32_t expected) { od_zlib_stream_reset(expected); }
od_zlib_status_t od_inflate_app_push(od_span_t input, bool final)
{ return od_zlib_stream_push(input.p, input.n, final); }
od_zlib_status_t od_inflate_app_poll(uint8_t *out, size_t cap, size_t *produced)
{ return od_zlib_stream_poll(out, cap, produced); }
const char *od_inflate_app_error(void) { return od_zlib_stream_error(); }
uint32_t od_inflate_app_output_count(void) { return od_zlib_stream_output_count(); }

void od_xfer_app_prepare_start(void) { }
bool od_xfer_app_panel_info(od_xfer_panel_info_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof *out);
    out->width = 8u;
    out->height = 8u;
    out->partial_enabled = true;
    return od_color_direct_geometry(OD_COLOR_SCHEME_MONO, 8u, 8u, &out->geometry)
        == OD_COLOR_OK;
}
bool od_xfer_app_begin_full(const od_color_geometry_t *geometry)
{ (void)geometry; return true; }
bool od_xfer_app_begin_partial(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                               uint32_t plane_bytes)
{ (void)x; (void)y; (void)width; (void)height; (void)plane_bytes; return true; }
uint32_t od_xfer_app_write(uint32_t offset, od_span_t data)
{
    if (offset != s_written || data.n > UINT32_MAX) return 0u;
    s_written += (uint32_t)data.n;
    return (uint32_t)data.n;
}
od_mut_span_t od_xfer_app_inflate_scratch(void)
{ return od_mut_span_make(s_scratch, sizeof s_scratch); }
void od_xfer_app_abort(od_xfer_abort_reason_t reason) { (void)reason; }
od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner)
{ (void)owner; return OD_XFER_BARRIER_PROCEED; }
void od_xfer_app_barrier_abort(const od_reply_t *owner) { (void)owner; }
bool od_xfer_app_refresh(uint8_t mode, bool *completed)
{ (void)mode; if (completed == NULL) return false; *completed = true; return true; }
uint32_t od_xfer_app_displayed_etag(void) { return 1u; }
void od_xfer_app_set_displayed_etag(uint32_t etag) { (void)etag; }
uint32_t od_xfer_app_now_ms(void) { return 0u; }

void _od_log(int level, const char *fmt, ...)
{
    va_list ap;
    (void)level;
    (void)fmt;
    va_start(ap, fmt);
    va_end(ap);
}

void fz_pipe_reset(void)
{
    od_xfer_reset();
    s_written = 0u;
}

od_cmd_ctx_t fz_pipe_ctx(uint16_t wire_len, bool was_protected)
{
    od_cmd_ctx_t ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.rp.origin = OD_ORIGIN_BLE;
    ctx.rp.tag = 1u;
    ctx.wire_len = wire_len;
    ctx.was_protected = was_protected;
    return ctx;
}

void fz_pipe_open(uint8_t flags, uint8_t window, uint8_t ack_every,
                  uint16_t client_max_frame)
{
    uint8_t start[sizeof(struct PipeStartRequest) + sizeof(struct PipePartialExt)] = { 0u };
    size_t start_n = sizeof(struct PipeStartRequest);
    od_cmd_ctx_t ctx;

    flags &= PIPE_FLAG_COMPRESSED | PIPE_FLAG_PARTIAL;
    start[0] = PIPE_VERSION;
    start[1] = flags;
    start[2] = window;
    start[3] = ack_every;
    start[4] = (uint8_t)client_max_frame;
    start[5] = (uint8_t)(client_max_frame >> 8);
    start[6] = (flags & PIPE_FLAG_PARTIAL) != 0u ? 16u : 8u;
    if ((flags & PIPE_FLAG_PARTIAL) != 0u) {
        start[10] = 1u;  /* old_etag, little-endian */
        start[18] = 8u;  /* width, little-endian */
        start[20] = 8u;  /* height, little-endian */
        start_n = sizeof start;
    }
    ctx = fz_pipe_ctx((uint16_t)(2u + start_n), false);
    (void)od_pipe_start(&ctx, od_span_make(start, start_n));
}
