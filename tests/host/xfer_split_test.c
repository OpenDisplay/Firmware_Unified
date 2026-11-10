/* xfer_split_test -- a dual-controller (SPLIT_HALVES) upload end to end on a build with
 * OD_XFER_SPLIT_HALVES=1, the way ESP32 composes it: CMD 0x70/0x71/0x72 through od_xfer, the
 * od_xfer_app glue forwarding to od_display, and a recording panel bus that tags every data byte
 * with the chip-select it went out on.
 *
 * The transfer units (od_xfer*.c, od_pipe.c) are compiled with the flag in their own archive,
 * because od_xfer_panel_info_t changes size with it. The same frame on a panel that does not
 * offer split_halves_ok is still refused at START with nothing touched. */

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
#include "od_reply.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "od_zlib_inflate.h"
#include "opendisplay_structs.h"

#include <stdio.h>
#include <string.h>

#if !OD_XFER_SPLIT_HALVES
#error "xfer_split_test needs OD_XFER_SPLIT_HALVES=1"
#endif

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

/* ------------------------------------------------------------------ fake NVS --- */

static uint8_t g_store[OD_CONFIG_STORE_HEADER_SIZE + 4096u];
static uint32_t g_store_len;
static uint32_t g_payload_len;

static void store_reset(void)
{
    memset(g_store, 0, sizeof g_store);
    g_payload_len = 3u;
    g_store[OD_CONFIG_STORE_HEADER_SIZE + 2u] = 1u;
    g_store_len = 0u;
}

static void store_packet(uint8_t id, const uint8_t *body, uint16_t len)
{
    uint8_t *p = &g_store[OD_CONFIG_STORE_HEADER_SIZE + g_payload_len];

    p[0] = 0u;
    p[1] = id;
    p[2] = (uint8_t)len;
    p[3] = (uint8_t)(len >> 8);
    memcpy(p + 4, body, len);
    g_payload_len += 4u + len;
}

static void store_commit(void)
{
    const uint32_t data_len = g_payload_len + 2u;
    const uint32_t magic = OD_CONFIG_STORE_MAGIC;

    for (unsigned i = 0; i < 4u; i++) {
        g_store[i] = (uint8_t)(magic >> (8u * i));
        g_store[12u + i] = (uint8_t)(data_len >> (8u * i));
    }
    g_store_len = OD_CONFIG_STORE_HEADER_SIZE + data_len;
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

static uint8_t g_cs;
static uint8_t g_on_cs[2][256];
static uint32_t g_on_cs_n[2];
static unsigned g_refresh_cmds[2];
static uint32_t g_now;
static uint8_t g_last_cmd;

static void bus_cmd(uint8_t c)
{
    g_last_cmd = c;
    if (c == 0x12u && g_cs < 2u) {
        g_refresh_cmds[g_cs]++;
    }
}

/* Image data only (after cmd 0x10); the sleep sequence's own data byte is not counted. */
static void bus_data(const uint8_t *d, size_t n)
{
    if (g_last_cmd != 0x10u) {
        return;
    }
    for (size_t i = 0; i < n && g_cs < 2u; i++) {
        if (g_on_cs_n[g_cs] < sizeof g_on_cs[0]) {
            g_on_cs[g_cs][g_on_cs_n[g_cs]] = d[i];
        }
        g_on_cs_n[g_cs]++;
    }
}

static void bus_reset(uint32_t ms) { g_now += ms * 3u; }
static int bus_busy(void) { return (int)od_epd_cfg_busy_idle_level(); }
static void bus_delay(uint32_t ms) { g_now += ms; }
static uint32_t bus_now(void) { return g_now; }
static bool bus_pin(uint8_t pin, uint8_t level) { (void)pin; (void)level; return true; }

static bool bus_cs(uint8_t which)
{
    g_cs = which;
    return true;
}

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
static bool g_split_ok = true;
static unsigned g_panel_up;
static unsigned g_panel_down;
static uint8_t g_scratch[256];
static uint32_t g_etag;

const struct DisplayConfig *od_drive_app_display(void) { return g_disp; }
const od_epd_cfg_bus_t *od_drive_app_bus(void) { return &g_bus; }
void od_drive_app_abort(void) { od_display_abort(); }
int8_t od_drive_app_temp_c(void) { return OD_DRIVE_TEMP_UNKNOWN; }

bool od_drive_app_panel_up(const struct DisplayConfig *d)
{
    (void)d;
    g_panel_up++;
    g_cs = 0u;
    return true;
}

void od_drive_app_panel_down(void) { g_panel_down++; }

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

/* The od_xfer_app glue as targets/esp32-idf/src/display_service.cpp writes it for SPI panels. */
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
    out->partial_enabled = false;
    out->split_halves_ok = g_split_ok;
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
    return true;
}

uint32_t od_xfer_app_displayed_etag(void) { return g_etag; }
void od_xfer_app_set_displayed_etag(uint32_t etag) { g_etag = etag; }
uint32_t od_xfer_app_now_ms(void) { return g_now; }

/* Replies: recorded as the first two bytes of the last frame (ACK/NACK + opcode). */
static uint8_t g_last_reply[2];
static unsigned g_replies;

static od_txq_status_t record(const uint8_t *frame, uint16_t len)
{
    if (len >= 2u) {
        g_last_reply[0] = frame[0];
        g_last_reply[1] = frame[1];
    }
    g_replies++;
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

/* BWGBRY_SPLIT 16x8: two 4 bpp halves of 8x8 = 32 B each. One full mode whose sequence writes
 * IMAGE_SLICE 0/2 on CS 0, then IMAGE_SLICE 1/2 on CS 1, then refreshes both. */
static void provision(bool second_cs)
{
    static const uint8_t seq[] = {
        0x01, 0x10, 0x00, 0x07, 0x00, 0x02,   /* cmd 0x10, IMAGE_SLICE 0 of 2 */
        0x00, 0x08, 0x01,                     /* CS_SELECT 1 */
        0x01, 0x10, 0x00, 0x07, 0x01, 0x02,   /* cmd 0x10, IMAGE_SLICE 1 of 2 */
        0x01, 0x12, 0x00, 0x01, 0x00, 0x00,   /* cmd 0x12 */
    };
    uint8_t panel[16] = { 0u, OD_PANEL_FLAG_BUSY_USABLE, 1u, 0xE8, 0x03, 0u, 0u,
                          0u, 1u, 2u, 3u, 1u, 3u, 0x02, 0x07, 0xA5 };
    uint8_t mode[37 + sizeof seq];

    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.loaded = true;
    g_cfg.display_count = 1u;
    g_disp->pixel_width = 16u;
    g_disp->pixel_height = 8u;
    g_disp->color_scheme = OD_COLOR_SCHEME_BWGBRY_SPLIT;
    g_disp->cs_pin = 5u;
    g_disp->cs_pin_2 = second_cs ? 9u : 0xFFu;
    g_disp->reset_pin = 6u;
    g_disp->dc_pin = 7u;
    g_disp->busy_pin = 8u;

    memset(mode, 0, sizeof mode);
    strncpy((char *)&mode[2], "full", 12);
    mode[14] = 0xFFu;
    mode[15] = 1u;
    memset(&mode[19], 0xFF, 8);
    mode[35] = (uint8_t)sizeof seq;
    memcpy(&mode[37], seq, sizeof seq);

    store_reset();
    store_packet(OD_PKT_DISPLAY_PANEL, panel, sizeof panel);
    store_packet(OD_PKT_DISPLAY_MODE, mode, (uint16_t)sizeof mode);
    store_commit();

    od_xfer_reset();
    memset(g_on_cs, 0, sizeof g_on_cs);
    g_on_cs_n[0] = g_on_cs_n[1] = 0u;
    g_refresh_cmds[0] = g_refresh_cmds[1] = 0u;
    g_panel_up = g_panel_down = 0u;
    g_replies = 0u;
    g_split_ok = true;
}

static const od_reply_t OWNER = { OD_ORIGIN_BLE, 7u };
static od_tx_reservation_t g_reservation;

/* --------------------------------------------------------------------- cases --- */

static void test_split_upload_lands_per_cs(void)
{
    od_cmd_ctx_t ctx = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);
    uint8_t frame[64];
    unsigned i;

    CASE("0x70/0x71/0x72 SPLIT_HALVES: half 0 on cs_pin, half 1 on cs_pin_2");
    provision(true);
    for (i = 0u; i < 32u; i++) {
        frame[i] = (uint8_t)(0x10u + i);
        frame[32u + i] = (uint8_t)(0x80u + i);
    }
    CHECK(od_xfer_direct_start(&ctx, od_span_none()) == OD_CMD_OK);
    CHECK(od_display_active());
    /* Uneven chunks, one of them straddling the half boundary. */
    CHECK(od_xfer_data(&ctx, od_span_make(frame, 20u)) == OD_CMD_OK);
    CHECK(od_xfer_data(&ctx, od_span_make(frame + 20, 30u)) == OD_CMD_OK);
    CHECK(od_xfer_data(&ctx, od_span_make(frame + 50, 14u)) == OD_CMD_OK);
    CHECK(od_xfer_end(&ctx, od_span_none()) == OD_CMD_OK);
    CHECK(!od_display_active() && !od_xfer_active());
    CHECK(g_on_cs_n[0] == 32u && g_on_cs_n[1] == 32u);
    CHECK(memcmp(g_on_cs[0], frame, 32u) == 0);
    CHECK(memcmp(g_on_cs[1], frame + 32, 32u) == 0);
    CHECK(g_refresh_cmds[0] == 0u && g_refresh_cmds[1] == 1u);
    CHECK(g_panel_up == 1u && g_panel_down == 1u);
}

static void test_split_refused_without_target_support(void)
{
    od_cmd_ctx_t ctx = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);

    CASE("split_halves_ok false: START NACKs before the panel is touched");
    provision(true);
    g_split_ok = false;
    CHECK(od_xfer_direct_start(&ctx, od_span_none()) == OD_CMD_NACK);
    CHECK(!od_xfer_active() && !od_display_active());
    CHECK(g_panel_up == 0u && g_on_cs_n[0] == 0u && g_on_cs_n[1] == 0u);

    CASE("no cs_pin_2: CS_SELECT is unsupported, START NACKs and the panel goes back down");
    provision(false);
    CHECK(od_xfer_direct_start(&ctx, od_span_none()) == OD_CMD_NACK);
    CHECK(!od_xfer_active() && !od_display_active());
    CHECK(g_panel_up == g_panel_down && g_on_cs_n[0] == 0u && g_on_cs_n[1] == 0u);
}

int main(void)
{
    test_split_upload_lands_per_cs();
    test_split_refused_without_target_support();
    printf("xfer_split: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
