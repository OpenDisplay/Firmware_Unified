/* epd_seq_test -- the shared od_epd sequence executor, against a recording io table.
 *
 * Covers the escapes whose behaviour is not exercised by any panel on the bench: CS_SELECT
 * (0x08), READ_SCRATCH (0x09) and WRITE_SCRATCH (0x0A), plus the validation rule that a refused
 * sequence executes nothing, and the streaming reader: a step longer than the window, a storage
 * read that fails or comes back short at any point of the walk, and a config write landing
 * between two reads. Only the hardware beneath od_epd is faked. */

#include "od_epd.h"

#include <stdio.h>
#include <string.h>

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

/* ------------------------------------------------------------- recording io --- */

enum { OP_CMD = 1, OP_DATA, OP_RESET, OP_CS, OP_READ };

struct op {
    int     kind;
    uint8_t a;          /* cmd byte, cs index, pin_ref */
    uint8_t n;          /* data/read length */
    uint8_t d[16];
};

static struct op g_ops[64];
static unsigned g_nops;
static uint8_t  g_seq[2048];
static uint16_t g_seq_len;
static uint8_t  g_read_src[16];
static bool     g_cs2_wired;

static void rec(int kind, uint8_t a, const uint8_t *d, size_t n)
{
    if (g_nops >= sizeof g_ops / sizeof g_ops[0]) {
        return;
    }
    struct op *o = &g_ops[g_nops++];
    memset(o, 0, sizeof *o);
    o->kind = kind;
    o->a = a;
    o->n = (uint8_t)n;
    if (d != NULL) {
        memcpy(o->d, d, n < sizeof o->d ? n : sizeof o->d);
    }
}

static void io_cmd(uint8_t c)                    { rec(OP_CMD, c, NULL, 0u); }
static void io_data(const uint8_t *d, size_t n)  { rec(OP_DATA, 0u, d, n); }
static void io_reset(uint32_t ms)                { (void)ms; rec(OP_RESET, 0u, NULL, 0u); }
static void io_delay(uint32_t ms)                { (void)ms; }
static bool io_busy(void)                        { return true; }   /* idle level 1 */

/* Storage faults: the g_fail_call'th seq_read (1-based; 0 = never) returns 0 bytes, or one byte
 * short when g_fail_short is set, and
 * the g_bump_call'th bumps the config generation after it returns (a config write landing
 * between two reads). */
static unsigned g_read_calls;
static unsigned g_fail_call;
static bool     g_fail_short;
static unsigned g_bump_call;
static uint32_t g_gen;

static uint16_t io_seq_read(uint8_t mode, uint16_t off, uint8_t *dst, uint16_t len)
{
    (void)mode;
    ++g_read_calls;
    if ((uint32_t)off + len > g_seq_len) {
        return 0u;
    }
    memcpy(dst, &g_seq[off], len);
    if (g_read_calls == g_bump_call) {
        ++g_gen;
    }
    if (g_read_calls == g_fail_call) {
        return g_fail_short ? (uint16_t)(len - 1u) : 0u;
    }
    return len;
}

static uint32_t io_config_gen(void) { return g_gen; }

static od_epd_mode_t g_mode;

static bool io_mode_read(uint8_t index, od_epd_mode_t *out)
{
    if (index != 0u) {
        return false;
    }
    *out = g_mode;
    return true;
}

static bool io_cs_select(uint8_t cs)
{
    if (cs == 1u && !g_cs2_wired) {
        return false;
    }
    rec(OP_CS, cs, NULL, 0u);
    return true;
}

static bool io_read_bytes(uint8_t pin_ref, uint8_t *dst, uint8_t n)
{
    memcpy(dst, g_read_src, n);
    rec(OP_READ, pin_ref, NULL, n);
    return true;
}

static uint8_t g_lut[16];

static bool io_lut_info(uint8_t slot, uint16_t *len_out)
{
    if (slot != 0u) {
        return false;
    }
    *len_out = sizeof g_lut;
    return true;
}

static uint16_t io_lut_read(uint8_t slot, uint16_t off, uint8_t *dst, uint16_t len)
{
    if (slot != 0u || (uint32_t)off + len > sizeof g_lut) {
        return 0u;
    }
    memcpy(dst, &g_lut[off], len);
    return len;
}

static od_epd_io_t g_io;
static od_panel_desc_t g_desc;
static od_epd_t g_epd;

static void setup(const uint8_t *seq, uint16_t len, uint8_t passes, bool cs_hook, bool rd_hook)
{
    memset(g_ops, 0, sizeof g_ops);
    g_nops = 0u;
    g_read_calls = 0u;
    g_fail_call = 0u;
    g_fail_short = false;
    g_bump_call = 0u;
    g_gen = 7u;
    memcpy(g_seq, seq, len);
    g_seq_len = len;
    g_cs2_wired = true;
    for (unsigned k = 0; k < sizeof g_read_src; ++k) {
        g_read_src[k] = (uint8_t)(0xA0u + k);
    }

    memset(&g_io, 0, sizeof g_io);
    g_io.cmd = io_cmd;
    g_io.data = io_data;
    g_io.reset = io_reset;
    g_io.busy_level = io_busy;
    g_io.delay_ms = io_delay;
    g_io.seq_read = io_seq_read;
    g_io.mode_read = io_mode_read;
    g_io.cs_select = cs_hook ? io_cs_select : NULL;
    g_io.read_bytes = rd_hook ? io_read_bytes : NULL;

    memset(&g_desc, 0, sizeof g_desc);
    g_desc.width = 8u;
    g_desc.height = 8u;
    g_desc.busy_usable = true;
    g_desc.busy_idle_level = 1u;
    g_desc.busy_timeout_ms = 100u;
    g_desc.mode_count = 1u;

    memset(&g_mode, 0, sizeof g_mode);
    g_mode.passes = passes;
    g_mode.seq_len = len;
    memset(g_mode.lut_slot, 0xFF, sizeof g_mode.lut_slot);

    CHECK(od_epd_init(&g_epd, &g_desc, &g_io) == OD_EPD_RC_OK);
    CHECK(od_epd_select_mode(&g_epd, 0u) == OD_EPD_RC_OK);
}

static unsigned count_kind(int kind)
{
    unsigned n = 0u;
    for (unsigned k = 0; k < g_nops; ++k) {
        n += (g_ops[k].kind == kind) ? 1u : 0u;
    }
    return n;
}

/* ------------------------------------------------------------------ cases --- */

static void test_refused_escapes_execute_nothing(void)
{
    static const uint8_t cs_seq[] = { 0x00, 0x08, 0x01, 0x01, 0x12, 0x00, 0x03, 0x00, 0x00 };
    static const uint8_t rd_seq[] = { 0x01, 0x70, 0x00, 0x09, 0x14, 0x02, 0x00, 0x03, 0x00, 0x00 };

    CASE("CS_SELECT with no cs_select hook is refused before any panel I/O");
    setup(cs_seq, sizeof cs_seq, 1u, false, true);
    CHECK(od_epd_begin_pass(&g_epd, 0u) == OD_EPD_RC_UNSUP);
    CHECK(g_nops == 0u);

    CASE("READ_SCRATCH with no read_bytes hook is refused before any panel I/O");
    setup(rd_seq, sizeof rd_seq, 1u, true, false);
    CHECK(od_epd_begin_pass(&g_epd, 0u) == OD_EPD_RC_UNSUP);
    CHECK(g_nops == 0u);
}

static void test_malformed_args_are_seq_errors(void)
{
    static const uint8_t cs_bad[]  = { 0x00, 0x08, 0x02, 0x00, 0x03, 0x00, 0x00 };
    static const uint8_t rd_zero[] = { 0x00, 0x09, 0x14, 0x00, 0x00, 0x03, 0x00, 0x00 };
    static const uint8_t rd_big[]  = { 0x00, 0x09, 0x14, 0x11, 0x00, 0x03, 0x00, 0x00 };
    static const uint8_t wr_big[]  = { 0x00, 0x0A, 0x30, 0x0F, 0x02, 0x00, 0x03, 0x00, 0x00 };

    CASE("CS_SELECT index > 1 is malformed");
    setup(cs_bad, sizeof cs_bad, 1u, true, true);
    CHECK(od_epd_begin_pass(&g_epd, 0u) == OD_EPD_RC_SEQ);
    CHECK(g_nops == 0u);

    CASE("READ_SCRATCH of 0 bytes is malformed");
    setup(rd_zero, sizeof rd_zero, 1u, true, true);
    CHECK(od_epd_begin_pass(&g_epd, 0u) == OD_EPD_RC_SEQ);

    CASE("READ_SCRATCH past the 16-byte scratch is malformed");
    setup(rd_big, sizeof rd_big, 1u, true, true);
    CHECK(od_epd_begin_pass(&g_epd, 0u) == OD_EPD_RC_SEQ);

    CASE("WRITE_SCRATCH whose slice runs past the scratch is malformed");
    setup(wr_big, sizeof wr_big, 1u, true, true);
    CHECK(od_epd_begin_pass(&g_epd, 0u) == OD_EPD_RC_SEQ);
    CHECK(g_nops == 0u);
}

static void test_read_then_write_scratch(void)
{
    /* cmd 0x70 (a read-back register), READ 3 bytes from DATA_PIN, then send cmd 0x30 with
     * scratch[1..3) -- the UC8159 EEPROM -> register shape. */
    static const uint8_t seq[] = {
        0x01, 0x70,
        0x00, 0x09, 0x14, 0x03,
        0x00, 0x0A, 0x30, 0x01, 0x02,
        0x00, 0x03,
        0x00, 0x00,
    };

    CASE("READ_SCRATCH lands bytes that WRITE_SCRATCH then sends as data");
    setup(seq, sizeof seq, 1u, true, true);
    CHECK(od_epd_begin_pass(&g_epd, 0u) == OD_EPD_RC_OK);
    CHECK(count_kind(OP_READ) == 1u);
    {
        unsigned k = 0u;
        while (k < g_nops && g_ops[k].kind != OP_READ) { ++k; }
        CHECK(k < g_nops && g_ops[k].a == 0x14u && g_ops[k].n == 3u);
        CHECK(k + 2u < g_nops);
        CHECK(g_ops[k + 1u].kind == OP_CMD && g_ops[k + 1u].a == 0x30u);
        CHECK(g_ops[k + 2u].kind == OP_DATA && g_ops[k + 2u].n == 2u);
        CHECK(g_ops[k + 2u].d[0] == 0xA1u && g_ops[k + 2u].d[1] == 0xA2u);
    }
}

static void test_scratch_is_zeroed_per_pass(void)
{
    /* Pass 0 reads into scratch; pass 1 writes from it without reading. The write must see
     * zeros: a pass may not inherit the previous pass's read-back. */
    static const uint8_t seq[] = {
        0x00, 0x0A, 0x31, 0x00, 0x02,
        0x00, 0x09, 0x14, 0x02,
        0x00, 0x03,
        0x00, 0x00,
    };
    unsigned k;

    CASE("scratch starts zeroed at every pass");
    setup(seq, sizeof seq, 2u, true, true);
    CHECK(od_epd_begin_pass(&g_epd, 0u) == OD_EPD_RC_OK);
    CHECK(od_epd_finish_pass(&g_epd) == OD_EPD_RC_OK);
    g_nops = 0u;
    CHECK(od_epd_begin_pass(&g_epd, 1u) == OD_EPD_RC_OK);
    for (k = 0u; k < g_nops && !(g_ops[k].kind == OP_CMD && g_ops[k].a == 0x31u); ++k) { }
    CHECK(k + 1u < g_nops);
    CHECK(g_ops[k + 1u].kind == OP_DATA && g_ops[k + 1u].n == 2u);
    CHECK(g_ops[k + 1u].d[0] == 0u && g_ops[k + 1u].d[1] == 0u);
}

static void test_cs_select_is_sticky_and_resets_per_pass(void)
{
    static const uint8_t seq[] = {
        0x01, 0x10,               /* cmd 0x10 on CS 0 */
        0x00, 0x08, 0x01,         /* select cs_pin_2 */
        0x01, 0x11,               /* cmd 0x11 on CS 1 */
        0x00, 0x03,
        0x00, 0x00,
    };

    CASE("every pass begins by selecting CS 0, and CS_SELECT holds until changed");
    setup(seq, sizeof seq, 2u, true, true);
    CHECK(od_epd_begin_pass(&g_epd, 0u) == OD_EPD_RC_OK);
    CHECK(g_nops >= 4u);
    CHECK(g_ops[0].kind == OP_CS && g_ops[0].a == 0u);
    {
        unsigned k;
        for (k = 0u; k < g_nops && !(g_ops[k].kind == OP_CS && g_ops[k].a == 1u); ++k) { }
        CHECK(k + 1u < g_nops && g_ops[k + 1u].kind == OP_CMD && g_ops[k + 1u].a == 0x11u);
    }
    CHECK(od_epd_finish_pass(&g_epd) == OD_EPD_RC_OK);
    g_nops = 0u;
    CHECK(od_epd_begin_pass(&g_epd, 1u) == OD_EPD_RC_OK);
    CHECK(g_nops >= 1u && g_ops[0].kind == OP_CS && g_ops[0].a == 0u);

    CASE("CS_SELECT 1 on a board whose cs_pin_2 the hook rejects stops the pass");
    setup(seq, sizeof seq, 1u, true, true);
    g_cs2_wired = false;
    CHECK(od_epd_begin_pass(&g_epd, 0u) == OD_EPD_RC_UNSUP);
}

static void test_lut_slice_after_image(void)
{
    /* Head: 0x10 + IMAGE. Tail: waveform bytes 8..12 into 0x32, then refresh 0x12. The LUT
     * context must survive the pause at the image marker. */
    static const uint8_t seq[] = {
        0x01, 0x10, 0x00, 0x03,
        0x01, 0x32, 0x00, 0x0B, 0x08, 0x00, 0x04, 0x00,
        0x01, 0x12,
        0x00, 0x00,
    };
    static const uint8_t img[4] = { 1, 2, 3, 4 };
    unsigned k;

    CASE("LUT_SLICE after the image marker streams the pass's slot");
    for (k = 0u; k < sizeof g_lut; ++k) {
        g_lut[k] = (uint8_t)(0x40u + k);
    }
    setup(seq, sizeof seq, 1u, false, false);
    g_io.lut_info = io_lut_info;
    g_io.lut_read = io_lut_read;
    g_mode.lut_slot[0] = 0u;
    CHECK(od_epd_select_mode(&g_epd, 0u) == OD_EPD_RC_OK);
    CHECK(od_epd_begin_pass(&g_epd, 0u) == OD_EPD_RC_OK);
    CHECK(od_epd_write_image_chunk(&g_epd, img, sizeof img) == OD_EPD_RC_OK);
    g_nops = 0u;
    CHECK(od_epd_finish_pass(&g_epd) == OD_EPD_RC_OK);
    for (k = 0u; k < g_nops && !(g_ops[k].kind == OP_CMD && g_ops[k].a == 0x32u); ++k) { }
    CHECK(k + 1u < g_nops && g_ops[k + 1u].kind == OP_DATA && g_ops[k + 1u].n == 4u);
    CHECK(k + 1u < g_nops && g_ops[k + 1u].d[0] == 0x48u && g_ops[k + 1u].d[3] == 0x4Bu);
    CHECK(count_kind(OP_CMD) == 2u);
}

/* A sequence longer than the window with one step longer than it, an image pause and a tail:
 *   [1][0x10]  [201][0x32][200 B]  [3][0x11][a][b]  IMAGE  [1][0x12]  BUSY  [2][0x13][x] x12  END
 * and a two-byte sleep sequence [2][0x07][0xA5] END. */
#define LONG_STEP 200u
static uint16_t build_long(uint8_t *out)
{
    uint16_t n = 0u;

    out[n++] = 1u; out[n++] = 0x10u;
    out[n++] = (uint8_t)(LONG_STEP + 1u); out[n++] = 0x32u;
    for (unsigned k = 0u; k < LONG_STEP; ++k) {
        out[n++] = (uint8_t)k;
    }
    out[n++] = 3u; out[n++] = 0x11u; out[n++] = 0xAAu; out[n++] = 0xBBu;
    out[n++] = 0u; out[n++] = 3u;
    out[n++] = 1u; out[n++] = 0x12u;
    out[n++] = 0u; out[n++] = 1u;
    for (unsigned k = 0u; k < 12u; ++k) {
        out[n++] = 2u; out[n++] = 0x13u; out[n++] = (uint8_t)k;
    }
    out[n++] = 0u; out[n++] = 0u;
    return n;
}

static const uint8_t SLEEP_SEQ[] = { 2u, 0x07u, 0xA5u, 0u, 0u };

static void setup_long(void)
{
    static uint8_t seq[300];
    const uint16_t len = build_long(seq);

    setup(seq, len, 1u, false, false);
    memcpy(g_desc.sleep_seq, SLEEP_SEQ, sizeof SLEEP_SEQ);
    g_desc.sleep_seq_len = sizeof SLEEP_SEQ;
    g_io.config_gen = io_config_gen;
}

static unsigned count_cmd(uint8_t c)
{
    unsigned n = 0u;
    for (unsigned k = 0; k < g_nops; ++k) {
        n += (g_ops[k].kind == OP_CMD && g_ops[k].a == c) ? 1u : 0u;
    }
    return n;
}

/* begin, one image chunk, finish, then the caller's sleep -- the od_display order, whatever the
 * outcome. Returns the first failure, or OD_OK. */
static int drive_long(void)
{
    static const uint8_t img[4] = { 9, 9, 9, 9 };
    int rc = od_epd_begin_pass(&g_epd, 0u);

    if (rc == OD_EPD_RC_OK) {
        rc = od_epd_write_image_chunk(&g_epd, img, sizeof img);
    }
    if (rc == OD_EPD_RC_OK) {
        rc = od_epd_finish_pass(&g_epd);
    }
    (void)od_epd_sleep(&g_epd);
    return rc;
}

/* The walk's outcome after a storage fault: either refused before any panel I/O (nothing on the
 * bus but the caller's sleep, which also runs on a clean pass), or the panel was put to sleep
 * exactly once, as the last thing on the bus, and the refresh never ran. */
static unsigned g_pre_io;
static unsigned g_post_io;

static void check_failed_cleanly(int rc, int want_rc)
{
    CHECK(rc == want_rc);
    CHECK(count_cmd(0x07u) == 1u);
    CHECK(g_nops >= 2u && g_ops[g_nops - 2u].kind == OP_CMD && g_ops[g_nops - 2u].a == 0x07u);
    CHECK(g_ops[g_nops - 1u].kind == OP_DATA && g_ops[g_nops - 1u].d[0] == 0xA5u);
    CHECK(count_cmd(0x12u) == 0u || count_cmd(0x13u) < 12u);
    if (count_kind(OP_RESET) == 0u) {
        CHECK(g_nops == 2u);                 /* refused before I/O: only the caller's sleep */
        ++g_pre_io;
    } else {
        ++g_post_io;
    }
}

static void test_long_step_is_chunked(void)
{
    unsigned k;
    unsigned got = 0u;

    CASE("a step longer than the window goes out as one command and window-sized data runs");
    setup_long();
    CHECK(drive_long() == OD_EPD_RC_OK);
    for (k = 0u; k < g_nops && !(g_ops[k].kind == OP_CMD && g_ops[k].a == 0x32u); ++k) { }
    CHECK(k < g_nops);
    for (++k; k < g_nops && g_ops[k].kind == OP_DATA; ++k) {
        CHECK(g_ops[k].n <= OD_EPD_SEQ_WINDOW);
        CHECK(g_ops[k].d[0] == (uint8_t)got);
        got += g_ops[k].n;
    }
    CHECK(got == LONG_STEP);
    CHECK(count_cmd(0x13u) == 12u && count_cmd(0x12u) == 1u && count_cmd(0x07u) == 1u);
}

static void test_read_fault_at_every_call(void)
{
    unsigned clean_calls;
    char name[96];

    CASE("the long sequence needs several reads in validation and again in the walk");
    setup_long();
    CHECK(drive_long() == OD_EPD_RC_OK);
    clean_calls = g_read_calls;
    CHECK(clean_calls >= 4u);

    g_pre_io = g_post_io = 0u;
    for (unsigned short_by = 0u; short_by < 2u; ++short_by) {
        for (unsigned call = 1u; call <= clean_calls; ++call) {
            snprintf(name, sizeof name, "seq_read %u of %u %s", call, clean_calls,
                     short_by ? "returns one byte short" : "fails");
            CASE(name);
            setup_long();
            g_fail_call = call;
            g_fail_short = short_by != 0u;
            check_failed_cleanly(drive_long(), OD_EPD_RC_IO);
        }
    }
    CASE("read faults landed both before the first panel I/O and mid-walk");
    CHECK(g_pre_io > 0u && g_post_io > 0u);
}

static void test_config_write_mid_pass(void)
{
    unsigned clean_calls;
    char name[96];

    CASE("a config write during the image pause fails the pass and sleeps the panel once");
    setup_long();
    CHECK(od_epd_begin_pass(&g_epd, 0u) == OD_EPD_RC_OK);
    ++g_gen;
    CHECK(od_epd_finish_pass(&g_epd) == OD_EPD_RC_STALE);
    CHECK(count_cmd(0x12u) == 0u);
    CHECK(od_epd_sleep(&g_epd) == OD_EPD_RC_OK);
    CHECK(count_cmd(0x07u) == 1u);
    CHECK(g_ops[g_nops - 2u].kind == OP_CMD && g_ops[g_nops - 2u].a == 0x07u);

    CASE("the next pass after a stale one runs normally");
    g_nops = 0u;
    CHECK(drive_long() == OD_EPD_RC_OK);
    CHECK(count_cmd(0x12u) == 1u && count_cmd(0x07u) == 1u);

    setup_long();
    CHECK(drive_long() == OD_EPD_RC_OK);
    clean_calls = g_read_calls;
    g_pre_io = g_post_io = 0u;
    for (unsigned call = 1u; call < clean_calls; ++call) {
        snprintf(name, sizeof name, "config generation moves after seq_read %u of %u", call,
                 clean_calls);
        CASE(name);
        setup_long();
        g_bump_call = call;
        check_failed_cleanly(drive_long(), OD_EPD_RC_STALE);
    }
    CASE("config writes landed both before the first panel I/O and mid-walk");
    CHECK(g_pre_io > 0u && g_post_io > 0u);
}

int main(void)
{
    test_refused_escapes_execute_nothing();
    test_malformed_args_are_seq_errors();
    test_read_then_write_scratch();
    test_scratch_is_zeroed_per_pass();
    test_cs_select_is_sticky_and_resets_per_pass();
    test_lut_slice_after_image();
    test_long_step_is_chunked();
    test_read_fault_at_every_call();
    test_config_write_mid_pass();
    printf("epd_seq: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
