/* od_health_app.h -- Nordic actions for shared/core/od_health.h (rollback kind): confirm a swapped-in
 * MCUboot image only once it has served GATT, revert one that demonstrably cannot.
 * The event hooks are safe from any thread (BT RX callbacks); the work runs in tick(). */
#ifndef OD_HEALTH_APP_H
#define OD_HEALTH_APP_H

void od_health_app_boot(void);
void od_health_app_link_opened(void);
void od_health_app_link_closed(void);
void od_health_app_att_seen(void);
void od_health_app_served(void);
/* From opendisplay_ble_process() (main thread). */
void od_health_app_tick(void);

#endif /* OD_HEALTH_APP_H */
