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
#include "opendisplay_protocol.h"

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

    CASE("DUP-PANEL-ORDER: two 0xB1 for one display -- od_epd_cfg takes the LAST");
    begin_cfg(&c, 1u);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, g_b, panel_body(g_b, 0u, sleep_a, sizeof sleep_a));
    fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, g_b, panel_body(g_b, 0u, sleep_b, sizeof sleep_b));
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0));
    fx_bus_clear();
    od_epd_cfg_sleep();
    /* Every other duplicate in the stack resolves first-match (od_cfg_find, the 0xB2 table,
     * BG22's od_epd_shim_available, find_lut) and this one resolves last-match: a host
     * appending a corrected 0xB1 gets it here and not on BG22. */
    KNOWN_BUG("DUP-PANEL-ORDER", strstr(fx_trace, "C a1") != NULL);

    CASE("MODE-BY-POSITION: the START mode byte selects by packet order, not 0xAE.mode_index");
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 1u, "colour16", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0xFFu, SEQ_NOLUT, sizeof SEQ_NOLUT);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(load(&g_d0));
    CHECK(od_epd_cfg_find_mode("full") == 1);
    /* DisplayModeHead.mode_index: "mode id named by CMD_DRIVE_WRITE_START's mode byte". A config
     * whose packets are not written in index order runs the wrong mode for every START, and
     * the 0xB0 palette (keyed by mode_index) describes a different mode than is driven. */
    KNOWN_BUG("MODE-BY-POSITION", od_epd_cfg_find_mode("full") == 0);

    CASE("mode names: case-insensitive, NUL-padded, 12 bytes");
    CHECK(od_epd_cfg_find_mode("FULL") == 1);
    CHECK(od_epd_cfg_find_mode("COLOUR16") == 0);
    CHECK(od_epd_cfg_find_mode("colour") == -1);
    CHECK(od_epd_cfg_find_mode("") == -1);
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

    CASE("a LUT body longer than 535 B is absent too");
    begin_cfg(&c, 1u);
    add_panel(&c, 0u);
    add_mode(&c, 0u, 0u, "full", 0, 0, 0, 0u, SEQ_PLAIN, sizeof SEQ_PLAIN);
    {
        static uint8_t big[4u + 600u];
        fx_cfg_ext(&c, OD_PKT_DISPLAY_LUT, big, lut_body(big, 0u, 0u, 1u, 536u));
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

    CASE("SUBSTITUTE: the lowest covering mode, reported back through *mode");
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
    mode = 1u;
    CHECK(od_epd_cfg_select_policy(&mode, 45, NULL) == 0u && mode == 2u);
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

    CASE("a sleep_seq_len past 16 B is dropped, not truncated");
    begin_cfg(&c, 1u);
    {
        uint8_t s[17];
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

int main(void)
{
    test_golden();
    test_two_displays();
    test_duplicates_and_identity();
    test_lut_crc();
    test_temperature();
    test_malformed_sequences();
    test_sleep_sequence();
    test_busy();
    test_config_write_during_pass();
    printf("epd_cfg: %u known bugs\n", fx_xfail_count);
    return OD_CHECK_REPORT_NONEMPTY("epd_cfg", 130);
}
