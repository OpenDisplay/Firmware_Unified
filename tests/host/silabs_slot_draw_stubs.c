/* Target glue the BG22 display file links against, for silabs_slot_draw_test. Nothing here is
 * on the slot-draw path except the clock; these only satisfy the link. */

#include "od_hal_radio.h"
#include "od_sec2_app.h"
#include "od_txq.h"
#include "od_cmd_flash.h"
#include "od_imgstore.h"
#include "opendisplay_pipe.h"
#include "opendisplay_ble.h"
#include "em_cmu.h"
#include "em_system.h"
#include "sl_sleeptimer.h"

#include <string.h>

static uint32_t s_now_ms;

uint32_t od_hal_uptime_ms(void) { return s_now_ms; }
uint32_t od_sec2_app_now_ms(void) { return s_now_ms; }
void od_sec2_app_uid(uint8_t uid[8]) { memset(uid, 0x11, 8u); }
uint8_t od_sec2_app_dflags(void) { return 0u; }
const struct SecurityConfig *od_sec2_app_security(void) { return NULL; }

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag, const uint8_t *data,
                                    uint16_t len)
{
    (void)origin; (void)tag; (void)data; (void)len;
    return OD_RADIO_SENT;
}
bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{
    (void)origin; (void)tag;
    return false;
}
void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    (void)rp; (void)len; (void)why;
}

bool od_cmd_flash_take_arm(uint8_t *slot_out) { (void)slot_out; return false; }
void od_cmd_flash_clear_arm(void) { }

bool od_imgstore_begin(uint8_t index, uint16_t w, uint16_t h, uint8_t scheme, uint8_t passes,
                       uint32_t payload_len)
{
    (void)index; (void)w; (void)h; (void)scheme; (void)passes; (void)payload_len;
    return false;
}
bool od_imgstore_write(const uint8_t *data, uint32_t len) { (void)data; (void)len; return false; }
bool od_imgstore_finish(uint32_t *digest_out) { (void)digest_out; return false; }
void od_imgstore_abort(void) { }

uint8_t opendisplay_pipe_connection(void) { return 0xFFu; }
bool opendisplay_pipe_flush_before_refresh(uint32_t tag, uint32_t deadline_ms)
{
    (void)tag; (void)deadline_ms;
    return true;
}
void opendisplay_pipe_abort_xfer_barrier(uint32_t tag) { (void)tag; }

void opendisplay_ble_set_connectable(bool connectable) { (void)connectable; }
uint16_t opendisplay_ble_get_app_version(void) { return 0x0300u; }
uint8_t opendisplay_ble_get_app_version_patch(void) { return 0u; }

void CMU_ClockEnable(CMU_Clock_TypeDef clock, bool enable) { (void)clock; (void)enable; }
uint64_t SYSTEM_GetUnique(void) { return 0x0102030405060708ull; }
void sl_sleeptimer_delay_millisecond(uint16_t time_ms) { s_now_ms += time_ms; }
