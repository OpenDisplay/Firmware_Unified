/* refresh_modes: every mode of panels/generated/refresh_modes.json (refresh_modes.inc) through the
 * real od_epd_cfg / od_epd on a recording bus, as a stored config.
 *
 * Per mode: it selects, every pass begins and finishes, the trace carries what the safety rules
 * of the lab handover demand (no TSC read 0x40 at all, POF with its data byte in the sleep, the
 * custom-table PSR bit only where a table is uploaded, a 0x20 table per pass that names a slot).
 * The host-built-table mode is refused before any I/O without INLINE_LUT and runs with it.
 * Built twice: with the LUT cache (ESP32 / Nordic) and without it (BG22), where only panels
 * whose tables are stored uncompressed are expected to run their table modes. */
#include "od_check.h"
#include "drive_fixture.h"

#include "od_cfg_index.h"
#include "od_color.h"
#include "od_epd.h"
#include "od_epd_cfg.h"
#include "od_epd_lut.h"
#include "opendisplay_protocol.h"

#include <stdio.h>
#include <string.h>

#include "refresh_modes.inc"

static const char *g_cs_last;

static bool rm_cs_select(uint8_t which)
{
    fx_trace_line(which ? "S 1" : "S 0");
    g_cs_last = which ? "1" : "0";
    return true;
}

static od_epd_cfg_bus_t g_bus;

static struct DisplayConfig store(const rm_panel_t *p)
{
    fx_cfg_t c;
    struct DisplayConfig d = fx_display(0u, p->panel.width, p->panel.height, p->panel.scheme);

    d.cs_pin_2 = 7u;
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_panel(&c, &p->panel, 0u);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    return d;
}

static bool is_experimental(const rm_panel_t *p, uint8_t mode)
{
    for (unsigned i = 0; i < p->n_modes; i++) {
        if (p->modes[i] == mode) {
            return true;
        }
    }
    return false;
}

/* The mode's 0xAE body, its stored slots, and whether each slot has a 0xAF. */
static const uint8_t *mode_body(const rm_panel_t *p, uint8_t mode)
{
    for (unsigned i = 0; i < p->panel.count; i++) {
        if (p->panel.pkts[i].id == 0xAE && p->panel.pkts[i].body[1] == mode) {
            return p->panel.pkts[i].body;
        }
    }
    return NULL;
}

static const fx_panel_pkt_t *slot_pkt(const rm_panel_t *p, uint8_t slot)
{
    for (unsigned i = 0; i < p->panel.count; i++) {
        if (p->panel.pkts[i].id == 0xAF && (p->panel.pkts[i].body[1] & 0x0Fu) == slot) {
            return &p->panel.pkts[i];
        }
    }
    return NULL;
}

static unsigned count(const char *needle)
{
    unsigned n = 0u;

    for (const char *s = fx_trace; (s = strstr(s, needle)) != NULL; s++) {
        n++;
    }
    return n;
}

/* Image markers in a mode's sequence: IMAGE (one whole plane) or IMAGE_SLICE (part of one).
 * *windowed is set when the sequence has a WINDOW step. */
static unsigned image_markers(const uint8_t *body, bool *windowed)
{
    const uint16_t n = (uint16_t)(body[35] | (body[36] << 8));
    const uint8_t *q = body + 37;
    unsigned k = 0u;

    *windowed = false;
    for (uint16_t i = 0u; i + 1u < n;) {
        if (q[i] != 0u) {
            i = (uint16_t)(i + 1u + q[i]);
            continue;
        }
        switch (q[i + 1u]) {
        case 0x00: return k;
        case OD_EPD_ESC_IMAGE: k++; i += 2u; break;
        case OD_EPD_ESC_IMAGE_SLICE: k++; i += 4u; break;
        case OD_EPD_ESC_BUSY: case OD_EPD_ESC_LUT: i += 2u; break;
        case OD_EPD_ESC_CS_SELECT: i += 3u; break;
        case OD_EPD_ESC_DELAY: case OD_EPD_ESC_SET_PIN: i += 4u; break;
        case OD_EPD_ESC_LUT_SLICE: i += 6u; break;
        case OD_EPD_ESC_WAVE: i = (uint16_t)(i + 3u + 2u * q[i + 2u]); break;
        case OD_EPD_ESC_WINDOW: *windowed = true; i = (uint16_t)(i + 3u + 2u * q[i + 2u]); break;
        case OD_EPD_ESC_FILL: i += 5u; break;
        default: return 0u;
        }
    }
    return k;
}

static bool drive(const uint8_t *body, uint8_t passes, const od_color_geometry_t *g)
{
    static uint8_t frame[1u << 20];
    bool windowed;
    const unsigned planes = image_markers(body, &windowed);

    if (planes == 0u) {
        return false;
    }
    memset(frame, 0x11, sizeof frame);
    for (uint8_t pass = 0u; pass < passes; pass++) {
        if (!od_epd_cfg_begin_pass(pass)) {
            return false;
        }
        for (unsigned pl = 0u; pl < planes; pl++) {
            const uint32_t part = g->total_bytes / planes;

            if (pl > 0u && !od_epd_cfg_next_plane()) {
                return false;
            }
            for (uint32_t off = 0u; off < part;) {
                const uint32_t n = part - off < sizeof frame ? part - off : (uint32_t)sizeof frame;

                if (!od_epd_cfg_write(frame, n)) {
                    return false;
                }
                off += n;
            }
        }
        if (!od_epd_cfg_finish()) {
            return false;
        }
    }
    od_epd_cfg_sleep();
    return true;
}

static bool needs_slot(const uint8_t *body, uint8_t pass, uint8_t *slot)
{
    *slot = body[19u + pass];
    return *slot != 0xFFu;
}

static void test_panel(const rm_panel_t *p)
{
    const struct DisplayConfig d = store(p);
    od_color_geometry_t g;
    uint8_t mode_count;

    printf("  %s\n", p->panel.name);
    CASE("the panel config loads and declares every mode");
    fx_busy_level = 1;
    CHECK(od_epd_cfg_load(&d, &g_bus));
    mode_count = od_epd_cfg_mode_count();
    CHECK(mode_count == p->modes[p->n_modes - 1u] + 1u);
    CHECK(od_color_direct_geometry(d.color_scheme, d.pixel_width, d.pixel_height, &g) ==
          OD_COLOR_OK);

    for (uint8_t m = 0u; m < mode_count; m++) {
        const uint8_t *body = mode_body(p, m);
        od_epd_cfg_mode_t info;
        bool stored = true;
        bool any_lut = false;
        uint8_t slot = 0xFFu;
        od_color_geometry_t mg;

        if (body == NULL) {
            continue;
        }
        {
            bool windowed;

            /* A stock windowed partial runs only through the partial path, which tests/panels
             * replays for every preset; this suite drives whole-frame passes. */
            (void)image_markers(body, &windowed);
            if (windowed && !is_experimental(p, m)) {
                continue;
            }
        }
        CASE("each mode selects, runs every pass and sleeps with POF 02 00");
        CHECK(od_epd_cfg_load(&d, &g_bus) && od_epd_cfg_select(m, &info));
        for (uint8_t pass = 0u; pass < info.passes; pass++) {
            if (needs_slot(body, pass, &slot)) {
                const fx_panel_pkt_t *s = slot_pkt(p, slot);

                any_lut = true;
                if (s == NULL || ((s->body[1] & OD_LUT_SLOT_COMPRESSED) && OD_EPD_LUT_CACHE == 0)) {
                    stored = false;
                }
            }
        }
        fx_bus_clear();
        fx_busy_level = 1;
        if (!stored) {
            CASE("a table mode whose slot is absent here is refused before any panel I/O");
            CHECK(!od_epd_cfg_begin_pass(0u) && od_epd_cfg_last_rc() == OD_EPD_RC_LINT);
            CHECK(fx_reset_count == 0u && fx_cmd_count == 0u);
            continue;
        }
        /* A mode with its own scheme (0x003B gray4: 4-gray on a mono display) has its own frame. */
        CHECK(od_color_direct_geometry(info.color_scheme == 0xFFu ? d.color_scheme : info.color_scheme,
                                       d.pixel_width, d.pixel_height, &mg) == OD_COLOR_OK);
        if (!drive(body, info.passes, &mg)) {
            printf("    mode %u failed rc=%d\n", (unsigned)m, od_epd_cfg_last_rc());
            CHECK(false);
            continue;
        }
        CHECK(strstr(fx_trace, "C 40") == NULL);              /* rule 4: no TSC read */
        CHECK(count("C 12\n") >= (unsigned)info.passes);
        /* Rules 6 and the PSR bit are the lab handover's, for its S6 / JD-BWRY families; a panel
         * without a lut_family (UC8179 GEN2) keeps its stock POF and register-LUT PSR. */
        if (p->family == 0u) {
            continue;
        }
        CHECK(strstr(fx_trace, "C 02\nD 00\n") != NULL);       /* rule 6: POF 02 00 */
        if (is_experimental(p, m) && any_lut) {
            CASE("table modes upload 0x20 per chip per pass and set the custom-table PSR bit");
            CHECK(count("C 20\n") >= (unsigned)info.passes);
            CHECK(strstr(fx_trace, "C 00\nD 5f e9") != NULL || strstr(fx_trace, "C 00\nD df e9") != NULL ||
                  strstr(fx_trace, "C 00\nD 0f a9") != NULL || strstr(fx_trace, "C 00\nD 0f 89") != NULL);
        }
        if (is_experimental(p, m) && !any_lut) {
            CASE("stock-waveform experimental modes upload no table");
            CHECK(strstr(fx_trace, "C 20\n") == NULL);
        }
    }
}

typedef struct {
    const uint8_t *p;
    uint16_t n;
} rm_src_t;

static uint16_t rm_read(void *ctx, uint16_t off, uint8_t *dst, uint16_t len)
{
    const rm_src_t *s = (const rm_src_t *)ctx;

    if ((uint32_t)off + len > s->n) {
        return 0u;
    }
    memcpy(dst, s->p + off, len);
    return len;
}

/* 7.3in hosttable: slot 5 has no 0xAF; it runs only with an inline table for this transfer. */
static void test_inline(void)
{
    const rm_panel_t *p = &k_rm_panels[0];
    const struct DisplayConfig d = store(p);
    const fx_panel_pkt_t *fast6 = slot_pkt(p, 2u);
    static uint8_t stream[5u + OD_LUT_MAX_BYTES + 4096u];
    od_color_geometry_t g;
    od_epd_cfg_stream_t st;
    uint16_t raw_n = 0u;
    uint8_t *raw = &stream[5];

    CHECK(strcmp(p->panel.name, "s73_spectra6") == 0 && fast6 != NULL);
    if (fast6 == NULL) {
        return;
    }
    CHECK(od_color_direct_geometry(d.color_scheme, d.pixel_width, d.pixel_height, &g) ==
          OD_COLOR_OK);
    CHECK(od_epd_cfg_load(&d, &g_bus) && od_epd_cfg_select(8u, NULL));
    fx_bus_clear();
#if OD_EPD_LUT_CACHE > 0
    CASE("hosttable: the inline table (F1d6 as a host sends it, raw) runs through the stream");
    {
        rm_src_t src = { fast6->body + 4, (uint16_t)(fast6->n - 4u) };
        uint16_t crc;
        uint32_t total, off = 0u;

        CHECK(od_lut_inflate(rm_read, &src, src.n, raw, OD_LUT_MAX_BYTES, &raw_n) && raw_n == 800u);
        crc = od_lut_crc16(0xFFFFu, raw, raw_n);
        stream[0] = 5u;
        stream[1] = (uint8_t)raw_n;
        stream[2] = (uint8_t)(raw_n >> 8);
        stream[3] = (uint8_t)crc;
        stream[4] = (uint8_t)(crc >> 8);
        CHECK(od_epd_cfg_inline_expect((uint16_t)(5u + raw_n)));
        CHECK(od_epd_cfg_stream_init(&st, &g, 1u, false, NULL, 0u));
        total = 5u + raw_n + g.total_bytes;
        memset(&stream[5u + raw_n], 0x11, sizeof stream - 5u - raw_n);
        while (off < total) {
            const uint32_t pre = 5u + raw_n;
            const uint32_t n = off < pre ? pre - off : (total - off < 4096u ? total - off : 4096u);
            const uint8_t *src_b = off < pre ? &stream[off] : &stream[pre];

            if (!od_epd_cfg_stream_write(&st, off, src_b, n, NULL)) {
                CHECK(false);
                break;
            }
            off += n;
        }
        CHECK(od_epd_cfg_finish());
        od_epd_cfg_sleep();
        CHECK(count("C 20\n") == 1u && strstr(fx_trace, "C 40") == NULL);
        CHECK(strstr(fx_trace, "C 02\nD 00\n") != NULL);
    }
#else
    CASE("hosttable on a build without the LUT cache: INLINE_LUT is refused");
    CHECK(!od_epd_cfg_inline_expect(805u));
    (void)st;
    (void)raw;
    (void)raw_n;
    (void)rm_read;
#endif
}

int main(void)
{
    g_bus = FX_BUS;
    g_bus.cs_select = rm_cs_select;
    for (size_t i = 0; i < sizeof k_rm_panels / sizeof k_rm_panels[0]; i++) {
        test_panel(&k_rm_panels[i]);
    }
    test_inline();
    (void)fx_xfail_count;
    return OD_CHECK_REPORT_NONEMPTY("refresh_modes", 20);
}
