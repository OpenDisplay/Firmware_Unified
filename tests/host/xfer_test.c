#include "od_color.h"
#include "od_inflate_app.h"
#include "od_log.h"
#include "od_reply.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "od_zlib_inflate.h"
#include "od_cmd_test_ctx.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case;

#define CHECK(cond) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond); \
    } \
} while (0)
#define CASE(name) (g_case = (name))

typedef struct {
    bool plain;
    uint16_t len;
    uint8_t bytes[8];
} reply_record_t;

static reply_record_t g_replies[16];
static unsigned g_reply_n;
static int g_fail_app_reply;
static od_xfer_panel_info_t g_panel;
static od_color_geometry_t g_begin_geometry;
static bool g_panel_info_ok;
static bool g_begin_ok;
static bool g_refresh_call_ok;
static bool g_refresh_completed;
static od_xfer_barrier_t g_barrier;
static unsigned g_begin_full_calls;
static unsigned g_begin_partial_calls;
static unsigned g_write_calls;
static unsigned g_abort_calls;
static unsigned g_barrier_calls;
static unsigned g_barrier_abort_calls;
static unsigned g_refresh_calls;
static unsigned g_prepare_start_calls;
static unsigned g_pipe_cancel_calls;
static bool g_pipe_active;
static od_xfer_abort_reason_t g_abort_reasons[16];
static uint8_t g_refresh_modes[16];
static uint32_t g_offsets[16];
static uint32_t g_lengths[16];
static uint32_t g_consume_limit;
static uint32_t g_etag;
static bool g_refresh_stores;   /* the target stored the frame (image-store preload), not shown */
static unsigned g_callback_sequence;
static unsigned g_etag_clear_sequence;
static unsigned g_abort_sequence;
static uint8_t g_scratch[64];
static uint8_t g_written[256];
static size_t g_written_n;
static uint32_t g_now_ms;
static od_tx_reservation_t g_reservation;
static const od_reply_t OWNER = { OD_ORIGIN_BLE, 7u };
static const od_reply_t OTHER = { OD_ORIGIN_LAN_PLAIN, 9u };

#define LOG_MAX 32u
static struct {
    int level;
    char text[256];
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

static bool logged(int level, const char *needle)
{
    unsigned i;

    for (i = 0u; i < g_log_n; ++i) {
        if (g_logs[i].level == level && strstr(g_logs[i].text, needle) != NULL) {
            return true;
        }
    }
    return false;
}

static unsigned log_count_exact(int level, const char *text)
{
    unsigned count = 0u;
    unsigned i;

    for (i = 0u; i < g_log_n; ++i) {
        if (g_logs[i].level == level && strcmp(g_logs[i].text, text) == 0) {
            ++count;
        }
    }
    return count;
}

static unsigned log_count_prefix(int level, const char *prefix)
{
    unsigned count = 0u;
    unsigned i;

    for (i = 0u; i < g_log_n; ++i) {
        if (g_logs[i].level == level
            && strncmp(g_logs[i].text, prefix, strlen(prefix)) == 0) {
            ++count;
        }
    }
    return count;
}

static od_txq_status_t record_reply(bool plain, const uint8_t *frame, uint16_t len)
{
    if (!plain && g_fail_app_reply == (int)g_reply_n) {
        ++g_reply_n;
        return OD_TXQ_SEAL_FAILED;
    }
    if (g_reply_n < sizeof g_replies / sizeof g_replies[0]) {
        reply_record_t *r = &g_replies[g_reply_n];
        r->plain = plain;
        r->len = len;
        if (len <= sizeof r->bytes) {
            memcpy(r->bytes, frame, len);
        }
    }
    ++g_reply_n;
    return OD_TXQ_OK;
}

od_txq_status_t od_reply(od_tx_reservation_t *r, const od_reply_t *rp,
                         const uint8_t *frame, uint16_t len)
{
    (void)r;
    (void)rp;
    return record_reply(false, frame, len);
}

od_txq_status_t od_reply_plain(od_tx_reservation_t *r, const od_reply_t *rp,
                               const uint8_t *frame, uint16_t len)
{
    (void)r;
    (void)rp;
    return record_reply(true, frame, len);
}

void od_inflate_app_reset(uint32_t expected_output_size)
{
    od_zlib_stream_reset(expected_output_size);
}

od_zlib_status_t od_inflate_app_push(od_span_t input, bool final)
{
    return od_zlib_stream_push(input.p, input.n, final);
}

od_zlib_status_t od_inflate_app_poll(uint8_t *output, size_t capacity, size_t *produced)
{
    return od_zlib_stream_poll(output, capacity, produced);
}

const char *od_inflate_app_error(void) { return od_zlib_stream_error(); }
uint32_t od_inflate_app_output_count(void) { return od_zlib_stream_output_count(); }

bool od_xfer_app_panel_info(od_xfer_panel_info_t *out)
{
    if (g_panel_info_ok && out != NULL) {
        *out = g_panel;
    }
    return g_panel_info_ok;
}

void od_xfer_app_prepare_start(void)
{
    ++g_prepare_start_calls;
    if (g_pipe_active) {
        g_pipe_active = false;
        ++g_pipe_cancel_calls;
    }
}

bool od_xfer_app_begin_full(const od_color_geometry_t *geometry)
{
    ++g_begin_full_calls;
    if (geometry != NULL) {
        g_begin_geometry = *geometry;
    }
    return g_begin_ok && geometry != NULL;
}

bool od_xfer_app_begin_partial(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                               uint32_t plane_bytes)
{
    ++g_begin_partial_calls;
    return g_begin_ok && x == 0u && y == 0u && width <= g_panel.width
        && height <= g_panel.height && plane_bytes == ((uint32_t)width + 7u) / 8u * height;
}

uint32_t od_xfer_app_write(uint32_t stream_offset, od_span_t data)
{
    uint32_t consumed = (uint32_t)data.n;
    if (g_write_calls < sizeof g_offsets / sizeof g_offsets[0]) {
        g_offsets[g_write_calls] = stream_offset;
        g_lengths[g_write_calls] = (uint32_t)data.n;
    }
    ++g_write_calls;
    if (consumed > g_consume_limit) {
        consumed = g_consume_limit;
    }
    if (consumed <= sizeof g_written - g_written_n) {
        memcpy(g_written + g_written_n, data.p, consumed);
        g_written_n += consumed;
    }
    return consumed;
}

od_mut_span_t od_xfer_app_inflate_scratch(void)
{
    return od_mut_span_make(g_scratch, sizeof g_scratch);
}

void od_xfer_app_abort(od_xfer_abort_reason_t reason)
{
    g_abort_sequence = ++g_callback_sequence;
    if (g_abort_calls < sizeof g_abort_reasons / sizeof g_abort_reasons[0]) {
        g_abort_reasons[g_abort_calls] = reason;
    }
    ++g_abort_calls;
}

od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner)
{
    ++g_barrier_calls;
    CHECK(owner != NULL && owner->origin == OWNER.origin && owner->tag == OWNER.tag);
    CHECK(log_count_prefix(OD_LOG_INFO, "DW complete:") == 0u);
    return g_barrier;
}

void od_xfer_app_barrier_abort(const od_reply_t *owner)
{
    ++g_barrier_abort_calls;
    CHECK(owner != NULL && owner->origin == OWNER.origin && owner->tag == OWNER.tag);
}

bool od_xfer_app_refresh(uint8_t mode, bool *completed)
{
    if (g_refresh_calls < sizeof g_refresh_modes / sizeof g_refresh_modes[0]) {
        g_refresh_modes[g_refresh_calls] = mode;
    }
    ++g_refresh_calls;
    CHECK(mode <= 2u);
    CHECK(log_count_prefix(OD_LOG_INFO, "DW complete:") == 0u);
    g_now_ms += 30000u;
    if (completed != NULL) {
        *completed = g_refresh_completed;
    }
    if (g_refresh_stores) {
        od_xfer_note_refresh_stored();
    }
    return g_refresh_call_ok;
}

uint32_t od_xfer_app_displayed_etag(void) { return g_etag; }
void od_xfer_app_set_displayed_etag(uint32_t etag)
{
    if (etag == 0u) {
        g_etag_clear_sequence = ++g_callback_sequence;
    }
    g_etag = etag;
}
uint32_t od_xfer_app_now_ms(void) { return g_now_ms; }

static void setup(void)
{
    od_xfer_reset();
    memset(g_replies, 0, sizeof g_replies);
    memset(g_offsets, 0, sizeof g_offsets);
    memset(g_lengths, 0, sizeof g_lengths);
    memset(g_written, 0, sizeof g_written);
    memset(g_abort_reasons, 0, sizeof g_abort_reasons);
    memset(g_refresh_modes, 0, sizeof g_refresh_modes);
    memset(&g_reservation, 0, sizeof g_reservation);
    g_reply_n = 0u;
    g_fail_app_reply = -1;
    memset(&g_panel, 0, sizeof g_panel);
    memset(&g_begin_geometry, 0, sizeof g_begin_geometry);
    CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_MONO, 16u, 2u, &g_panel.geometry)
          == OD_COLOR_OK);
    g_panel.width = 16u;
    g_panel.height = 2u;
    g_panel.partial_enabled = true;
    g_panel_info_ok = true;
    g_begin_ok = true;
    g_refresh_call_ok = true;
    g_refresh_completed = true;
    g_barrier = OD_XFER_BARRIER_PROCEED;
    g_begin_full_calls = 0u;
    g_begin_partial_calls = 0u;
    g_write_calls = 0u;
    g_abort_calls = 0u;
    g_barrier_calls = 0u;
    g_barrier_abort_calls = 0u;
    g_refresh_calls = 0u;
    g_prepare_start_calls = 0u;
    g_pipe_cancel_calls = 0u;
    g_pipe_active = false;
    g_consume_limit = UINT32_MAX;
    g_etag = 0x11223344u;
    g_refresh_stores = false;
    g_callback_sequence = 0u;
    g_etag_clear_sequence = 0u;
    g_abort_sequence = 0u;
    g_written_n = 0u;
    g_now_ms = 1234u;
    g_log_n = 0u;
}

static uint32_t adler32(od_span_t bytes)
{
    uint32_t a = 1u;
    uint32_t b = 0u;
    size_t i;
    for (i = 0u; i < bytes.n; ++i) {
        a = (a + bytes.p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static size_t make_stored(od_span_t plain, uint8_t *out)
{
    const uint16_t len = (uint16_t)plain.n;
    const uint16_t inv = (uint16_t)~len;
    const uint32_t sum = adler32(plain);
    out[0] = 0x18u; out[1] = 0x19u; out[2] = 0x01u;
    out[3] = (uint8_t)len; out[4] = (uint8_t)(len >> 8);
    out[5] = (uint8_t)inv; out[6] = (uint8_t)(inv >> 8);
    memcpy(out + 7u, plain.p, plain.n);
    out[7u + plain.n] = (uint8_t)(sum >> 24);
    out[8u + plain.n] = (uint8_t)(sum >> 16);
    out[9u + plain.n] = (uint8_t)(sum >> 8);
    out[10u + plain.n] = (uint8_t)sum;
    return plain.n + 11u;
}

static void test_raw_direct(void)
{
    od_cmd_ctx_t owner = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);
    od_cmd_ctx_t other = od_test_cmd_ctx(OTHER, &g_reservation, 2u, false);
    od_cmd_ctx_t stale = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 8u },
                                         &g_reservation, 2u, false);
    /* THE OWNER'S TAG ON A DIFFERENT LINK. Without this the suite only ever varies the tag, or
     * varies both fields at once, so an owner check that compared the tag alone would pass
     * everything here -- tags are per-transport and collide across links by construction. */
    od_cmd_ctx_t crosslink = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_LAN_PLAIN, OWNER.tag },
                                             &g_reservation, 2u, false);
    od_reply_t recorded_owner;
    uint8_t tolerated[3] = { 1u, 2u, 3u };
    uint8_t data[6] = { 10u, 11u, 12u, 13u, 14u, 15u };
    uint8_t end[5] = { 1u, 0xAAu, 0xBBu, 0xCCu, 0xDDu };

    CASE("raw direct offsets owner and overrun");
    setup();
    g_pipe_active = true;
    CHECK(od_xfer_direct_start(&owner, od_span_make(tolerated, sizeof tolerated)) == OD_CMD_OK);
    CHECK(g_prepare_start_calls == 1u);
    CHECK(!g_pipe_active && g_pipe_cancel_calls == 1u);
    CHECK(od_xfer_owner(&recorded_owner) && recorded_owner.origin == OWNER.origin
          && recorded_owner.tag == OWNER.tag);
    CHECK(g_begin_full_calls == 1u && g_reply_n == 1u && !g_replies[0].plain);
    CHECK(od_xfer_data(&other, od_span_make(data, 2u)) == OD_CMD_OK);
    CHECK(g_write_calls == 0u && g_reply_n == 1u);
    CHECK(od_xfer_data(&stale, od_span_make(data, 2u)) == OD_CMD_OK);
    CHECK(g_write_calls == 0u && g_reply_n == 1u);
    CHECK(od_xfer_data(&crosslink, od_span_make(data, 2u)) == OD_CMD_OK);
    CHECK(g_write_calls == 0u && g_reply_n == 1u);
    CHECK(od_xfer_data(&owner, od_span_none()) == OD_CMD_OK);
    CHECK(g_write_calls == 0u && g_reply_n == 1u);
    CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_OK);
    CHECK(g_write_calls == 1u && g_offsets[0] == 0u && g_lengths[0] == 4u);
    CHECK(g_written_n == 4u);
    CHECK(od_xfer_end(&owner, od_span_make(end, sizeof end)) == OD_CMD_OK);
    CHECK(g_barrier_calls == 1u && g_refresh_calls == 1u && !od_xfer_active());
    CHECK(!od_xfer_owner(&recorded_owner));
    CHECK(g_etag == 0xAABBCCDDu);
    CHECK(g_reply_n == 4u && !g_replies[1].plain && !g_replies[2].plain
          && !g_replies[3].plain);
}

static void test_transfer_logging(void)
{
    od_cmd_ctx_t owner = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);
    uint8_t raw[100];
    uint8_t plain[4] = { 0x21u, 0x22u, 0x23u, 0x24u };
    uint8_t compressed[32];
    uint8_t start[4] = { 4u, 0u, 0u, 0u };
    size_t compressed_n;

    CASE("shared raw transfer progress and quiet snapshot");
    setup();
    memset(raw, 0xA5, sizeof raw);
    g_panel.geometry.total_bytes = sizeof raw;
    CHECK(!od_xfer_log_quiet(CMD_DIRECT_WRITE_DATA));
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
    CHECK(logged(OD_LOG_INFO, "DW start: 100 bytes expected, raw (uncompressed)"));
#else
    CHECK(!logged(OD_LOG_INFO, "DW start:"));
#endif
    CHECK(!od_xfer_log_quiet(CMD_DIRECT_WRITE_DATA));
    CHECK(od_xfer_data(&owner, od_span_make(raw, 6u)) == OD_CMD_OK);
    CHECK(od_xfer_log_quiet(CMD_DIRECT_WRITE_DATA));
    CHECK(od_xfer_log_quiet(CMD_PIPE_WRITE_DATA));
    CHECK(!od_xfer_log_quiet(CMD_DIRECT_WRITE_END));
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    CHECK(logged(OD_LOG_DEBUG, "DW frame 1: 6 bytes: A5 A5 A5 A5 A5 A5"));
    CHECK(logged(OD_LOG_DEBUG, "DW expecting ~17 chunks"));
    CHECK(logged(OD_LOG_DEBUG, "DW 6% (1 chunks, 6/100 bytes)"));
#else
    CHECK(!logged(OD_LOG_DEBUG, "DW frame 1:"));
#endif
    CHECK(od_xfer_data(&owner, od_span_make(raw + 6u, 9u)) == OD_CMD_OK);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    CHECK(logged(OD_LOG_DEBUG, "DW 15% (2 chunks, 15/100 bytes)"));
#endif
    CHECK(od_xfer_data(&owner, od_span_make(raw + 15u, 85u)) == OD_CMD_OK);
    g_now_ms = 2468u;
    CHECK(od_xfer_end(&owner, od_span_none()) == OD_CMD_OK);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    CHECK(logged(OD_LOG_DEBUG, "DW final frame 3: 85 bytes"));
#else
    CHECK(!logged(OD_LOG_DEBUG, "DW final frame"));
#endif
    CHECK(logged(OD_LOG_INFO,
                 "DW complete: direct full rx=0.1KB wr=0.1/0.1KB n=3 t=1.2s r=0.1KB/s"));
    CHECK(!od_xfer_log_quiet(CMD_DIRECT_WRITE_DATA));

    CASE("shared compressed transfer records on-wire bytes");
    setup();
    compressed_n = make_stored(od_span_make(plain, sizeof plain), compressed);
    CHECK(compressed_n == 15u);
    CHECK(od_xfer_direct_start(&owner, od_span_make(start, sizeof start)) == OD_CMD_OK);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
    CHECK(logged(OD_LOG_INFO, "DW start: 4 bytes expected, zlib streaming"));
#endif
    CHECK(od_xfer_data(&owner, od_span_make(compressed, compressed_n)) == OD_CMD_OK);
    g_now_ms = 2234u;
    CHECK(od_xfer_end(&owner, od_span_none()) == OD_CMD_OK);
    CHECK(logged(OD_LOG_INFO,
                 "direct full rx=0.0KB wr=0.0/0.0KB n=1 t=1.0s r=0.0KB/s z=0.27x"));
}

static void test_start_boundaries(void)
{
    od_cmd_ctx_t owner = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);
    uint8_t body[5] = { 4u, 0u, 0u, 0u, 0x18u };
    size_t n;

    CASE("direct START lengths zero through three are raw");
    for (n = 0u; n <= 3u; ++n) {
        setup();
        CHECK(od_xfer_direct_start(&owner, od_span_make(body, n)) == OD_CMD_OK);
        CHECK(g_begin_full_calls == 1u && g_write_calls == 0u);
        od_xfer_reset();
    }

    CASE("direct START lengths four and five defer truncation to END");
    for (n = 4u; n <= 5u; ++n) {
        setup();
        CHECK(od_xfer_direct_start(&owner, od_span_make(body, n)) == OD_CMD_OK);
        CHECK(od_xfer_end(&owner, od_span_none()) == OD_CMD_NACK);
        CHECK(!od_xfer_active() && g_replies[g_reply_n - 1u].plain);
    }

    CASE("replacement START aborts old owner once");
    setup();
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
    CHECK(g_abort_calls == 1u && od_xfer_active());
    CHECK(g_abort_reasons[0] == OD_XFER_ABORT_REPLACED);
}

static void test_short_write_and_barrier(void)
{
    od_cmd_ctx_t owner = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);
    uint8_t data[2] = { 1u, 2u };

    CASE("short write is plain refusal");
    setup();
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
    g_consume_limit = 1u;
    CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_NACK);
    CHECK(g_abort_calls == 1u && !od_xfer_active());
    CHECK(g_abort_reasons[0] == OD_XFER_ABORT_STREAM_FAILED);
    CHECK(g_reply_n == 2u && g_replies[1].plain);

    CASE("ack enqueue failure recovers once");
    setup();
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
    CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_OK);
    CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_OK);
    g_fail_app_reply = (int)g_reply_n;
    CHECK(od_xfer_end(&owner, od_span_none()) == OD_CMD_NACK);
    CHECK(g_barrier_abort_calls == 1u && g_barrier_calls == 0u && g_refresh_calls == 0u);

    CASE("START and DATA reply substitution aborts active transfer");
    setup();
    g_fail_app_reply = 0;
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_NACK);
    CHECK(g_abort_calls == 1u && g_abort_reasons[0] == OD_XFER_ABORT_REPLY_FAILED);
    setup();
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
    g_fail_app_reply = (int)g_reply_n;
    CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_NACK);
    CHECK(g_abort_calls == 1u && g_abort_reasons[0] == OD_XFER_ABORT_REPLY_FAILED);

    CASE("final-status substitution leaves completed state clear");
    setup();
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
    CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_OK);
    CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_OK);
    g_fail_app_reply = (int)g_reply_n + 1;
    CHECK(od_xfer_end(&owner, od_span_none()) == OD_CMD_NACK);
    CHECK(!od_xfer_active() && g_abort_calls == 0u && g_refresh_calls == 1u);

    CASE("barrier abort recovers once");
    setup();
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
    CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_OK);
    CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_OK);
    g_barrier = OD_XFER_BARRIER_ABORT;
    CHECK(od_xfer_end(&owner, od_span_none()) == OD_CMD_NACK);
    CHECK(g_barrier_abort_calls == 1u && g_barrier_calls == 1u && g_refresh_calls == 0u);
}

static void complete_raw_direct(const od_cmd_ctx_t *owner, od_span_t end)
{
    uint8_t data[4] = { 1u, 2u, 3u, 4u };
    CHECK(od_xfer_direct_start(owner, od_span_none()) == OD_CMD_OK);
    CHECK(od_xfer_data(owner, od_span_make(data, sizeof data)) == OD_CMD_OK);
    CHECK(od_xfer_end(owner, end) == OD_CMD_OK);
}

static void test_end_and_refresh_boundaries(void)
{
    od_cmd_ctx_t owner = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);
    uint8_t end[6] = { 0u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u };
    size_t n;

    CASE("direct END lengths and selectors");
    for (n = 0u; n <= 5u; ++n) {
        setup();
        end[0] = (uint8_t)(n == 1u ? 1u : 2u);
        complete_raw_direct(&owner, od_span_make(end, n));
        CHECK(g_refresh_calls == 1u);
        CHECK(g_refresh_modes[0] == (n == 1u ? 1u : 0u));
    }

    CASE("direct refresh timeout etag asymmetry");
    setup();
    g_refresh_completed = false;
    complete_raw_direct(&owner, od_span_none());
    CHECK(g_etag == 0x11223344u);
    CHECK(g_replies[g_reply_n - 1u].bytes[1] == RESP_DIRECT_WRITE_REFRESH_TIMEOUT);

    setup();
    g_refresh_completed = false;
    complete_raw_direct(&owner, od_span_make(end, 5u));
    CHECK(g_etag == 0u);

    CASE("refresh invocation failure abort reason");
    setup();
    g_refresh_call_ok = false;
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
    {
        uint8_t data[4] = { 1u, 2u, 3u, 4u };
        CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_OK);
    }
    CHECK(od_xfer_end(&owner, od_span_none()) == OD_CMD_NACK);
    CHECK(g_abort_calls == 1u && g_abort_reasons[0] == OD_XFER_ABORT_REFRESH_FAILED);
}

static void test_compressed_direct(void)
{
    od_cmd_ctx_t owner = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);
    uint8_t plain[4] = { 0x21u, 0x22u, 0x23u, 0x24u };
    uint8_t start[32];
    size_t compressed_n;

    CASE("compressed inline exact");
    setup();
    compressed_n = make_stored(od_span_make(plain, sizeof plain), start + 4u);
    start[0] = 4u; start[1] = 0u; start[2] = 0u; start[3] = 0u;
    CHECK(od_xfer_direct_start(&owner, od_span_make(start, compressed_n + 4u)) == OD_CMD_OK);
    CHECK(g_written_n == sizeof plain && memcmp(g_written, plain, sizeof plain) == 0);
    CHECK(od_xfer_end(&owner, od_span_none()) == OD_CMD_OK);

    CASE("compressed declared size mismatch");
    setup();
    start[0] = 5u;
    CHECK(od_xfer_direct_start(&owner, od_span_make(start, compressed_n + 4u)) == OD_CMD_NACK);
    CHECK(g_begin_full_calls == 0u && g_reply_n == 1u && g_replies[0].plain);
}

static void test_controller_planes_incomplete(void)
{
    od_cmd_ctx_t owner = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);
    uint8_t short_planes[7] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u };

    CASE("controller-plane geometry and incomplete END");
    setup();
    CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_BWR, 16u, 2u, &g_panel.geometry)
          == OD_COLOR_OK);
    CHECK(g_panel.geometry.layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES);
    CHECK(g_panel.geometry.part_bytes[0] == 4u && g_panel.geometry.part_bytes[1] == 4u);
    CHECK(g_panel.geometry.initial_plane == OD_COLOR_PLANE_0);
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
    CHECK(g_begin_geometry.layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES);
    CHECK(g_begin_geometry.part_bytes[0] == 4u && g_begin_geometry.part_bytes[1] == 4u);
    CHECK(g_begin_geometry.initial_plane == OD_COLOR_PLANE_0);
    CHECK(od_xfer_data(&owner, od_span_make(short_planes, sizeof short_planes)) == OD_CMD_OK);
    CHECK(od_xfer_end(&owner, od_span_none()) == OD_CMD_NACK);
    CHECK(g_abort_calls == 1u && g_abort_reasons[0] == OD_XFER_ABORT_INCOMPLETE);
    CHECK(!od_xfer_active() && g_replies[g_reply_n - 1u].plain);
}

static void make_partial_start(uint8_t out[17], uint8_t flags, uint32_t old_etag,
                               uint32_t new_etag)
{
    memset(out, 0, 17u);
    out[0] = flags;
    out[1] = (uint8_t)(old_etag >> 24); out[2] = (uint8_t)(old_etag >> 16);
    out[3] = (uint8_t)(old_etag >> 8); out[4] = (uint8_t)old_etag;
    out[5] = (uint8_t)(new_etag >> 24); out[6] = (uint8_t)(new_etag >> 16);
    out[7] = (uint8_t)(new_etag >> 8); out[8] = (uint8_t)new_etag;
    out[13] = 0u; out[14] = 8u;
    out[15] = 0u; out[16] = 1u;
}

static void test_partial(void)
{
    od_cmd_ctx_t owner = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);
    uint8_t start[17];
    uint8_t a = 0xA5u;
    uint8_t b = 0x5Au;

    CASE("partial planes and etag commit");
    setup();
    make_partial_start(start, 0u, g_etag, 0x55667788u);
    CHECK(od_xfer_partial_start(&owner, od_span_make(start, sizeof start)) == OD_CMD_OK);
    CHECK(g_begin_partial_calls == 1u);
    CHECK(od_xfer_data(&owner, od_span_make(&a, 1u)) == OD_CMD_OK);
    CHECK(od_xfer_data(&owner, od_span_make(&b, 1u)) == OD_CMD_OK);
    CHECK(g_offsets[0] == 0u && g_offsets[1] == 1u);
    g_now_ms = 2234u;
    CHECK(od_xfer_end(&owner, od_span_none()) == OD_CMD_OK);
    CHECK(g_etag == 0x55667788u && g_refresh_calls == 1u);
    CHECK(logged(OD_LOG_INFO,
                 "DW complete: direct partial rx=0.0KB wr=0.0/0.0KB n=2 "
                 "t=1.0s r=0.0KB/s"));

    CASE("partial validation clears etag and replies plain");
    setup();
    make_partial_start(start, 0x80u, g_etag, 0x55667788u);
    CHECK(od_xfer_partial_start(&owner, od_span_make(start, sizeof start)) == OD_CMD_NACK);
    CHECK(g_etag == 0u && g_begin_partial_calls == 0u);
    CHECK(g_reply_n == 1u && g_replies[0].plain && g_replies[0].len == 4u);

    CASE("partial incomplete end clears etag");
    setup();
    make_partial_start(start, 0u, g_etag, 0x55667788u);
    CHECK(od_xfer_partial_start(&owner, od_span_make(start, sizeof start)) == OD_CMD_OK);
    CHECK(od_xfer_data(&owner, od_span_make(&a, 1u)) == OD_CMD_OK);
    CHECK(od_xfer_end(&owner, od_span_none()) == OD_CMD_NACK);
    CHECK(g_etag == 0u && g_abort_calls == 1u);
    CHECK(g_abort_reasons[0] == OD_XFER_ABORT_INCOMPLETE);
    CHECK(g_etag_clear_sequence != 0u && g_etag_clear_sequence < g_abort_sequence);
    CHECK(g_replies[g_reply_n - 1u].plain);
}

static void test_partial_end_boundaries(void)
{
    od_cmd_ctx_t owner = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);
    uint8_t start[17];
    uint8_t data[2] = { 0xA5u, 0x5Au };
    uint8_t end[2] = { 0u, 0u };
    uint8_t selector;

    CASE("partial END empty and every selector");
    setup();
    make_partial_start(start, 0u, g_etag, 0x55667788u);
    CHECK(od_xfer_partial_start(&owner, od_span_make(start, sizeof start)) == OD_CMD_OK);
    CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_OK);
    CHECK(od_xfer_end(&owner, od_span_none()) == OD_CMD_OK);
    CHECK(g_refresh_modes[0] == 2u);
    for (selector = 0u; selector <= 2u; ++selector) {
        setup();
        make_partial_start(start, 0u, g_etag, 0x55667788u);
        CHECK(od_xfer_partial_start(&owner, od_span_make(start, sizeof start)) == OD_CMD_OK);
        CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_OK);
        end[0] = selector;
        CHECK(od_xfer_end(&owner, od_span_make(end, 1u)) == OD_CMD_OK);
        CHECK(g_refresh_modes[0] == (selector <= 1u ? selector : 2u));
    }

    CASE("partial END rejects length two");
    setup();
    make_partial_start(start, 0u, g_etag, 0x55667788u);
    CHECK(od_xfer_partial_start(&owner, od_span_make(start, sizeof start)) == OD_CMD_OK);
    CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_OK);
    CHECK(od_xfer_end(&owner, od_span_make(end, sizeof end)) == OD_CMD_NACK);
    CHECK(g_abort_calls == 1u && g_abort_reasons[0] == OD_XFER_ABORT_STREAM_FAILED);
}

static void test_partial_compressed_and_boundaries(void)
{
    od_cmd_ctx_t owner = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);
    uint8_t start[17];
    uint8_t plain[8] = { 0xA5u, 0x5Au, 1u, 2u, 3u, 4u, 5u, 6u };
    uint8_t compressed[32];
    size_t compressed_n;

    CASE("compressed partial");
    setup();
    make_partial_start(start, 1u, g_etag, 0x55667788u);
    compressed_n = make_stored(od_span_make(plain, 2u), compressed);
    CHECK(od_xfer_partial_start(&owner, od_span_make(start, sizeof start)) == OD_CMD_OK);
    CHECK(od_xfer_data(&owner, od_span_make(compressed, compressed_n)) == OD_CMD_OK);
    CHECK(od_xfer_end(&owner, od_span_none()) == OD_CMD_OK);
    CHECK(g_written_n == 2u && memcmp(g_written, plain, 2u) == 0);

    CASE("partial plane boundary at every offered split");
    for (size_t split = 1u; split < sizeof plain; ++split) {
        setup();
        make_partial_start(start, 0u, g_etag, 0x55667788u);
        start[13] = 0u; start[14] = 16u;
        start[15] = 0u; start[16] = 2u;
        CHECK(od_xfer_partial_start(&owner, od_span_make(start, sizeof start)) == OD_CMD_OK);
        CHECK(od_xfer_data(&owner, od_span_make(plain, split)) == OD_CMD_OK);
        CHECK(od_xfer_data(&owner, od_span_make(plain + split, sizeof plain - split)) == OD_CMD_OK);
        CHECK(g_offsets[0] == 0u && g_offsets[1] == split);
        CHECK(od_xfer_end(&owner, od_span_none()) == OD_CMD_OK);
    }
}

static void test_short_consumption_range(void)
{
    od_cmd_ctx_t owner = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);
    uint8_t data[4] = { 1u, 2u, 3u, 4u };
    uint32_t consumed;

    CASE("every short consumption refuses");
    for (consumed = 0u; consumed < sizeof data; ++consumed) {
        setup();
        CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
        g_consume_limit = consumed;
        CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_NACK);
        CHECK(g_abort_calls == 1u && g_abort_reasons[0] == OD_XFER_ABORT_STREAM_FAILED);
    }
}

static void test_reset_and_geometry(void)
{
    od_cmd_ctx_t owner = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);

    CASE("geometry rejected before activation");
    setup();
    g_panel.geometry.total_bytes = 0u;
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_NACK);
    CHECK(g_begin_full_calls == 0u && g_abort_calls == 0u && !od_xfer_active());
    CHECK(g_prepare_start_calls == 1u);

    CASE("panel info and split layout rejected before activation");
    setup();
    g_panel_info_ok = false;
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_NACK);
    CHECK(g_begin_full_calls == 0u && g_prepare_start_calls == 1u);
    setup();
    g_panel.geometry.layout = OD_COLOR_LAYOUT_SPLIT_HALVES;
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_NACK);
    CHECK(g_begin_full_calls == 0u && g_prepare_start_calls == 1u);

    CASE("begin failure aborts as start failure");
    setup();
    g_begin_ok = false;
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_NACK);
    CHECK(g_abort_calls == 1u && g_abort_reasons[0] == OD_XFER_ABORT_START_FAILED);

#if SIZE_MAX > UINT32_MAX
    CASE("oversize span rejected before prepare");
    setup();
    CHECK(od_xfer_direct_start(&owner,
          od_span_make((const uint8_t *)1, (size_t)UINT32_MAX + 1u)) == OD_CMD_NACK);
    CHECK(g_prepare_start_calls == 0u && g_begin_full_calls == 0u);
#endif

    CASE("reset aborts once");
    setup();
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
    od_xfer_reset();
    od_xfer_reset();
    CHECK(g_abort_calls == 1u && !od_xfer_active());
    CHECK(g_abort_reasons[0] == OD_XFER_ABORT_RESET);
}

static void test_terminal_outcomes_and_timeout(void)
{
    od_cmd_ctx_t owner = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);
    uint8_t data[2] = { 1u, 2u };
    uint8_t compressed_start[4] = { 4u, 0u, 0u, 0u };
    uint8_t invalid_stream[3] = { 0u, 0u, 0u };

    CASE("replacement and reset each summarize an admitted episode once");
    setup();
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
    CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_OK);
    g_now_ms = 2234u;
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
    CHECK(log_count_exact(OD_LOG_INFO,
          "DW ended: outcome=replaced mode=direct full rx=2 written=2/4 chunks=1 "
          "elapsed=1000 ms") == 1u);
    od_xfer_reset();
    CHECK(log_count_prefix(OD_LOG_INFO, "DW ended: outcome=aborted cause=reset") == 1u);
    od_xfer_reset();
    CHECK(log_count_prefix(OD_LOG_INFO, "DW ended: outcome=aborted cause=reset") == 1u);

    CASE("timeout is strict, wrap-safe, reporting-only and terminal-once");
    setup();
    g_now_ms = 100u;
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
    CHECK(!od_xfer_report_timeout(5099u, 5000u));
    CHECK(!od_xfer_report_timeout(5100u, 5000u));
    CHECK(!od_xfer_report_timeout(5101u, 0u));
    CHECK(od_xfer_report_timeout(5101u, 5000u));
    CHECK(od_xfer_active() && g_abort_calls == 0u);
    CHECK(log_count_prefix(OD_LOG_ERROR, "DW failed: cause=timeout") == 1u);
    CHECK(od_xfer_report_timeout(5102u, 5000u));
    CHECK(log_count_prefix(OD_LOG_ERROR, "DW failed: cause=timeout") == 1u);
    od_xfer_reset();
    CHECK(!od_xfer_active() && g_abort_calls == 1u);
    CHECK(log_count_prefix(OD_LOG_INFO, "DW ended:") == 0u);

    setup();
    g_now_ms = UINT32_MAX - 10u;
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
    CHECK(!od_xfer_report_timeout(9u, 20u));
    CHECK(od_xfer_report_timeout(10u, 20u));
    od_xfer_reset();

    CASE("stream and sink failures have distinct terminal causes");
    setup();
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_OK);
    g_consume_limit = 1u;
    CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_NACK);
    CHECK(log_count_exact(OD_LOG_ERROR,
          "DW failed: cause=panel write failed mode=direct full rx=2 written=0/4 chunks=1 "
          "elapsed=0 ms phase=DATA offset=0 offered=2") == 1u);

    setup();
    CHECK(od_xfer_direct_start(&owner,
                               od_span_make(compressed_start, sizeof compressed_start))
          == OD_CMD_OK);
    CHECK(od_xfer_data(&owner, od_span_make(invalid_stream, sizeof invalid_stream))
          == OD_CMD_NACK);
    CHECK(log_count_exact(OD_LOG_WARN,
          "DW failed: cause=malformed compressed stream mode=direct full rx=3 written=0/4 "
          "chunks=1 elapsed=0 ms phase=DATA zlib") == 1u);
}

static void test_admission_warning_budget(void)
{
    od_cmd_ctx_t owner = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);
    uint8_t direct[4] = { 5u, 0u, 0u, 0u };
    uint8_t partial[17];

    CASE("transfer peer warnings share one five-second budget");
    setup();
    g_now_ms = 100000u;
    CHECK(od_xfer_direct_start(&owner, od_span_make(direct, sizeof direct)) == OD_CMD_NACK);
    CHECK(log_count_exact(OD_LOG_WARN,
          "Direct write START refused: declared size mismatch") == 1u);
    make_partial_start(partial, 0x80u, g_etag, 0x55667788u);
    CHECK(od_xfer_partial_start(&owner, od_span_make(partial, sizeof partial)) == OD_CMD_NACK);
    CHECK(log_count_prefix(OD_LOG_WARN, "Partial write START refused:") == 0u);
    g_now_ms += 5000u;
    CHECK(od_xfer_partial_start(&owner, od_span_make(partial, sizeof partial)) == OD_CMD_NACK);
    CHECK(log_count_exact(OD_LOG_WARN,
          "Partial write START refused: unsupported flags") == 1u);

    CASE("local admission failures bypass the peer budget");
    setup();
    g_panel_info_ok = false;
    CHECK(od_xfer_direct_start(&owner, od_span_none()) == OD_CMD_NACK);
    CHECK(log_count_exact(OD_LOG_ERROR,
          "Direct write START failed: panel geometry unavailable") == 1u);
}

/* H5: a direct write that the target STORED (Nordic image-store preload) rather than drew must
 * leave the displayed etag alone -- the glass still shows the previous image. The next transfer
 * that does draw is back to the ordinary rule. */
static void test_stored_refresh_keeps_displayed_etag(void)
{
    od_cmd_ctx_t owner = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);
    uint8_t start[1] = { 0u };
    uint8_t data[4] = { 1u, 2u, 3u, 4u };
    uint8_t end[5] = { 1u, 0xAAu, 0xBBu, 0xCCu, 0xDDu };

    CASE("stored (preload) END keeps the displayed etag");
    setup();
    g_refresh_stores = true;
    CHECK(od_xfer_direct_start(&owner, od_span_make(start, sizeof start)) == OD_CMD_OK);
    CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_OK);
    CHECK(od_xfer_end(&owner, od_span_make(end, sizeof end)) == OD_CMD_OK);
    CHECK(g_refresh_calls == 1u && !od_xfer_active());
    CHECK(g_etag == 0x11223344u);

    CASE("the next drawn END sets the etag again");
    setup();
    CHECK(od_xfer_direct_start(&owner, od_span_make(start, sizeof start)) == OD_CMD_OK);
    CHECK(od_xfer_data(&owner, od_span_make(data, sizeof data)) == OD_CMD_OK);
    CHECK(od_xfer_end(&owner, od_span_make(end, sizeof end)) == OD_CMD_OK);
    CHECK(g_etag == 0xAABBCCDDu);
}

int main(void)
{
    test_stored_refresh_keeps_displayed_etag();
    test_raw_direct();
    test_transfer_logging();
    test_start_boundaries();
    test_short_write_and_barrier();
    test_short_consumption_range();
    test_compressed_direct();
    test_controller_planes_incomplete();
    test_end_and_refresh_boundaries();
    test_partial();
    test_partial_end_boundaries();
    test_partial_compressed_and_boundaries();
    test_reset_and_geometry();
    test_terminal_outcomes_and_timeout();
    test_admission_warning_budget();
    printf("xfer: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
