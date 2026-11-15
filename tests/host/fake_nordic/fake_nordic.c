/* fake_nordic.c -- see fake_nordic.h. Drivers, not handlers.
 *
 * Every function here is one the target's command code calls DOWNWARD. None of them knows what a
 * correct reply looks like, and none of them may be told: the frames come from the production
 * handlers linked above, which is the whole reason this profile is worth more than the portable
 * one for the vectors it covers.
 */

#include "fake_nordic.h"

#include "od_xfer_app.h"
#include "opendisplay_ble.h"
#include "opendisplay_buzzer.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_config_storage.h"
#include "opendisplay_display.h"
#include "opendisplay_led.h"

#include <stdarg.h>
#include "od_nfc_app.h"

#include <string.h>

int      fake_disp_data_rc;
int      fake_disp_prepare_rc;
int      fake_disp_refresh_rc;
bool     fake_disp_refresh_ok;
int      fake_disp_start_rc;
int      fake_disp_partial_start_rc;
uint8_t  fake_disp_partial_err;
uint32_t fake_disp_total_bytes;
uint32_t fake_disp_written;
bool     fake_disp_dw_active;
unsigned fake_disp_aborts;
unsigned fake_disp_refreshes;

static uint32_t fake_displayed_etag;
static uint8_t fake_inflate_scratch[256];

bool     fake_store_init_ok;
bool     fake_store_save_ok;
bool     fake_store_load_ok;
bool     fake_store_clear_ok;
unsigned fake_store_saves;
unsigned fake_store_reloads;
uint8_t  fake_store_blob[FAKE_STORE_MAX];
uint32_t fake_store_len;

uint16_t fake_ble_version;
uint8_t  fake_ble_patch;
uint8_t  fake_ble_msd[16];
unsigned fake_ble_dfu;
unsigned fake_ble_deep_sleep;
int      fake_led_activate_rc;
int      fake_led_stop_rc;
bool     fake_led_stop_had_index;
int      fake_buzzer_rc;
bool     fake_nfc_read_ok;
bool     fake_nfc_write_ok;
uint8_t  fake_nfc_rec_type;
uint16_t fake_nfc_read_len;
unsigned fake_nfc_write_calls;
uint8_t  fake_nfc_write_rec_type;
uint16_t fake_nfc_write_len;
uint8_t  fake_nfc_write_data[FAKE_NFC_WRITE_MAX];

uint32_t fake_k_uptime_ms;
uint32_t fake_k_slept_ms;
unsigned fake_nvic_resets;

void fake_nordic_reset(void)
{
    /* A benign, working device. Every knob a vector cares about is then set from its state,
     * so a vector's declared preconditions are the only thing that differs between runs. */
    fake_disp_data_rc = 0;
    fake_disp_prepare_rc = 0;
    fake_disp_refresh_rc = 0;
    fake_disp_refresh_ok = true;
    fake_disp_start_rc = 0;
    fake_disp_partial_start_rc = 0;
    fake_disp_partial_err = 0x07u;
    fake_disp_total_bytes = 4096u;
    fake_disp_written = 0u;
    fake_disp_dw_active = false;
    fake_disp_aborts = 0u;
    fake_disp_refreshes = 0u;
    fake_displayed_etag = 0u;

    fake_store_init_ok = true;
    fake_store_save_ok = true;
    fake_store_load_ok = true;
    fake_store_clear_ok = true;
    fake_store_saves = 0u;
    fake_store_reloads = 0u;
    memset(fake_store_blob, 0, sizeof fake_store_blob);
    fake_store_len = 0u;

    fake_ble_version = 0x0105u;
    fake_ble_patch = 3u;
    memset(fake_ble_msd, 0, sizeof fake_ble_msd);
    fake_ble_dfu = 0u;
    fake_ble_deep_sleep = 0u;
    fake_led_activate_rc = 0;
    fake_led_stop_rc = 0;
    fake_led_stop_had_index = false;
    fake_buzzer_rc = 0;
    fake_nfc_read_ok = true;
    fake_nfc_write_ok = true;
    fake_nfc_rec_type = 0x01u;
    fake_nfc_read_len = 4u;
    fake_nfc_write_calls = 0u;
    fake_nfc_write_rec_type = 0u;
    fake_nfc_write_len = 0u;
    memset(fake_nfc_write_data, 0, sizeof fake_nfc_write_data);

    fake_k_uptime_ms = 1000u;
    fake_k_slept_ms = 0u;
    fake_nvic_resets = 0u;
}

/* ------------------------------------------------------------------------------------ panel --- */

bool opendisplay_display_boot_apply(void) { return false; }
void opendisplay_display_park_pins(void)  { }

/* ---------------------------------------------------------------------- shared transfer seam --- */

void od_xfer_app_prepare_start(void)
{
}

bool od_xfer_app_panel_info(od_xfer_panel_info_t *out)
{
    if (out == NULL || fake_disp_total_bytes == 0u) {
        return false;
    }
    memset(out, 0, sizeof *out);
    if (od_color_direct_geometry(OD_COLOR_SCHEME_MONO, 128u, 256u, &out->geometry)
        != OD_COLOR_OK || out->geometry.total_bytes != fake_disp_total_bytes) {
        return false;
    }
    out->width = 128u;
    out->height = 256u;
    out->partial_enabled = true;
    return true;
}

bool od_xfer_app_begin_full(const od_color_geometry_t *geometry)
{
    if (geometry == NULL || fake_disp_start_rc != 0) {
        return false;
    }
    fake_disp_dw_active = true;
    fake_disp_written = 0u;
    return true;
}

bool od_xfer_app_begin_partial(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                               uint32_t plane_bytes)
{
    (void)x; (void)y; (void)width; (void)height; (void)plane_bytes;
    if (fake_disp_partial_start_rc != 0) {
        return false;
    }
    fake_disp_dw_active = true;
    fake_disp_written = 0u;
    return true;
}

uint32_t od_xfer_app_write(uint32_t stream_offset, od_span_t data)
{
    if (fake_disp_data_rc != 0 || stream_offset != fake_disp_written) {
        return 0u;
    }
    fake_disp_written += (uint32_t)data.n;
    return (uint32_t)data.n;
}

od_mut_span_t od_xfer_app_inflate_scratch(void)
{
    return od_mut_span_make(fake_inflate_scratch, sizeof fake_inflate_scratch);
}

void od_xfer_app_abort(od_xfer_abort_reason_t reason)
{
    (void)reason;
    ++fake_disp_aborts;
    fake_disp_dw_active = false;
    fake_disp_written = 0u;
}

od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner)
{
    (void)owner;
    return OD_XFER_BARRIER_PROCEED;
}

void od_xfer_app_barrier_abort(const od_reply_t *owner)
{
    (void)owner;
    ++fake_disp_aborts;
    fake_disp_dw_active = false;
    fake_disp_written = 0u;
}

bool od_xfer_app_refresh(uint8_t mode, bool *completed)
{
    (void)mode;
    ++fake_disp_refreshes;
    if (fake_disp_refresh_rc != 0 || completed == NULL) {
        return false;
    }
    *completed = fake_disp_refresh_ok;
    fake_disp_dw_active = false;
    return true;
}

uint32_t od_xfer_app_displayed_etag(void) { return fake_displayed_etag; }
void od_xfer_app_set_displayed_etag(uint32_t etag) { fake_displayed_etag = etag; }
uint32_t od_xfer_app_now_ms(void) { return fake_k_uptime_ms; }

/* ----------------------------------------------------------------------------------- storage --- */

bool initConfigStorage(void) { return fake_store_init_ok; }

/* od_config_store_save_error() is the real one: od_config_store.c is linked for the CRC the SEC2
 * key records use. Its busy flag is set only by od_config_store_save(), which this fake store
 * never calls, so a refused save always reads as the generic failure. */

bool saveConfig(uint8_t *data, uint32_t len)
{
    if (!fake_store_save_ok) {
        return false;
    }
    if (len > FAKE_STORE_MAX) {
        return false;
    }
    memcpy(fake_store_blob, data, len);
    fake_store_len = len;
    ++fake_store_saves;
    return true;
}

bool loadConfig(uint8_t *data, uint32_t *len)
{
    if (!fake_store_load_ok || fake_store_len == 0u) {
        return false;      /* "nothing stored" is a real state, and a distinct answer on the wire */
    }
    if (*len < fake_store_len) {
        return false;
    }
    memcpy(data, fake_store_blob, fake_store_len);
    *len = fake_store_len;
    return true;
}

bool clearStoredConfig(void)
{
    if (!fake_store_clear_ok) {
        return false;
    }
    fake_store_len = 0u;
    return true;
}


/* --------------------------------------------------------------------------- BLE and friends --- */

uint16_t opendisplay_ble_get_app_version(void)       { return fake_ble_version; }
uint8_t  opendisplay_ble_get_app_version_patch(void) { return fake_ble_patch; }
void     opendisplay_ble_schedule_dfu(void)          { ++fake_ble_dfu; }
void     opendisplay_ble_schedule_deep_sleep(void)   { ++fake_ble_deep_sleep; }
void     opendisplay_ble_reload_config_from_nvm(void){ ++fake_store_reloads; }

void opendisplay_ble_copy_msd_bytes(uint8_t out[16]) { memcpy(out, fake_ble_msd, 16); }

bool od_nfc_app_read(uint8_t *rec_type, uint8_t *out, uint16_t *out_len, uint16_t max)
{
    uint16_t n = fake_nfc_read_len;

    if (!fake_nfc_read_ok) {
        return false;
    }
    if (n > max) {
        n = max;
    }
    if (rec_type != NULL) {
        *rec_type = fake_nfc_rec_type;
    }
    memset(out, 0x5Au, n);
    *out_len = n;
    return true;
}

bool od_nfc_app_write(uint8_t rec_type, const uint8_t *data, uint16_t len)
{
    ++fake_nfc_write_calls;
    fake_nfc_write_rec_type = rec_type;
    fake_nfc_write_len = len;
    if (data != NULL && len <= FAKE_NFC_WRITE_MAX) {
        memcpy(fake_nfc_write_data, data, len);
    }
    return fake_nfc_write_ok;
}

int opendisplay_led_activate(uint8_t index, const uint8_t *p, uint16_t n)
{
    (void)index; (void)p; (void)n;
    return fake_led_activate_rc;
}

int opendisplay_led_stop(uint8_t index, bool has_index)
{
    (void)index;
    fake_led_stop_had_index = has_index;
    return fake_led_stop_rc;
}

int opendisplay_buzzer_activate(const uint8_t *p, uint16_t n)
{
    (void)p; (void)n;
    return fake_buzzer_rc;
}

/* The parsed security configuration. The suite owns the device's security state through the
 * od_sec2_app hooks, so the command code sees none here. */
const struct SecurityConfig *od_get_parsed_security(void) { return NULL; }

/* ----------------------------------------------------------------------------------- kernel --- */

void k_msleep(int32_t ms)      { fake_k_slept_ms += (uint32_t)((ms > 0) ? ms : 0); }
uint32_t k_uptime_get_32(void) { return fake_k_uptime_ms; }
void NVIC_SystemReset(void)    { ++fake_nvic_resets; }

/* od_log.h is the target's; od_log.c is Zephyr-bound. Bodies are discarded, but the arguments are
 * still compiled, so a wrong conversion specifier is a build error here as on the device. */
void _od_log(int level, const char *fmt, ...);
void _od_log(int level, const char *fmt, ...)
{
    va_list ap;
    (void)level; (void)fmt;
    va_start(ap, fmt);
    va_end(ap);
}

/* The watchdog seam od_cmd_reply.c services around a blocking flush. */
void od_watchdog_app_boot(void)          { }
void od_watchdog_app_arm(void)           { }
void od_watchdog_app_service(void)       { }
bool od_watchdog_app_safe_mode(void)     { return false; }
void od_watchdog_app_phase(uint8_t p)    { (void)p; }
