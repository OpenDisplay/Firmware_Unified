/* od_health_app.h -- ESP32 actions for shared/core/od_health.h (rollback kind): an OTA image
 * the bootloader boots as PENDING_VERIFY is marked valid only once it has served a GATT write
 * (or after 24 h, or at the deep-sleep hold, with no dead link); one whose links demonstrably
 * carry no ATT is marked invalid and rebooted, so the bootloader boots the previous OTA slot.
 *
 * Event hooks are safe from the NimBLE host task (flag-only); the work runs in tick(), from
 * loop(). Without CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE every image reads as confirmed. */
#ifndef OD_HEALTH_APP_ESP_H
#define OD_HEALTH_APP_ESP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void od_health_app_boot(void);
void od_health_app_link_opened(void);
void od_health_app_link_closed(void);
void od_health_app_att_seen(void);
void od_health_app_served(void);
/* od_smp's image-confirm command marked the running image valid. */
void od_health_app_confirmed_externally(void);
void od_health_app_tick(void);
/* From enterDeepSleep(): false = stay awake for now (the image is unconfirmed and still inside
 * OD_HEALTH_SLEEP_HOLD_MS). May confirm before returning true; may revert and not return. */
bool od_health_app_before_sleep(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_HEALTH_APP_ESP_H */
