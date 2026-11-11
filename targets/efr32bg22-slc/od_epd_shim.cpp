/* od_epd_shim.cpp -- see od_epd_shim.h. The ONLY translation unit that sees the adapter. */
#include "od_epd_shim.h"

#include "od_cfg_index.h"
#include "od_config_store.h"
#include "od_epd_adapter.h"
#include "od_log.h"
#include "opendisplay_structs.h"

#include "sl_sleeptimer.h"

static OdEpdAdapter s_epd;
static bool s_in_pass;

/* A pass streams its sequence from the stored config and pauses at the image marker while the
 * frame arrives, so a config write is refused for exactly that span. */
static void set_in_pass(bool on)
{
    s_in_pass = on;
    od_config_store_hold_writes(on);
}

bool od_epd_shim_available(const struct DisplayConfig *d)
{
    /* Matched by the body's instance_number, first 0xB1 wins -- the same packet
     * od_epd_desc_from_config() loads -- and mode 0 must serve a legacy upload. */
    return d != NULL && od_cfg_has_drive_data(d->instance_number, d->color_scheme);
}

bool od_epd_shim_begin(uint8_t display_instance, uint8_t mode_index,
                       uint8_t rotation_quarters,
                       uint8_t dc, uint8_t rst, uint8_t busy, uint8_t cs,
                       uint8_t mosi, uint8_t sck)
{
    set_in_pass(false);

    if (s_epd.setPanelType(display_instance) != BBEP_SUCCESS) {
        od_log_warn("od_epd: no drive data for display %u", (unsigned)display_instance);
        return false;
    }
    s_epd.setRotation((int)rotation_quarters * 90);
    s_epd.initIO(dc, rst, busy, cs, mosi, sck, 0u);

    if (s_epd.selectModeIndex(mode_index) != BBEP_SUCCESS) {
        od_log_warn("od_epd: mode %u not in config (have %u)",
                    (unsigned)mode_index, (unsigned)s_epd.modeCount());
        return false;
    }
    if (s_epd.beginPass(0u) != BBEP_SUCCESS) {
        od_log_warn("od_epd: beginPass failed rc=%d", s_epd.lastRc());
        return false;
    }
    set_in_pass(true);
    return true;
}

bool od_epd_shim_write(const uint8_t *data, uint32_t len)
{
    if (!s_in_pass || data == NULL || len == 0u) {
        return false;
    }
    s_epd.writeData(data, (int)len);
    return true;
}

bool od_epd_shim_next_plane(void)
{
    if (!s_in_pass) {
        return false;
    }
    if (s_epd.nextPlane() != BBEP_SUCCESS) {
        set_in_pass(false);        /* the walk is over; nothing reads the config any more */
        return false;
    }
    return true;
}

bool od_epd_shim_finish(void)
{
    if (!s_in_pass) {
        return false;
    }
    const bool ok = s_epd.finishPass() == BBEP_SUCCESS;   /* the tail is read from storage */
    set_in_pass(false);
    return ok;
}

void od_epd_shim_sleep(void)
{
    set_in_pass(false);
    if (s_epd.ready()) {
        s_epd.sleep(DEEP_SLEEP);
    }
}

/* --- config-driven drive path (CMD_DRIVE_WRITE_START) ----------------------------------------- */

static void mode_header_out(od_epd_shim_mode_t *out)
{
    if (out == NULL) {
        return;
    }
    const od_epd_mode_t &m = s_epd.activeMode();
    out->passes       = m.passes != 0u ? m.passes : 1u;   /* a mode declaring 0 still runs once */
    out->color_scheme = m.color_scheme;
    out->temp_min_c   = m.temp_min_c;
    out->temp_max_c   = m.temp_max_c;
    out->temp_policy  = m.temp_policy;
}

bool od_epd_shim_drive_begin(uint8_t display_instance, uint8_t mode_index,
                            uint8_t rotation_quarters,
                            uint8_t dc, uint8_t rst, uint8_t busy, uint8_t cs,
                            uint8_t mosi, uint8_t sck, od_epd_shim_mode_t *mode_out)
{
    set_in_pass(false);

    if (s_epd.setPanelType(display_instance) != BBEP_SUCCESS) {
        od_log_warn("drive: no drive data for display %u", (unsigned)display_instance);
        return false;
    }
    s_epd.setRotation((int)rotation_quarters * 90);
    s_epd.initIO(dc, rst, busy, cs, mosi, sck, 0u);
    return od_epd_shim_reselect(mode_index, mode_out);
}

bool od_epd_shim_reselect(uint8_t mode_index, od_epd_shim_mode_t *mode_out)
{
    if (s_epd.selectModeIndex(mode_index) != BBEP_SUCCESS) {
        od_log_warn("drive: mode %u not in config (have %u)",
                    (unsigned)mode_index, (unsigned)s_epd.modeCount());
        return false;
    }
    mode_header_out(mode_out);
    return true;
}

bool od_epd_shim_begin_pass(uint8_t pass)
{
    if (s_epd.beginPass(pass) != BBEP_SUCCESS) {
        od_log_warn("drive: beginPass %u failed rc=%d", (unsigned)pass, s_epd.lastRc());
        set_in_pass(false);
        return false;
    }
    set_in_pass(true);
    return true;
}

int od_epd_shim_last_rc(void)
{
    return s_epd.lastRc();
}

/* --- raw SPI passthrough (CMD_SPI_RAW) -------------------------------------------------------
 *
 * s_in_pass stays FALSE throughout a raw session, so od_epd_shim_write() keeps refusing: the
 * pass state machine is not merely bypassed here, it is still asserting that nothing is in
 * flight. A raw caller uses od_epd_shim_raw_data() instead, which is why that one is separate
 * rather than a flag on the existing write.
 */
bool od_epd_shim_raw_begin(uint8_t display_instance, uint8_t rotation_quarters,
                           uint8_t dc, uint8_t rst, uint8_t busy, uint8_t cs,
                           uint8_t mosi, uint8_t sck, bool run_panel_init)
{
    set_in_pass(false);

    if (s_epd.setPanelType(display_instance) != BBEP_SUCCESS) {
        od_log_warn("spi_raw: no drive data for display %u", (unsigned)display_instance);
        return false;
    }
    s_epd.setRotation((int)rotation_quarters * 90);
    s_epd.initIO(dc, rst, busy, cs, mosi, sck, 0u);

    /* Default is pins and power only -- nothing is clocked out, so virgin register state is
     * observable and the caller owns the whole sequence. That is the useful default for
     * bring-up: a session that initialised first could not answer "what does this panel look
     * like before anyone talks to it".
     *
     * run_panel_init stops at the image marker, so even then the caller still owns everything
     * after the registers. */
    if (run_panel_init) {
        if (s_epd.selectModeIndex(0u) != BBEP_SUCCESS) {
            od_log_warn("spi_raw: mode 0 not in config (have %u)", (unsigned)s_epd.modeCount());
            return false;
        }
        if (s_epd.beginPass(0u) != BBEP_SUCCESS) {
            od_log_warn("spi_raw: beginPass failed rc=%d", s_epd.lastRc());
            return false;
        }
    }
    return true;
}

void od_epd_shim_raw_cmd(uint8_t cmd, const uint8_t *data, uint32_t len)
{
    s_epd.writeCmd(cmd);
    if (data != NULL && len != 0u) {
        s_epd.writeRaw(data, (int)len);
    }
}

void od_epd_shim_raw_data(const uint8_t *data, uint32_t len)
{
    if (data != NULL && len != 0u) {
        s_epd.writeRaw(data, (int)len);
    }
}

void od_epd_shim_raw_busy(uint32_t timeout_ms, uint16_t *elapsed_ms_out, bool *still_busy_out)
{
    const uint32_t t0 = sl_sleeptimer_get_tick_count();
    uint32_t waited = 0u;

    while (s_epd.isBusy()) {
        waited = sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count() - t0);
        if (waited >= timeout_ms) {
            break;
        }
        sl_sleeptimer_delay_millisecond(2u);
    }
    waited = sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count() - t0);

    if (elapsed_ms_out != NULL) {
        *elapsed_ms_out = (uint16_t)((waited > 0xFFFFu) ? 0xFFFFu : waited);
    }
    /* Re-read rather than reuse the loop condition: the distinction the host needs is
     * "released" vs "timed out", and those differ only in the pin's state right now. */
    if (still_busy_out != NULL) {
        *still_busy_out = s_epd.isBusy();
    }
}

bool od_epd_shim_raw_reset(uint8_t hold_ms)
{
    return s_epd.reset(hold_ms) == BBEP_SUCCESS;
}
