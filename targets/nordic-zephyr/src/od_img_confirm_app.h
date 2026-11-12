/* od_img_confirm_app.h -- Nordic half of shared/core/od_img_confirm.h: confirm a swapped-in
 * MCUboot image only once it has served GATT, revert one that demonstrably cannot.
 * The event hooks are safe from any thread (BT RX callbacks); the work runs in tick(). */
#ifndef OD_IMG_CONFIRM_APP_H
#define OD_IMG_CONFIRM_APP_H

void od_img_confirm_app_boot(void);
void od_img_confirm_app_link_opened(void);
void od_img_confirm_app_link_closed(void);
void od_img_confirm_app_att_seen(void);
void od_img_confirm_app_served(void);
/* From opendisplay_ble_process() (main thread). */
void od_img_confirm_app_tick(void);

#endif /* OD_IMG_CONFIRM_APP_H */
