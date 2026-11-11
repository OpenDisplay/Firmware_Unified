/* drive_cmd: the shared CMD_DRIVE_WRITE_START (0x0085) and CMD_SPI_RAW (0x0084) handlers, over
 * the real od_epd_cfg / od_epd / od_xfer below them and a fake target seam above the panel.
 *
 * Asserts the exact reply bytes of every ACK and NACK, that every refusal after the panel came
 * up takes it down again, and which refusals reach the bus at all. BG22 keeps its own 0x0085
 * handler (targets/efr32bg22-slc/od_cmd_drive.c), covered by silabs_fault_test; this is the
 * Nordic / ESP32 one. Links the BG22-profile archive because that is the one host build with
 * OD_CAP_DRIVE_WRITE and OD_CAP_SPI_RAW set; the handlers have no BG22-specific code.
 */
#include "od_check.h"
#include "drive_fixture.h"

#include "od_caps.h"
#include "od_cfg_index.h"
#include "od_cmd_app.h"
#include "od_cmd_test_ctx.h"
#include "od_drive_app.h"
#include "od_epd.h"
#include "od_epd_cfg.h"
#include "od_inflate_app.h"
#include "od_reply.h"
#include "od_config_store.h"
#include "od_spi_raw_app.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "od_zlib_inflate.h"
#include "opendisplay_protocol.h"

typedef char drive_cap_on[(OD_CAP_DRIVE_WRITE && OD_CAP_SPI_RAW) ? 1 : -1];

/* ------------------------------------------------------------------------ reply capture -- */

static uint8_t  g_rep[16];
static uint16_t g_rep_n;
static unsigned g_replies;

od_txq_status_t od_reply(od_tx_reservation_t *r, const od_reply_t *rp, const uint8_t *frame,
                         uint16_t len)
{
    (void)r; (void)rp;
    g_replies++;
    g_rep_n = len < sizeof g_rep ? len : (uint16_t)sizeof g_rep;
    memcpy(g_rep, frame, g_rep_n);
    return OD_TXQ_OK;
}
od_txq_status_t od_reply_plain(od_tx_reservation_t *r, const od_reply_t *rp,
                               const uint8_t *frame, uint16_t len)
{
    return od_reply(r, rp, frame, len);
}

static bool reply_is(const uint8_t *want, uint16_t n)
{
    return g_rep_n == n && memcmp(g_rep, want, n) == 0;
}

/* --------------------------------------------------------------------- target seam fake -- */

static struct DisplayConfig g_disp;
static bool g_have_disp = true;
static bool g_up;
static unsigned g_ups, g_downs, g_aborts;
static int8_t g_temp = OD_DRIVE_TEMP_UNKNOWN;
static bool g_stuck_busy;
static od_epd_cfg_stream_t g_stream;
static bool g_streaming;

const struct DisplayConfig *od_drive_app_display(void) { return g_have_disp ? &g_disp : NULL; }
const od_epd_cfg_bus_t *od_drive_app_bus(void) { return &FX_BUS; }
void od_drive_app_abort(void)
{
    g_aborts++;
    g_streaming = false;
    g_up = false;
}
bool od_drive_app_panel_up(const struct DisplayConfig *d)
{
    (void)d;
    g_ups++;
    g_up = true;
    fx_busy_level = (int)od_epd_cfg_busy_idle_level();
    fx_busy_release_after = g_stuck_busy ? 1000000u : 0u;
    return true;
}
void od_drive_app_panel_down(void) { g_downs++; g_up = false; }
bool od_drive_app_adopt(const od_color_geometry_t *frame, uint8_t passes, bool compressed,
                        od_span_t tail)
{
    g_streaming = od_epd_cfg_stream_init(&g_stream, frame, passes, compressed, tail.p,
                                         (uint16_t)tail.n);
    return g_streaming;
}
int8_t od_drive_app_temp_c(void) { return g_temp; }

/* od_xfer's target seam: DATA lands in the drive stream, as on Nordic. */
void od_inflate_app_reset(uint32_t expected) { od_zlib_stream_reset(expected); }
od_zlib_status_t od_inflate_app_push(od_span_t input, bool final)
{ return od_zlib_stream_push(input.p, input.n, final); }
od_zlib_status_t od_inflate_app_poll(uint8_t *out, size_t cap, size_t *produced)
{ return od_zlib_stream_poll(out, cap, produced); }
const char *od_inflate_app_error(void) { return od_zlib_stream_error(); }
uint32_t od_inflate_app_output_count(void) { return od_zlib_stream_output_count(); }
bool od_xfer_app_panel_info(od_xfer_panel_info_t *out) { (void)out; return false; }
void od_xfer_app_prepare_start(void) {}
bool od_xfer_app_begin_full(const od_color_geometry_t *g) { (void)g; return false; }
uint32_t od_xfer_app_write(uint32_t o, od_span_t d)
{
    if (!g_streaming || !od_epd_cfg_stream_write(&g_stream, o, d.p, d.n, NULL)) {
        return 0u;
    }
    return (uint32_t)d.n;
}
od_mut_span_t od_xfer_app_inflate_scratch(void) { return od_mut_span_none(); }
void od_xfer_app_abort(od_xfer_abort_reason_t reason) { (void)reason; od_drive_app_abort(); }
od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *o)
{ (void)o; return OD_XFER_BARRIER_ABORT; }
void od_xfer_app_barrier_abort(const od_reply_t *o) { (void)o; }
bool od_xfer_app_refresh(uint8_t m, bool *c) { (void)m; (void)c; return false; }
uint32_t od_xfer_app_now_ms(void) { return 0u; }

/* SPI_RAW's seam, as Nordic and ESP32 implement it over od_epd_cfg. */
static bool g_raw_open;
int opendisplay_display_spi_raw_open(bool run_panel_init)
{
    const struct DisplayConfig *d = od_drive_app_display();

    od_drive_app_abort();
    g_raw_open = false;
    if (d == NULL || !od_epd_cfg_load(d, &FX_BUS)) {
        return -OD_ERR_SPI_RAW_NO_CONFIG;
    }
    if (!od_drive_app_panel_up(d)) {
        return -OD_ERR_SPI_RAW_PANEL;
    }
    if (run_panel_init && (!od_epd_cfg_select(0u, NULL) || !od_epd_cfg_begin_pass(0u))) {
        od_drive_app_panel_down();
        return -OD_ERR_SPI_RAW_PANEL;
    }
    if (run_panel_init) {
        od_epd_cfg_raw_detach();
    }
    g_raw_open = true;
    return 0;
}
int opendisplay_display_spi_raw_cmd(const uint8_t *data, uint16_t len)
{
    if (!g_raw_open) return -OD_ERR_SPI_RAW_NOT_OPEN;
    if (data == NULL || len == 0u) return -OD_ERR_SPI_RAW_BAD_ARGS;
    od_epd_cfg_raw_cmd(data, len);
    return 0;
}
int opendisplay_display_spi_raw_data(const uint8_t *data, uint16_t len)
{
    if (!g_raw_open) return -OD_ERR_SPI_RAW_NOT_OPEN;
    if (data == NULL || len == 0u) return -OD_ERR_SPI_RAW_BAD_ARGS;
    od_epd_cfg_raw_data(data, len);
    return 0;
}
int opendisplay_display_spi_raw_busy(uint32_t timeout_ms, uint16_t *elapsed, bool *still)
{
    if (!g_raw_open) return -OD_ERR_SPI_RAW_NOT_OPEN;
    od_epd_cfg_raw_busy(timeout_ms, elapsed, still);
    return 0;
}
int opendisplay_display_spi_raw_reset(uint8_t hold_ms)
{
    if (!g_raw_open) return -OD_ERR_SPI_RAW_NOT_OPEN;
    od_epd_cfg_raw_reset(hold_ms);
    return 0;
}
void opendisplay_display_spi_raw_close(void)
{
    if (g_raw_open) od_drive_app_panel_down();
    g_raw_open = false;
}
bool opendisplay_display_spi_raw_is_open(void) { return g_raw_open; }

/* ------------------------------------------------------------------------------ helpers -- */

static od_tx_reservation_t g_res;

static od_cmd_result_t start(const uint8_t *b, size_t n)
{
    od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 1u }, &g_res,
                                       (uint16_t)(n + 2u), false);
    g_rep_n = 0u;
    fx_bus_clear();
    fx_busy_level = 1;
    return od_cmd_app_drive_write_start(&ctx, od_span_make(b, n));
}

static od_cmd_result_t raw(const uint8_t *b, size_t n)
{
    od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 1u }, &g_res,
                                       (uint16_t)(n + 2u), false);
    g_rep_n = 0u;
    return od_cmd_app_spi_raw(&ctx, od_span_make(b, n));
}

static uint8_t g_b[700];

/* 16x4 MONO display 0 with modes: 0 "full" [10..30 REJECT], 1 "cold" [-20..0 SUBSTITUTE],
 * 2 "pins" (SET_PIN: unsupported by FX_BUS), 3 "busy" (waits on BUSY before the image). */
static void store_cfg(bool with_drive)
{
    static const uint8_t seq_ok[] = { 1, 0x12, 0, 3, 2, 0x22, 0xF7, 0, 1 };
    static const uint8_t seq_pin[] = { 0, 6, 0, 1, 0, 3 };
    static const uint8_t seq_busy[] = { 1, 0x12, 0, 1, 0, 3 };
    static const uint8_t panel[13] = { 0, 0x01, 0x01, 0x64, 0x00, 0, 0, 0, 1, 2, 3, 0, 0 };
    fx_cfg_t c;

    g_disp = fx_display(0u, 16u, 4u, 0u);
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &g_disp);
    if (with_drive) {
        fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, panel, sizeof panel);
        const struct { const char *name; int8_t lo, hi; uint8_t pol; const uint8_t *s; uint16_t n; }
        m[] = {
            { "full", 10, 30, OD_DRIVE_TEMP_REJECT, seq_ok, sizeof seq_ok },
            { "cold", -20, 0, OD_DRIVE_TEMP_SUBSTITUTE, seq_ok, sizeof seq_ok },
            { "pins", 0, 0, 0, seq_pin, sizeof seq_pin },
            { "busy", 0, 0, 0, seq_busy, sizeof seq_busy },
        };
        for (uint8_t i = 0; i < 4u; i++) {
            memset(g_b, 0, 37u);
            g_b[1] = i;
            strncpy((char *)&g_b[2], m[i].name, 12u);
            g_b[14] = 0xFFu;
            g_b[15] = 1u;
            g_b[16] = (uint8_t)m[i].lo;
            g_b[17] = (uint8_t)m[i].hi;
            g_b[18] = m[i].pol;
            memset(&g_b[19], 0xFF, 8u);
            g_b[35] = (uint8_t)m[i].n;
            memcpy(&g_b[37], m[i].s, m[i].n);
            fx_cfg_ext(&c, OD_PKT_DISPLAY_MODE, g_b, 37u + m[i].n);
        }
    }
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
}

#define NACK(err, mode) ((const uint8_t[]){ RESP_NACK, RESP_DRIVE_WRITE_START, (err), (mode) }), 4u

/* -------------------------------------------------------------------------------- tests -- */

static void test_start_args(void)
{
    store_cfg(true);

    CASE("short body: BAD_ARGS, mode byte 0");
    {
        const uint8_t b[4] = { 1, 0, 0, 0 };
        CHECK(start(b, sizeof b) == OD_CMD_NACK && reply_is(NACK(OD_ERR_DRIVE_BAD_ARGS, 0u)));
        CHECK(start(NULL, 0u) == OD_CMD_NACK && reply_is(NACK(OD_ERR_DRIVE_BAD_ARGS, 0u)));
    }
    CASE("reserved bytes non-zero: BAD_ARGS echoing the mode");
    {
        const uint8_t b1[5] = { 2, 0, 0, 1, 0 };
        const uint8_t b2[5] = { 2, 0, 0, 0, 0x80 };
        CHECK(start(b1, 5u) == OD_CMD_NACK && reply_is(NACK(OD_ERR_DRIVE_BAD_ARGS, 2u)));
        CHECK(start(b2, 5u) == OD_CMD_NACK && reply_is(NACK(OD_ERR_DRIVE_BAD_ARGS, 2u)));
    }
    CASE("PIPELINED: UNSUPPORTED before anything is touched");
    {
        const unsigned ups = g_ups, aborts = g_aborts;
        const uint8_t b[5] = { 0, 0, DRIVE_WRITE_FLAG_PIPELINED, 0, 0 };
        CHECK(start(b, 5u) == OD_CMD_NACK && reply_is(NACK(OD_ERR_DRIVE_UNSUPPORTED, 0u)));
        CHECK(g_ups == ups && g_aborts == aborts && fx_cmd_count == 0u);
    }
    CASE("no display configured: NO_CONFIG");
    {
        const uint8_t b[5] = { 0, 0, 0, 0, 0 };
        g_have_disp = false;
        CHECK(start(b, 5u) == OD_CMD_NACK && reply_is(NACK(OD_ERR_DRIVE_NO_CONFIG, 0u)));
        g_have_disp = true;
    }
    CASE("a 2.x config without drive packets: NO_CONFIG, the panel never powered");
    {
        const uint8_t b[5] = { 0, 0, 0, 0, 0 };
        store_cfg(false);
        const unsigned ups = g_ups;
        CHECK(start(b, 5u) == OD_CMD_NACK && reply_is(NACK(OD_ERR_DRIVE_NO_CONFIG, 0u)));
        CHECK(g_ups == ups && fx_cmd_count == 0u && !od_xfer_active());
        store_cfg(true);
    }
    CASE("a mode past the declared ones: BAD_MODE echoing it");
    {
        const uint8_t b[5] = { 4, 0, 0, 0, 0 };
        CHECK(start(b, 5u) == OD_CMD_NACK && reply_is(NACK(OD_ERR_DRIVE_BAD_MODE, 4u)));
        CHECK(!g_up);
    }
}

static void test_start_temperature(void)
{
    const uint8_t b0[5] = { 0, 0, 0, 0, 0 };
    const uint8_t b1[5] = { 1, 0, 0, 0, 0 };

    store_cfg(true);
    CASE("REJECT out of window: TEMP_RANGE before power-up");
    g_temp = 31;
    {
        const unsigned ups = g_ups;
        CHECK(start(b0, 5u) == OD_CMD_NACK && reply_is(NACK(OD_ERR_DRIVE_TEMP_RANGE, 0u)));
        CHECK(g_ups == ups);
    }
    CASE("REJECT at the inclusive edge: ACK [00 85 mode passes temp]");
    g_temp = 30;
    CHECK(start(b0, 5u) == OD_CMD_OK);
    CHECK(reply_is((const uint8_t[]){ RESP_ACK, RESP_DRIVE_WRITE_START, 0u, 1u, 30u }, 5u));
    CHECK(g_up && od_xfer_active());

    CASE("SUBSTITUTE: the ACK names the mode actually run, the NACK would name the request");
    g_temp = 20;
    CHECK(start(b1, 5u) == OD_CMD_OK);
    CHECK(reply_is((const uint8_t[]){ RESP_ACK, RESP_DRIVE_WRITE_START, 0u, 1u, 20u }, 5u));
    g_temp = -5;
    CHECK(start(b1, 5u) == OD_CMD_OK);
    CHECK(reply_is((const uint8_t[]){ RESP_ACK, RESP_DRIVE_WRITE_START, 1u, 1u, 0xFBu }, 5u));
    g_temp = 50;
    /* SUBSTITUTE falls back to mode 0 only. Mode 2 declares no window ([0,0], covers
     * everything) and is NOT a substitute; mode 0 does not cover 50 C, so TEMP_RANGE. A NACK
     * echoes the requested mode; only an ACK reports a substitution. */
    CHECK(start(b1, 5u) == OD_CMD_NACK &&
          reply_is(NACK(OD_ERR_DRIVE_TEMP_RANGE, 1u)));
    CHECK(!g_up);

    CASE("no reading: policy skipped, ACK temp byte 0x7F");
    g_temp = OD_DRIVE_TEMP_UNKNOWN;
    CHECK(start(b0, 5u) == OD_CMD_OK);
    CHECK(reply_is((const uint8_t[]){ RESP_ACK, RESP_DRIVE_WRITE_START, 0u, 1u, 0x7Fu }, 5u));
}

static void test_start_panel_failures(void)
{
    const uint8_t ok[5] = { 0, 0, 0, 0, 0 };
    const uint8_t bad[5] = { 2, 0, 0, 0, 0 };

    store_cfg(true);
    g_temp = OD_DRIVE_TEMP_UNKNOWN;

    CASE("DRIVE-NACK-ZOMBIE: a refused START ends the active transfer with its panel");
    CHECK(start(ok, 5u) == OD_CMD_OK && od_xfer_active() && g_streaming);
    CHECK(start(bad, 5u) == OD_CMD_NACK);
    CHECK(!g_up && !g_streaming);         /* drive_begin()'s od_drive_app_abort() ran */
    /* The app abort does not touch od_xfer, so drive_begin() resets it alongside; otherwise
     * the old transfer stays active with no panel and defers the display service. */
    CHECK(!od_xfer_active());

    CASE("a START refused before the panel is touched (reserved bytes) leaves the transfer");
    CHECK(start(ok, 5u) == OD_CMD_OK && od_xfer_active() && g_streaming);
    {
        const uint8_t rsv[5] = { 0, 0, 0, 1, 0 };
        CHECK(start(rsv, 5u) == OD_CMD_NACK && reply_is(NACK(OD_ERR_DRIVE_BAD_ARGS, 0u)));
    }
    CHECK(od_xfer_active() && g_streaming);

    CASE("an accepted START replaces the active transfer");
    CHECK(start(ok, 5u) == OD_CMD_OK && od_xfer_active() && g_streaming);
    od_xfer_reset();

    CASE("an escape this bus cannot run: SEQ_UNSUPPORTED, refused before any I/O, panel down");
    {
        const uint8_t b[5] = { 2, 0, 0, 0, 0 };
        CHECK(start(b, 5u) == OD_CMD_NACK && reply_is(NACK(OD_ERR_DRIVE_SEQ_UNSUPPORTED, 2u)));
        CHECK(!g_up && fx_cmd_count == 0u && fx_reset_count == 0u && !od_xfer_active());
    }
    CASE("BUSY never releasing in pass 0: PANEL_BUSY, panel down, config writes not held");
    {
        const uint8_t b[5] = { 3, 0, 0, 0, 0 };
        static uint8_t ws[OD_CONFIG_STORE_MAX_RECORD];
        const uint8_t payload[3] = { 0, 0, 1 };
        g_stuck_busy = true;
        const od_cmd_result_t r = start(b, 5u);
        g_stuck_busy = false;
        CHECK(r == OD_CMD_NACK && !g_up && !od_xfer_active());
        CHECK(reply_is(NACK(OD_ERR_DRIVE_PANEL_BUSY, 3u)));
        CHECK(od_config_store_save(ws, sizeof ws, payload, sizeof payload) == OD_CONFIG_STORE_OK);
        store_cfg(true);
    }
}

static void test_start_sizes(void)
{
    store_cfg(true);
    g_temp = OD_DRIVE_TEMP_UNKNOWN;

    CASE("uncompressed inline first bytes are accepted and stream into pass 0");
    {
        uint8_t b[5 + 8] = { 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8 };
        CHECK(start(b, sizeof b) == OD_CMD_OK && od_xfer_active());
    }
    CASE("uncompressed inline bytes past the whole frame: BAD_SIZE, panel down");
    {
        uint8_t b[5 + 9] = { 0, 0, 0, 0, 0 };   /* 16x4 MONO frame = 8 bytes */
        CHECK(start(b, sizeof b) == OD_CMD_NACK && reply_is(NACK(OD_ERR_DRIVE_BAD_SIZE, 0u)));
        CHECK(!g_up && !od_xfer_active());
    }
    CASE("compressed, declared length != passes*frame: BAD_SIZE, panel down");
    {
        uint8_t b[5 + 4] = { 0, 0, DRIVE_WRITE_FLAG_COMPRESSED, 0, 0, 9, 0, 0, 0 };
        CHECK(start(b, sizeof b) == OD_CMD_NACK && reply_is(NACK(OD_ERR_DRIVE_BAD_SIZE, 0u)));
        CHECK(!g_up && !od_xfer_active());
    }
    CASE("compressed without the 4-byte length: BAD_SIZE");
    {
        uint8_t b[5 + 3] = { 0, 0, DRIVE_WRITE_FLAG_COMPRESSED, 0, 0, 8, 0, 0 };
        CHECK(start(b, sizeof b) == OD_CMD_NACK && reply_is(NACK(OD_ERR_DRIVE_BAD_SIZE, 0u)));
        CHECK(!g_up);
    }
    CASE("compressed with the right length and no stream bytes yet: ACK");
    {
        uint8_t b[5 + 4] = { 0, 0, DRIVE_WRITE_FLAG_COMPRESSED, 0, 0, 8, 0, 0, 0 };
        CHECK(start(b, sizeof b) == OD_CMD_OK && od_xfer_active());
    }
    CASE("a second START while one is active replaces it (abort first)");
    {
        const unsigned aborts = g_aborts;
        const uint8_t b[5] = { 0, 0, 0, 0, 0 };
        CHECK(start(b, 5u) == OD_CMD_OK && g_aborts > aborts);
    }
    od_xfer_reset();
    od_drive_app_abort();
}

#define RNACK(err, sub, diag) ((const uint8_t[]){ RESP_NACK, RESP_SPI_RAW, (err), (sub), (diag) }), 5u
#define RACK(sub) ((const uint8_t[]){ RESP_ACK, RESP_SPI_RAW, (sub) }), 3u

static void test_spi_raw(void)
{
    store_cfg(true);

    CASE("empty body: 5-byte NACK, sub 0xFF");
    CHECK(raw(NULL, 0u) == OD_CMD_NACK);
    CHECK(g_rep_n == 5u && g_rep[0] == RESP_NACK && g_rep[1] == RESP_SPI_RAW &&
          g_rep[2] == OD_ERR_SPI_RAW_BAD_ARGS && g_rep[3] == 0xFFu);

    CASE("CMD before OPEN: NOT_OPEN");
    {
        const uint8_t b[2] = { SPI_RAW_SUB_CMD, 0x12 };
        CHECK(raw(b, 2u) == OD_CMD_NACK);
        CHECK(g_rep_n == 5u && g_rep[2] == OD_ERR_SPI_RAW_NOT_OPEN && g_rep[3] == SPI_RAW_SUB_CMD);
    }
    CASE("CLOSE when not open is an ACK");
    {
        const uint8_t b[1] = { SPI_RAW_SUB_CLOSE };
        CHECK(raw(b, 1u) == OD_CMD_OK && reply_is(RACK(SPI_RAW_SUB_CLOSE)));
    }
    CASE("unknown sub-opcode");
    {
        const uint8_t b[1] = { 0x40 };
        CHECK(raw(b, 1u) == OD_CMD_NACK && g_rep[2] == OD_ERR_SPI_RAW_UNKNOWN_SUB && g_rep[3] == 0x40);
    }
    CASE("OPEN on a 2.x config: NO_CONFIG, diag byte = load stage 2 (no panel) | packet count");
    {
        const uint8_t b[1] = { SPI_RAW_SUB_OPEN };
        store_cfg(false);
        CHECK(raw(b, 1u) == OD_CMD_NACK);
        CHECK(g_rep_n == 5u && g_rep[2] == OD_ERR_SPI_RAW_NO_CONFIG && g_rep[3] == 0u);
        CHECK((g_rep[4] >> 5) == 2u && (g_rep[4] & 0x1Fu) == (uint8_t)od_cfg_index_count());
        CHECK(!g_up);
        store_cfg(true);
    }
    CASE("OPEN (no init), CMD, DATA, DATA_NOACK, BUSY, RESET, CLOSE");
    {
        const uint8_t open[1] = { SPI_RAW_SUB_OPEN };
        const uint8_t cmd[3] = { SPI_RAW_SUB_CMD, 0x10, 0xAA };
        const uint8_t data[3] = { SPI_RAW_SUB_DATA, 0x01, 0x02 };
        const uint8_t noack[2] = { SPI_RAW_SUB_DATA_NOACK, 0x03 };
        const uint8_t busy[3] = { SPI_RAW_SUB_BUSY, 0x64, 0x00 };
        const uint8_t busy_short[2] = { SPI_RAW_SUB_BUSY, 0x64 };
        const uint8_t rst[1] = { SPI_RAW_SUB_RESET };
        const uint8_t close[1] = { SPI_RAW_SUB_CLOSE };
        const uint8_t empty_cmd[1] = { SPI_RAW_SUB_CMD };

        fx_bus_clear();
        CHECK(raw(open, 1u) == OD_CMD_OK && reply_is(RACK(SPI_RAW_SUB_OPEN)));
        CHECK(fx_cmd_count == 0u && fx_reset_count == 0u);      /* nothing clocked out */
        CHECK(raw(cmd, 3u) == OD_CMD_OK && reply_is(RACK(SPI_RAW_SUB_CMD)));
        CHECK(raw(data, 3u) == OD_CMD_OK && reply_is(RACK(SPI_RAW_SUB_DATA)));
        const unsigned before = g_replies;
        CHECK(raw(noack, 2u) == OD_CMD_OK && g_replies == before);
        CHECK(strstr(fx_trace, "C 10\nD aa\nD 01 02\nD 03\n") != NULL);
        fx_busy_level = 1;
        fx_busy_release_after = 3u;
        CHECK(raw(busy, 3u) == OD_CMD_OK);
        CHECK(g_rep_n == 6u && g_rep[0] == RESP_ACK && g_rep[2] == SPI_RAW_SUB_BUSY &&
              g_rep[5] == 0u);
        CHECK(raw(busy_short, 2u) == OD_CMD_NACK && g_rep[2] == OD_ERR_SPI_RAW_BAD_ARGS);
        CHECK(raw(empty_cmd, 1u) == OD_CMD_NACK && g_rep[2] == OD_ERR_SPI_RAW_BAD_ARGS);
        CHECK(raw(rst, 1u) == OD_CMD_OK && strstr(fx_trace, "R 10") != NULL);
        CHECK(raw(close, 1u) == OD_CMD_OK && !g_raw_open && !g_up);
    }
    CASE("OPEN with PANEL_INIT runs mode 0 up to the image marker");
    {
        const uint8_t open[2] = { SPI_RAW_SUB_OPEN, SPI_RAW_OPEN_FLAG_PANEL_INIT };
        fx_bus_clear();
        CHECK(raw(open, 2u) == OD_CMD_OK && fx_reset_count == 1u);
        CHECK(strstr(fx_trace, "C 12") != NULL && strstr(fx_trace, "C 22") == NULL);
        opendisplay_display_spi_raw_close();
    }
    CASE("an open PANEL_INIT session does not hold config writes");
    {
        const uint8_t open[2] = { SPI_RAW_SUB_OPEN, SPI_RAW_OPEN_FLAG_PANEL_INIT };
        static uint8_t ws[OD_CONFIG_STORE_MAX_RECORD];
        const uint8_t payload[3] = { 0, 0, 1 };
        CHECK(raw(open, 2u) == OD_CMD_OK && g_raw_open);
        CHECK(od_config_store_save(ws, sizeof ws, payload, sizeof payload) == OD_CONFIG_STORE_OK);
        opendisplay_display_spi_raw_close();
        store_cfg(true);
    }
    CASE("a config write during a pass: BUSY, and the NACK byte says so once");
    {
        static uint8_t ws[OD_CONFIG_STORE_MAX_RECORD];
        const uint8_t payload[3] = { 0, 0, 1 };
        od_config_store_hold_writes(true);
        CHECK(od_config_store_save(ws, sizeof ws, payload, sizeof payload) == OD_CONFIG_STORE_BUSY);
        CHECK(od_config_store_save_error() == OD_ERR_CONFIG_WRITE_BUSY);
        CHECK(od_config_store_save_error() == OD_ERR_CONFIG_WRITE_FAILED);
        od_config_store_hold_writes(false);
        CHECK(od_config_store_save(ws, sizeof ws, payload, 99999u) == OD_CONFIG_STORE_TOO_BIG);
        CHECK(od_config_store_save_error() == OD_ERR_CONFIG_WRITE_FAILED);
        store_cfg(true);
    }
    CASE("N1: OPEN ends an upload in flight");
    {
        const uint8_t ok[5] = { 0, 0, 0, 0, 0 };
        const uint8_t open[1] = { SPI_RAW_SUB_OPEN };
        g_temp = 20;
        CHECK(start(ok, 5u) == OD_CMD_OK && od_xfer_active());
        CHECK(raw(open, 1u) == OD_CMD_OK && reply_is(RACK(SPI_RAW_SUB_OPEN)));
        CHECK(!od_xfer_active());
        opendisplay_display_spi_raw_close();
        g_temp = OD_DRIVE_TEMP_UNKNOWN;
    }
}

int main(void)
{
    test_start_args();
    test_start_temperature();
    test_start_panel_failures();
    test_start_sizes();
    test_spi_raw();
    printf("drive_cmd: %u known bugs\n", fx_xfail_count);
    return OD_CHECK_REPORT_NONEMPTY("drive_cmd", 60);
}
