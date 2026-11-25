/* epd_cfg: od_epd_cfg + od_epd over real stored configs, asserting the exact bus stream.
 *
 * Part 1 -- GOLDEN TRACES. For every representative panel (tests/vectors/drive_panels.inc) and
 * every declared mode: load, select, stream every pass of a deterministic frame, refresh,
 * sleep. The recorded bus (reset / command / data / delay, see drive_fixture.h) is compared to
 * tests/vectors/epd_golden/<panel>.trace. A change to od_epd, od_epd_cfg, od_cfg_index or the
 * panel data that alters a single byte on the wire fails here with the first differing line.
 * Regenerate deliberately with  OD_GOLDEN_UPDATE=1 ./od_epd_cfg_test  and review the diff.
 *
 * Part 2 -- the config-to-driver binding's edges: two displays, duplicates, mode identity,
 * LUT CRC, temperature policy boundaries, malformed sequences (refused before any I/O), the
 * sleep sequence, raw BUSY.
 */
#include "od_check.h"
#include "drive_fixture.h"

#include "od_cfg_index.h"
#include "od_color.h"
#include "od_config_store.h"
#include "od_epd.h"
#include "od_epd_cfg.h"
#include "od_epd_lut.h"
#include "opendisplay_protocol.h"

#include "lut_vectors.inc"

#ifndef OD_GOLDEN_DIR
#error "OD_GOLDEN_DIR must name tests/vectors/epd_golden"
#endif

static uint8_t g_frame[800u * 480u / 2u];

/* ---------------------------------------------------------------------------- golden traces -- */

static bool run_mode(const struct DisplayConfig *d, uint8_t mode, char *hdr, size_t hdr_cap)
{
    od_epd_cfg_mode_t m;
    od_color_geometry_t g;
    od_epd_cfg_stream_t st;
    uint8_t sel = mode;

    if (od_epd_cfg_select_policy(&sel, OD_EPD_CFG_TEMP_UNKNOWN, &m) != 0u) {
        snprintf(hdr, hdr_cap, "# mode %u: select refused", (unsigned)mode);
        fx_trace_line(hdr);
        return false;
    }
    if (od_color_direct_geometry(m.color_scheme == 0xFFu ? d->color_scheme : m.color_scheme,
                                 d->pixel_width, d->pixel_height, &g) != OD_COLOR_OK ||
        g.total_bytes > sizeof g_frame) {
        snprintf(hdr, hdr_cap, "# mode %u: no geometry", (unsigned)mode);
        fx_trace_line(hdr);
        return false;
    }
    snprintf(hdr, hdr_cap, "# mode %u passes %u scheme %u frame %u", (unsigned)mode,
             (unsigned)m.passes, (unsigned)m.color_scheme, (unsigned)g.total_bytes);
    fx_trace_line(hdr);
    if (!od_epd_cfg_begin_pass(0u)) {
        snprintf(hdr, hdr_cap, "# begin_pass rc %d", od_epd_cfg_last_rc());
        fx_trace_line(hdr);
        return false;
    }
    if (!od_epd_cfg_stream_init(&st, &g, m.passes, false, NULL, 0u)) {
        fx_trace_line("# stream_init refused");
        return false;
    }
    for (uint8_t p = 0; p < m.passes; p++) {
        for (uint32_t i = 0; i < g.total_bytes; i++) {
            g_frame[i] = (uint8_t)(i * 31u + p * 7u + mode);
        }
        /* Two writes per frame, split off a page boundary, as DATA chunks arrive. */
        const uint32_t cut = g.total_bytes / 3u;
        if (!od_epd_cfg_stream_write(&st, (uint32_t)p * g.total_bytes, g_frame, cut, NULL) ||
            !od_epd_cfg_stream_write(&st, (uint32_t)p * g.total_bytes + cut, g_frame + cut,
                                     g.total_bytes - cut, NULL)) {
            fx_trace_line("# stream_write refused");
            return false;
        }
    }
    if (!od_epd_cfg_finish()) {
        fx_trace_line("# finish refused");
        return false;
    }
    return true;
}

static char *read_file(const char *path, size_t *n_out)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1u);
    if (buf == NULL || fread(buf, 1u, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    *n_out = (size_t)n;
    return buf;
}

static void report_first_diff(const char *want, const char *got)
{
    unsigned line = 1u;
    const char *w = want, *g = got;

    while (*w != '\0' && *w == *g) {
        if (*w == '\n') {
            line++;
        }
        w++;
        g++;
    }
    const char *ws = w, *gs = g;
    while (ws > want && ws[-1] != '\n') { ws--; }
    while (gs > got && gs[-1] != '\n') { gs--; }
    printf("  first difference at line %u\n    want: %.*s\n    got:  %.*s\n", line,
           (int)strcspn(ws, "\n"), ws, (int)strcspn(gs, "\n"), gs);
}

/* SET_PIN for the golden bus: recorded like every other bus call. */
static bool fx_pin(uint8_t pin, uint8_t level)
{
    char line[24];
    snprintf(line, sizeof line, "P %02x %u", pin, (unsigned)level);
    fx_trace_line(line);
    return true;
}

static void test_golden(void)
{
    od_epd_cfg_bus_t bus = FX_BUS;
    bus.pin_write = fx_pin;

    const bool update = getenv("OD_GOLDEN_UPDATE") != NULL;

    for (unsigned p = 0; p < FX_PANEL_COUNT; p++) {
        const fx_panel_t *pan = &FX_PANELS[p];
        char path[512], hdr[96];
        size_t n = 0u;

        CASE(pan->name);
        const struct DisplayConfig d = fx_store_panel(pan);
        fx_bus_clear();
        fx_busy_level = 1;
        fx_clock_ms = 0u;
        if (!od_epd_cfg_load(&d, &bus)) {
            fx_trace_line("# load refused");
        } else {
            /* A BUSY pin that always reads the panel's own idle level. */
            fx_busy_level = (int)od_epd_cfg_busy_idle_level();
            uint8_t map[4];
            CHECK(od_epd_cfg_code_map(map));
            snprintf(hdr, sizeof hdr, "# %s: %u modes, code_map %02x %02x %02x %02x, idle %u",
                     pan->name, (unsigned)od_epd_cfg_mode_count(), map[0], map[1], map[2],
                     map[3], (unsigned)od_epd_cfg_busy_idle_level());
            fx_trace_line(hdr);
            for (uint8_t m = 0; m < od_epd_cfg_mode_count(); m++) {
                (void)run_mode(&d, m, hdr, sizeof hdr);
            }
            fx_trace_line("# sleep");
            od_epd_cfg_sleep();
        }
        CHECK(!fx_trace_overflow);
        snprintf(path, sizeof path, "%s/%s.trace", OD_GOLDEN_DIR, pan->name);
        if (update) {
            FILE *f = fopen(path, "wb");
            CHECK(f != NULL);
            if (f != NULL) {
                fwrite(fx_trace, 1u, fx_trace_n, f);
                fclose(f);
                printf("  wrote %s (%u B)\n", path, (unsigned)fx_trace_n);
            }
            continue;
        }
        char *want = read_file(path, &n);
        CHECK(want != NULL);
        if (want == NULL) {
            printf("  missing golden %s (run with OD_GOLDEN_UPDATE=1)\n", path);
            continue;
        }
        const bool same = n == fx_trace_n && memcmp(want, fx_trace, n) == 0;
        CHECK(same);
        if (!same) {
            report_first_diff(want, fx_trace);
        }
        /* Every declared mode must actually drive: no refusal lines in the golden itself. */
        CHECK(strstr(fx_trace, "refused") == NULL);
        free(want);
    }
}

/* ----------------------------------------------------------------------- synthetic packets -- */

/* An 0xAE body: 37-byte head + seq. */
static uint32_t mode_body(uint8_t *out, uint8_t inst, uint8_t index, const char *name,
                          uint8_t passes, int8_t tmin, int8_t tmax, uint8_t policy,
                          uint8_t lut0, const uint8_t *seq, uint16_t seq_len)
{
    memset(out, 0, 37u);
    out[0] = inst;
    out[1] = index;
    strncpy((char *)&out[2], name, 12u);
    out[14] = 0xFFu;
    out[15] = passes;
    out[16] = (uint8_t)tmin;
    out[17] = (uint8_t)tmax;
    out[18] = policy;
    memset(&out[19], 0xFF, 8u);
    out[19] = lut0;
    out[35] = (uint8_t)seq_len;
    out[36] = (uint8_t)(seq_len >> 8);
    memcpy(&out[37], seq, seq_len);
    return 37u + seq_len;
}

/* An 0xB1 body: usable BUSY idle high, 1 s timeout, code map 0 1 3 2, optional sleep seq. */
static uint32_t panel_body(uint8_t *out, uint8_t inst, const uint8_t *sleep, uint8_t sleep_len)
{
    static const uint8_t head[13] = { 0, 0x01, 0x01, 0xE8, 0x03, 0x00, 0x00,
                                      0x00, 0x01, 0x03, 0x02, 0x01, 0x00 };
    memcpy(out, head, sizeof head);
    out[0] = inst;
    out[12] = sleep_len;
    if (sleep_len > 0u) {
        memcpy(&out[13], sleep, sleep_len);
    }
    return 13u + sleep_len;
}

static uint16_t lut_crc(const uint8_t *d, uint32_t n)
{
    uint16_t crc = 0xFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)d[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint32_t lut_body(uint8_t *out, uint8_t inst, uint8_t slot, uint8_t fill, uint16_t n)
{
    out[0] = inst;
    out[1] = slot;
    for (uint16_t i = 0; i < n; i++) {
        out[4u + i] = (uint8_t)(fill + i);
    }
    const uint16_t c = lut_crc(&out[4], n);
    out[2] = (uint8_t)c;
    out[3] = (uint8_t)(c >> 8);
    return 4u + n;
}

/* [1][0x12]  [1][0x20] [0 4] LUT  [0 3] IMAGE  [2][0x22][0xF7] [0 1] BUSY  [0 0] */
static const uint8_t SEQ_PLAIN[] = { 1, 0x12, 1, 0x20, 0, 4, 0, 3, 2, 0x22, 0xF7, 0, 1, 0, 0 };
static const uint8_t SEQ_NOLUT[] = { 1, 0x12, 0, 3, 2, 0x22, 0xF7, 0, 1, 0, 0 };

static struct DisplayConfig g_d0, g_d1;
static uint8_t g_b[1200];

static bool load(const struct DisplayConfig *d)
{
    fx_bus_clear();
    fx_busy_level = 1;
    return od_epd_cfg_load(d, &FX_BUS);
}

/* Stores [legacy][0x20 x ndisp] + the caller's packets via the builder `c`. */
static void begin_cfg(fx_cfg_t *c, unsigned ndisp)
{
    g_d0 = fx_display(0u, 16u, 4u, 0u);
    g_d1 = fx_display(1u, 16u, 4u, 0u);
    fx_cfg_begin(c, 1u);
    fx_cfg_legacy_head(c, &g_d0);
    if (ndisp > 1u) {
        fx_cfg_legacy(c, 0x20u, &g_d1, sizeof g_d1);
    }
}

static void add_panel(fx_cfg_t *c, uint8_t inst)
{
    fx_cfg_ext(c, OD_PKT_DISPLAY_PANEL, g_b, panel_body(g_b, inst, NULL, 0u));
}

static void add_mode(fx_cfg_t *c, uint8_t inst, uint8_t index, const char *name, int8_t tmin,
                     int8_t tmax, uint8_t policy, uint8_t lut0, const uint8_t *seq,
                     uint16_t seq_len)
{
    fx_cfg_ext(c, OD_PKT_DISPLAY_MODE, g_b,
               mode_body(g_b, inst, index, name, 1u, tmin, tmax, policy, lut0, seq, seq_len));
}

/* Run pass 0 of the selected mode with a 8-byte frame; returns begin_pass's verdict. */
static bool pass0(void)
{
    static const uint8_t px[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    if (!od_epd_cfg_begin_pass(0u)) {
        return false;
    }
    (void)od_epd_cfg_write(px, sizeof px);
    return od_epd_cfg_finish();
}

static void test_two_displays(void)
{
    fx_cfg_t c;

    CASE("LUT-INSTANCE-BLIND: display 1 gets its own waveform for the same slot number");
    begin_cfg(&c, 2u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0u, SEQ_PLAIN, sizeof SEQ_PLAIN);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_LUT, g_b, lut_body(g_b, 0u, 0u, 0x10u, 40u));
    add_panel(&c, 1u);
    add_mode(&c, 1u, 0u, "full", 0, 0, 0, 0u, SEQ_PLAIN, sizeof SEQ_PLAIN);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_LUT, g_b, lut_body(g_b, 1u, 0u, 0xA0u, 40u));
    fx_cfg_finish(&c);
    fx_cfg_store(&c);

    CHECK(load(&g_d1));
    CHECK(od_epd_cfg_mode_count() == 1u);
    CHECK(od_epd_cfg_select(0u, NULL));
    CHECK(pass0());
    /* The waveform goes out after the 0x20 command as one 40-byte data block. */
    const char *at = strstr(fx_trace, "C 20\nD ");
    CHECK(at != NULL);
    /* 0xAF.instance_number is "display instance this slot belongs to" in the contract;
     * find_lut() matches it with lut_slot, so display 1 streams its own waveform. */
    CHECK(at != NULL && strncmp(at + 7, "a0 a1", 5) == 0);

    CASE("display 0 in the same config is unaffected");
    CHECK(load(&g_d0));
    CHECK(od_epd_cfg_select(0u, NULL) && pass0());
    at = strstr(fx_trace, "C 20\nD ");
    CHECK(at != NULL && strncmp(at + 7, "10 11", 5) == 0);

    CASE("a display with no drive packets of its own does not borrow the other's");
    begin_cfg(&c, 2u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(!load(&g_d1));
    CHECK(!od_epd_cfg_has_drive_data(&g_d1));
    CHECK(od_epd_cfg_has_drive_data(&g_d0));
}

static void test_duplicates_and_identity(void)
{
    fx_cfg_t c;
    uint8_t sleep_a[] = { 1, 0xA1, 0, 0 };
    uint8_t sleep_b[] = { 1, 0xB2, 0, 0 };

    CASE("two 0xB1 for one display: the FIRST wins, as in od_cfg_find, the 0xB2 table and BG22");
    begin_cfg(&c, 1u);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, g_b, panel_body(g_b, 0u, sleep_a, sizeof sleep_a));
    fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, g_b, panel_body(g_b, 0u, sleep_b, sizeof sleep_b));
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0));
    fx_bus_clear();
    od_epd_cfg_sleep();
    CHECK(strstr(fx_trace, "C a1") != NULL);
    CHECK(strstr(fx_trace, "C b2") == NULL);

    CASE("a short first 0xB1 is not rescued by a valid second one");
    begin_cfg(&c, 1u);
    (void)panel_body(g_b, 0u, NULL, 0u);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, g_b, 5u);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, g_b, panel_body(g_b, 0u, sleep_b, sizeof sleep_b));
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(!od_epd_cfg_has_drive_data(&g_d0));
    CHECK(!load(&g_d0));

    CASE("mode_index is normative: packet order does not number the modes");
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 1u, "colour16", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0));
    CHECK(od_epd_cfg_find_mode("full") == 0);
    CHECK(od_epd_cfg_find_mode("colour16") == 1);
    {
        od_epd_cfg_mode_t m;
        uint8_t mode = 0u;
        CHECK(od_epd_cfg_select_policy(&mode, OD_EPD_CFG_TEMP_UNKNOWN, &m) == 0u && mode == 0u);
    }

    CASE("mode names: case-insensitive, NUL-padded, 12 bytes");
    CHECK(od_epd_cfg_find_mode("FULL") == 0);
    CHECK(od_epd_cfg_find_mode("COLOUR16") == 1);
    CHECK(od_epd_cfg_find_mode("colour") == -1);
    CHECK(od_epd_cfg_find_mode("") == -1);

    CASE("sparse mode indices: a gap is BAD_MODE, the index past it still selects");
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 5u, "fast", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0));
    CHECK(od_epd_cfg_select(5u, NULL));
    CHECK(!od_epd_cfg_select(3u, NULL));
    CHECK(!od_epd_cfg_select(6u, NULL));
    CHECK(od_epd_cfg_find_mode("fast") == 5);

    CASE("two 0xAE with one mode_index: the first wins, the shadowed one has no name");
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    add_mode(&c, 0u, 0u, "fast", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0));
    CHECK(od_epd_cfg_find_mode("full") == 0);
    CHECK(od_epd_cfg_find_mode("fast") == -1);
}

static void add_mode_fmt(fx_cfg_t *c, uint8_t index, uint8_t scheme, uint8_t passes)
{
    const uint32_t n = mode_body(g_b, 0u, index, "full", passes, 0, 0, 0, 0xFFu, SEQ_NOLUT,
                                 sizeof SEQ_NOLUT);
    g_b[14] = scheme;
    fx_cfg_ext(c, OD_PKT_DISPLAY_MODE, g_b, n);
}

static void test_mode0_invariant(void)
{
    fx_cfg_t c;

    CASE("mode 0 in the 0x20 scheme (or 0xFF), single pass: drivable by a legacy upload");
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode_fmt(&c, 0u, (uint8_t)g_d0.color_scheme, 1u);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_epd_cfg_has_drive_data(&g_d0));
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode_fmt(&c, 0u, 0xFFu, 1u);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_epd_cfg_has_drive_data(&g_d0));

    CASE("mode 0 in another scheme: flagged as needing migration");
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode_fmt(&c, 0u, (uint8_t)(g_d0.color_scheme + 1u), 1u);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(!od_epd_cfg_has_drive_data(&g_d0));
    CHECK(load(&g_d0));     /* still loadable: 0x0085 may name it */

    CASE("a multi-pass mode 0: flagged");
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode_fmt(&c, 0u, 0xFFu, 3u);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(!od_epd_cfg_has_drive_data(&g_d0));

    CASE("no mode 0 at all (only index 1): flagged");
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 1u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(!od_epd_cfg_has_drive_data(&g_d0));

    CASE("named modes: absent, or out of their REJECT/SUBSTITUTE window, are not usable");
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    add_mode(&c, 0u, 1u, "fast", 10, 30, OD_DRIVE_TEMP_REJECT, 0xFFu, SEQ_NOLUT,
             sizeof SEQ_NOLUT);
    add_mode(&c, 0u, 2u, "partial", 10, 30, OD_DRIVE_TEMP_ADVISORY, 0xFFu, SEQ_NOLUT,
             sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0));
    CHECK(od_epd_cfg_find_usable_mode("fast", 20) == 1);
    CHECK(od_epd_cfg_find_usable_mode("fast", 5) == -1);
    CHECK(od_epd_cfg_find_usable_mode("fast", OD_EPD_CFG_TEMP_UNKNOWN) == 1);
    CHECK(od_epd_cfg_find_usable_mode("partial", 5) == 2);      /* ADVISORY runs anyway */
    CHECK(od_epd_cfg_find_usable_mode("colour16", 20) == -1);
}

static void test_lut_crc(void)
{
    fx_cfg_t c;

    CASE("LUT CRC mismatch: the slot reads as absent, and a LUT step then refuses the pass");
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0u, SEQ_PLAIN, sizeof SEQ_PLAIN);
    {
        const uint32_t n = lut_body(g_b, 0u, 0u, 0x10u, 40u);
        g_b[10] ^= 0x01u;                     /* one data bit */
        fx_cfg_ext(&c, OD_PKT_DISPLAY_LUT, g_b, n);
    }
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0));
    CHECK(od_epd_cfg_select(0u, NULL));
    CHECK(!od_epd_cfg_begin_pass(0u));
    CHECK(od_epd_cfg_last_rc() == OD_EPD_RC_LUT);
    CHECK(strstr(fx_trace, "C 20\nD ") == NULL);   /* no waveform bytes reached the panel */

    CASE("a LUT body longer than 1024 B is absent too");
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0u, SEQ_PLAIN, sizeof SEQ_PLAIN);
    {
        static uint8_t big[4u + 1100u];
        fx_cfg_ext(&c, OD_PKT_DISPLAY_LUT, big, lut_body(big, 0u, 0u, 1u, 1025u));
    }
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
    CHECK(!od_epd_cfg_begin_pass(0u) && od_epd_cfg_last_rc() == OD_EPD_RC_LUT);

    CASE("lut_slot 0xFF with a LUT step in the sequence: refused, not 'stock waveform'");
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_PLAIN, sizeof SEQ_PLAIN);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
    CHECK(!od_epd_cfg_begin_pass(0u) && od_epd_cfg_last_rc() == OD_EPD_RC_LUT);
}

static void test_temperature(void)
{
    fx_cfg_t c;
    uint8_t mode;

    CASE("REJECT: the window is inclusive at both ends");
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 0u, "full", 10, 30, OD_DRIVE_TEMP_REJECT, 0xFFu, SEQ_NOLUT,
             sizeof SEQ_NOLUT);
    add_mode(&c, 0u, 1u, "cold", -20, -5, OD_DRIVE_TEMP_REJECT, 0xFFu, SEQ_NOLUT,
             sizeof SEQ_NOLUT);
    add_mode(&c, 0u, 2u, "anytime", 0, 0, OD_DRIVE_TEMP_REJECT, 0xFFu, SEQ_NOLUT,
             sizeof SEQ_NOLUT);
    add_mode(&c, 0u, 3u, "inverted", 30, 10, OD_DRIVE_TEMP_REJECT, 0xFFu, SEQ_NOLUT,
             sizeof SEQ_NOLUT);
    add_mode(&c, 0u, 4u, "policy7", 10, 30, 7u, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0));
    static const struct { int8_t t; uint8_t err; } rej[] = {
        { 9, OD_ERR_DRIVE_TEMP_RANGE }, { 10, 0u }, { 20, 0u }, { 30, 0u },
        { 31, OD_ERR_DRIVE_TEMP_RANGE }, { -128, OD_ERR_DRIVE_TEMP_RANGE },
        { 126, OD_ERR_DRIVE_TEMP_RANGE }, { OD_EPD_CFG_TEMP_UNKNOWN, 0u },
    };
    for (unsigned i = 0; i < sizeof rej / sizeof rej[0]; i++) {
        mode = 0u;
        CHECK(od_epd_cfg_select_policy(&mode, rej[i].t, NULL) == rej[i].err);
        CHECK(mode == 0u);
    }

    CASE("signed window below zero");
    mode = 1u;
    CHECK(od_epd_cfg_select_policy(&mode, -10, NULL) == 0u);
    mode = 1u;
    CHECK(od_epd_cfg_select_policy(&mode, -4, NULL) == OD_ERR_DRIVE_TEMP_RANGE);
    mode = 1u;
    CHECK(od_epd_cfg_select_policy(&mode, -21, NULL) == OD_ERR_DRIVE_TEMP_RANGE);

    CASE("[0,0] means 'no window': a real 0..0 C window cannot be expressed");
    mode = 2u;
    CHECK(od_epd_cfg_select_policy(&mode, 100, NULL) == 0u);

    CASE("an inverted window never matches: every reading is out of range");
    mode = 3u;
    CHECK(od_epd_cfg_select_policy(&mode, 20, NULL) == OD_ERR_DRIVE_TEMP_RANGE);

    CASE("an unknown policy value behaves as ADVISORY");
    mode = 4u;
    CHECK(od_epd_cfg_select_policy(&mode, 99, NULL) == 0u);

    CASE("a mode index past the declared modes");
    mode = 5u;
    CHECK(od_epd_cfg_select_policy(&mode, 20, NULL) == OD_ERR_DRIVE_BAD_MODE);

    CASE("SUBSTITUTE: falls back to mode 0 when mode 0 covers, reported through *mode");
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 0u, "warm", 21, 40, OD_DRIVE_TEMP_REJECT, 0xFFu, SEQ_NOLUT,
             sizeof SEQ_NOLUT);
    add_mode(&c, 0u, 1u, "cool", 0, 20, OD_DRIVE_TEMP_SUBSTITUTE, 0xFFu, SEQ_NOLUT,
             sizeof SEQ_NOLUT);
    add_mode(&c, 0u, 2u, "alsowarm", 25, 45, OD_DRIVE_TEMP_REJECT, 0xFFu, SEQ_NOLUT,
             sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0));
    mode = 1u;
    CHECK(od_epd_cfg_select_policy(&mode, 30, NULL) == 0u && mode == 0u);
    mode = 1u;
    CHECK(od_epd_cfg_select_policy(&mode, 21, NULL) == 0u && mode == 0u);
    mode = 1u;
    CHECK(od_epd_cfg_select_policy(&mode, 20, NULL) == 0u && mode == 1u);
    mode = 1u;   /* mode 2 covers 45 C but is not a substitute; mode 0 does not cover it */
    CHECK(od_epd_cfg_select_policy(&mode, 45, NULL) == OD_ERR_DRIVE_TEMP_RANGE);
    mode = 1u;
    CHECK(od_epd_cfg_select_policy(&mode, 46, NULL) == OD_ERR_DRIVE_TEMP_RANGE);
    {
        od_epd_cfg_mode_t m;
        mode = 1u;
        CHECK(od_epd_cfg_select_policy(&mode, 30, &m) == 0u);
        CHECK(m.temp_min_c == 21 && m.temp_max_c == 40);   /* the substitute's own window */
    }
}

/* A config with exactly one mode carrying `seq`; returns begin_pass's rc. */
static int try_seq(const uint8_t *seq, uint16_t len, bool *selected)
{
    fx_cfg_t c;

    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, seq, len);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    *selected = load(&g_d0) && od_epd_cfg_select(0u, NULL);
    if (!*selected) {
        return 1;
    }
    fx_bus_clear();
    return od_epd_cfg_begin_pass(0u) ? 0 : od_epd_cfg_last_rc();
}

static void test_malformed_sequences(void)
{
    static uint8_t seq[1100];
    bool sel = false;

    CASE("no END escape: legal, the walk ends at seq_len");
    {
        const uint8_t s[] = { 1, 0x12, 0, 3, 2, 0x22, 0xF7 };
        CHECK(try_seq(s, sizeof s, &sel) == 0 && sel);
        CHECK(od_epd_cfg_finish());
        CHECK(strstr(fx_trace, "C 22\nD f7") != NULL);
    }

    CASE("no image marker: SEQ, after the init ran (there is nowhere for the frame to go)");
    {
        const uint8_t s[] = { 1, 0x12, 0, 1, 0, 0 };
        CHECK(try_seq(s, sizeof s, &sel) == OD_EPD_RC_SEQ);
    }

    CASE("a step whose length runs past the sequence: refused before any I/O");
    {
        const uint8_t s[] = { 1, 0x12, 0, 3, 9, 0x22, 0xF7 };
        CHECK(try_seq(s, sizeof s, &sel) == OD_EPD_RC_SEQ);
        CHECK(fx_reset_count == 0u && fx_cmd_count == 0u);
    }

    CASE("escape prefix as the last byte: refused before any I/O");
    {
        const uint8_t s[] = { 1, 0x12, 0, 3, 0 };
        CHECK(try_seq(s, sizeof s, &sel) == OD_EPD_RC_SEQ);
        CHECK(fx_reset_count == 0u);
    }

    CASE("unknown escape 0x7E and a reserved 0x80+ escape: UNSUP before any I/O");
    {
        const uint8_t s1[] = { 0, 0x7E, 0, 3 };
        const uint8_t s2[] = { 0, 0x80, 1, 0xAA, 0, 3 };
        CHECK(try_seq(s1, sizeof s1, &sel) == OD_EPD_RC_UNSUP && fx_reset_count == 0u);
        /* The contract makes 0x80+ escapes length-carrying "so a future OPTIONAL escape can be
         * skipped"; this executor refuses them wholesale instead (allowed: 'MUST refuse an
         * escape it does not know'). Pinned so a change is deliberate. */
        CHECK(try_seq(s2, sizeof s2, &sel) == OD_EPD_RC_UNSUP && fx_reset_count == 0u);
    }

    CASE("an escape whose fixed args are cut short");
    {
        const uint8_t s[] = { 1, 0x12, 0, 3, 0, 5, 0x10 };   /* DELAY with one byte */
        CHECK(try_seq(s, sizeof s, &sel) == OD_EPD_RC_SEQ && fx_reset_count == 0u);
    }

    CASE("SUBST count 0 and count 33");
    {
        const uint8_t s1[] = { 0, 2, 0, 0, 3 };
        CHECK(try_seq(s1, sizeof s1, &sel) == OD_EPD_RC_SEQ);
        seq[0] = 0; seq[1] = 2; seq[2] = 33;
        memset(&seq[3], 0, 66u);
        seq[69] = 0; seq[70] = 3;
        CHECK(try_seq(seq, 71u, &sel) == OD_EPD_RC_SEQ);
    }

    CASE("CS_SELECT on a display without cs_pin_2: UNSUP (no hook offered), nothing sent");
    {
        const uint8_t s[] = { 0, 8, 1, 0, 3 };
        CHECK(try_seq(s, sizeof s, &sel) == OD_EPD_RC_UNSUP && fx_reset_count == 0u);
    }

    CASE("SET_PIN / READ_SCRATCH with no bus hook: UNSUP, nothing sent");
    {
        const uint8_t s1[] = { 0, 6, 0, 1, 0, 3 };
        const uint8_t s2[] = { 0, 9, 0x11, 4, 0, 3 };
        CHECK(try_seq(s1, sizeof s1, &sel) == OD_EPD_RC_UNSUP && fx_reset_count == 0u);
        CHECK(try_seq(s2, sizeof s2, &sel) == OD_EPD_RC_UNSUP && fx_reset_count == 0u);
    }

    CASE("WRITE_SCRATCH past the 16-byte scratch");
    {
        const uint8_t s[] = { 0, 0x0A, 0x30, 10, 7, 0, 3 };
        CHECK(try_seq(s, sizeof s, &sel) == OD_EPD_RC_SEQ && fx_reset_count == 0u);
    }

    CASE("IMAGE_SLICE of 0 / part >= of");
    {
        const uint8_t s1[] = { 0, 7, 0, 0 };
        const uint8_t s2[] = { 0, 7, 2, 2 };
        CHECK(try_seq(s1, sizeof s1, &sel) == OD_EPD_RC_SEQ);
        CHECK(try_seq(s2, sizeof s2, &sel) == OD_EPD_RC_SEQ);
    }

    CASE("FILL with bpp code 3 is refused by the validator");
    {
        const uint8_t s[] = { 0, 0x0D, 0x24, 0xFF, 0x03, 0, 3 };
        CHECK(try_seq(s, sizeof s, &sel) == OD_EPD_RC_SEQ && fx_reset_count == 0u);
    }

    CASE("step cap: OD_EPD_MAX_SEQ_STEPS + 1 steps are refused, OD_EPD_MAX_SEQ_STEPS run");
    for (unsigned extra = 0u; extra < 2u; extra++) {
        uint16_t n = 0u;
        for (unsigned k = 0; k + 1u < OD_EPD_MAX_SEQ_STEPS + extra; k++) {
            seq[n++] = 1;
            seq[n++] = 0x00;
        }
        seq[n++] = 0;
        seq[n++] = 3;
        CHECK(try_seq(seq, n, &sel) == (extra ? OD_EPD_RC_SEQ : 0));
        CHECK(extra ? fx_reset_count == 0u : od_epd_cfg_finish());
    }

    CASE("no RAM cap on the sequence: 1028 B with four 255-byte steps streams in window chunks");
    {
        uint16_t n = 0u;
        for (unsigned k = 0; k < 4u; k++) {
            seq[n++] = 255u;
            seq[n++] = (uint8_t)(0x40u + k);
            for (unsigned j = 0; j < 254u; j++) {
                seq[n++] = (uint8_t)(j ^ k);
            }
        }
        seq[n++] = 0;
        seq[n++] = 3;
        seq[n++] = 1;
        seq[n++] = 0x12;
        CHECK(n == 1028u);
        CHECK(try_seq(seq, n, &sel) == 0 && sel);
        CHECK(od_epd_cfg_finish());
        CHECK(fx_cmd_count == 5u && fx_data_bytes == 4u * 254u);
        /* cmd + data longer than the window: the first window (cmd + OD_EPD_SEQ_WINDOW - 1 data
         * bytes), then the rest of the data; same bytes, one more data() call. */
        {
            const unsigned d0 = OD_EPD_SEQ_WINDOW - 1u;
            char want[64];
            snprintf(want, sizeof want, "C 40\nD[%u] crc32=%08x\nD[%u] crc32=%08x\nC 41", d0,
                     (unsigned)od_config_store_crc32(&seq[2], d0), 254u - d0,
                     (unsigned)od_config_store_crc32(&seq[2u + d0], 254u - d0));
            CHECK(strstr(fx_trace, want) != NULL);
        }
    }

    CASE("seq_len larger than the packet body: rejected at select");
    {
        fx_cfg_t c;
        begin_cfg(&c, 1u);
        add_panel(&c, 0u);
        const uint32_t n = mode_body(g_b, 0u, 0u, "full", 1u, 0, 0, 0, 0xFFu, SEQ_NOLUT,
                                     sizeof SEQ_NOLUT);
        g_b[35] = (uint8_t)(sizeof SEQ_NOLUT + 1u);
        fx_cfg_ext(&c, OD_PKT_DISPLAY_MODE, g_b, n);
        fx_cfg_finish(&c);
        fx_cfg_store(&c);
        CHECK(load(&g_d0));
        CHECK(!od_epd_cfg_select(0u, NULL));
    }

    CASE("passes 0 and 9 are rejected at select");
    {
        fx_cfg_t c;
        begin_cfg(&c, 1u);
        add_panel(&c, 0u);
        fx_cfg_ext(&c, OD_PKT_DISPLAY_MODE, g_b,
                   mode_body(g_b, 0u, 0u, "zero", 0u, 0, 0, 0, 0xFFu, SEQ_NOLUT,
                             sizeof SEQ_NOLUT));
        fx_cfg_ext(&c, OD_PKT_DISPLAY_MODE, g_b,
                   mode_body(g_b, 0u, 1u, "nine", 9u, 0, 0, 0, 0xFFu, SEQ_NOLUT,
                             sizeof SEQ_NOLUT));
        fx_cfg_finish(&c);
        fx_cfg_store(&c);
        CHECK(load(&g_d0));
        CHECK(!od_epd_cfg_select(0u, NULL) && !od_epd_cfg_select(1u, NULL));
    }

    CASE("a pass past the mode's pass count");
    {
        CHECK(try_seq(SEQ_NOLUT, sizeof SEQ_NOLUT, &sel) == 0);
        CHECK(od_epd_cfg_finish());
        CHECK(!od_epd_cfg_begin_pass(1u) && od_epd_cfg_last_rc() == OD_EPD_RC_MODE);
    }

    CASE("write and finish without an open pass are refused");
    {
        static const uint8_t px[2] = { 1, 2 };
        CHECK(!od_epd_cfg_write(px, 2u));
        CHECK(!od_epd_cfg_finish());
    }
}

static int g_cs_last = -1;
static bool fx_cs(uint8_t which)
{
    g_cs_last = which;
    return true;
}

static void test_sleep_sequence(void)
{
    fx_cfg_t c;
    od_epd_cfg_bus_t bus = FX_BUS;
    struct DisplayConfig d;

    bus.cs_select = fx_cs;
    d = g_d0;
    d.cs_pin_2 = 9u;
    {
        /* EPD-SLEEP-UNVALIDATED: the 0xB1 sleep sequence goes through od_epd_validate() like
         * a mode sequence, and a refused one touches nothing -- not even the CS reset.
         * CS_SELECT 5 used to reach the hook; FILL with bpp code 3 read past bpp_of[] in
         * run_fill() (fuzz_config_drive, tests/fuzz/corpus/config_drive/regress_sleep_fill_bpp3). */
        static const struct { const char *name; uint8_t s[6]; uint8_t n; } bad[] = {
            { "sleep seq: CS_SELECT 5 is refused before I/O",      { 0, 8, 5, 0, 0 },     5u },
            { "sleep seq: FILL bpp code 3 is refused before I/O",  { 0, 0x0D, 0x24, 0xFF, 3 }, 5u },
            { "sleep seq: truncated plain step is refused",        { 4, 0x10, 1 },        3u },
            { "sleep seq: unknown escape is refused",              { 0, 0x7E },           2u },
        };
        for (unsigned k = 0u; k < sizeof bad / sizeof bad[0]; k++) {
            CASE(bad[k].name);
            begin_cfg(&c, 1u);
            fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, g_b, panel_body(g_b, 0u, bad[k].s, bad[k].n));
            add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
            fx_cfg_finish(&c);
            fx_cfg_store(&c);
            fx_bus_clear();
            fx_busy_level = 1;
            CHECK(od_epd_cfg_load(&d, &bus));
            g_cs_last = -1;
            od_epd_cfg_sleep();
            CHECK(g_cs_last == -1);
            CHECK(fx_cmd_count == 0u);
        }
    }

    CASE("sleep seq: a valid one still runs (CS reset, then the step)");
    begin_cfg(&c, 1u);
    {
        const uint8_t s[] = { 2, 0x10, 0x01, 0, 8, 1 };
        fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, g_b, panel_body(g_b, 0u, s, sizeof s));
    }
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    fx_bus_clear();
    CHECK(od_epd_cfg_load(&d, &bus));
    g_cs_last = -1;
    od_epd_cfg_sleep();
    CHECK(fx_cmd_count == 1u);
    CHECK(g_cs_last == 1);

    CASE("a sleep_seq_len past OD_EPD_SLEEP_SEQ_MAX is dropped, not truncated");
    begin_cfg(&c, 1u);
    {
        uint8_t s[OD_EPD_SLEEP_SEQ_MAX + 1u];
        memset(s, 0, sizeof s);
        s[0] = 1; s[1] = 0x07;
        fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, g_b, panel_body(g_b, 0u, s, sizeof s));
    }
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0));
    fx_bus_clear();
    od_epd_cfg_sleep();
    CHECK(fx_cmd_count == 0u);

    CASE("sleep_seq_len claiming more than the 0xB1 body holds is dropped");
    begin_cfg(&c, 1u);
    {
        const uint8_t s[] = { 1, 0x07 };
        const uint32_t n = panel_body(g_b, 0u, s, sizeof s);
        g_b[12] = 6u;
        fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, g_b, n);
    }
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0));
    fx_bus_clear();
    od_epd_cfg_sleep();
    CHECK(fx_cmd_count == 0u);

    CASE("an 0xB1 shorter than its 13-byte head: no panel");
    begin_cfg(&c, 1u);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, g_b, 12u);
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(!load(&g_d0));
}

static void test_busy(void)
{
    fx_cfg_t c;
    uint16_t elapsed = 0u;
    bool still = false;

    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);

    CASE("BUSY that never releases: the in-sequence wait times out with RC_BUSY");
    CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
    fx_busy_release_after = 100000u;
    fx_clock_ms = 0u;
    CHECK(!od_epd_cfg_begin_pass(0u));
    CHECK(od_epd_cfg_last_rc() == OD_EPD_RC_BUSY);
    CHECK(fx_clock_ms >= 1000u && fx_clock_ms <= 1010u);    /* 0xB1 timeout is 1000 ms */

    CASE("raw BUSY: release before the timeout, at it, and never");
    fx_busy_release_after = 3u;
    fx_clock_ms = 0u;
    od_epd_cfg_raw_busy(100u, &elapsed, &still);
    CHECK(!still && elapsed == 6u);
    fx_busy_release_after = 100000u;
    od_epd_cfg_raw_busy(100u, &elapsed, &still);
    CHECK(still && elapsed >= 100u && elapsed <= 102u);
    fx_busy_release_after = 0u;
    od_epd_cfg_raw_busy(0xFFFFFFu, &elapsed, &still);
    CHECK(!still && elapsed == 0u);
    fx_busy_release_after = 100000u;
    od_epd_cfg_raw_busy(70000u, &elapsed, &still);
    CHECK(still && elapsed == 0xFFFFu);                    /* saturates, does not wrap */
    fx_busy_release_after = 0u;

    CASE("no BUSY pin: the raw wait reports not-busy at once");
    fx_busy_level = -1;
    od_epd_cfg_raw_busy(1000u, &elapsed, &still);
    CHECK(!still && elapsed == 0u);
}

static unsigned count_str(const char *hay, const char *needle)
{
    unsigned n = 0u;
    for (const char *at = strstr(hay, needle); at != NULL; at = strstr(at + 1, needle)) {
        ++n;
    }
    return n;
}

/* A pass streams its sequence from the stored record and pauses at the image marker, so a
 * config write is refused for exactly the span of the pass; a write that lands anyway (clear()
 * is never held) fails the pass on the generation check and the panel is put to sleep once. */
static void test_config_write_during_pass(void)
{
    static const uint8_t sleep[] = { 2u, 0x07u, 0xA5u, 0u, 0u };
    static const uint8_t px[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static uint8_t ws[OD_CONFIG_STORE_HEADER_SIZE + 64u];
    static const uint8_t other[24] = { 1, 2, 3 };
    fx_cfg_t c;
    unsigned writes;

    begin_cfg(&c, 1u);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, g_b, panel_body(g_b, 0u, sleep, sizeof sleep));
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);

    CASE("a config write while a pass is paused on its image marker is refused, medium untouched");
    CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
    CHECK(od_epd_cfg_begin_pass(0u));
    writes = fx_nvs_writes;
    CHECK(od_config_store_save(ws, sizeof ws, other, sizeof other) == OD_CONFIG_STORE_BUSY);
    CHECK(fx_nvs_writes == writes);
    CHECK(od_epd_cfg_write(px, sizeof px));
    CHECK(od_epd_cfg_finish());
    CHECK(strstr(fx_trace, "C 22") != NULL);

    CASE("the write goes through once the pass has finished");
    CHECK(od_config_store_save(ws, sizeof ws, other, sizeof other) == OD_CONFIG_STORE_OK);
    CHECK(fx_nvs_writes == writes + 1u);

    CASE("ending a pass early with sleep releases the hold too");
    fx_cfg_store(&c);
    CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
    CHECK(od_epd_cfg_begin_pass(0u));
    od_epd_cfg_sleep();
    CHECK(od_config_store_save(ws, sizeof ws, other, sizeof other) == OD_CONFIG_STORE_OK);

    CASE("a clear() mid-pass fails the pass on the generation check; panel sleeps exactly once");
    fx_cfg_store(&c);
    CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
    CHECK(od_epd_cfg_begin_pass(0u));
    CHECK(od_epd_cfg_write(px, sizeof px));
    CHECK(od_config_store_clear() == OD_CONFIG_STORE_OK);
    CHECK(!od_epd_cfg_finish());
    CHECK(od_epd_cfg_last_rc() == OD_EPD_RC_STALE);
    CHECK(strstr(fx_trace, "C 22") == NULL);          /* the refresh never ran */
    od_epd_cfg_sleep();
    CHECK(count_str(fx_trace, "C 07") == 1u);
    CHECK(od_config_store_save(ws, sizeof ws, other, sizeof other) == OD_CONFIG_STORE_OK);
}


/* ------------------------------------------------------------ refresh-mode additions (F1/F2/F5/F7) -- */

/* 0xB1 with the v1 tail: [ver 1][key 2][rev 2][flags][budget][family][busy_timeout_s]. */
static uint32_t panel_body_ext(uint8_t *out, uint8_t inst, uint8_t family, uint8_t timeout_s)
{
    const uint32_t n = panel_body(out, inst, NULL, 0u);
    const uint8_t ext[OD_PANEL_EXT_V1_LEN] = { 1u, 0x23u, 0x00u, 1u, 0u, 0u, 0xFFu, family,
                                               timeout_s };

    memcpy(&out[n], ext, sizeof ext);
    return n + (uint32_t)sizeof ext;
}

static uint32_t lut_body_bytes(uint8_t *out, uint8_t inst, uint8_t slot_byte, const uint8_t *d,
                               uint16_t n)
{
    const uint16_t c = lut_crc(d, n);

    out[0] = inst;
    out[1] = slot_byte;
    out[2] = (uint8_t)c;
    out[3] = (uint8_t)(c >> 8);
    memcpy(&out[4], d, n);
    return 4u + n;
}

/* The waveform as the executor streams it: "C 20" then 64-byte "D" lines. */
static bool trace_has_lut(const uint8_t *t, uint16_t n)
{
    static char want[8u * 1024u];
    size_t o = (size_t)snprintf(want, sizeof want, "C 20\n");

    for (uint16_t i = 0u; i < n; i += 64u) {
        const unsigned left = (unsigned)n - (unsigned)i;
        const unsigned k = left < 64u ? left : 64u;

        o += (size_t)snprintf(want + o, sizeof want - o, "D");
        for (unsigned j = 0u; j < k; j++) {
            o += (size_t)snprintf(want + o, sizeof want - o, " %02x", (unsigned)t[i + j]);
        }
        o += (size_t)snprintf(want + o, sizeof want - o, "\n");
    }
    return strstr(fx_trace, want) != NULL;
}

static const lut_vector_t *vector_of(uint8_t family)
{
    for (size_t i = 0; i < sizeof k_lut_vectors / sizeof k_lut_vectors[0]; i++) {
        if (k_lut_vectors[i].family == family) {
            return &k_lut_vectors[i];
        }
    }
    return NULL;
}

static void store_one(fx_cfg_t *c, uint8_t family, uint8_t timeout_s, const uint8_t *seq,
                      uint16_t seq_len, uint8_t slot)
{
    begin_cfg(c, 1u);
    fx_cfg_ext(c, OD_PKT_DISPLAY_PANEL, g_b, panel_body_ext(g_b, 0u, family, timeout_s));
    add_mode(c, 0u, 0u, "full", 0, 0, 0, slot, seq, seq_len);
}

static void test_refresh_modes(void)
{
    static uint8_t body[4u + OD_LUT_MAX_BYTES];
    const lut_vector_t *s6 = vector_of(OD_LUT_FAMILY_S6_800);
    const lut_vector_t *jd = vector_of(OD_LUT_FAMILY_JD_BWRY);
    fx_cfg_t c;

    CHECK(s6 != NULL && jd != NULL);
    if (s6 == NULL || jd == NULL) {
        return;
    }

    for (int e = 0; e < 3; e++) {
        CASE("a deflated 0xAF slot (bit 7) streams the inflated table, CRC over stored bytes");
        store_one(&c, OD_LUT_FAMILY_S6_800, 0u, SEQ_PLAIN, sizeof SEQ_PLAIN, 2u);
        fx_cfg_ext(&c, OD_PKT_DISPLAY_LUT, body,
                   lut_body_bytes(body, 0u, 0x80u | 2u, s6->z[e], s6->z_len[e]));
        fx_cfg_finish(&c);
        fx_cfg_store(&c);
        CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
        CHECK(pass0());
        CHECK(trace_has_lut(s6->raw, s6->raw_len));
    }

    CASE("a deflated slot without a family tail still inflates (no lint)");
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 5u, SEQ_PLAIN, sizeof SEQ_PLAIN);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_LUT, body,
               lut_body_bytes(body, 0u, 0x80u | 5u, jd->z[0], jd->z_len[0]));
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL) && pass0());
    CHECK(trace_has_lut(jd->raw, jd->raw_len));

    CASE("slot ids 8..15 resolve; reserved bits 4..6 make the packet no slot");
    store_one(&c, OD_LUT_FAMILY_JD_BWRY, 0u, SEQ_PLAIN, sizeof SEQ_PLAIN, 13u);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_LUT, body, lut_body_bytes(body, 0u, 13u, jd->raw, jd->raw_len));
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL) && pass0());
    CHECK(trace_has_lut(jd->raw, jd->raw_len));
    store_one(&c, OD_LUT_FAMILY_JD_BWRY, 0u, SEQ_PLAIN, sizeof SEQ_PLAIN, 13u);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_LUT, body,
               lut_body_bytes(body, 0u, 0x10u | 13u, jd->raw, jd->raw_len));
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
    CHECK(!od_epd_cfg_begin_pass(0u) && od_epd_cfg_last_rc() == OD_EPD_RC_LINT);

    CASE("CRC over the inflated bytes (wrong convention): slot absent, refused before I/O");
    store_one(&c, OD_LUT_FAMILY_S6_800, 0u, SEQ_PLAIN, sizeof SEQ_PLAIN, 2u);
    {
        const uint32_t n = lut_body_bytes(body, 0u, 0x82u, s6->z[0], s6->z_len[0]);
        const uint16_t bad = lut_crc(s6->raw, s6->raw_len);

        body[2] = (uint8_t)bad;
        body[3] = (uint8_t)(bad >> 8);
        fx_cfg_ext(&c, OD_PKT_DISPLAY_LUT, body, n);
    }
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
    CHECK(!od_epd_cfg_begin_pass(0u) && od_epd_cfg_last_rc() == OD_EPD_RC_LINT);
    CHECK(fx_reset_count == 0u && fx_cmd_count == 0u);

    CASE("S6 table with 8 zero bytes: refused before I/O when the family is declared");
    {
        static uint8_t t[800];

        memcpy(t, s6->raw, s6->raw_len);
        memset(&t[300], 0, 8u);
        store_one(&c, OD_LUT_FAMILY_S6_800, 0u, SEQ_PLAIN, sizeof SEQ_PLAIN, 0u);
        fx_cfg_ext(&c, OD_PKT_DISPLAY_LUT, body, lut_body_bytes(body, 0u, 0u, t, s6->raw_len));
        fx_cfg_finish(&c);
        fx_cfg_store(&c);
        CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
        CHECK(!od_epd_cfg_begin_pass(0u) && od_epd_cfg_last_rc() == OD_EPD_RC_LINT);
        CHECK(fx_reset_count == 0u);

        CASE("... the same table with no family declared runs (configs before the tail)");
        begin_cfg(&c, 1u);
        add_panel(&c, 0u);
        add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0u, SEQ_PLAIN, sizeof SEQ_PLAIN);
        fx_cfg_ext(&c, OD_PKT_DISPLAY_LUT, body, lut_body_bytes(body, 0u, 0u, t, s6->raw_len));
        fx_cfg_finish(&c);
        fx_cfg_store(&c);
        CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL) && pass0());
    }

    CASE("never-send: an OTP program command in a declared family is refused before I/O");
    {
        static const uint8_t seq[] = { 2, 0xA0, 0x00, 1, 0x12, 0, 3, 0, 1, 0, 0 };

        store_one(&c, OD_LUT_FAMILY_JD_BWRY, 0u, seq, sizeof seq, 0xFFu);
        fx_cfg_finish(&c);
        fx_cfg_store(&c);
        CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
        CHECK(!od_epd_cfg_begin_pass(0u) && od_epd_cfg_last_rc() == OD_EPD_RC_LINT);
        CHECK(fx_reset_count == 0u);
        begin_cfg(&c, 1u);
        add_panel(&c, 0u);
        add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, seq, sizeof seq);
        fx_cfg_finish(&c);
        fx_cfg_store(&c);
        CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL) && pass0());
    }
    CASE("never-send also covers SUBST and WRITE_SCRATCH command bytes");
    {
        static const uint8_t subst[] = { 0, 2, 2, 0x00, 0x91, 0x00, 0x01, 0, 3, 0, 0 };
        static const uint8_t wscr[] = { 0, 10, 0xFB, 0, 1, 0, 3, 0, 0 };

        store_one(&c, OD_LUT_FAMILY_S6_800, 0u, subst, sizeof subst, 0xFFu);
        fx_cfg_finish(&c);
        fx_cfg_store(&c);
        CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
        CHECK(!od_epd_cfg_begin_pass(0u) && od_epd_cfg_last_rc() == OD_EPD_RC_LINT);
        store_one(&c, OD_LUT_FAMILY_S6_800, 0u, wscr, sizeof wscr, 0xFFu);
        fx_cfg_finish(&c);
        fx_cfg_store(&c);
        CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
        CHECK(!od_epd_cfg_begin_pass(0u) && od_epd_cfg_last_rc() == OD_EPD_RC_LINT);
    }

    CASE("a TSC read (0x40) between waveform upload and refresh is refused; before it is fine");
    {
        static const uint8_t after[] = { 1, 0x20, 0, 4, 1, 0x40, 0, 3, 2, 0x12, 0x00, 0, 1, 0, 0 };
        static const uint8_t before[] = { 1, 0x40, 1, 0x20, 0, 4, 0, 3, 2, 0x12, 0x00, 0, 1,
                                          1, 0x40, 0, 0 };

        store_one(&c, OD_LUT_FAMILY_S6_800, 0u, after, sizeof after, 0u);
        fx_cfg_ext(&c, OD_PKT_DISPLAY_LUT, body,
                   lut_body_bytes(body, 0u, 0u, s6->raw, s6->raw_len));
        fx_cfg_finish(&c);
        fx_cfg_store(&c);
        CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
        CHECK(!od_epd_cfg_begin_pass(0u) && od_epd_cfg_last_rc() == OD_EPD_RC_LINT);
        store_one(&c, OD_LUT_FAMILY_S6_800, 0u, before, sizeof before, 0u);
        fx_cfg_ext(&c, OD_PKT_DISPLAY_LUT, body,
                   lut_body_bytes(body, 0u, 0u, s6->raw, s6->raw_len));
        fx_cfg_finish(&c);
        fx_cfg_store(&c);
        CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL) && pass0());
    }

    CASE("0xB1 busy_timeout_s wins over busy_timeout_ms (120 s floor for cold bands)");
    store_one(&c, 0u, 120u, SEQ_NOLUT, sizeof SEQ_NOLUT, 0xFFu);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
    fx_busy_release_after = 1000000u;
    fx_clock_ms = 0u;
    CHECK(!od_epd_cfg_begin_pass(0u) && od_epd_cfg_last_rc() == OD_EPD_RC_BUSY);
    CHECK(fx_clock_ms >= 120000u && fx_clock_ms <= 120010u);
    fx_busy_release_after = 0u;

    CASE("a v1 tail with busy_timeout_s 0 keeps busy_timeout_ms");
    store_one(&c, 0u, 0u, SEQ_NOLUT, sizeof SEQ_NOLUT, 0xFFu);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
    fx_busy_release_after = 1000000u;
    fx_clock_ms = 0u;
    CHECK(!od_epd_cfg_begin_pass(0u));
    CHECK(fx_clock_ms >= 1000u && fx_clock_ms <= 1010u);
    fx_busy_release_after = 0u;
}

static const uint8_t k_frame8[8] = { 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7 };

/* `want` appears in the concatenation of every "D" line (the frame may arrive in any slices). */
static bool trace_data_has(const uint8_t *want, size_t n)
{
    static uint8_t all[64u * 1024u];
    size_t m = 0u;
    const char *p = fx_trace;

    while ((p = strstr(p, "\nD ")) != NULL) {
        p += 2;
        while (*p == ' ' && m < sizeof all) {
            all[m++] = (uint8_t)strtoul(p + 1, (char **)&p, 16);
        }
    }
    for (size_t i = 0u; i + n <= m; i++) {
        if (memcmp(&all[i], want, n) == 0) {
            return true;
        }
    }
    return false;
}

/* INLINE_LUT through the stream: [slot][len:2][crc:2][table] prefixes, then the frame. */
static uint32_t inline_entry(uint8_t *out, uint8_t slot, const uint8_t *t, uint16_t n)
{
    const uint16_t c = lut_crc(t, n);

    out[0] = slot;
    out[1] = (uint8_t)n;
    out[2] = (uint8_t)(n >> 8);
    out[3] = (uint8_t)c;
    out[4] = (uint8_t)(c >> 8);
    memcpy(&out[5], t, n);
    return 5u + n;
}

static bool run_inline(const uint8_t *prefix, uint16_t prefix_n, uint32_t chunk)
{
    static uint8_t stream[4096];
    od_color_geometry_t g;
    od_epd_cfg_stream_t st;
    const uint32_t frame = 8u;   /* 16x4 px mono = 8 B */
    uint32_t off = 0u;

    if (od_color_direct_geometry(g_d0.color_scheme, g_d0.pixel_width, g_d0.pixel_height, &g) !=
            OD_COLOR_OK || g.total_bytes != frame) {
        return false;
    }
    memcpy(stream, prefix, prefix_n);
    for (uint32_t i = 0u; i < frame; i++) {
        stream[prefix_n + i] = (uint8_t)(0xC0u + i);
    }
    if (!od_epd_cfg_inline_expect(prefix_n) ||
        !od_epd_cfg_stream_init(&st, &g, 1u, false, NULL, 0u)) {
        return false;
    }
    while (off < prefix_n + frame) {
        const uint32_t n = (prefix_n + frame - off) < chunk ? (prefix_n + frame - off) : chunk;

        if (!od_epd_cfg_stream_write(&st, off, stream + off, n, NULL)) {
            return false;
        }
        off += n;
    }
    return od_epd_cfg_finish();
}

static void test_inline_lut(void)
{
    static uint8_t prefix[2200];
    static uint8_t stored[4u + 800u];
    const lut_vector_t *s6 = vector_of(OD_LUT_FAMILY_S6_800);
    fx_cfg_t c;
    uint32_t n;

    if (s6 == NULL) {
        CHECK(false);
        return;
    }
    static const uint32_t chunks[] = { 1u, 7u, 64u, 244u, 4096u };
    for (size_t k = 0; k < sizeof chunks / sizeof chunks[0]; k++) {
        CASE("INLINE_LUT: an inline table shadows the stored slot, at any DATA chunking");
        store_one(&c, OD_LUT_FAMILY_S6_800, 0u, SEQ_PLAIN, sizeof SEQ_PLAIN, 1u);
        {
            static uint8_t other[800];

            memcpy(other, s6->raw, sizeof other);
            other[100] ^= 0x01u;   /* still lints; differs from the inline table */
            fx_cfg_ext(&c, OD_PKT_DISPLAY_LUT, stored, lut_body_bytes(stored, 0u, 1u, other, 800u));
        }
        fx_cfg_finish(&c);
        fx_cfg_store(&c);
        CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
        n = inline_entry(prefix, 1u, s6->raw, s6->raw_len);
        CHECK(fx_reset_count == 0u);
        CHECK(run_inline(prefix, (uint16_t)n, chunks[k]));
        CHECK(fx_reset_count == 1u);                     /* pass 0 began after the prefix */
        CHECK(trace_has_lut(s6->raw, s6->raw_len));
        CHECK(trace_data_has(k_frame8, sizeof k_frame8));
    }

    CASE("INLINE_LUT: without a stored slot at all");
    store_one(&c, OD_LUT_FAMILY_S6_800, 0u, SEQ_PLAIN, sizeof SEQ_PLAIN, 4u);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
    n = inline_entry(prefix, 4u, s6->raw, s6->raw_len);
    CHECK(run_inline(prefix, (uint16_t)n, 100u));
    CHECK(trace_has_lut(s6->raw, s6->raw_len));

    CASE("INLINE_LUT: the next load drops the inline table");
    CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
    CHECK(!od_epd_cfg_begin_pass(0u) && od_epd_cfg_last_rc() == OD_EPD_RC_LINT);

    CASE("INLINE_LUT: bad CRC, failed lint, duplicate slot, bad slot id, short prefix all refuse");
    {
        static uint8_t t[800];

        CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
        n = inline_entry(prefix, 4u, s6->raw, s6->raw_len);
        prefix[10] ^= 0x01u;
        CHECK(!run_inline(prefix, (uint16_t)n, 64u));
        CHECK(fx_reset_count == 0u);

        CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
        memcpy(t, s6->raw, sizeof t);
        memset(&t[500], 0, 8u);
        n = inline_entry(prefix, 4u, t, sizeof t);
        CHECK(!run_inline(prefix, (uint16_t)n, 64u));

        CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
        n = inline_entry(prefix, 4u, s6->raw, s6->raw_len);
        n += inline_entry(prefix + n, 4u, s6->raw, s6->raw_len);
        CHECK(!run_inline(prefix, (uint16_t)n, 64u));

        CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
        n = inline_entry(prefix, 16u, s6->raw, s6->raw_len);
        CHECK(!run_inline(prefix, (uint16_t)n, 64u));

        CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
        n = inline_entry(prefix, 4u, s6->raw, s6->raw_len);
        CHECK(!run_inline(prefix, (uint16_t)(n - 3u), 64u));   /* prefix ends inside a table */
        CHECK(fx_reset_count == 0u);
    }

    CASE("INLINE_LUT: two tables for two slots (cache size permitting)");
    {
        static const uint8_t seq2[] = { 1, 0x20, 0, 4, 0, 3, 0, 1, 0, 0 };

        begin_cfg(&c, 1u);
        fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, g_b, panel_body_ext(g_b, 0u, OD_LUT_FAMILY_S6_800, 0u));
        fx_cfg_ext(&c, OD_PKT_DISPLAY_MODE, g_b,
                   mode_body(g_b, 0u, 0u, "full", 1u, 0, 0, 0, 6u, seq2, sizeof seq2));
        fx_cfg_finish(&c);
        fx_cfg_store(&c);
        CHECK(load(&g_d0) && od_epd_cfg_select(0u, NULL));
        n = inline_entry(prefix, 3u, s6->raw, s6->raw_len);
        const lut_vector_t *s6b = NULL;

        for (size_t v = 0; v < sizeof k_lut_vectors / sizeof k_lut_vectors[0]; v++) {
            if (k_lut_vectors[v].family == OD_LUT_FAMILY_S6_800 && &k_lut_vectors[v] != s6) {
                s6b = &k_lut_vectors[v];
            }
        }
        CHECK(s6b != NULL);
        n += inline_entry(prefix + n, 6u, s6b->raw, s6b->raw_len);
        CHECK(run_inline(prefix, (uint16_t)n, 50u) == (OD_EPD_LUT_CACHE >= 2u));
    }
}

int main(void)
{
    test_golden();
    test_two_displays();
    test_duplicates_and_identity();
    test_mode0_invariant();
    test_lut_crc();
    test_temperature();
    test_malformed_sequences();
    test_sleep_sequence();
    test_busy();
    test_config_write_during_pass();
    test_refresh_modes();
    test_inline_lut();
    printf("epd_cfg: %u known bugs\n", fx_xfail_count);
    return OD_CHECK_REPORT_NONEMPTY("epd_cfg", 130);
}
