/* display_test -- the shared config-driven display path (od_display over od_epd_cfg / od_epd),
 * from a stored config blob to the bytes on a recorded panel bus.
 *
 * The config store is a fake od_hal_nvs holding a real record (store header, payload preamble,
 * length-prefixed drive packets, CRC tail). The panel is a recording od_epd_cfg_bus_t. Real
 * panel data (display_presets.inc) covers BWRY with its 3-pass 16-colour mode, MONO with its
 * fast and partial modes, and BWR planes; synthetic panels cover GRAY4, 4 bpp, split halves over
 * CS_SELECT, WINDOW/FILL, temperature policy and a config without drive packets. */

#include "od_boot_app.h"
#include "od_boot_screen.h"
#include "od_color.h"
#include "od_config.h"
#include "od_config_store.h"
#include "od_display.h"
#include "od_display_app.h"
#include "od_drive_app.h"
#include "od_epd.h"
#include "od_epd_cfg.h"
#include "od_hal_nvs.h"
#include "opendisplay_structs.h"

#include <stdio.h>
#include <string.h>

#include "display_presets.inc"

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
    g_payload_len = 3u;   /* [0x00 0x00][version] */
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

static void store_framed(const uint8_t *framed, size_t len)
{
    memcpy(&g_store[OD_CONFIG_STORE_HEADER_SIZE + g_payload_len], framed, len);
    g_payload_len += (uint32_t)len;
}

static void store_commit(void)
{
    const uint32_t data_len = g_payload_len + 2u;   /* CRC tail, not checked by the index */
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

/* --------------------------------------------------------------- recording bus --- */

enum { OP_CMD = 1, OP_DATA, OP_RESET, OP_CS, OP_PIN };

struct op {
    int      kind;
    uint8_t  a;        /* cmd byte, cs index, pin */
    uint8_t  first;    /* first data byte */
    uint32_t n;        /* data length */
};

#define MAX_OPS 4096u
static struct op g_ops[MAX_OPS];
static unsigned g_nops;
static bool g_ops_overflow;
static uint32_t g_now;
static uint8_t g_cs;

static void rec(int kind, uint8_t a, const uint8_t *d, size_t n)
{
    /* Consecutive data writes under one command are one op: the stream chunks however it likes. */
    if (kind == OP_DATA && g_nops > 0u && g_ops[g_nops - 1u].kind == OP_DATA) {
        g_ops[g_nops - 1u].n += (uint32_t)n;
        return;
    }
    if (g_nops >= MAX_OPS) {
        g_ops_overflow = true;
        return;
    }
    g_ops[g_nops].kind = kind;
    g_ops[g_nops].a = a;
    g_ops[g_nops].first = (d != NULL && n > 0u) ? d[0] : 0u;
    g_ops[g_nops].n = (uint32_t)n;
    g_nops++;
}

static void bus_cmd(uint8_t c)                   { rec(OP_CMD, c, NULL, 0u); }
static void bus_data(const uint8_t *d, size_t n) { rec(OP_DATA, g_cs, d, n); }
static void bus_reset(uint32_t ms)               { g_now += ms * 3u; rec(OP_RESET, 0u, NULL, 0u); }
static int  bus_busy(void)                       { return (int)od_epd_cfg_busy_idle_level(); }
static void bus_delay(uint32_t ms)               { g_now += ms; }
static uint32_t bus_now(void)                    { return g_now; }

static bool bus_pin(uint8_t pin, uint8_t level)
{
    rec(OP_PIN, pin, &level, 1u);
    return true;
}

static bool bus_cs(uint8_t which)
{
    g_cs = which;
    rec(OP_CS, which, NULL, 0u);
    return true;
}

static od_epd_cfg_bus_t g_bus = {
    .cmd = bus_cmd,
    .data = bus_data,
    .reset = bus_reset,
    .busy_read = bus_busy,
    .delay_ms = bus_delay,
    .now_ms = bus_now,
    .pin_write = bus_pin,
    .cs_select = bus_cs,
};

static void ops_reset(void)
{
    g_nops = 0u;
    g_ops_overflow = false;
    g_cs = 0u;
}

/* Data bytes sent right after the nth (0-based) occurrence of `cmd`, or -1 if it never came. */
static long data_after(uint8_t cmd, unsigned nth, uint8_t *first)
{
    unsigned seen = 0u;

    for (unsigned k = 0u; k < g_nops; k++) {
        if (g_ops[k].kind == OP_CMD && g_ops[k].a == cmd && seen++ == nth) {
            if (k + 1u < g_nops && g_ops[k + 1u].kind == OP_DATA) {
                if (first != NULL) {
                    *first = g_ops[k + 1u].first;
                }
                return (long)g_ops[k + 1u].n;
            }
            return 0;
        }
    }
    return -1;
}

static unsigned count_cmd(uint8_t cmd)
{
    unsigned n = 0u;

    for (unsigned k = 0u; k < g_nops; k++) {
        n += (g_ops[k].kind == OP_CMD && g_ops[k].a == cmd) ? 1u : 0u;
    }
    return n;
}

/* ------------------------------------------------------------- target seams --- */

static struct od_config g_cfg;
static struct DisplayConfig *g_disp = &g_cfg.displays[0];
static unsigned g_panel_up;
static unsigned g_panel_down;
static bool g_has_display = true;
static int8_t g_temp = 25;
static od_display_phase_t g_last_phase = OD_DISPLAY_PHASE_IDLE;

const struct DisplayConfig *od_drive_app_display(void) { return g_has_display ? g_disp : NULL; }
const od_epd_cfg_bus_t *od_drive_app_bus(void)         { return &g_bus; }
void od_drive_app_abort(void)                          { od_display_abort(); }
int8_t od_drive_app_temp_c(void)                       { return g_temp; }

bool od_drive_app_panel_up(const struct DisplayConfig *d)
{
    (void)d;
    g_panel_up++;
    return true;
}

void od_drive_app_panel_down(void)
{
    g_panel_down++;
}

bool od_drive_app_adopt(const od_color_geometry_t *frame, uint8_t passes, bool compressed,
                        od_span_t tail)
{
    return od_display_adopt_drive(frame, passes, compressed, tail);
}

void od_display_app_phase(od_display_phase_t phase) { g_last_phase = phase; }

uint32_t od_boot_app_device_id24(void) { return 0x123456u; }
void od_boot_app_firmware_version(uint8_t *a, uint8_t *b, uint8_t *c) { *a = 3; *b = 0; *c = 0; }
float od_boot_app_battery_volts(void) { return 3.0f; }
float od_boot_app_chip_temp_c(void) { return 25.0f; }

/* ------------------------------------------------------------------- builders --- */

static void display(uint16_t w, uint16_t h, uint8_t scheme)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.loaded = true;
    g_cfg.display_count = 1u;
    g_disp->instance_number = 0u;
    g_disp->pixel_width = w;
    g_disp->pixel_height = h;
    g_disp->color_scheme = scheme;
    g_disp->cs_pin = 5u;
    g_disp->cs_pin_2 = 0xFFu;
    g_disp->reset_pin = 6u;
    g_disp->dc_pin = 7u;
    g_disp->busy_pin = 8u;
    g_has_display = true;
    g_temp = 25;
    g_panel_up = 0u;
    g_panel_down = 0u;
    ops_reset();
}

/* 0xB1: BUSY usable, idle high, 1 s timeout, identity code map, `modes` modes, sleep 0x07 A5. */
static void panel_packet(uint8_t modes)
{
    uint8_t b[13 + 3] = { 0u, OD_PANEL_FLAG_BUSY_USABLE, 1u, 0xE8, 0x03, 0u, 0u,
                          0u, 1u, 2u, 3u, 0u, 3u, 0x02, 0x07, 0xA5 };
    b[11] = modes;
    store_packet(OD_PKT_DISPLAY_PANEL, b, sizeof b);
}

static void mode_packet(uint8_t index, const char *name, uint8_t scheme, uint8_t passes,
                        int8_t tmin, int8_t tmax, uint8_t policy,
                        const uint8_t *seq, uint16_t seq_len)
{
    uint8_t b[37 + 512];

    memset(b, 0, sizeof b);
    b[0] = 0u;
    b[1] = index;
    strncpy((char *)&b[2], name, 12);
    b[14] = scheme;
    b[15] = passes;
    b[16] = (uint8_t)tmin;
    b[17] = (uint8_t)tmax;
    b[18] = policy;
    memset(&b[19], 0xFF, 8);
    b[35] = (uint8_t)seq_len;
    b[36] = (uint8_t)(seq_len >> 8);
    memcpy(&b[37], seq, seq_len);
    store_packet(OD_PKT_DISPLAY_MODE, b, (uint16_t)(37u + seq_len));
}

static void fill(uint8_t *buf, uint32_t n, uint8_t v)
{
    memset(buf, v, n);
}

static uint8_t g_frame[65536u];

/* Stream `n` bytes in uneven chunks, the way the transfer plane delivers them. */
static bool stream(const uint8_t *p, uint32_t n)
{
    static const uint32_t chunks[] = { 7u, 244u, 1u, 100u, 33u };
    uint32_t off = 0u;
    unsigned c = 0u;

    while (off < n) {
        uint32_t k = chunks[c++ % 5u];

        if (k > n - off) {
            k = n - off;
        }
        if (!od_display_write(off, p + off, k)) {
            return false;
        }
        off += k;
    }
    return true;
}

static od_color_geometry_t geom(void)
{
    od_color_geometry_t g;

    memset(&g, 0, sizeof g);
    (void)od_color_direct_geometry(g_disp->color_scheme, g_disp->pixel_width,
                                   g_disp->pixel_height, &g);
    return g;
}

/* ---------------------------------------------------------------------- cases --- */

static void test_mono_synthetic(void)
{
    /* SSD1680-like: SW reset, BUSY, RAM 0x24, IMAGE, 0x22 F7, 0x20, BUSY. */
    static const uint8_t seq[] = { 0x01, 0x12, 0x00, 0x01, 0x01, 0x24, 0x00, 0x03,
                                   0x02, 0x22, 0xF7, 0x01, 0x20, 0x00, 0x01, 0x00, 0x00 };
    od_color_geometry_t g;
    uint8_t first = 0u;

    CASE("MONO: one IMAGE takes the whole 1 bpp frame, then the tail and the sleep sequence");
    display(16, 8, OD_COLOR_SCHEME_MONO);
    store_reset();
    panel_packet(1u);
    mode_packet(0u, "full", 0xFFu, 1u, 0, 0, OD_DRIVE_TEMP_ADVISORY, seq, sizeof seq);
    store_commit();
    g = geom();
    CHECK(g.total_bytes == 16u);
    CHECK(!od_display_needs_migration());
    CHECK(od_display_begin_full(&g) == OD_DISPLAY_OK);
    CHECK(g_panel_up == 1u && od_display_active());
    fill(g_frame, 16u, 0x5Au);
    CHECK(stream(g_frame, 16u));
    CHECK(!od_display_write(16u, g_frame, 1u));
    CHECK(od_display_refresh(0u));
    CHECK(!od_display_active() && g_panel_down == 1u);
    CHECK(data_after(0x24, 0, &first) == 16 && first == 0x5Au);
    CHECK(data_after(0x22, 0, &first) == 1 && first == 0xF7u);
    CHECK(count_cmd(0x20) == 1u);
    CHECK(data_after(0x07, 0, &first) == 1 && first == 0xA5u);   /* sleep sequence ran */
    CHECK(!g_ops_overflow);
}

static void two_plane_case(uint8_t scheme, const char *name)
{
    static const uint8_t seq[] = { 0x01, 0x10, 0x00, 0x03, 0x01, 0x13, 0x00, 0x03,
                                   0x01, 0x12, 0x00, 0x01, 0x00, 0x00 };
    static const uint8_t one[] = { 0x01, 0x10, 0x00, 0x03, 0x01, 0x12, 0x00, 0x00 };
    od_color_geometry_t g;
    uint8_t first = 0u;

    CASE(name);
    display(16, 8, scheme);
    store_reset();
    panel_packet(1u);
    mode_packet(0u, "full", 0xFFu, 1u, 0, 0, 0u, seq, sizeof seq);
    store_commit();
    g = geom();
    CHECK(g.layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES && g.total_bytes == 32u);
    CHECK(od_display_begin_full(&g) == OD_DISPLAY_OK);
    fill(g_frame, 16u, 0xA0u);
    fill(g_frame + 16, 16u, 0xB0u);
    CHECK(stream(g_frame, 32u));
    CHECK(od_display_refresh(0u));
    CHECK(data_after(0x10, 0, &first) == 16 && first == 0xA0u);
    CHECK(data_after(0x13, 0, &first) == 16 && first == 0xB0u);

    CASE("a two-plane frame with only one image marker fails at the plane boundary");
    display(16, 8, scheme);
    store_reset();
    panel_packet(1u);
    mode_packet(0u, "full", 0xFFu, 1u, 0, 0, 0u, one, sizeof one);
    store_commit();
    g = geom();
    CHECK(od_display_begin_full(&g) == OD_DISPLAY_OK);
    CHECK(od_display_write(0u, g_frame, 16u));
    CHECK(!od_display_write(16u, g_frame + 16, 16u));
    od_display_abort();
    CHECK(!od_display_active() && g_panel_down == 1u);
}

static void test_packed_single_marker(uint8_t scheme, uint32_t want, const char *name)
{
    static const uint8_t seq[] = { 0x01, 0x10, 0x00, 0x03, 0x01, 0x12, 0x00, 0x01, 0x00, 0x00 };
    od_color_geometry_t g;

    CASE(name);
    display(16, 8, scheme);
    store_reset();
    panel_packet(1u);
    mode_packet(0u, "full", 0xFFu, 1u, 0, 0, 0u, seq, sizeof seq);
    store_commit();
    g = geom();
    CHECK(g.total_bytes == want);
    CHECK(od_display_begin_full(&g) == OD_DISPLAY_OK);
    fill(g_frame, want, 0x12u);
    CHECK(stream(g_frame, want));
    CHECK(od_display_refresh(0u));
    CHECK(data_after(0x10, 0, NULL) == (long)want);
}

static void test_split_halves_cs_select(void)
{
    /* Dual-controller: CS 0 gets half 0, CS 1 gets half 1, both refresh. */
    static const uint8_t seq[] = {
        0x01, 0x10, 0x00, 0x07, 0x00, 0x02,
        0x00, 0x08, 0x01,
        0x01, 0x10, 0x00, 0x07, 0x01, 0x02,
        0x01, 0x12, 0x00, 0x01, 0x00, 0x00,
    };
    od_color_geometry_t g;
    unsigned k;
    long cs1_bytes = -1;

    CASE("BWGBRY_SPLIT: IMAGE_SLICE 0/2 and 1/2 land on cs_pin and cs_pin_2");
    display(16, 8, OD_COLOR_SCHEME_BWGBRY_SPLIT);
    g_disp->cs_pin_2 = 9u;
    store_reset();
    panel_packet(1u);
    mode_packet(0u, "full", 0xFFu, 1u, 0, 0, 0u, seq, sizeof seq);
    store_commit();
    g = geom();
    CHECK(g.layout == OD_COLOR_LAYOUT_SPLIT_HALVES && g.total_bytes == 64u);
    CHECK(od_display_begin_full(&g) == OD_DISPLAY_OK);
    fill(g_frame, 32u, 0x11u);
    fill(g_frame + 32, 32u, 0x22u);
    CHECK(stream(g_frame, 64u));
    CHECK(od_display_refresh(0u));
    for (k = 0u; k < g_nops; k++) {
        if (g_ops[k].kind == OP_DATA && g_ops[k].a == 1u && g_ops[k].first == 0x22u) {
            cs1_bytes = (long)g_ops[k].n;
        }
    }
    CHECK(data_after(0x10, 0, NULL) == 32);
    CHECK(cs1_bytes == 32);

    CASE("CS_SELECT on a board without cs_pin_2 is refused before the panel is touched");
    display(16, 8, OD_COLOR_SCHEME_BWGBRY_SPLIT);
    store_reset();
    panel_packet(1u);
    mode_packet(0u, "full", 0xFFu, 1u, 0, 0, 0u, seq, sizeof seq);
    store_commit();
    g = geom();
    CHECK(od_display_begin_full(&g) == OD_DISPLAY_SEQ_UNSUPPORTED);
    CHECK(!od_display_active() && g_panel_down == g_panel_up);
    CHECK(count_cmd(0x10) == 0u);
}

static void load_preset(const uint8_t *framed, size_t len)
{
    store_reset();
    store_framed(framed, len);
    store_commit();
}

static void test_bwry_real_and_colour16(void)
{
    od_color_geometry_t g;
    od_epd_cfg_mode_t hdr;
    uint8_t mode = 1u;
    uint8_t map[4];
    unsigned refreshes;

    CASE("EP29YR (glass): mode 0 streams the 2 bpp frame through the real sequence");
    display(168, 384, OD_COLOR_SCHEME_BWRY);
    load_preset(k_preset_EP29YR, sizeof k_preset_EP29YR);
    g = geom();
    CHECK(g.total_bytes == 16128u);
    CHECK(od_display_begin_full(&g) == OD_DISPLAY_OK);
    fill(g_frame, 16128u, 0x55u);
    CHECK(stream(g_frame, 16128u));
    CHECK(od_display_refresh(0u));
    CHECK(g_panel_down == 1u);
    CHECK(od_epd_cfg_code_map(map));

    CASE("EP29YR colour16: three passes, one refresh each, through the drive seam");
    display(168, 384, OD_COLOR_SCHEME_BWRY);
    load_preset(k_preset_EP29YR, sizeof k_preset_EP29YR);
    CHECK(od_epd_cfg_load(g_disp, &g_bus));
    CHECK(od_epd_cfg_select_policy(&mode, g_temp, &hdr) == 0u);
    CHECK(hdr.passes == 3u);
    CHECK(od_drive_app_panel_up(g_disp));
    CHECK(od_epd_cfg_begin_pass(0u));
    g = geom();
    CHECK(od_drive_app_adopt(&g, hdr.passes, false, od_span_make(NULL, 0u)));
    fill(g_frame, 3u * 16128u, 0x1Bu);
    ops_reset();
    CHECK(stream(g_frame, 3u * 16128u));
    refreshes = count_cmd(0x12);
    CHECK(od_display_refresh(0u));
    CHECK(count_cmd(0x12) == refreshes + 1u && refreshes == 2u);
    CHECK(!od_display_active());

    CASE("after a drive session an ordinary upload opens mode 0 again");
    CHECK(od_display_begin_full(&g) == OD_DISPLAY_OK);
    od_display_abort();
}

static void test_mono_real_fast_and_partial(void)
{
    od_color_geometry_t g;
    uint8_t first = 0u;
    const uint32_t plane = 400u / 8u * 300u;

    CASE("EP42B full: 0x22 F7 tail");
    display(400, 300, OD_COLOR_SCHEME_MONO);
    load_preset(k_preset_EP42B, sizeof k_preset_EP42B);
    g = geom();
    CHECK(od_display_begin_full(&g) == OD_DISPLAY_OK);
    fill(g_frame, plane, 0xFFu);
    CHECK(stream(g_frame, plane));
    CHECK(od_display_refresh(0u));
    CHECK(data_after(0x22, 0, &first) == 1 && first == 0xF7u);
    CHECK(count_cmd(0x22) == 2u);                         /* the refresh, then sleep's 0x22 83 */
    CHECK(data_after(0x22, 1, &first) == 1 && first == 0x83u);

    CASE("EP42B refresh byte FAST runs the 'fast' mode's tail after mode 0's head");
    display(400, 300, OD_COLOR_SCHEME_MONO);
    load_preset(k_preset_EP42B, sizeof k_preset_EP42B);
    g = geom();
    CHECK(od_display_begin_full(&g) == OD_DISPLAY_OK);
    CHECK(stream(g_frame, plane));
    CHECK(od_display_refresh(1u));
    CHECK(count_cmd(0x22) >= 1u);
    {
        long n = -1;
        unsigned k = count_cmd(0x22);
        CHECK(k > 1u);
        n = data_after(0x22, k - 2u, &first);             /* last before sleep's 0x22 83 */
        CHECK(n == 1 && first == 0xC7u);
    }
    CHECK(data_after(0x22, 0, &first) == 1 && first != 0xF7u);

    CASE("EP42B partial: OLD plane to 0x26, NEW plane to 0x24 (IMAGE_SLICE 0/2, 1/2)");
    display(400, 300, OD_COLOR_SCHEME_MONO);
    load_preset(k_preset_EP42B, sizeof k_preset_EP42B);
    CHECK(od_display_partial_available());
    CHECK(od_display_begin_partial(0u, 0u, 400u, 300u, plane) == OD_DISPLAY_OK);
    fill(g_frame, plane, 0x0Fu);
    fill(g_frame + plane, plane, 0xF0u);
    CHECK(stream(g_frame, 2u * plane));
    CHECK(od_display_refresh(0u));
    CHECK(data_after(0x26, 0, &first) == (long)plane && first == 0x0Fu);
    CHECK(data_after(0x24, 0, &first) == (long)plane && first == 0xF0u);
    CHECK(data_after(0x22, 0, &first) == 1 && first == 0xFFu);

    CASE("a rectangle smaller than the panel needs a sequence with WINDOW");
    display(400, 300, OD_COLOR_SCHEME_MONO);
    load_preset(k_preset_EP42B, sizeof k_preset_EP42B);
    CHECK(od_display_begin_partial(8u, 0u, 16u, 8u, 16u) == OD_DISPLAY_SEQ_UNSUPPORTED);
    CHECK(!od_display_active() && g_panel_down == g_panel_up);
    CHECK(count_cmd(0x24) == 0u && count_cmd(0x26) == 0u);
}

static void test_bwr_real(void)
{
    od_color_geometry_t g;
    const uint32_t plane = 640u / 8u * 384u;

    CASE("EP74R (BWR planes): plane 0 after the second 0x13, plane 1 after 0x10 (bb_epaper order)");
    display(640, 384, OD_COLOR_SCHEME_BWR);
    load_preset(k_preset_EP74R, sizeof k_preset_EP74R);
    g = geom();
    CHECK(g.total_bytes == 2u * plane);
    CHECK(od_display_begin_full(&g) == OD_DISPLAY_OK);
    fill(g_frame, plane, 0x01u);
    fill(g_frame + plane, plane, 0x02u);
    CHECK(stream(g_frame, 2u * plane));
    CHECK(od_display_refresh(0u));
    {
        uint8_t first = 0u;
        CHECK(data_after(0x13, 1, &first) == (long)plane && first == 0x01u);
        CHECK(data_after(0x10, 0, &first) == (long)plane && first == 0x02u);
    }
}

static void test_window_fill_partial(void)
{
    /* SSD16xx partial with the window placed by the sequence and both RAM planes filled first:
     * FILL 0x26 FF, FILL 0x24 FF, WINDOW 0x44 [x0/8][x1/8], WINDOW 0x45 [y0 lo][y0 hi][y1 lo]
     * [y1 hi], 0x26 IMAGE_SLICE 0/2, WINDOW 0x44 again, 0x24 IMAGE_SLICE 1/2, 0x22 FF, 0x20. */
    static const uint8_t seq[] = {
        0x00, 0x0D, 0x26, 0xFF, 0x00,
        0x00, 0x0D, 0x24, 0xFF, 0x00,
        0x00, 0x0C, 0x03, 0x00, 0x44, 0x01, 0x08, 0x01, 0x09,
        0x00, 0x0C, 0x05, 0x00, 0x45, 0x01, 0x02, 0x01, 0x22, 0x01, 0x03, 0x01, 0x23,
        0x01, 0x26, 0x00, 0x07, 0x00, 0x02,
        0x00, 0x0C, 0x03, 0x00, 0x44, 0x01, 0x08, 0x01, 0x09,
        0x01, 0x24, 0x00, 0x07, 0x01, 0x02,
        0x02, 0x22, 0xFF, 0x01, 0x20, 0x00, 0x01, 0x00, 0x00,
    };
    static const uint8_t full[] = { 0x01, 0x24, 0x00, 0x03, 0x01, 0x20, 0x00, 0x00 };
    uint8_t first = 0u;
    unsigned k;
    bool saw_window = false;

    CASE("WINDOW/FILL: planes filled whole-panel, rectangle bytes after the window");
    display(64, 32, OD_COLOR_SCHEME_MONO);
    store_reset();
    panel_packet(2u);
    mode_packet(0u, "full", 0xFFu, 1u, 0, 0, 0u, full, sizeof full);
    mode_packet(1u, "Partial", 0xFFu, 1u, 0, 0, 0u, seq, sizeof seq);
    store_commit();
    CHECK(od_display_partial_available());
    /* x 16..31, y 4..11: 2 bytes x 8 rows = 16 B per plane. */
    CHECK(od_display_begin_partial(16u, 4u, 16u, 8u, 16u) == OD_DISPLAY_OK);
    fill(g_frame, 16u, 0x3Cu);
    fill(g_frame + 16, 16u, 0xC3u);
    CHECK(stream(g_frame, 32u));
    CHECK(od_display_refresh(0u));
    CHECK(data_after(0x26, 0, &first) == 64 / 8 * 32 && first == 0xFFu);   /* FILL */
    CHECK(data_after(0x24, 0, &first) == 64 / 8 * 32 && first == 0xFFu);
    CHECK(data_after(0x26, 1, &first) == 16 && first == 0x3Cu);
    CHECK(data_after(0x24, 1, &first) == 16 && first == 0xC3u);
    for (k = 0u; k + 1u < g_nops; k++) {
        if (g_ops[k].kind == OP_CMD && g_ops[k].a == 0x44u) {
            saw_window = g_ops[k + 1u].kind == OP_DATA && g_ops[k + 1u].n == 2u &&
                         g_ops[k + 1u].first == 16u / 8u;
            break;
        }
    }
    CHECK(saw_window);
    CHECK(data_after(0x45, 0, &first) == 4 && first == 4u);

    CASE("mode names match case-insensitively; a FULL session has no partial tail");
    CHECK(od_epd_cfg_find_mode("partial") == 1);
}

static void test_needs_migration(void)
{
    od_color_geometry_t g;
    uint8_t row[680];
    uint8_t qr[256];
    const struct od_boot_bufs bufs = { row, sizeof row, qr, sizeof qr };
    struct SecurityConfig sec;

    memset(&sec, 0, sizeof sec);

    CASE("no stored record at all: needs migration, nothing driven");
    display(16, 8, OD_COLOR_SCHEME_MONO);
    store_reset();
    CHECK(od_display_needs_migration());
    g = geom();
    CHECK(od_display_begin_full(&g) == OD_DISPLAY_NEEDS_MIGRATION);
    CHECK(od_display_boot(&g_cfg, &sec, &bufs) == OD_DISPLAY_NEEDS_MIGRATION);
    CHECK(g_panel_up == 0u && g_nops == 0u);
    CHECK(!od_display_partial_available());

    CASE("a stored legacy config without 0xAE/0xB1: needs migration");
    store_reset();
    store_commit();
    CHECK(od_display_needs_migration());
    CHECK(od_display_begin_partial(0u, 0u, 16u, 8u, 16u) == OD_DISPLAY_NEEDS_MIGRATION);
    CHECK(g_panel_up == 0u && g_nops == 0u);

    CASE("a panel packet with no mode is still not drivable");
    store_reset();
    panel_packet(0u);
    store_commit();
    CHECK(od_display_needs_migration());
    CHECK(od_display_begin_full(&g) == OD_DISPLAY_NEEDS_MIGRATION);

    CASE("no display at all is NO_DISPLAY, not needs-migration");
    g_has_display = false;
    CHECK(!od_display_needs_migration());
    CHECK(od_display_begin_full(&g) == OD_DISPLAY_NO_DISPLAY);
    CHECK(g_panel_up == 0u);
}

static void test_temperature(void)
{
    static const uint8_t seq[] = { 0x01, 0x24, 0x00, 0x03, 0x01, 0x20, 0x00, 0x00 };
    od_color_geometry_t g;
    uint8_t row[680];
    uint8_t qr[256];
    const struct od_boot_bufs bufs = { row, sizeof row, qr, sizeof qr };
    struct SecurityConfig sec;

    memset(&sec, 0, sizeof sec);
    CASE("temp_policy REJECT out of window: refused before power-up");
    display(128, 64, OD_COLOR_SCHEME_MONO);
    store_reset();
    panel_packet(1u);
    mode_packet(0u, "full", 0xFFu, 1u, 0, 40, OD_DRIVE_TEMP_REJECT, seq, sizeof seq);
    store_commit();
    g_temp = 50;
    g = geom();
    CHECK(od_display_begin_full(&g) == OD_DISPLAY_TEMP_RANGE);
    CHECK(g_panel_up == 0u && g_nops == 0u);

    CASE("the boot screen treats REJECT as advisory");
    CHECK(od_display_boot(&g_cfg, &sec, &bufs) == OD_DISPLAY_OK);
    CHECK(data_after(0x24, 0, NULL) == 128 / 8 * 64);

    CASE("an unreadable sensor skips the policy");
    display(128, 64, OD_COLOR_SCHEME_MONO);
    g_temp = OD_EPD_CFG_TEMP_UNKNOWN;
    CHECK(od_display_begin_full(&g) == OD_DISPLAY_OK);
    od_display_abort();
}

static void boot_case(uint16_t w, uint16_t h, uint8_t scheme, const uint8_t *seq,
                      uint16_t seq_len, uint8_t cmd0, uint8_t cmd1, const char *name)
{
    od_color_geometry_t g;
    uint8_t row[680];
    uint8_t qr[256];
    const struct od_boot_bufs bufs = { row, sizeof row, qr, sizeof qr };
    struct SecurityConfig sec;

    memset(&sec, 0, sizeof sec);
    CASE(name);
    display(w, h, scheme);
    store_reset();
    panel_packet(1u);
    mode_packet(0u, "full", 0xFFu, 1u, 0, 0, 0u, seq, seq_len);
    store_commit();
    g = geom();
    CHECK(od_display_boot(&g_cfg, &sec, &bufs) == OD_DISPLAY_OK);
    CHECK(!od_display_active() && g_panel_down == 1u);
    if (cmd1 == 0u) {
        CHECK(data_after(cmd0, 0, NULL) == (long)g.total_bytes);
    } else {
        CHECK(data_after(cmd0, 0, NULL) == (long)g.part_bytes[0]);
        CHECK(data_after(cmd1, 0, NULL) == (long)g.part_bytes[1]);
    }
    CHECK(!g_ops_overflow);
}

static void test_boot_screens(void)
{
    static const uint8_t one[] = { 0x01, 0x10, 0x00, 0x03, 0x01, 0x12, 0x00, 0x01, 0x00, 0x00 };
    static const uint8_t two[] = { 0x01, 0x10, 0x00, 0x03, 0x01, 0x13, 0x00, 0x03,
                                   0x01, 0x12, 0x00, 0x01, 0x00, 0x00 };

    boot_case(296, 128, OD_COLOR_SCHEME_MONO, one, sizeof one, 0x10, 0, "boot MONO: whole frame");
    boot_case(296, 128, OD_COLOR_SCHEME_BWR, two, sizeof two, 0x10, 0x13, "boot BWR: two planes");
    boot_case(296, 128, OD_COLOR_SCHEME_GRAY4, two, sizeof two, 0x10, 0x13,
              "boot GRAY4: two 1 bpp planes");
    boot_case(168, 384, OD_COLOR_SCHEME_BWRY, one, sizeof one, 0x10, 0, "boot BWRY: 2 bpp");
    boot_case(400, 480, OD_COLOR_SCHEME_BWGBRY, one, sizeof one, 0x10, 0, "boot BWGBRY: 4 bpp");
    boot_case(600, 448, OD_COLOR_SCHEME_SEVEN_COLOR, one, sizeof one, 0x10, 0,
              "boot SEVEN_COLOR: 4 bpp");
}

int main(void)
{
    test_mono_synthetic();
    two_plane_case(OD_COLOR_SCHEME_BWR, "BWR synthetic: IMAGE, IMAGE take one plane each");
    two_plane_case(OD_COLOR_SCHEME_GRAY4, "GRAY4 synthetic: two 1 bpp controller planes");
    test_packed_single_marker(OD_COLOR_SCHEME_BWRY, 32u, "BWRY synthetic: 2 bpp, one marker");
    test_packed_single_marker(OD_COLOR_SCHEME_BWGBRY, 64u, "BWGBRY synthetic: 4 bpp, one marker");
    test_split_halves_cs_select();
    test_bwry_real_and_colour16();
    test_mono_real_fast_and_partial();
    test_bwr_real();
    test_window_fill_partial();
    test_needs_migration();
    test_temperature();
    test_boot_screens();
    printf("display: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
