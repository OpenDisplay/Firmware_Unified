/* od_panel_fastepd -- the FastEPD backend of od_hal_panel.
 *
 * A THIN adapter over src/display_fastepd.cpp, deliberately: that file is the working IT8951 /
 * Inkplate driver and MIGRATION.md's "rewriting working drivers" is an explicit non-goal. What
 * this file adds is only what the HAL contract requires and the driver does not provide --
 * chiefly the non-blocking refresh (correction 3) and symmetric error returns (correction 4).
 *
 * BUFFERED BACKEND: FastEPD holds a full frame in PSRAM and blits it, so caps.needs_framebuffer
 * is true and a partial update can be composed from what is already there. That is the opposite
 * of the bb_epaper backend and is exactly why the caps query exists.
 */
#include "od_hal_panel.h"

#if defined(OPENDISPLAY_FASTEPD)

#include "display_fastepd.h"
#include "opendisplay_structs.h"
#include "protocol_pending.h"
#include "od_log.h"

/* Session state. Single-session by construction: the loop task is the only writer, and the
 * core never opens two at once. */
static bool s_full_open    = false;
static bool s_region_open  = false;
static bool s_refreshing   = false;
static bool s_init_ok      = false;

/* The panel families FastEPD drives: the IT8951 SPI path (Seeed reTerminal ED103TC2) and the
 * native parallel path (Soldered Inkplate, M5Stack PaperS3). display_technology 0 or 1 is also required -- a
 * matching IC on another technology is a panel FastEPD cannot drive. Kept identical to
 * fastepd_driver_used() in display_service.cpp, which is still the live predicate until the
 * repoint; if one changes, both must. */
static bool fastepd_ops_claims(uint16_t panel_ic_type, uint8_t display_technology)
{
    const bool it8951 = (panel_ic_type == OD_PANEL_IC_ED103TC2_1872X1404 ||
                         panel_ic_type == OD_PANEL_IC_ED103TC2_1872X1404_4GRAY ||
                         panel_ic_type == OD_PANEL_IC_M5PAPER_960X540);
    const bool parallel = (panel_ic_type == OD_PANEL_IC_INKPLATE5V2_1280X720 ||
                           panel_ic_type == OD_PANEL_IC_INKPLATE10_1200X825 ||
                           panel_ic_type == OD_PANEL_IC_M5PAPERS3_960X540);
    if (!it8951 && !parallel) return false;
    return (display_technology == 0 || display_technology == 1);
}

static int fastepd_ops_init(const struct DisplayConfig *d, const struct SystemConfig *sys,
                            uint16_t panel_ic_type, od_panel_caps_t *caps_out)
{
    if (d == NULL || sys == NULL || caps_out == NULL) {
        return OD_PANEL_EINVAL;
    }
    opendisplay_fastepd_load_pins_from_display(d, sys, panel_ic_type);
    if (fastepd_init_failed()) {
        od_log_error("panel: FastEPD init failed");
        s_init_ok = false;
        return OD_PANEL_ERR;
    }
    fastepd_prepare_hardware();
    fastepd_epaper_begin();

    caps_out->width       = d->pixel_width;
    caps_out->height      = d->pixel_height;
    /* 4-gray is a distinct IT8951 mode, not a colour scheme, which is why it is keyed on the
     * panel IC rather than on d->color_scheme like every other format below. */
    caps_out->fmt         = (panel_ic_type == OD_PANEL_IC_ED103TC2_1872X1404_4GRAY)
                                ? OD_PIX_4GRAY : OD_PIX_1BPP;
    caps_out->plane_count = (caps_out->fmt == OD_PIX_4GRAY) ? 2 : 1;
    caps_out->needs_framebuffer = true;
    /* fastepd_partial_* exists and works on both FastEPD paths. */
    caps_out->supports_partial  = true;
    s_init_ok = true;
    return OD_PANEL_OK;
}

static int fastepd_ops_begin(void)
{
    if (!s_init_ok) return OD_PANEL_ERR;
    fastepd_direct_write_reset();
    s_full_open   = true;
    s_region_open = false;
    return OD_PANEL_OK;
}

static int fastepd_ops_begin_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (!s_init_ok) return OD_PANEL_ERR;
    fastepd_partial_prepare(x, y, w, h);
    s_region_open = true;
    s_full_open   = false;
    return OD_PANEL_OK;
}

static int fastepd_ops_write(const uint8_t *bytes, uint32_t len)
{
    if (bytes == NULL || len == 0) return OD_PANEL_EINVAL;
    if (s_region_open) {
        /* Already returns bool -- the half of FastEPD that reports failure. */
        return fastepd_partial_write_chunk(bytes, len) ? OD_PANEL_OK : OD_PANEL_ERR;
    }
    if (s_full_open) {
        /* CORRECTION 4 BITES HERE: fastepd_direct_write_chunk() returns void and silently
         * truncates when the frame buffer is full. There is no status to forward, so this
         * cannot manufacture one -- it reports OK and the truncation stays invisible.
         * Giving that function a return type is the real fix and belongs with the repoint of
         * display_fastepd.cpp; recorded here so the gap is findable rather than assumed
         * closed by the int in this signature. */
        fastepd_direct_write_chunk(bytes, len);
        return OD_PANEL_OK;
    }
    return OD_PANEL_EINVAL;   /* no session open */
}

static int fastepd_ops_refresh_start(od_refresh_t mode)
{
    if (!s_init_ok) return OD_PANEL_ERR;
    if (s_region_open) {
        bool ok = fastepd_partial_refresh((int)mode);
        s_region_open = false;
        s_refreshing  = ok;
        return ok ? OD_PANEL_OK : OD_PANEL_ERR;
    }
    fastepd_direct_refresh((int)mode);
    s_full_open  = false;
    s_refreshing = true;
    return OD_PANEL_OK;
}

static bool fastepd_ops_refresh_busy(void)
{
    if (!s_refreshing) {
        return false;
    }
    /* THE CONTRACT'S NON-BLOCKING REFRESH IS NOT FULLY HONOURED YET, and pretending otherwise
     * would be worse than saying so. FastEPD exposes only fastepd_wait_refresh(timeout_sec),
     * which BLOCKS; there is no "is it done" query to poll. Called with a 0 s timeout it
     * degenerates to a single non-blocking check on the IT8951 path, which is what this does --
     * but the Inkplate parallel path completes inside fastepd_direct_refresh() itself, so it is
     * already idle by the time anyone asks.
     *
     * The real fix is a busy query in display_fastepd.cpp (it8951WaitForReady already polls a
     * ready bit; that predicate needs exposing rather than inventing). Until then this reports
     * "done" as soon as the driver's own check passes, which is correct but coarser than the
     * contract promises. */
    if (fastepd_wait_refresh(0)) {
        s_refreshing = false;
        return false;
    }
    return true;
}

static void fastepd_ops_sleep(void)
{
    fastepd_sleep_after_refresh();
    fastepd_direct_sleep();
    s_refreshing = false;
}

static void fastepd_ops_abort(void)
{
    /* No teardown call to make: FastEPD's write state is reset by the next
     * direct_write_reset() / partial_prepare(). Dropping the session flags is the whole of it,
     * and it must be idempotent because the abort path can run twice. */
    s_full_open   = false;
    s_region_open = false;
    s_refreshing  = false;
}

static void fastepd_ops_mark_deinitialized(void)
{
    fastepd_mark_hw_deinitialized();
    s_full_open   = false;
    s_region_open = false;
    s_refreshing  = false;
    s_init_ok     = false;
}

extern "C" const struct od_panel_ops od_panel_ops_fastepd = {
    "fastepd",
    fastepd_ops_claims,
    fastepd_ops_init,
    fastepd_ops_begin,
    fastepd_ops_begin_region,
    fastepd_ops_write,
    fastepd_ops_refresh_start,
    fastepd_ops_refresh_busy,
    fastepd_ops_sleep,
    fastepd_ops_abort,
    fastepd_ops_mark_deinitialized,
};

#endif /* OPENDISPLAY_FASTEPD */
