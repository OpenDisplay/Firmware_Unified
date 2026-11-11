/* Host drivers for targets/efr32bg22-slc/od_cmd_silabs.c. No expected reply bytes live here. */

#include "fake_silabs.h"

#include "od_config_asm.h"
#include "od_color.h"
#include "od_core.h"
#include "od_sec2.h"
#include "od_sec2_app.h"
#include "od_txq.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_config_storage.h"
#include "opendisplay_display.h"
#include "opendisplay_led.h"
#include "opendisplay_pipe.h"

#include "od_nfc_app.h"

#include <string.h>

bool fake_silabs_store_ok;
unsigned fake_silabs_store_attempts;
unsigned fake_silabs_store_saves;
unsigned fake_silabs_store_reloads;
unsigned fake_silabs_store_clears;
bool fake_silabs_save_saw_queue_empty;
bool fake_silabs_reload_saw_queued;
bool fake_silabs_reload_saw_authenticated;
uint8_t fake_silabs_store_blob[2048];
uint32_t fake_silabs_store_len;
bool fake_silabs_xfer_active;
bool fake_silabs_refresh_ok;
unsigned fake_silabs_refreshes;
unsigned fake_silabs_aborts;
unsigned fake_silabs_resets;
bool fake_silabs_nfc_read_ok;
uint16_t fake_silabs_nfc_read_len;
bool     fake_silabs_nfc_write_ok;
unsigned fake_silabs_nfc_write_calls;
uint8_t  fake_silabs_nfc_write_rec_type;
uint16_t fake_silabs_nfc_write_len;
uint8_t  fake_silabs_nfc_write_data[FAKE_SILABS_NFC_WRITE_MAX];

static struct od_config_asm s_assembler;
static uint32_t s_xfer_written;
static uint8_t s_inflate_scratch[256];

struct od_config fake_silabs_global_config;

const struct GlobalConfig *opendisplay_get_global_config(void)
{
    return &fake_silabs_global_config;
}

void fake_silabs_reset(void)
{
    memset(&fake_silabs_global_config, 0, sizeof fake_silabs_global_config);
    fake_silabs_store_ok = true;
    fake_silabs_store_attempts = 0u;
    fake_silabs_store_saves = 0u;
    fake_silabs_store_reloads = 0u;
    fake_silabs_store_clears = 0u;
    fake_silabs_save_saw_queue_empty = false;
    fake_silabs_reload_saw_queued = false;
    fake_silabs_reload_saw_authenticated = false;
    fake_silabs_xfer_active = false;
    s_xfer_written = 0u;
    fake_silabs_refresh_ok = true;
    fake_silabs_refreshes = 0u;
    fake_silabs_aborts = 0u;
    fake_silabs_resets = 0u;
    fake_silabs_nfc_read_ok = true;
    fake_silabs_nfc_read_len = 4u;
    fake_silabs_nfc_write_ok = true;
    fake_silabs_nfc_write_calls = 0u;
    fake_silabs_nfc_write_rec_type = 0u;
    fake_silabs_nfc_write_len = 0u;
    memset(fake_silabs_nfc_write_data, 0, sizeof fake_silabs_nfc_write_data);
    fake_silabs_store_len = 0u;
    memset(fake_silabs_store_blob, 0, sizeof fake_silabs_store_blob);
    od_config_asm_reset(&s_assembler);
}

struct od_config_asm *opendisplay_config_assembler(void) { return &s_assembler; }
bool initConfigStorage(void) { return fake_silabs_store_ok; }

bool saveConfig(uint8_t *data, uint32_t len)
{
    ++fake_silabs_store_attempts;
    fake_silabs_save_saw_queue_empty = od_txq_depth() == 0u;
    if (!fake_silabs_store_ok || data == NULL || len > sizeof fake_silabs_store_blob) return false;
    memcpy(fake_silabs_store_blob, data, len);
    fake_silabs_store_len = len;
    ++fake_silabs_store_saves;
    od_config_asm_reset(&s_assembler);
    return true;
}

bool loadConfig(uint8_t *data, uint32_t *len)
{
    if (!fake_silabs_store_ok || data == NULL || len == NULL || fake_silabs_store_len == 0u ||
        *len < fake_silabs_store_len) return false;
    memcpy(data, fake_silabs_store_blob, fake_silabs_store_len);
    *len = fake_silabs_store_len;
    return true;
}

bool clearStoredConfig(void)
{
    if (!fake_silabs_store_ok) return false;
    fake_silabs_store_len = 0u;
    ++fake_silabs_store_clears;
    return true;
}

const struct SecurityConfig *od_get_parsed_security(void) { return NULL; }

uint16_t opendisplay_ble_get_app_version(void) { return 0x0105u; }
uint8_t opendisplay_ble_get_app_version_patch(void) { return 3u; }
void opendisplay_ble_schedule_dfu(void) { }
void opendisplay_ble_schedule_deep_sleep(void) { }
void opendisplay_ble_reload_config_from_nvm(void)
{
    fake_silabs_reload_saw_queued = od_txq_depth() != 0u;
    fake_silabs_reload_saw_authenticated = od_sec2_session_live(NULL);
    ++fake_silabs_store_reloads;
}
void opendisplay_ble_copy_msd_bytes(uint8_t out[16]) { memset(out, 0, 16u); }

bool od_nfc_app_read(uint8_t *type, uint8_t *data, uint16_t *len, uint16_t max)
{
    const uint16_t n = fake_silabs_nfc_read_len;
    if (!fake_silabs_nfc_read_ok || n > max) return false;
    *type = 1u;
    memset(data, 0x5au, n);
    *len = n;
    return true;
}

bool od_nfc_app_write(uint8_t type, const uint8_t *data, uint16_t len)
{
    ++fake_silabs_nfc_write_calls;
    fake_silabs_nfc_write_rec_type = type;
    fake_silabs_nfc_write_len = len;
    if (data != NULL && len <= FAKE_SILABS_NFC_WRITE_MAX) {
        memcpy(fake_silabs_nfc_write_data, data, len);
    }
    return fake_silabs_nfc_write_ok;
}

void od_xfer_app_prepare_start(void)
{
    if (fake_silabs_xfer_active) opendisplay_display_abort();
}

bool od_xfer_app_panel_info(od_xfer_panel_info_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof *out);
    if (od_color_direct_geometry(OD_COLOR_SCHEME_MONO, 128u, 256u, &out->geometry)
        != OD_COLOR_OK) return false;
    out->width = 128u;
    out->height = 256u;
    out->partial_enabled = false;
    return true;
}

bool od_xfer_app_begin_full(const od_color_geometry_t *geometry)
{
    if (geometry == NULL || geometry->total_bytes != 4096u) return false;
    fake_silabs_xfer_active = true;
    s_xfer_written = 0u;
    return true;
}

uint32_t od_xfer_app_write(uint32_t stream_offset, od_span_t data)
{
    if (!fake_silabs_xfer_active || stream_offset != s_xfer_written || data.n > UINT32_MAX)
        return 0u;
    s_xfer_written += (uint32_t)data.n;
    return (uint32_t)data.n;
}

od_mut_span_t od_xfer_app_inflate_scratch(void)
{ return od_mut_span_make(s_inflate_scratch, sizeof s_inflate_scratch); }

void od_xfer_app_abort(od_xfer_abort_reason_t reason)
{ (void)reason; opendisplay_display_abort(); }

od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner)
{
    uint32_t deadline = od_xfer_app_now_ms() + 2000u;
    return owner != NULL && opendisplay_pipe_flush_before_refresh(owner->tag, deadline)
        ? OD_XFER_BARRIER_PROCEED : OD_XFER_BARRIER_ABORT;
}

void od_xfer_app_barrier_abort(const od_reply_t *owner)
{
    opendisplay_display_abort();
    if (owner != NULL) opendisplay_pipe_abort_xfer_barrier(owner->tag);
}

bool od_xfer_app_refresh(uint8_t mode, bool *completed)
{
    (void)mode;
    if (!fake_silabs_xfer_active || completed == NULL) return false;
    ++fake_silabs_refreshes;
    fake_silabs_xfer_active = false;
    *completed = fake_silabs_refresh_ok;
    return true;
}

uint32_t od_xfer_app_now_ms(void) { return od_sec2_app_now_ms(); }

void opendisplay_display_abort(void)
{ ++fake_silabs_aborts; fake_silabs_xfer_active = false; }
void opendisplay_display_boot_apply(void) { }
void opendisplay_display_park_pins(void) { }
void opendisplay_display_power_off(void) { }

int opendisplay_led_activate(uint8_t index, const uint8_t *data, uint16_t len)
{ (void)index; (void)data; (void)len; return 0; }
int opendisplay_led_stop(uint8_t index, bool present)
{ (void)index; (void)present; return 0; }

#ifndef OD_TEST_REAL_SILABS_PIPE
bool opendisplay_pipe_flush_before_refresh(uint32_t tag, uint32_t deadline_ms)
{ (void)tag; (void)deadline_ms; return true; }
void opendisplay_pipe_close_tag(uint32_t tag) { (void)tag; }
void opendisplay_pipe_abort_xfer_barrier(uint32_t tag)
{ (void)tag; ++fake_silabs_resets; od_core_reset(); }
void opendisplay_pipe_reset_transport(void)
{
    ++fake_silabs_resets;
    if (!od_xfer_frames_may_arrive()) opendisplay_display_abort();
    od_core_reset();
}
void sl_bt_run(void) { }
#endif
void NVIC_SystemReset(void) { ++fake_silabs_resets; }
