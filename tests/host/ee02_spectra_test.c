/* ee02_spectra_test -- the 7.09in dual-controller Spectra 6 (GDEB0709E01, 1200x1600) on the EE02
 * board, driven through the config-driven path the way ESP32 composes it, compared with the lab
 * firmware that drove this glass (ee02_spectra709/src/main.cpp).
 *
 * The panel is two controllers behind one glass: main = x 0..599 on cs_pin, slave = x 600..1199
 * on cs_pin_2, one shared BUSY that is the wired AND of both. The reference below is the lab
 * firmware's stream, transcribed from its source: its init table with per-chip targeting, its
 * CCSET + DTM per half, its PON / DRF / POF to both chips, and its row renderer (two pixels per
 * byte, left pixel in the high nibble, one 300-byte row per half).
 *
 * What is replayed: a full 960,000-byte frame through CMD 0x70/0x71/0x72 (raw and zlib), into
 * od_display, od_epd_cfg and od_epd, onto a recording bus with the ESP32 bus's framing (od_epd_esp:
 * cmd() and data() each frame CS; CS_SELECT picks which CS). Two configs:
 *   - the panel database's 0x0042 drive packets (what the website's `ep709` preset ships);
 *   - k_lab_order_seq, the lab firmware's own order written as a mode-0 sequence.
 * What is checked, byte for byte: the init command list and its main/slave/both targeting against
 * the lab table; the bytes behind each chip's DTM against the lab renderer; the refresh tail; the
 * 60 s BUSY cap; that no OTP-program (0x90/0x91) or A0/A1 command is ever sent.
 *
 * The multi-pass case runs the custom-waveform design of the EE02 report (k_multipass_seq, two
 * 807-byte 0xAF slots). Built as is, it pins today's refusal of such a slot; built again with
 * OD_EPD_LUT_BYTES=1024 (od_ee02_spectra_lut1k_test), it checks per-pass PLL, PSR, the 800-byte
 * 0x20 image on both chips and both frames.
 *
 * Not checked, and not checkable here: anything on glass. Nothing in this file has run on
 * hardware. */

#include "od_cmd_test_ctx.h"
#include "od_color.h"
#include "od_config.h"
#include "od_config_store.h"
#include "od_display.h"
#include "od_display_app.h"
#include "od_drive_app.h"
#include "od_epd.h"
#include "od_epd_cfg.h"
#include "od_hal_nvs.h"
#include "od_inflate_app.h"
#include "od_reply.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "od_zlib_inflate.h"
#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"

#include <stdio.h>
#include <string.h>

#include "ee02_spectra_preset.inc"

#if !OD_XFER_SPLIT_HALVES
#error "ee02_spectra_test needs OD_XFER_SPLIT_HALVES=1 (ESP32's composition)"
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

#define PANEL_W   1200u
#define PANEL_H   1600u
#define HALF_W    (PANEL_W / 2u)
#define HALF_ROW  (HALF_W / 2u)              /* 300 B: 4 bpp, two pixels per byte */
#define HALF_BYTES (HALF_ROW * PANEL_H)      /* 480,000 B */
#define FRAME_BYTES (2u * HALF_BYTES)        /* 960,000 B */

/* ------------------------------------------------------- the lab firmware's stream --- */

enum { T_M = 1, T_S = 2, T_B = 3 };

typedef struct {
    uint8_t mask, cmd, len;
    uint8_t data[9];
} lab_step_t;

/* ee02_spectra709/src/main.cpp k_init, verbatim. */
static const lab_step_t k_lab_init[] = {
    {T_M, 0x74, 9, {0x00, 0x0C, 0x0C, 0xD9, 0xDD, 0xDD, 0x15, 0x15, 0x55}},
    {T_B, 0xF0, 6, {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10}},
    {T_B, 0x00, 2, {0xDF, 0x69}},
    {T_M, 0xA5, 3, {0x44, 0x54, 0x00}},
    {T_B, 0x50, 1, {0x37}},
    {T_B, 0x60, 2, {0x03, 0x03}},
    {T_B, 0x86, 1, {0x10}},
    {T_B, 0xE3, 1, {0x22}},
    {T_B, 0x61, 4, {0x04, 0xB0, 0x03, 0x20}},
    {T_M, 0x01, 6, {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38}},
    {T_M, 0xB6, 1, {0x07}},
    {T_M, 0x06, 2, {0xE0, 0x20}},
    {T_M, 0xB7, 1, {0x01}},
    {T_M, 0x05, 2, {0xE0, 0x20}},
    {T_M, 0xB0, 1, {0x01}},
    {T_M, 0xB1, 1, {0x02}},
};
#define LAB_INIT_N (sizeof k_lab_init / sizeof k_lab_init[0])

/* The lab frame: a background nibble plus rectangles, later ones on top (main.cpp addRect /
 * identPattern), extended with codes 2 (Y) and 6 (G) and rectangles that straddle the seam, so
 * every driven code and the x = 599 | 600 boundary are covered. */
typedef struct {
    uint16_t x, y, w, h;
    uint8_t n;
} rect_t;

static const rect_t k_rects[] = {
    {16, 16, 40, 40, 0},
    {PANEL_W - 16 - 40, 16, 40, 40, 0},
    {16, PANEL_H - 16 - 40, 40, 40, 0},
    {PANEL_W - 16 - 40, PANEL_H - 16 - 40, 40, 40, 0},
    {120, 120, 240, 40, 3},
    {120, 120, 40, 360, 3},
    {HALF_W + 120, 120, 120, 120, 5},
    {HALF_W - 2, 0, 4, PANEL_H, 0},
    {500, 700, 200, 101, 2},       /* across the seam, odd height */
    {599, 900, 2, 300, 6},         /* exactly the two seam columns */
    {301, 1201, 777, 33, 6},       /* odd x and width: nibble-split edges in both halves */
    {0, 1599, PANEL_W, 1, 5},      /* last row */
    {900, 300, 51, 40, 7},         /* hold codes: streamed verbatim, never clamped */
    {41, 1400, 1100, 3, 4},
};
static const uint8_t k_bg = 1u;   /* white */

/* main.cpp renderRow: half 0 = main (x 0..599), 1 = slave (x 600..1199). */
static void lab_render_row(uint16_t y, uint8_t half, uint8_t *out)
{
    static uint8_t px[HALF_W];
    const int x0 = half ? (int)HALF_W : 0;

    memset(px, k_bg & 0x0F, sizeof px);
    for (size_t i = 0; i < sizeof k_rects / sizeof k_rects[0]; i++) {
        const rect_t *r = &k_rects[i];
        int a = (int)r->x - x0;
        int b = (int)r->x + r->w - x0;

        if (y < r->y || y >= r->y + r->h || b <= 0 || a >= (int)HALF_W) {
            continue;
        }
        if (a < 0) {
            a = 0;
        }
        if (b > (int)HALF_W) {
            b = (int)HALF_W;
        }
        memset(px + a, r->n, (size_t)(b - a));
    }
    for (unsigned i = 0; i < HALF_ROW; i++) {
        out[i] = (uint8_t)((px[2u * i] << 4) | px[2u * i + 1u]);
    }
}

/* The lab's DTM payload for one chip: 1600 rows of 300 B. */
static uint8_t g_lab_half[2][HALF_BYTES];

static void lab_frame(void)
{
    for (uint8_t half = 0; half < 2u; half++) {
        for (uint16_t y = 0; y < PANEL_H; y++) {
            lab_render_row(y, half, &g_lab_half[half][(uint32_t)y * HALF_ROW]);
        }
    }
}

/* ------------------------------------------- the host's frame (independent encoder) --- */

/* The pixel at (x, y), from the same rectangles but evaluated per pixel. */
static uint8_t pixel(uint16_t x, uint16_t y)
{
    uint8_t n = k_bg;

    for (size_t i = 0; i < sizeof k_rects / sizeof k_rects[0]; i++) {
        const rect_t *r = &k_rects[i];

        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) {
            n = r->n;
        }
    }
    return n;
}

/* BWGBRY_SPLIT (scheme 8) as a client encodes it: the left half's rows, then the right half's,
 * each row ceil(600 * 4 / 8) = 300 B, left pixel in the high nibble. */
static uint8_t g_frame[FRAME_BYTES];

static void host_frame(void)
{
    uint32_t o = 0u;

    for (uint8_t half = 0; half < 2u; half++) {
        for (uint16_t y = 0; y < PANEL_H; y++) {
            for (uint16_t x = (uint16_t)(half * HALF_W); x < (half + 1u) * HALF_W; x += 2u) {
                g_frame[o++] = (uint8_t)((pixel(x, y) << 4) | pixel((uint16_t)(x + 1u), y));
            }
        }
    }
}

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

static void store_raw(const uint8_t *framed, size_t n)
{
    memcpy(&g_store[OD_CONFIG_STORE_HEADER_SIZE + g_payload_len], framed, n);
    g_payload_len += (uint32_t)n;
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

/* One token per bus transaction, in order. A command and the data calls after it on the same CS
 * are one CMD token (the ESP32 bus frames CS around each call; the controller sees one command).
 * Consecutive BUSY reads with the executor's 5 ms poll delays between them are one BUSY token. */
enum { TK_RESET = 1, TK_CMD, TK_BUSY, TK_DELAY, TK_ORPHAN_DATA };

typedef struct {
    uint8_t kind;
    uint8_t cs;
    uint8_t cmd;
    uint8_t len;          /* data bytes kept in d[] (<= 16) */
    uint32_t n;           /* data bytes sent */
    uint8_t d[16];
    uint32_t ms;          /* DELAY: ms; BUSY: ms spent busy */
} token_t;

static token_t g_tok[512];
static unsigned g_ntok;
static bool g_tok_overflow;

static uint8_t g_cs;
static uint32_t g_now;
static uint32_t g_busy_until;
static uint32_t g_refresh_ms;      /* how long DRF holds BUSY */
static bool g_last_read_busy;
/* The data behind every DTM (0x10) and waveform-register (0x20) command, in the order they went
 * out: a single pass sends DTM to cs0 then cs1 (captures 0 and 1), a second pass adds 2 and 3. */
#define MAX_CAP 4u
#define LUT_CAP 1024u
static uint8_t g_cap[MAX_CAP][HALF_BYTES];
static uint32_t g_cap_n[MAX_CAP];
static uint8_t g_cap_cs[MAX_CAP];
static unsigned g_ncap;
static uint8_t g_lut[MAX_CAP][LUT_CAP];
static uint32_t g_lut_n[MAX_CAP];
static uint8_t g_lut_cs[MAX_CAP];
static unsigned g_nlut;

static token_t *push(uint8_t kind)
{
    if (g_ntok >= sizeof g_tok / sizeof g_tok[0]) {
        g_tok_overflow = true;
        return &g_tok[g_ntok - 1u];
    }
    memset(&g_tok[g_ntok], 0, sizeof g_tok[0]);
    g_tok[g_ntok].kind = kind;
    g_tok[g_ntok].cs = g_cs;
    return &g_tok[g_ntok++];
}

static token_t *last(void) { return g_ntok ? &g_tok[g_ntok - 1u] : NULL; }

static void bus_cmd(uint8_t c)
{
    token_t *t = push(TK_CMD);

    t->cmd = c;
    if (c == 0x10u && g_ncap < MAX_CAP) {
        g_cap_cs[g_ncap] = g_cs;
        g_cap_n[g_ncap++] = 0u;
    }
    if (c == 0x20u && g_nlut < MAX_CAP) {
        g_lut_cs[g_nlut] = g_cs;
        g_lut_n[g_nlut++] = 0u;
    }
    /* BUSY is the AND of both chips: a refresh holds it once DRF has reached cs1, the second copy
     * of the both-chip DRF. PON holds it briefly. */
    if (c == 0x04u && g_cs == 1u) {
        g_busy_until = g_now + 298u;
    }
}

static void bus_data(const uint8_t *d, size_t n)
{
    token_t *t = last();

    if (t == NULL || t->kind != TK_CMD || t->cs != g_cs) {
        t = push(TK_ORPHAN_DATA);
    }
    if (t->kind == TK_CMD && t->cmd == 0x10u && g_ncap > 0u) {
        const unsigned k = g_ncap - 1u;

        for (size_t i = 0; i < n; i++) {
            if (g_cap_n[k] < HALF_BYTES) {
                g_cap[k][g_cap_n[k]] = d[i];
            }
            g_cap_n[k]++;
        }
    }
    if (t->kind == TK_CMD && t->cmd == 0x20u && g_nlut > 0u) {
        const unsigned k = g_nlut - 1u;

        for (size_t i = 0; i < n; i++) {
            if (g_lut_n[k] < LUT_CAP) {
                g_lut[k][g_lut_n[k]] = d[i];
            }
            g_lut_n[k]++;
        }
    }
    for (size_t i = 0; i < n && t->len < sizeof t->d; i++) {
        t->d[t->len++] = d[i];
    }
    t->n += (uint32_t)n;
    /* The refresh starts on the main chip's DRF, which follows the slave's (README D9). */
    if (t->kind == TK_CMD && t->cmd == 0x12u && g_cs == 0u) {
        g_busy_until = g_now + g_refresh_ms;
    }
}

static void bus_reset(uint32_t ms)
{
    (void)push(TK_RESET);
    g_now += ms * 3u;
}

static int bus_busy(void)
{
    const bool busy = g_now < g_busy_until;
    token_t *t = last();

    if (t == NULL || t->kind != TK_BUSY) {
        (void)push(TK_BUSY);
    }
    g_last_read_busy = busy;
    return busy ? 0 : 1;   /* 0xB1 busy_idle_level 1: high = idle */
}

static void bus_delay(uint32_t ms)
{
    token_t *t = last();

    if (t != NULL && t->kind == TK_BUSY && g_last_read_busy && ms == 5u) {
        t->ms += ms;        /* the executor's poll interval inside one BUSY wait */
    } else {
        push(TK_DELAY)->ms = ms;
    }
    g_now += ms;
}

static uint32_t bus_now(void) { return g_now; }
static bool bus_pin(uint8_t pin, uint8_t level) { (void)pin; (void)level; return true; }

static bool bus_cs_select(uint8_t which)
{
    if (which > 1u) {
        return false;
    }
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
    .cs_select = bus_cs_select,
    .read_bytes = NULL,   /* as on ESP32: the panel bus has no MISO */
};

/* ------------------------------------------------------------- target seams --- */

static struct od_config g_cfg;
static struct DisplayConfig *g_disp = &g_cfg.displays[0];
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

/* targets/esp32-idf/src/display_service.cpp's od_xfer_app glue for SPI panels. */
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
    return od_display_write(stream_offset, data.p, data.n) ? (uint32_t)data.n : 0u;
}

od_mut_span_t od_xfer_app_inflate_scratch(void) { return od_mut_span_make(g_scratch, sizeof g_scratch); }
void od_xfer_app_abort(od_xfer_abort_reason_t reason) { (void)reason; od_display_abort(); }
od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner) { (void)owner; return OD_XFER_BARRIER_PROCEED; }
void od_xfer_app_barrier_abort(const od_reply_t *owner) { (void)owner; od_display_abort(); }

static bool g_refresh_completed;

bool od_xfer_app_refresh(uint8_t mode, bool *completed)
{
    if (completed == NULL || !od_display_active()) {
        return false;
    }
    *completed = od_display_refresh(mode);
    g_refresh_completed = *completed;
    return true;
}

uint32_t od_xfer_app_displayed_etag(void) { return g_etag; }
void od_xfer_app_set_displayed_etag(uint32_t etag) { g_etag = etag; }
uint32_t od_xfer_app_now_ms(void) { return g_now; }

static uint8_t g_last_reply[2];

static od_txq_status_t record(const uint8_t *frame, uint16_t len)
{
    if (len >= 2u) {
        g_last_reply[0] = frame[0];
        g_last_reply[1] = frame[1];
    }
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

/* The EE02's 0x20 display block: 1200x1600, BWGBRY_SPLIT, its pins (ee02_spectra_preset.inc). */
static void display_block(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.loaded = true;
    g_cfg.display_count = 1u;
    g_disp->panel_ic_type = EE02_PANEL_IC;
    g_disp->pixel_width = PANEL_W;
    g_disp->pixel_height = PANEL_H;
    g_disp->color_scheme = OD_COLOR_SCHEME_BWGBRY_SPLIT;
    g_disp->data_pin = EE02_PIN_DATA;
    g_disp->clk_pin = EE02_PIN_CLK;
    g_disp->cs_pin = EE02_PIN_CS;
    g_disp->cs_pin_2 = EE02_PIN_CS_2;
    g_disp->dc_pin = EE02_PIN_DC;
    g_disp->reset_pin = EE02_PIN_RESET;
    g_disp->busy_pin = EE02_PIN_BUSY;
    g_disp->aux_pin_1 = EE02_PIN_MISO;
}

static void reset_bus(uint32_t refresh_ms)
{
    g_ntok = 0u;
    g_tok_overflow = false;
    g_cs = 0u;
    g_now = 0u;
    g_busy_until = 0u;
    g_refresh_ms = refresh_ms;
    g_ncap = g_nlut = 0u;
    g_panel_up = g_panel_down = 0u;
    g_refresh_completed = false;
    od_xfer_reset();
}

static void provision_database(void)
{
    display_block();
    store_reset();
    store_raw(k_preset_EP133S, sizeof k_preset_EP133S);
    store_commit();
}

/* The same 0xB1 as the database (BUSY usable, idle high, 60 s cap, sleep = POF to both chips),
 * with k_lab_order_seq as mode 0. */
static void provision_lab_order(void)
{
    uint8_t mode[37 + sizeof k_lab_order_seq];

    display_block();
    memset(mode, 0, sizeof mode);
    strncpy((char *)&mode[2], "full", 12);
    mode[14] = 0xFFu;
    mode[15] = 1u;
    memset(&mode[19], 0xFF, 8);
    mode[35] = (uint8_t)(sizeof k_lab_order_seq & 0xFFu);
    mode[36] = (uint8_t)(sizeof k_lab_order_seq >> 8);
    memcpy(&mode[37], k_lab_order_seq, sizeof k_lab_order_seq);

    store_reset();
    /* k_preset_EP133S starts with the framed 0xB1: [0][B1][len:2][body]. */
    store_raw(k_preset_EP133S, 4u + (size_t)(k_preset_EP133S[2] | (k_preset_EP133S[3] << 8)));
    store_packet(OD_PKT_DISPLAY_MODE, mode, (uint16_t)sizeof mode);
    store_commit();
}

static const od_reply_t OWNER = { OD_ORIGIN_BLE, 7u };
static od_tx_reservation_t g_reservation;

/* An uncompressed 0x70/0x71/0x72 upload in 230-byte chunks (a BLE-sized payload). */
static bool upload_raw(void)
{
    od_cmd_ctx_t ctx = od_test_cmd_ctx(OWNER, &g_reservation, 2u, false);
    bool ok = od_xfer_direct_start(&ctx, od_span_none()) == OD_CMD_OK;

    for (uint32_t o = 0u; ok && o < FRAME_BYTES; o += 230u) {
        const uint32_t n = FRAME_BYTES - o < 230u ? FRAME_BYTES - o : 230u;

        ok = od_xfer_data(&ctx, od_span_make(g_frame + o, n)) == OD_CMD_OK;
    }
    return ok && od_xfer_end(&ctx, od_span_none()) == OD_CMD_OK;
}

/* The same frame as a zlib stream of stored blocks (no compressor in the host gate), behind the
 * 4-byte declared length: the compressed START with a total above 64 KiB. */
static uint8_t g_z[FRAME_BYTES + FRAME_BYTES / 65535u * 5u + 64u];

static size_t zlib_stored(void)
{
    uint32_t a = 1u, b = 0u, o = 0u;
    size_t n = 0u;

    /* CMF 0x18: deflate with a 512-byte window, the firmware's OPENDISPLAY_ZLIB_WINDOW_BITS 9
     * (a client compresses with wbits 9); FLG 0x19 makes the header check a multiple of 31. */
    g_z[n++] = 0x18u;
    g_z[n++] = 0x19u;
    while (o < FRAME_BYTES) {
        const uint32_t len = FRAME_BYTES - o > 65535u ? 65535u : FRAME_BYTES - o;

        g_z[n++] = (uint8_t)(o + len == FRAME_BYTES ? 1u : 0u);
        g_z[n++] = (uint8_t)len;
        g_z[n++] = (uint8_t)(len >> 8);
        g_z[n++] = (uint8_t)~len;
        g_z[n++] = (uint8_t)(~len >> 8);
        memcpy(&g_z[n], g_frame + o, len);
        n += len;
        o += len;
    }
    for (uint32_t i = 0; i < FRAME_BYTES; i++) {
        a = (a + g_frame[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    const uint32_t adler = (b << 16) | a;
    for (int k = 3; k >= 0; k--) {
        g_z[n++] = (uint8_t)(adler >> (8 * k));
    }
    return n;
}

static bool upload_zlib(void)
{
    static uint8_t start[4];
    const size_t zn = zlib_stored();
    od_cmd_ctx_t ctx = od_test_cmd_ctx(OWNER, &g_reservation, 6u, false);
    bool ok;

    start[0] = (uint8_t)FRAME_BYTES;
    start[1] = (uint8_t)(FRAME_BYTES >> 8);
    start[2] = (uint8_t)(FRAME_BYTES >> 16);
    start[3] = (uint8_t)(FRAME_BYTES >> 24);
    ok = od_xfer_direct_start(&ctx, od_span_make(start, sizeof start)) == OD_CMD_OK;
    for (size_t o = 0u; ok && o < zn; o += 230u) {
        const size_t n = zn - o < 230u ? zn - o : 230u;

        ok = od_xfer_data(&ctx, od_span_make(g_z + o, n)) == OD_CMD_OK;
    }
    return ok && od_xfer_end(&ctx, od_span_none()) == OD_CMD_OK;
}

/* ------------------------------------------------------------ stream analysis --- */

/* Bus commands with their targeting: a command and its data sent to cs0 and then, unchanged, to
 * cs1 is one "both" command (panel database README D6: od_epd has no both-CS select). */
typedef struct {
    uint8_t mask, cmd;
    uint32_t n;
    uint8_t len;
    const uint8_t *d;
    unsigned tok;           /* index of the first token */
} txn_t;

static txn_t g_txn[256];
static unsigned g_ntxn;

static void collapse(unsigned from, unsigned to)
{
    g_ntxn = 0u;
    for (unsigned i = from; i < to && g_ntxn < sizeof g_txn / sizeof g_txn[0]; i++) {
        const token_t *t = &g_tok[i];
        txn_t *x;

        if (t->kind != TK_CMD) {
            continue;
        }
        x = &g_txn[g_ntxn++];
        x->mask = t->cs == 0u ? T_M : T_S;
        x->cmd = t->cmd;
        x->n = t->n;
        x->len = t->len;
        x->d = t->d;
        x->tok = i;
        /* Look past escapes (CS_SELECT emits no token) for the cs1 twin. */
        if (t->cs == 0u && t->cmd != 0x10u) {
            for (unsigned j = i + 1u; j < to; j++) {
                const token_t *u = &g_tok[j];

                if (u->kind != TK_CMD) {
                    break;
                }
                if (u->cs == 1u && u->cmd == t->cmd && u->n == t->n && u->len == t->len &&
                    memcmp(u->d, t->d, t->len) == 0) {
                    x->mask = T_B;
                    i = j;
                }
                break;
            }
        }
    }
}

/* Index of the first CMD token with `cmd` at or after `from`, or g_ntok. */
static unsigned find_cmd(unsigned from, uint8_t cmd)
{
    for (unsigned i = from; i < g_ntok; i++) {
        if (g_tok[i].kind == TK_CMD && g_tok[i].cmd == cmd) {
            return i;
        }
    }
    return g_ntok;
}

static bool init_matches_lab(unsigned from, unsigned to)
{
    collapse(from, to);
    if (g_ntxn != LAB_INIT_N) {
        printf("  init: %u commands, lab has %u\n", g_ntxn, (unsigned)LAB_INIT_N);
        return false;
    }
    for (unsigned k = 0; k < LAB_INIT_N; k++) {
        const lab_step_t *l = &k_lab_init[k];
        const txn_t *x = &g_txn[k];

        if (x->mask != l->mask || x->cmd != l->cmd || x->n != l->len ||
            memcmp(x->d, l->data, l->len) != 0) {
            printf("  init step %u: got mask %u cmd %02X n %u, lab mask %u cmd %02X n %u\n", k,
                   x->mask, x->cmd, (unsigned)x->n, l->mask, l->cmd, l->len);
            return false;
        }
    }
    return true;
}

/* Every init command is followed by the lab's 10 ms gap before the next one goes out. */
static bool init_gaps_are_10ms(unsigned from, unsigned to)
{
    unsigned gaps = 0u;

    for (unsigned i = from; i < to; i++) {
        if (g_tok[i].kind == TK_DELAY) {
            if (g_tok[i].ms != 10u) {
                return false;
            }
            gaps++;
        }
    }
    return gaps == LAB_INIT_N;
}

static bool never_sent_forbidden(void)
{
    for (unsigned i = 0; i < g_ntok; i++) {
        const uint8_t c = g_tok[i].cmd;

        if (g_tok[i].kind == TK_CMD && (c == 0x90u || c == 0x91u || c == 0xA0u || c == 0xA1u)) {
            return false;
        }
    }
    return true;
}

static bool no_orphan_data(void)
{
    for (unsigned i = 0; i < g_ntok; i++) {
        if (g_tok[i].kind == TK_ORPHAN_DATA) {
            return false;
        }
    }
    return true;
}

/* DTM captures k (main) and k + 1 (slave) carry exactly these two halves. */
static bool dtm_pair_is(unsigned k, const uint8_t *main_half, const uint8_t *slave_half)
{
    return g_ncap >= k + 2u && g_cap_cs[k] == 0u && g_cap_cs[k + 1u] == 1u &&
           g_cap_n[k] == HALF_BYTES && g_cap_n[k + 1u] == HALF_BYTES &&
           memcmp(g_cap[k], main_half, HALF_BYTES) == 0 &&
           memcmp(g_cap[k + 1u], slave_half, HALF_BYTES) == 0;
}

static bool frame_matches_lab(void)
{
    return g_ncap == 2u && dtm_pair_is(0u, g_lab_half[0], g_lab_half[1]);
}

/* Token-level shape of a tail: a string of 'R' reset, 'B' BUSY, 'D' delay, and the commands as
 * "<cmd hex><m|s|b>" -- easy to compare with a literal and to print on failure. */
static void tail_shape(unsigned from, char *out, size_t cap)
{
    size_t o = 0u;

    out[0] = '\0';
    for (unsigned i = from; i < g_ntok && o + 8u < cap; i++) {
        const token_t *t = &g_tok[i];

        switch (t->kind) {
        case TK_BUSY:
            out[o++] = 'B';
            break;
        case TK_DELAY:
            o += (size_t)snprintf(out + o, cap - o, "D%u", (unsigned)t->ms);
            break;
        case TK_RESET:
            out[o++] = 'R';
            break;
        case TK_CMD: {
            const bool twin = t->cs == 0u && t->cmd != 0x10u && i + 1u < g_ntok &&
                              g_tok[i + 1u].kind == TK_CMD && g_tok[i + 1u].cs == 1u &&
                              g_tok[i + 1u].cmd == t->cmd && g_tok[i + 1u].n == t->n &&
                              memcmp(g_tok[i + 1u].d, t->d, t->len) == 0;

            o += (size_t)snprintf(out + o, cap - o, "%02X%c", t->cmd,
                                  twin ? 'b' : (t->cs == 0u ? 'm' : 's'));
            if (twin) {
                i++;
            }
            break;
        }
        default:
            out[o++] = '?';
            break;
        }
        if (o + 1u < cap) {
            out[o++] = ' ';
            out[o] = '\0';
        }
    }
}

/* --------------------------------------------------------------------- cases --- */

static void test_database_preset(bool compressed)
{
    char shape[512];
    unsigned pon, ccset;

    CASE(compressed ? "0x0042 preset, zlib upload: init, targeting, frame vs the lab firmware"
                    : "0x0042 preset, raw upload: init, targeting, frame vs the lab firmware");
    provision_database();
    reset_bus(30200u);
    CHECK(compressed ? upload_zlib() : upload_raw());
    CHECK(g_refresh_completed);
    CHECK(!g_tok_overflow && no_orphan_data());
    CHECK(g_panel_up == 1u && g_panel_down == 1u);

    /* The pass starts with the executor's reset and a BUSY wait. */
    CHECK(g_ntok > 2u && g_tok[0].kind == TK_RESET && g_tok[1].kind == TK_BUSY);

    /* Init = everything before the first PON: the lab table exactly, per chip. */
    pon = find_cmd(0u, 0x04u);
    CHECK(pon < g_ntok);
    CHECK(init_matches_lab(0u, pon));
    CHECK(init_gaps_are_10ms(0u, pon));

    /* The frame: each chip's DTM carries the lab renderer's bytes, nothing else, nothing more. */
    CHECK(frame_matches_lab());

    /* The database order differs from the lab's: PON goes out BEFORE the frame (the lab powers on
     * after the DTM), and there are no 30 ms gaps around DRF. Pinned here so a change to the
     * generated data is seen; whether it matters on glass is open (REPORT.md). */
    ccset = find_cmd(pon, 0xE0u);
    CHECK(ccset < g_ntok && ccset > pon);
    tail_shape(pon, shape, sizeof shape);
    CHECK(strcmp(shape, "04b B E0b B 10m 10s 12s 12m B 02b B 02b ") == 0);
    if (strcmp(shape, "04b B E0b B 10m 10s 12s 12m B 02b B 02b ") != 0) {
        printf("  tail: %s\n", shape);
    }
    CHECK(never_sent_forbidden());
}

static void test_lab_order(void)
{
    char shape[512];
    unsigned ccset;

    CASE("lab-order sequence: the lab firmware's whole stream from one 0xAE mode");
    provision_lab_order();
    reset_bus(30200u);
    CHECK(upload_raw());
    CHECK(g_refresh_completed);
    CHECK(!g_tok_overflow && no_orphan_data());
    ccset = find_cmd(0u, 0xE0u);
    CHECK(ccset < g_ntok);
    CHECK(init_matches_lab(0u, ccset));
    CHECK(init_gaps_are_10ms(0u, ccset));
    CHECK(frame_matches_lab());
    tail_shape(ccset, shape, sizeof shape);
    /* main.cpp pushFrame (CCSET both, wait, DTM main, DTM slave) then refreshFull (PON both, wait,
     * 30 ms, DRF slave then main (README D9), wait, 30 ms, POF both, wait); the sleep sequence's POF follows. */
    CHECK(strcmp(shape, "E0b D1 B 10m 10s 04b D1 B D30 12s 12m D1 B D30 02b D1 B 02b ") == 0);
    if (strcmp(shape, "E0b D1 B 10m 10s 04b D1 B D30 12s 12m D1 B D30 02b D1 B 02b ") != 0) {
        printf("  tail: %s\n", shape);
    }
    CHECK(never_sent_forbidden());
}

static void test_busy_cap(void)
{
    unsigned drf;
    const token_t *wait;

    CASE("BUSY stuck after DRF: the wait gives up at the 0xB1 cap (60 s), POF goes to both");
    provision_database();
    reset_bus(100000000u);            /* the refresh does not end in this test */
    (void)upload_raw();
    CHECK(!g_refresh_completed);
    /* The END reply is the 2.x "refresh timed out" ACK, [00][74]. */
    CHECK(g_last_reply[0] == RESP_ACK && g_last_reply[1] == RESP_DIRECT_WRITE_REFRESH_TIMEOUT);
    drf = find_cmd(0u, 0x12u);
    CHECK(drf + 2u < g_ntok);
    /* drf is the slave's copy, drf + 1 the main chip's (README D9), then the BUSY wait. */
    wait = &g_tok[drf + 2u];
    CHECK(wait->kind == TK_BUSY && wait->ms == 60000u);
    /* The mode's own POF never ran (the walk stopped at the failed wait); the 0xB1 sleep
     * sequence's POF to both chips did, and the rail went down. Nothing pulsed RST: the lab saw a
     * wedged chip ignore POF, which this path does not handle (REPORT.md). */
    {
        char shape[128];

        tail_shape(drf + 2u, shape, sizeof shape);
        CHECK(strcmp(shape, "B 02b ") == 0);
        if (strcmp(shape, "B 02b ") != 0) {
            printf("  after DRF: %s\n", shape);
        }
    }
    CHECK(g_panel_down == 1u);
}

static void test_geometry(void)
{
    od_color_geometry_t g;

    CASE("BWGBRY_SPLIT 1200x1600: two 600x1600 halves at 4 bpp, 960,000 B");
    CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_BWGBRY_SPLIT, PANEL_W, PANEL_H, &g) ==
          OD_COLOR_OK);
    CHECK(g.layout == OD_COLOR_LAYOUT_SPLIT_HALVES && g.bits_per_pixel == 4u);
    CHECK(g.part_width[0] == HALF_W && g.part_width[1] == HALF_W);
    CHECK(g.row_bytes[0] == HALF_ROW && g.part_bytes[0] == HALF_BYTES &&
          g.part_bytes[1] == HALF_BYTES && g.total_bytes == FRAME_BYTES);

    CASE("host encoder and lab renderer agree before any firmware is involved");
    CHECK(memcmp(g_frame, g_lab_half[0], HALF_BYTES) == 0);
    CHECK(memcmp(g_frame + HALF_BYTES, g_lab_half[1], HALF_BYTES) == 0);
}


/* ------------------------------------------- custom waveform + multi-pass (proposal) --- */

/* REPORT.md's design for the 42-colour mode, on today's escapes: mode 1, two passes, slot 0 and
 * slot 1, k_multipass_seq. A slot is the 7-byte prefix (PLL, PSR byte B) plus an 800-byte 0x20
 * image; the tables here are patterns, not waveforms. Pass 0 keeps PSR byte B 0x69 (the chip
 * runs its OTP waveform; the uploaded table is ignored, as the lab saw), pass 1 sets 0xE9 and
 * PLL 05. The pass-1 frame uses code 7 (hold) as well as the six driven codes. */
#define SLOT_LEN (7u + 800u)

static uint8_t g_frame2[FRAME_BYTES];

static uint8_t step_code(uint16_t x, uint16_t y)
{
    static const uint8_t codes[7] = { 0u, 1u, 2u, 3u, 5u, 6u, 7u };

    return codes[(x / 37u + y / 23u) % 7u];
}

static void host_frame2(void)
{
    uint32_t o = 0u;

    for (uint8_t half = 0; half < 2u; half++) {
        for (uint16_t y = 0; y < PANEL_H; y++) {
            for (uint16_t x = (uint16_t)(half * HALF_W); x < (half + 1u) * HALF_W; x += 2u) {
                g_frame2[o++] =
                    (uint8_t)((step_code(x, y) << 4) | step_code((uint16_t)(x + 1u), y));
            }
        }
    }
}

static uint8_t g_slot[2][SLOT_LEN];

static uint16_t crc16_ccitt(const uint8_t *d, size_t n)
{
    uint16_t crc = 0xFFFFu;

    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)d[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void mode_packet(uint8_t index, const char *name, uint8_t passes, const uint8_t *slots,
                        const uint8_t *seq, uint16_t seq_len)
{
    static uint8_t mode[37 + 512];

    memset(mode, 0, sizeof mode);
    mode[1] = index;
    strncpy((char *)&mode[2], name, 12);
    mode[14] = 0xFFu;
    mode[15] = passes;
    memset(&mode[19], 0xFF, 8);
    for (uint8_t p = 0; p < passes; p++) {
        mode[19 + p] = slots[p];
    }
    mode[35] = (uint8_t)seq_len;
    mode[36] = (uint8_t)(seq_len >> 8);
    memcpy(&mode[37], seq, seq_len);
    store_packet(OD_PKT_DISPLAY_MODE, mode, (uint16_t)(37u + seq_len));
}

static void provision_multipass(void)
{
    static const uint8_t prefix[2][7] = {
        { 0x04u, 0x69u, 0u, 0u, 0u, 0u, 0u },
        { 0x05u, 0xE9u, 0u, 0u, 0u, 0u, 0u },
    };
    static const uint8_t slots[2] = { 0u, 1u };
    const size_t b1 = 4u + (size_t)(k_preset_EP133S[2] | (k_preset_EP133S[3] << 8));
    uint8_t panel[64];
    uint8_t lut[4 + SLOT_LEN];

    display_block();
    store_reset();
    memcpy(panel, k_preset_EP133S, b1);
    panel[4 + 11] = 2u;                       /* DisplayPanelHead.mode_count */
    store_raw(panel, b1);
    mode_packet(0u, "full", 1u, slots, k_lab_order_seq, (uint16_t)sizeof k_lab_order_seq);
    mode_packet(1u, "colour42", 2u, slots, k_multipass_seq, (uint16_t)sizeof k_multipass_seq);
    for (uint8_t k = 0; k < 2u; k++) {
        memcpy(g_slot[k], prefix[k], 7u);
        for (unsigned i = 0; i < 800u; i++) {
            g_slot[k][7u + i] = (uint8_t)(k ? (0xFFu - i * 13u) : (i * 7u + 1u));
        }
        const uint16_t crc = crc16_ccitt(g_slot[k], SLOT_LEN);
        lut[0] = 0u;
        lut[1] = k;
        lut[2] = (uint8_t)crc;
        lut[3] = (uint8_t)(crc >> 8);
        memcpy(&lut[4], g_slot[k], SLOT_LEN);
        store_packet(OD_PKT_DISPLAY_LUT, lut, (uint16_t)sizeof lut);
    }
    store_commit();
}

/* Data of the n-th command `cmd` sent to chip `cs`, or NULL. */
static const token_t *nth_cmd(uint8_t cmd, uint8_t cs, unsigned n)
{
    for (unsigned i = 0; i < g_ntok; i++) {
        if (g_tok[i].kind == TK_CMD && g_tok[i].cmd == cmd && g_tok[i].cs == cs && n-- == 0u) {
            return &g_tok[i];
        }
    }
    return NULL;
}

static void test_multipass(void)
{
    od_epd_cfg_mode_t hdr;
    od_color_geometry_t g;
    bool ok;

    provision_multipass();
    reset_bus(20200u);
    CHECK(od_epd_cfg_load(g_disp, &g_bus));
    CHECK(od_epd_cfg_select(1u, &hdr) && hdr.passes == 2u);
    CHECK(od_color_direct_geometry(g_disp->color_scheme, PANEL_W, PANEL_H, &g) == OD_COLOR_OK);
    ok = od_epd_cfg_begin_pass(0u);

#if OD_EPD_LUT_BYTES < 807
    CASE("multi-pass, 807-byte slots: refused today (OD_EPD_LUT_BYTES 535), at the LUT step");
    CHECK(!ok && od_epd_cfg_last_rc() == OD_EPD_RC_LUT);
    /* The refusal comes after the init went out (the validator does not resolve slots): the PSR
     * SUBST of an absent slot sends byte B 0x00, and the bare 0x20 command goes out with no data
     * before LUT_SLICE fails. Recorded so a fix is visible. */
    CHECK(find_cmd(0u, 0x74u) < g_ntok);
    CHECK(g_ncap == 0u && g_nlut == 1u && g_lut_n[0] == 0u);
    {
        const token_t *psr = nth_cmd(0x00u, 0u, 1u);
        CHECK(psr != NULL && psr->n == 2u && psr->d[0] == 0xDFu && psr->d[1] == 0x00u);
    }
    od_epd_cfg_sleep();
#else
    od_epd_cfg_stream_t st;

    CASE("multi-pass, 807-byte slots (OD_EPD_LUT_BYTES raised): per-pass PLL, PSR, 0x20, frames");
    CHECK(ok);
    CHECK(ok && od_epd_cfg_stream_init(&st, &g, 2u, false, NULL, 0u));
    for (uint32_t o = 0u; ok && o < 2u * FRAME_BYTES; o += 230u) {
        const uint32_t n = 2u * FRAME_BYTES - o < 230u ? 2u * FRAME_BYTES - o : 230u;
        const uint8_t *src = o < FRAME_BYTES ? g_frame + o : g_frame2 + (o - FRAME_BYTES);
        /* 230 does not divide 960,000: split the chunk that straddles the pass boundary. */
        const uint32_t cut = (o < FRAME_BYTES && o + n > FRAME_BYTES) ? FRAME_BYTES - o : n;

        ok = od_epd_cfg_stream_write(&st, o, src, cut, NULL);
        if (ok && cut < n) {
            ok = od_epd_cfg_stream_write(&st, FRAME_BYTES, g_frame2, n - cut, NULL);
        }
    }
    CHECK(ok);
    CHECK(od_epd_cfg_finish());
    od_epd_cfg_sleep();
    CHECK(!g_tok_overflow && no_orphan_data());

    /* Both passes: reset + the lab init, byte for byte. */
    {
        const unsigned r1 = 0u;
        unsigned r2 = g_ntok;

        for (unsigned i = 1u; i < g_ntok; i++) {
            if (g_tok[i].kind == TK_RESET) {
                r2 = i;
                break;
            }
        }
        CHECK(g_tok[r1].kind == TK_RESET && r2 < g_ntok);
        CHECK(init_matches_lab(r1, find_cmd(r1, 0x30u)));
        CHECK(init_matches_lab(r2, find_cmd(r2, 0x30u)));
    }
    /* Per pass, from the slot prefix: PLL to both chips, then the PSR override to both. */
    for (uint8_t cs = 0; cs < 2u; cs++) {
        const token_t *pll0 = nth_cmd(0x30u, cs, 0u), *pll1 = nth_cmd(0x30u, cs, 1u);
        const token_t *psr0 = nth_cmd(0x00u, cs, 1u), *psr1 = nth_cmd(0x00u, cs, 3u);

        CHECK(pll0 && pll0->n == 1u && pll0->d[0] == 0x04u);
        CHECK(pll1 && pll1->n == 1u && pll1->d[0] == 0x05u);
        CHECK(psr0 && psr0->n == 2u && psr0->d[0] == 0xDFu && psr0->d[1] == 0x69u);
        CHECK(psr1 && psr1->n == 2u && psr1->d[0] == 0xDFu && psr1->d[1] == 0xE9u);
    }
    /* The 800-byte image to 0x20 on main then slave, each pass its own slot, prefix excluded. */
    CHECK(g_nlut == 4u);
    for (unsigned k = 0; k < g_nlut && k < 4u; k++) {
        CHECK(g_lut_cs[k] == (k & 1u) && g_lut_n[k] == 800u &&
              memcmp(g_lut[k], &g_slot[k / 2u][7], 800u) == 0);
    }
    /* Frames: pass 0's halves, then pass 1's (with hold code 7) behind the next DTM pair. */
    CHECK(g_ncap == 4u);
    CHECK(dtm_pair_is(0u, g_frame, g_frame + HALF_BYTES));
    CHECK(dtm_pair_is(2u, g_frame2, g_frame2 + HALF_BYTES));
    CHECK(nth_cmd(0x12u, 0u, 1u) != NULL && nth_cmd(0x12u, 0u, 2u) == NULL &&
          nth_cmd(0x12u, 1u, 1u) != NULL && nth_cmd(0x12u, 1u, 2u) == NULL);
    CHECK(never_sent_forbidden());
#endif
}

/* ------------------------------------------------------------- TSC read (MISO) --- */

/* A temperature read on each chip, as the lab does it: TSC (0x40) and two bytes in on SI1, which
 * this board wires to GPIO 8 = aux_pin_1. Nothing here is panel data: it shows what today's
 * escapes can say and what the ESP32 bus refuses. */
static const uint8_t k_tsc_seq[] = {
    0x01, 0x40, 0x00, 0x09, OD_SEQ_PINREF_AUX_1, 0x02,   /* main: TSC, read 2 */
    0x00, 0x08, 0x01,
    0x01, 0x40, 0x00, 0x09, OD_SEQ_PINREF_AUX_1, 0x02,   /* slave: TSC, read 2 */
    0x00, 0x08, 0x00,
    0x00, 0x03,
    0x00, 0x00,
};

static uint8_t g_rd_pin[2], g_rd_cs[2], g_rd_n[2];
static unsigned g_nrd;

static bool bus_read(uint8_t pin, uint8_t *dst, uint8_t n)
{
    if (g_nrd < 2u) {
        g_rd_pin[g_nrd] = pin;
        g_rd_cs[g_nrd] = g_cs;
        g_rd_n[g_nrd] = n;
    }
    g_nrd++;
    memset(dst, 0x5A, n);
    return true;
}

static void provision_tsc(void)
{
    static const uint8_t slots[1] = { 0xFFu };
    const size_t b1 = 4u + (size_t)(k_preset_EP133S[2] | (k_preset_EP133S[3] << 8));

    display_block();
    store_reset();
    store_raw(k_preset_EP133S, b1);
    mode_packet(0u, "tsc", 1u, slots, k_tsc_seq, (uint16_t)sizeof k_tsc_seq);
    store_commit();
}

static void test_tsc_read(void)
{
    od_epd_cfg_mode_t hdr;
    od_epd_cfg_bus_t with_miso = g_bus;

    CASE("TSC read on both chips: expressible via aux_pin_1, refused by a bus without MISO");
    provision_tsc();
    reset_bus(0u);
    CHECK(od_epd_cfg_load(g_disp, &g_bus));
    CHECK(od_epd_cfg_select(0u, &hdr));
    /* ESP32 today: read_bytes is NULL, so READ_SCRATCH is refused before any panel I/O. */
    CHECK(!od_epd_cfg_begin_pass(0u) && od_epd_cfg_last_rc() == OD_EPD_RC_UNSUP);
    CHECK(g_ntok == 0u);

    /* A bus with a read hook: one 2-byte read per chip, sampling GPIO 8, after TSC on that chip. */
    with_miso.read_bytes = bus_read;
    g_nrd = 0u;
    reset_bus(0u);
    CHECK(od_epd_cfg_load(g_disp, &with_miso));
    CHECK(od_epd_cfg_select(0u, &hdr));
    CHECK(od_epd_cfg_begin_pass(0u));
    CHECK(g_nrd == 2u);
    for (unsigned k = 0; k < 2u; k++) {
        const token_t *tsc = nth_cmd(0x40u, (uint8_t)k, 0u);

        CHECK(g_rd_pin[k] == EE02_PIN_MISO && g_rd_cs[k] == k && g_rd_n[k] == 2u);
        CHECK(tsc != NULL && tsc->n == 0u);
    }
    CHECK(never_sent_forbidden());
    od_epd_cfg_sleep();
}

int main(void)
{
    lab_frame();
    host_frame();
    host_frame2();
    test_geometry();
    test_database_preset(false);
    test_database_preset(true);
    test_lab_order();
    test_busy_cap();
    test_multipass();
    test_tsc_read();
    printf("ee02_spectra: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
