/* targets/efr32bg22-slc/opendisplay_display.cpp: a draw from a stored image slot (0x004B).
 *
 * The slot holds exactly the 0x0070 byte stream for this panel. On a two-plane scheme (BWR,
 * BWY, GRAY4: OD_COLOR_LAYOUT_CONTROLLER_PLANES) that stream is plane 0 then plane 1, and the
 * panel needs the shim to advance to the second image marker between them, or plane 1 lands
 * after plane 0's data command and the glass shows the wrong image under a success ACK. The
 * fakes below record where each byte went; the panel shim, the image store and the target
 * glue are fakes, the display file is the production one. */

#include "opendisplay_display.h"
#include "od_epd_shim.h"
#include "od_imgstore.h"
#include "od_color.h"
#include "od_cmd_flash.h"
#include "opendisplay_constants.h"
#include "opendisplay_runtime.h"

#include <stdio.h>
#include <string.h>

static unsigned s_checks, s_failures;

static void check(int condition, const char *what)
{
    ++s_checks;
    if (!condition) {
        ++s_failures;
        printf("FAIL: %s\n", what);
    }
}

/* ---- the configured panel ---- */

static struct GlobalConfig s_cfg;

const struct GlobalConfig *opendisplay_get_global_config(void) { return &s_cfg; }

/* ---- panel shim fake: bytes per plane, in order ---- */

#define MAX_STREAM 4096u
static uint8_t s_plane_bytes[2][MAX_STREAM];
static uint32_t s_plane_len[2];
static unsigned s_plane;            /* 0 = first image marker, 1 = second */
static unsigned s_next_plane_calls, s_finish_calls, s_begin_calls;
static bool s_in_pass;

bool od_epd_shim_available(const struct DisplayConfig *d) { (void)d; return true; }
bool od_epd_shim_begin(uint8_t display_instance, uint8_t mode_index, uint8_t rotation_quarters,
                       uint8_t dc, uint8_t reset, uint8_t busy, uint8_t cs, uint8_t data,
                       uint8_t clk)
{
    (void)display_instance; (void)mode_index; (void)rotation_quarters; (void)dc; (void)reset;
    (void)busy; (void)cs; (void)data; (void)clk;
    ++s_begin_calls;
    s_plane = 0u;
    s_in_pass = true;
    return true;
}
bool od_epd_shim_write(const uint8_t *data, uint32_t len)
{
    if (!s_in_pass || s_plane_len[s_plane] + len > MAX_STREAM) {
        return false;
    }
    memcpy(&s_plane_bytes[s_plane][s_plane_len[s_plane]], data, len);
    s_plane_len[s_plane] += len;
    return true;
}
uint8_t od_epd_shim_image_of(void) { return 1u; }
bool od_epd_shim_next_plane(void)
{
    ++s_next_plane_calls;
    if (s_plane != 0u) {
        return false;               /* the sequence declares two image markers */
    }
    s_plane = 1u;
    return true;
}
bool od_epd_shim_finish(void) { ++s_finish_calls; s_in_pass = false; return true; }
void od_epd_shim_sleep(void) { s_in_pass = false; }
bool od_epd_shim_drive_begin(uint8_t display_instance, uint8_t mode_index,
                             uint8_t rotation_quarters, uint8_t dc, uint8_t reset, uint8_t busy,
                             uint8_t cs, uint8_t data, uint8_t clk, od_epd_shim_mode_t *mode_out)
{
    (void)display_instance; (void)mode_index; (void)rotation_quarters; (void)dc; (void)reset;
    (void)busy; (void)cs; (void)data; (void)clk; (void)mode_out;
    return false;
}
bool od_epd_shim_reselect(uint8_t mode_index, od_epd_shim_mode_t *mode_out)
{
    (void)mode_index; (void)mode_out;
    return false;
}
bool od_epd_shim_begin_pass(uint8_t pass) { (void)pass; return false; }
int od_epd_shim_last_rc(void) { return 0; }
bool od_epd_shim_raw_begin(uint8_t display_instance, uint8_t rotation_quarters, uint8_t dc,
                           uint8_t reset, uint8_t busy, uint8_t cs, uint8_t data, uint8_t clk,
                           bool run_panel_init)
{
    (void)display_instance; (void)rotation_quarters; (void)dc; (void)reset; (void)busy;
    (void)cs; (void)data; (void)clk; (void)run_panel_init;
    return false;
}
void od_epd_shim_raw_cmd(uint8_t cmd, const uint8_t *data, uint32_t len)
{
    (void)cmd; (void)data; (void)len;
}
void od_epd_shim_raw_data(const uint8_t *data, uint32_t len) { (void)data; (void)len; }
void od_epd_shim_raw_busy(uint32_t timeout_ms, uint16_t *elapsed_ms_out, bool *still_busy_out)
{
    (void)timeout_ms; (void)elapsed_ms_out; (void)still_busy_out;
}
bool od_epd_shim_raw_reset(uint8_t hold_ms) { (void)hold_ms; return false; }

/* ---- image store fake: one valid slot ---- */

static uint8_t s_stored[MAX_STREAM];
static od_imgstore_slot_t s_slot;

bool od_imgstore_slot(uint8_t index, od_imgstore_slot_t *out)
{
    if (index != 0u) {
        return false;
    }
    *out = s_slot;
    return true;
}
bool od_imgstore_read(uint8_t index, uint32_t offset, uint8_t *dst, uint32_t len)
{
    if (index != 0u || offset > s_slot.payload_len || len > s_slot.payload_len - offset) {
        return false;
    }
    memcpy(dst, &s_stored[offset], len);
    return true;
}

/* ---- the scenario ---- */

static void set_panel(uint8_t scheme, uint16_t w, uint16_t h)
{
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.display_count = 1u;
    s_cfg.displays[0].pixel_width = w;
    s_cfg.displays[0].pixel_height = h;
    s_cfg.displays[0].color_scheme = scheme;
    s_cfg.system_config.pwr_pin = GPIO_PIN_UNUSED;
}

static uint32_t store_frame(uint8_t scheme, uint16_t w, uint16_t h)
{
    od_color_geometry_t g;

    if (od_color_direct_geometry(scheme, w, h, &g) != OD_COLOR_OK || g.total_bytes > MAX_STREAM) {
        return 0u;
    }
    for (uint32_t i = 0u; i < g.total_bytes; ++i) {
        s_stored[i] = (uint8_t)(i * 7u + 3u);
    }
    memset(&s_slot, 0, sizeof s_slot);
    s_slot.state = OD_SLOT_VALID;
    s_slot.width = w;
    s_slot.height = h;
    s_slot.colour_scheme = scheme;
    s_slot.payload_len = g.total_bytes;
    memset(s_plane_len, 0, sizeof s_plane_len);
    s_next_plane_calls = s_finish_calls = s_begin_calls = 0u;
    return g.layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES ? g.part_bytes[0] : 0u;
}

static void draw_slot0(void)
{
    check(od_display_from_slot_arm(0u), "slot 0 is armed");
    opendisplay_display_service();
}

static void test_two_plane_slot_draw(uint8_t scheme, const char *name)
{
    /* 200 x 40 at 1 bpp: 1000 B per plane, so plane 1 starts inside a 256-byte read. */
    set_panel(scheme, 200u, 40u);
    const uint32_t plane = store_frame(scheme, 200u, 40u);

    check(plane == 1000u, name);
    draw_slot0();
    check(s_begin_calls == 1u && s_finish_calls == 1u, "one panel pass, refreshed once");
    check(s_next_plane_calls == 1u, "the shim advances to the second image marker once");
    check(s_plane_len[0] == plane && s_plane_len[1] == plane, "each plane gets its own bytes");
    check(memcmp(s_plane_bytes[0], s_stored, plane) == 0, "plane 0 is the first half");
    check(memcmp(s_plane_bytes[1], s_stored + plane, plane) == 0, "plane 1 is the second half");
}

static void test_single_plane_slot_draw_unchanged(void)
{
    /* BWRY: packed rows, one plane -- the case hardware verified on 2026-11-13. */
    set_panel(OD_COLOR_SCHEME_BWRY, 200u, 40u);
    (void)store_frame(OD_COLOR_SCHEME_BWRY, 200u, 40u);
    draw_slot0();
    check(s_next_plane_calls == 0u, "a single-plane frame never advances the plane");
    check(s_plane_len[0] == s_slot.payload_len && s_plane_len[1] == 0u,
          "a single-plane frame is streamed whole");
    check(memcmp(s_plane_bytes[0], s_stored, s_slot.payload_len) == 0,
          "a single-plane frame is streamed byte for byte");
    check(s_finish_calls == 1u, "a single-plane frame is refreshed once");
}

static void test_slot_draw_leaves_display_idle(void)
{
    set_panel(OD_COLOR_SCHEME_BWR, 200u, 40u);
    (void)store_frame(OD_COLOR_SCHEME_BWR, 200u, 40u);
    draw_slot0();
    /* A second arm is only accepted when the transfer state was put back to idle. */
    check(od_display_from_slot_arm(0u), "the display is idle again after a slot draw");
    opendisplay_display_service();
}

int main(void)
{
    test_two_plane_slot_draw(OD_COLOR_SCHEME_BWR, "BWR is two 1000-byte planes");
    test_two_plane_slot_draw(OD_COLOR_SCHEME_BWY, "BWY is two 1000-byte planes");
    test_two_plane_slot_draw(OD_COLOR_SCHEME_GRAY4, "GRAY4 is two 1000-byte planes");
    test_single_plane_slot_draw_unchanged();
    test_slot_draw_leaves_display_idle();

    printf("silabs slot draw: %u checks, %u failures\n", s_checks, s_failures);
    return s_failures == 0u ? 0 : 1;
}
