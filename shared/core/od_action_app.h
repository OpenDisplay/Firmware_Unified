/* od_action_app -- the effects a target provides to od_action.
 *
 * A SEAM, not a HAL: these are not chip peripherals but existing subsystems the
 * action runner drives, and each target already has its own way of reaching them
 * (od_led wants pins out of config, the image store is BG22-only, the buzzer does
 * not exist on every board). Link-time C functions, like every other shared seam.
 *
 * od_action_app_has_buzzer() is a BUILD answer, not a runtime one: it decides
 * whether an entry is reported active or inactive at load, and a capability that
 * could change later would make that report flap.
 */
#ifndef OD_ACTION_APP_H
#define OD_ACTION_APP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* colour is the LedFlashPattern colour byte; brightness is 1..16. */
void od_action_app_led(uint8_t colour, uint8_t brightness);
void od_action_app_led_stop(void);

/* Queue a stored image for display. Returns immediately -- the refresh is seconds
 * long and belongs to the loop, not to a step. */
void od_action_app_display_slot(uint8_t slot);

void od_action_app_buzzer(uint8_t melody);
bool od_action_app_has_buzzer(void);

/* Device temperature in whole degrees C, or INT8_MIN when this build has no
 * sensor -- which is what makes a temperature entry report inactive at load
 * rather than never firing for a reason nothing records. */
int8_t od_action_app_temperature_c(void);

#ifdef __cplusplus
}
#endif
#endif /* OD_ACTION_APP_H */
