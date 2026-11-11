#include "fake_silabs.h"

#include "opendisplay_protocol.h"
#include "sl_bt_api.h"

#include <string.h>

uint32_t fake_silabs_now_ms;
uint32_t fake_silabs_run_advance_ms;
unsigned fake_silabs_run_calls;
unsigned fake_silabs_close_calls;
uint8_t fake_silabs_closed_connection;
uint32_t fake_silabs_notify_status;
uint16_t fake_silabs_att_mtu;
struct fake_silabs_sent_frame fake_silabs_sent[FAKE_SILABS_SENT_MAX];
unsigned fake_silabs_sent_n;
unsigned fake_silabs_delivered_n;
struct fake_silabs_resource_result fake_silabs_resource_script[FAKE_SILABS_RESOURCE_MAX];
unsigned fake_silabs_resource_script_n;
unsigned fake_silabs_resource_calls;
void (*fake_silabs_run_hook)(void);

void fake_silabs_bgapi_reset(void)
{
    fake_silabs_now_ms = 1000u;
    fake_silabs_run_advance_ms = 100u;
    fake_silabs_run_calls = 0u;
    fake_silabs_close_calls = 0u;
    fake_silabs_closed_connection = 0xffu;
    fake_silabs_notify_status = SL_STATUS_OK;
    fake_silabs_att_mtu = OD_BLE_MAX_FRAME;
    memset(fake_silabs_sent, 0, sizeof fake_silabs_sent);
    fake_silabs_sent_n = 0u;
    fake_silabs_delivered_n = 0u;
    memset(fake_silabs_resource_script, 0, sizeof fake_silabs_resource_script);
    fake_silabs_resource_script_n = 0u;
    fake_silabs_resource_calls = 0u;
    fake_silabs_run_hook = NULL;
}

uint32_t sl_sleeptimer_get_tick_count(void) { return fake_silabs_now_ms; }
uint32_t sl_sleeptimer_tick_to_ms(uint32_t ticks) { return ticks; }
/* The target's millisecond clock (od_hal_time.c in production), on the same fake time base. */
uint32_t od_hal_uptime_ms(void) { return fake_silabs_now_ms; }

void sl_bt_run(void)
{
    ++fake_silabs_run_calls;
    fake_silabs_now_ms += fake_silabs_run_advance_ms;
    if (fake_silabs_run_hook != NULL) fake_silabs_run_hook();
}

sl_status_t sl_bt_connection_close(uint8_t connection)
{
    ++fake_silabs_close_calls;
    fake_silabs_closed_connection = connection;
    return SL_STATUS_OK;
}

/* Every attempt is recorded, including refused ones. Two properties depend on that: a RETRY must
 * re-send byte-identical bytes without spending a nonce, and the application must be shown to hand
 * an over-MTU frame to the stack rather than pre-refusing it behind its own MTU guess. */
sl_status_t sl_bt_gatt_server_send_notification(uint8_t connection, uint16_t characteristic,
                                                 uint16_t value_len, const uint8_t *value)
{
    if (fake_silabs_sent_n < FAKE_SILABS_SENT_MAX) {
        struct fake_silabs_sent_frame *f = &fake_silabs_sent[fake_silabs_sent_n++];
        f->connection = connection;
        f->characteristic = characteristic;
        f->len = value_len;
        if (value_len <= sizeof f->data) memcpy(f->data, value, value_len);
    }
    /* The vendor rule, verbatim: "At most, ATT_MTU - 3 number of bytes can be sent in a
     * notification. The error SL_STATUS_COMMAND_TOO_LONG is returned if the value length exceeds
     * ATT_MTU - 3." It errors; it never truncates. */
    if (value_len > (uint16_t)(fake_silabs_att_mtu - 3u)) {
        return SL_STATUS_COMMAND_TOO_LONG;
    }
    if (fake_silabs_notify_status == SL_STATUS_OK) {
        ++fake_silabs_delivered_n;
    }
    return (sl_status_t)fake_silabs_notify_status;
}

sl_status_t sl_bt_resource_get_connection_tx_status(uint8_t connection, uint16_t *flags,
                                                     uint16_t *packets, uint32_t *bytes)
{
    struct fake_silabs_resource_result r = { SL_STATUS_OK, 0u, 0u, 0u };
    unsigned i = fake_silabs_resource_calls++;
    (void)connection;
    if (fake_silabs_resource_script_n != 0u) {
        if (i >= fake_silabs_resource_script_n) i = fake_silabs_resource_script_n - 1u;
        r = fake_silabs_resource_script[i];
    }
    *flags = r.flags;
    *packets = r.packets;
    *bytes = r.bytes;
    return (sl_status_t)r.status;
}
