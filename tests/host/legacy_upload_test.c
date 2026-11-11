/* legacy_upload_test -- a client that knows nothing about drive modes still gets a correct image.
 *
 * tests/vectors/legacy_uploads.inc holds, for one panel of every colour class, the panel
 * database's drive packets and the exact frames py-opendisplay 7.14.0 sends for an upload:
 * 0x70/0x71/0x72 compressed, uncompressed for the plane-layout classes, and PIPE (0x80/0x81/0x82)
 * for one. Each is replayed through od_xfer / od_pipe into od_display on a recording panel bus.
 *
 * Mode 0 is the fallback for such a client (docs/CONFIG_DRIVEN_DISPLAY.md, "Mode conventions").
 * Every replay must:
 *   - hand od_display exactly the frame py-opendisplay encoded (CRC-32 from the generator), so
 *     the 2.x pixel format for 0x20's scheme is the one the firmware streams;
 *   - put on the bus exactly what driving mode 0 directly with that frame puts there (same
 *     commands, same bytes, same order), compressed, uncompressed and PIPE alike.
 * And a config whose mode 0 breaks the invariant is refused at START with NEEDS_MIGRATION.
 *
 * Built like xfer_split_test (OD_XFER_SPLIT_HALVES=1, ESP32's composition), so the dual-CS
 * Spectra 6 class runs too. */

#include "od_cmd_test_ctx.h"
#include "od_color.h"
#include "od_config.h"
#include "od_config_store.h"
#include "od_display.h"
#include "od_display_app.h"
#include "od_drive_app.h"
#include "od_epd_cfg.h"
#include "od_hal_nvs.h"
#include "od_inflate_app.h"
#include "od_pipe.h"
#include "od_reply.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "od_zlib_inflate.h"
#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    uint16_t ic;
    uint16_t w;
    uint16_t h;
    uint8_t scheme;
    uint32_t frame_bytes;
    uint32_t frame_crc;
    const uint8_t *drive;
    size_t drive_n;
    const uint8_t *z;
    size_t z_n;
    const uint8_t *raw;
    size_t raw_n;
    const uint8_t *pipe;
    size_t pipe_n;
} lu_panel_t;

#include "../vectors/legacy_uploads.inc"

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

/* ------------------------------------------------------------------ fake NVS --- */

static uint8_t g_store[OD_CONFIG_STORE_HEADER_SIZE + 4096u];
static uint32_t g_store_len;

static void store_config(const uint8_t *framed, size_t n)
{
    const uint32_t payload = 3u + (uint32_t)n + 2u;   /* [rsv:2][version] packets [crc:2] */
    const uint32_t magic = OD_CONFIG_STORE_MAGIC;

    memset(g_store, 0, sizeof g_store);
    g_store[OD_CONFIG_STORE_HEADER_SIZE + 2u] = 1u;
    memcpy(&g_store[OD_CONFIG_STORE_HEADER_SIZE + 3u], framed, n);
    for (unsigned i = 0; i < 4u; i++) {
        g_store[i] = (uint8_t)(magic >> (8u * i));
        g_store[12u + i] = (uint8_t)(payload >> (8u * i));
    }
    g_store_len = OD_CONFIG_STORE_HEADER_SIZE + payload;
}

int od_hal_nvs_init(void) { return OD_HAL_NVS_OK; }

int od_hal_nvs_size(uint32_t *len_out)
{
    if (g_store_len == 0u) {
        return OD_HAL_NVS_ENOENT;
    }
    *len_out = g_store_len;
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_read(uint32_t offset, void *buf, uint32_t len)
{
    if (g_store_len == 0u) {
        return OD_HAL_NVS_ENOENT;
    }
    if (offset > g_store_len || len > g_store_len - offset) {
        return OD_HAL_NVS_E2BIG;
    }
    memcpy(buf, &g_store[offset], len);
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_write(const void *record, uint32_t len)
{
    (void)record;
    (void)len;
    return OD_HAL_NVS_EIO;
}

int od_hal_nvs_erase(void)
{
    g_store_len = 0u;
    return OD_HAL_NVS_OK;
}

/* ------------------------------------------------------- recording panel bus --- */

/* One entry per command (0x100 | cmd, 0x200 | cs) or data byte. Chunking is not recorded:
 * the same bytes in different DATA chunks give the same trace. */
#define TRACE_CAP (1u << 20)
static uint16_t *g_trace;
static uint32_t g_trace_n;
static uint32_t g_now;

static void trace(uint16_t v)
{
    if (g_trace_n < TRACE_CAP) {
        g_trace[g_trace_n] = v;
    }
    g_trace_n++;
}

static void bus_cmd(uint8_t c) { trace((uint16_t)(0x100u | c)); }

static void bus_data(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        trace(d[i]);
    }
}

static void bus_reset(uint32_t ms) { g_now += ms * 3u; trace(0x300u); }
static int bus_busy(void) { return (int)od_epd_cfg_busy_idle_level(); }
static void bus_delay(uint32_t ms) { g_now += ms; }
static uint32_t bus_now(void) { return g_now; }
static bool bus_pin(uint8_t pin, uint8_t level) { trace((uint16_t)(0x400u | level)); (void)pin; return true; }
static bool bus_cs(uint8_t which) { trace((uint16_t)(0x200u | which)); return true; }

static const od_epd_cfg_bus_t g_bus = {
    .cmd = bus_cmd,
    .data = bus_data,
    .reset = bus_reset,
    .busy_read = bus_busy,
    .delay_ms = bus_delay,
    .now_ms = bus_now,
    .pin_write = bus_pin,
    .cs_select = bus_cs,
};

/* ------------------------------------------------------------- target seams --- */

static struct od_config g_cfg;
static struct DisplayConfig *g_disp = &g_cfg.displays[0];
static uint8_t g_scratch[256];
static uint32_t g_etag;
/* What od_display was handed, in stream order: the frame as the firmware understood it. */
static uint8_t *g_frame;
static uint32_t g_frame_n;
static unsigned g_refreshed;

const struct DisplayConfig *od_drive_app_display(void) { return g_disp; }
const od_epd_cfg_bus_t *od_drive_app_bus(void) { return &g_bus; }
void od_drive_app_abort(void) { od_display_abort(); }
int8_t od_drive_app_temp_c(void) { return OD_DRIVE_TEMP_UNKNOWN; }
bool od_drive_app_panel_up(const struct DisplayConfig *d) { (void)d; return true; }
void od_drive_app_panel_down(void) {}

bool od_drive_app_adopt(const od_color_geometry_t *frame, uint8_t passes, bool compressed,
                        od_span_t tail)
{
    return od_display_adopt_drive(frame, passes, compressed, tail);
}

void od_display_app_phase(od_display_phase_t phase) { (void)phase; }

uint32_t od_boot_app_device_id24(void) { return 0x123456u; }
void od_boot_app_firmware_version(uint8_t *a, uint8_t *b, uint8_t *c) { *a = 3; *b = 0; *c = 0; }
float od_boot_app_battery_volts(void) { return 3.0f; }
float od_boot_app_chip_temp_c(void) { return 25.0f; }

void od_xfer_app_prepare_start(void)
{
    if (od_display_active()) {
        od_display_abort();
    }
}

bool od_xfer_app_panel_info(od_xfer_panel_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->needs_migration = od_display_needs_migration();
    if (od_color_direct_geometry(g_disp->color_scheme, g_disp->pixel_width,
                                 g_disp->pixel_height, &out->geometry) != OD_COLOR_OK) {
        return false;
    }
    out->width = g_disp->pixel_width;
    out->height = g_disp->pixel_height;
    out->partial_enabled = od_display_partial_available();
    out->split_halves_ok = true;
    return true;
}

bool od_xfer_app_begin_full(const od_color_geometry_t *geometry)
{
    return od_display_begin_full(geometry) == OD_DISPLAY_OK;
}

bool od_xfer_app_begin_partial(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t pb)
{
    return od_display_begin_partial(x, y, w, h, pb) == OD_DISPLAY_OK;
}

uint32_t od_xfer_app_write(uint32_t stream_offset, od_span_t data)
{
    if (stream_offset != g_frame_n || g_frame_n + data.n > 600000u) {
        return 0u;
    }
    memcpy(g_frame + g_frame_n, data.p, data.n);
    g_frame_n += (uint32_t)data.n;
    return od_display_write(stream_offset, data.p, data.n) ? (uint32_t)data.n : 0u;
}

od_mut_span_t od_xfer_app_inflate_scratch(void) { return od_mut_span_make(g_scratch, sizeof g_scratch); }
void od_xfer_app_abort(od_xfer_abort_reason_t reason) { (void)reason; od_display_abort(); }
od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner) { (void)owner; return OD_XFER_BARRIER_PROCEED; }
void od_xfer_app_barrier_abort(const od_reply_t *owner) { (void)owner; od_display_abort(); }

bool od_xfer_app_refresh(uint8_t mode, bool *completed)
{
    if (completed == NULL || !od_display_active()) {
        return false;
    }
    *completed = od_display_refresh(mode);
    g_refreshed += *completed ? 1u : 0u;
    return true;
}

uint32_t od_xfer_app_displayed_etag(void) { return g_etag; }
void od_xfer_app_set_displayed_etag(uint32_t etag) { g_etag = etag; }
uint32_t od_xfer_app_now_ms(void) { return g_now; }

/* Replies: every NACK (0xFF first byte) is counted; the last start reply is kept. */
static unsigned g_nacks;
static uint8_t g_last[4];

static od_txq_status_t record(const uint8_t *frame, uint16_t len)
{
    if (len >= 1u && frame[0] == RESP_NACK) {
        g_nacks++;
        if (getenv("LU_DEBUG") != NULL) {
            printf("NACK %02x %02x %02x\n", frame[1], len > 2 ? frame[2] : 0, len > 3 ? frame[3] : 0);
        }
    }
    memset(g_last, 0, sizeof g_last);
    memcpy(g_last, frame, len < sizeof g_last ? len : sizeof g_last);
    return OD_TXQ_OK;
}

od_txq_status_t od_reply(od_tx_reservation_t *r, const od_reply_t *rp, const uint8_t *frame,
                         uint16_t len)
{
    (void)r;
    (void)rp;
    return record(frame, len);
}

od_txq_status_t od_reply_plain(od_tx_reservation_t *r, const od_reply_t *rp,
                               const uint8_t *frame, uint16_t len)
{
    (void)r;
    (void)rp;
    return record(frame, len);
}

void od_inflate_app_reset(uint32_t n) { od_zlib_stream_reset(n); }
od_zlib_status_t od_inflate_app_push(od_span_t in, bool final) { return od_zlib_stream_push(in.p, in.n, final); }
od_zlib_status_t od_inflate_app_poll(uint8_t *o, size_t c, size_t *p) { return od_zlib_stream_poll(o, c, p); }
const char *od_inflate_app_error(void) { return od_zlib_stream_error(); }
uint32_t od_inflate_app_output_count(void) { return od_zlib_stream_output_count(); }

/* ---------------------------------------------------------------------- setup --- */

static const od_reply_t OWNER = { OD_ORIGIN_BLE, 7u };
static od_tx_reservation_t g_reservation;

static void provision(const lu_panel_t *p, const uint8_t *drive, size_t drive_n)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.loaded = true;
    g_cfg.display_count = 1u;
    g_disp->panel_ic_type = p->ic;
    g_disp->pixel_width = p->w;
    g_disp->pixel_height = p->h;
    g_disp->color_scheme = p->scheme;
    g_disp->reset_pin = 6u;
    g_disp->dc_pin = 7u;
    g_disp->busy_pin = 8u;
    g_disp->cs_pin = 5u;
    g_disp->cs_pin_2 = p->scheme == OD_COLOR_SCHEME_BWGBRY_SPLIT ? 9u : 0xFFu;
    store_config(drive, drive_n);
    od_xfer_reset();
    od_pipe_reset_state();
    g_trace_n = 0u;
    g_frame_n = 0u;
    g_nacks = 0u;
    g_refreshed = 0u;
}

/* Replays a [len:2 LE][opcode:2 BE][body] stream; returns the number of frames sent. */
static unsigned replay(const uint8_t *s, size_t n)
{
    unsigned frames = 0u;
    size_t off = 0u;

    while (off + 2u <= n) {
        const uint16_t len = (uint16_t)(s[off] | (s[off + 1u] << 8));
        const uint8_t *f = &s[off + 2u];
        const uint16_t op = (uint16_t)((f[0] << 8) | f[1]);
        const od_span_t body = od_span_make(f + 2, (size_t)(len - 2u));
        od_cmd_ctx_t ctx = od_test_cmd_ctx(OWNER, &g_reservation, len, false);

        switch (op) {
        case CMD_DIRECT_WRITE_START: (void)od_xfer_direct_start(&ctx, body); break;
        case CMD_DIRECT_WRITE_DATA:  (void)od_xfer_data(&ctx, body); break;
        case CMD_DIRECT_WRITE_END:   (void)od_xfer_end(&ctx, body); break;
        case CMD_PIPE_WRITE_START:   (void)od_pipe_start(&ctx, body); break;
        case CMD_PIPE_WRITE_DATA:    (void)od_pipe_data(&ctx, body); break;
        case CMD_PIPE_WRITE_END:     (void)od_pipe_end(&ctx, body); break;
        default: break;
        }
        off += 2u + len;
        frames++;
    }
    return frames;
}

/* The bus trace of mode 0 driven directly with `frame`, for comparison. */
static uint16_t *reference_trace(const uint8_t *frame, uint32_t n, uint32_t *len_out)
{
    od_color_geometry_t g;
    od_epd_cfg_stream_t st;
    uint16_t *copy;

    g_trace_n = 0u;
    if (od_color_direct_geometry(g_disp->color_scheme, g_disp->pixel_width,
                                 g_disp->pixel_height, &g) != OD_COLOR_OK ||
        g.total_bytes != n || !od_epd_cfg_load(g_disp, &g_bus) ||
        !od_epd_cfg_select(0u, NULL) || !od_epd_cfg_begin_pass(0u) ||
        !od_epd_cfg_stream_init(&st, &g, 1u, false, NULL, 0u) ||
        !od_epd_cfg_stream_write(&st, 0u, frame, n, NULL) || !od_epd_cfg_finish()) {
        return NULL;
    }
    od_epd_cfg_sleep();
    copy = malloc(sizeof(uint16_t) * (g_trace_n < TRACE_CAP ? g_trace_n : TRACE_CAP));
    if (copy != NULL) {
        memcpy(copy, g_trace, sizeof(uint16_t) * (g_trace_n < TRACE_CAP ? g_trace_n : TRACE_CAP));
    }
    *len_out = g_trace_n;
    return copy;
}

static void run_flow(const lu_panel_t *p, const char *flow, const uint8_t *s, size_t n)
{
    static char name[96];
    uint32_t ref_n = 0u;
    uint32_t got_n;
    uint16_t *got;
    uint16_t *ref;

    snprintf(name, sizeof name, "%s: %s", p->name, flow);
    g_case = name;
    provision(p, p->drive, p->drive_n);
    CHECK(replay(s, n) >= 2u);
    CHECK(g_nacks == 0u);
    CHECK(g_refreshed == 1u);
    CHECK(!od_display_active() && !od_xfer_active());
    CHECK(g_frame_n == p->frame_bytes);
    CHECK(od_config_store_crc32(g_frame, g_frame_n) == p->frame_crc);
    CHECK(g_trace_n < TRACE_CAP);

    got_n = g_trace_n;
    got = malloc(sizeof(uint16_t) * (got_n < TRACE_CAP ? got_n : TRACE_CAP));
    if (got == NULL) {
        CHECK(got != NULL);
        return;
    }
    memcpy(got, g_trace, sizeof(uint16_t) * (got_n < TRACE_CAP ? got_n : TRACE_CAP));
    ref = reference_trace(g_frame, g_frame_n, &ref_n);
    CHECK(ref != NULL);
    CHECK(ref_n == got_n);
    CHECK(ref != NULL && ref_n == got_n &&
          memcmp(ref, got, sizeof(uint16_t) * (got_n < TRACE_CAP ? got_n : TRACE_CAP)) == 0);
    /* And the frame is on the bus at all: at least frame_bytes data entries. */
    {
        uint32_t data = 0u;
        for (uint32_t i = 0u; i < got_n && i < TRACE_CAP; i++) {
            data += got[i] < 0x100u ? 1u : 0u;
        }
        CHECK(data >= p->frame_bytes);
    }
    free(got);
    free(ref);
}

/* Mode 0 rewritten to another colour scheme: the 2.x frame no longer fits it, so the START is
 * refused with NEEDS_MIGRATION and nothing reaches the panel. */
static void run_broken_mode0(const lu_panel_t *p)
{
    static uint8_t drive[4096];
    size_t off = 0u;
    bool patched = false;

    g_case = "mode 0 in another scheme: START NACK 0x08, nothing drawn";
    if (p->drive_n > sizeof drive) {
        return;
    }
    memcpy(drive, p->drive, p->drive_n);
    while (off + 4u <= p->drive_n) {
        const uint16_t len = (uint16_t)(drive[off + 2u] | (drive[off + 3u] << 8));
        uint8_t *body = &drive[off + 4u];

        if (drive[off + 1u] == OD_PKT_DISPLAY_MODE && body[1] == 0u && !patched) {
            body[14] = (uint8_t)(p->scheme == OD_COLOR_SCHEME_MONO ? OD_COLOR_SCHEME_BWR
                                                                   : OD_COLOR_SCHEME_MONO);
            patched = true;
        }
        off += 4u + len;
    }
    CHECK(patched);
    provision(p, drive, p->drive_n);
    /* START alone first, for the reply shape [FF][70][08][00]. */
    CHECK(replay(p->z, 2u + (size_t)(p->z[0] | (p->z[1] << 8))) == 1u);
    CHECK(g_last[0] == RESP_NACK && g_last[1] == RESP_DIRECT_WRITE_START_ACK &&
          g_last[2] == OD_ERR_NEEDS_MIGRATION && g_last[3] == 0u);
    (void)replay(p->z, p->z_n);
    CHECK(g_trace_n == 0u && g_frame_n == 0u && g_refreshed == 0u);
    CHECK(od_display_needs_migration());
}

int main(void)
{
    g_trace = malloc(sizeof(uint16_t) * TRACE_CAP);
    g_frame = malloc(600000u);
    if (g_trace == NULL || g_frame == NULL) {
        return 2;
    }
    for (size_t i = 0u; i < sizeof LU_PANELS / sizeof LU_PANELS[0]; i++) {
        const lu_panel_t *p = &LU_PANELS[i];

        run_flow(p, "0x70 compressed", p->z, p->z_n);
        if (p->raw != NULL) {
            run_flow(p, "0x70 uncompressed", p->raw, p->raw_n);
        }
        if (p->pipe != NULL) {
            run_flow(p, "PIPE uncompressed", p->pipe, p->pipe_n);
        }
    }
    run_broken_mode0(&LU_PANELS[0]);
    printf("legacy_upload: %u checks, %u failures\n", g_checks, g_failures);
    free(g_trace);
    free(g_frame);
    return g_failures == 0u ? 0 : 1;
}
