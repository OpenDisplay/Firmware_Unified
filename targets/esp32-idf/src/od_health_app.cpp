/* od_health_app.cpp -- see od_health_app.h and shared/core/od_health.h (rollback kind).
 *
 * Evidence on this target:
 *   link opened / closed   od_ble_evt_connect / od_ble_evt_disconnect (ble_transport_esp32.cpp)
 *   ATT seen               an ATT MTU exchange, a CCC (subscribe) write, an SMP write, or any
 *                          write to the OpenDisplay characteristic (od_ble_nimble.cpp and the
 *                          transport hooks)
 *   served                 a write to the OpenDisplay characteristic from the link owner,
 *                          handed to the RX queue (od_ble_evt_write)
 * NimBLE has no heap-failure event: on this target only the dead-link arm can revert.
 *
 * Why the deep-sleep hold: the ESP32 bootloader runs on every deep-sleep wake, and an image still
 * PENDING_VERIFY at that boot is rolled back. Sleeping unconfirmed would therefore undo every
 * update nobody wrote to within the idle window; holding sleep for OD_HEALTH_SLEEP_HOLD_MS gives
 * the updating client time to reconnect and prove the image. */
#include "od_health_app.h"

#include "od_health.h"
#include "od_log.h"

#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "sdkconfig.h"

void reboot();   /* device_control.cpp: tears NimBLE / WiFi down, then esp_restart() */

static struct od_health s_health;
static bool s_ready;

#define EV_OPEN   0x01u
#define EV_ATT    0x02u
#define EV_SERVED 0x04u
#define EV_CLOSE  0x08u
#define EV_EXT    0x10u
static uint32_t s_ev;            /* EV_*, set on the host task, cleared by tick() */
static uint32_t s_open_ms;
static uint32_t s_close_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void post(uint32_t ev)
{
    __atomic_fetch_or(&s_ev, ev, __ATOMIC_RELEASE);
}

static bool running_image_confirmed(void)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();

    if (run == NULL || esp_ota_get_state_partition(run, &st) != ESP_OK) {
        return true;   /* a factory / non-OTA partition: nothing to roll back */
    }
    return st != ESP_OTA_IMG_PENDING_VERIFY;
#else
    return true;
#endif
}

static const char *confirm_name(uint8_t r)
{
    switch (r) {
    case OD_HEALTH_CONFIRM_ALREADY:  return "already confirmed";
    case OD_HEALTH_CONFIRM_SERVED:   return "GATT write served";
    case OD_HEALTH_CONFIRM_FALLBACK: return "24 h with no dead link";
    case OD_HEALTH_CONFIRM_SLEEP:    return "sleep hold with no dead link";
    case OD_HEALTH_CONFIRM_EXTERNAL: return "confirmed by SMP";
    default:                         return "none";
    }
}

void od_health_app_boot(void)
{
    const bool confirmed = running_image_confirmed();

    od_health_init_rollback(&s_health, confirmed);
    s_ready = true;
    od_log_info("image: %s", confirmed ? "confirmed"
                : "UNCONFIRMED -- confirms on the first served GATT write");
}

void od_health_app_link_opened(void)
{
    s_open_ms = now_ms();
    post(EV_OPEN);
}

void od_health_app_link_closed(void)
{
    s_close_ms = now_ms();
    post(EV_CLOSE);
}

void od_health_app_att_seen(void) { post(EV_ATT); }
void od_health_app_served(void) { post(EV_ATT | EV_SERVED); }
void od_health_app_confirmed_externally(void) { post(EV_EXT); }

static void act(enum od_health_action a)
{
    if (a == OD_HEALTH_CONFIRM) {
        const esp_err_t rc = esp_ota_mark_app_valid_cancel_rollback();

        od_log_info("image confirmed (%s): rc=%d", confirm_name(s_health.confirm_reason), (int)rc);
        if (rc != ESP_OK) {
            od_health_confirm_failed(&s_health);   /* try again on the next evidence */
        }
    } else if (a == OD_HEALTH_RECOVER) {
        if (running_image_confirmed()) {
            od_health_confirmed_externally(&s_health);
            return;
        }
        if (!esp_ota_check_rollback_is_possible()) {
            od_log_error("image cannot serve GATT, but no other valid OTA slot exists: kept");
            od_health_confirmed_externally(&s_health);   /* stop asking */
            return;
        }
        od_log_error("image cannot serve GATT (reason %u, %u dead links): marking it invalid, "
                     "the bootloader reverts it", (unsigned)s_health.reason,
                     (unsigned)s_health.dead_links);
        (void)esp_ota_mark_app_invalid_rollback();
        reboot();
    }
}

void od_health_app_tick(void)
{
    uint32_t ev;

    if (!s_ready) {
        return;
    }
    ev = __atomic_exchange_n(&s_ev, 0u, __ATOMIC_ACQ_REL);
    if ((ev & EV_EXT) != 0u) {
        od_health_confirmed_externally(&s_health);
    }
    if ((ev & EV_OPEN) != 0u) {
        od_health_link_opened(&s_health, s_open_ms);
    }
    if ((ev & EV_ATT) != 0u) {
        od_health_att_seen(&s_health);
    }
    if ((ev & EV_SERVED) != 0u) {
        act(od_health_served(&s_health));
    }
    if ((ev & EV_CLOSE) != 0u) {
        const enum od_health_action v = od_health_link_closed(&s_health, s_close_ms);

        if (!s_health.confirmed && s_health.dead_links != 0u) {
            od_log_warn("image unconfirmed: link with no ATT (%u in a row)",
                        (unsigned)s_health.dead_links);
        }
        act(v);
    }
    act(od_health_poll(&s_health, now_ms()));
}

bool od_health_app_before_sleep(void)
{
    enum od_health_action a;

    if (!s_ready) {
        return true;
    }
    od_health_app_tick();   /* judge a link that closed just before the idle decision */
    a = od_health_before_sleep(&s_health, now_ms());
    if (a == OD_HEALTH_HOLD) {
        return false;
    }
    act(a);   /* CONFIRM, or RECOVER (does not return when it reverts) */
    return true;
}
