#include "od_xfer_app.h"
#include "od_inflate_app.h"
#include "od_xfer_app_test_stub.h"

static bool s_panel_ready;
static unsigned s_abort_calls;

void od_test_xfer_app_reset(void)
{
    s_panel_ready = false;
    s_abort_calls = 0u;
}

void od_test_xfer_app_set_panel_ready(bool ready) { s_panel_ready = ready; }
unsigned od_test_xfer_app_abort_calls(void) { return s_abort_calls; }

void od_inflate_app_reset(uint32_t expected_output) { (void)expected_output; }
od_zlib_status_t od_inflate_app_push(od_span_t input, bool final)
{ (void)input; (void)final; return OD_ZLIB_STATUS_ERROR; }
od_zlib_status_t od_inflate_app_poll(uint8_t *out, size_t cap, size_t *produced)
{ (void)out; (void)cap; if (produced != NULL) *produced = 0u; return OD_ZLIB_STATUS_ERROR; }
const char *od_inflate_app_error(void) { return "test stub"; }
uint32_t od_inflate_app_output_count(void) { return 0u; }

void od_xfer_app_prepare_start(void) { }
bool od_xfer_app_panel_info(od_xfer_panel_info_t *out)
{
    if (!s_panel_ready || out == NULL) {
        return false;
    }
    *out = (od_xfer_panel_info_t){ 0 };
    out->width = 32u;
    out->height = 8u;
    out->partial_enabled = true;
    return od_color_direct_geometry(OD_COLOR_SCHEME_MONO, out->width, out->height,
                                    &out->geometry) == OD_COLOR_OK;
}
bool od_xfer_app_begin_full(const od_color_geometry_t *geometry)
{ (void)geometry; return s_panel_ready; }
#if OD_CAP_PARTIAL
bool od_xfer_app_begin_partial(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                               uint32_t plane_bytes)
{
    (void)x; (void)y; (void)width; (void)height; (void)plane_bytes;
    return false;
}
#endif
uint32_t od_xfer_app_write(uint32_t stream_offset, od_span_t data)
{ (void)stream_offset; (void)data; return 0u; }
od_mut_span_t od_xfer_app_inflate_scratch(void) { return od_mut_span_none(); }
void od_xfer_app_abort(od_xfer_abort_reason_t reason) { (void)reason; ++s_abort_calls; }
od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner)
{ (void)owner; return OD_XFER_BARRIER_ABORT; }
void od_xfer_app_barrier_abort(const od_reply_t *owner) { (void)owner; }
bool od_xfer_app_refresh(uint8_t mode, bool *completed)
{ (void)mode; (void)completed; return false; }
#if OD_CAP_DISPLAYED_ETAG
uint32_t od_xfer_app_displayed_etag(void) { return 0u; }
void od_xfer_app_set_displayed_etag(uint32_t etag) { (void)etag; }
#endif
uint32_t od_xfer_app_now_ms(void) { return 0u; }
