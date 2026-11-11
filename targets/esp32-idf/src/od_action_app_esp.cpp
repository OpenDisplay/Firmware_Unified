/* od_action_app on ESP32 -- the effects the 0xB2 action runner can drive here.
 *
 * The LED goes through device_control's LED_ACTIVATE path, so an action flash is displaced by a
 * host LED command like any other run. There is no image store, and od_action loads a
 * DISPLAY_SLOT entry as inactive (OD_CAP_IMGSTORE is 0), so display_slot is never reached.
 *
 * BUZZER entries load as inactive too: od_action names a melody by id, and no melody table
 * exists for the id to index. The passive-buzzer press chirp stays on its flag bit for a config
 * without a table.
 */
#include "od_action_app.h"

#include <limits.h>

#include "device_control.h"
#include "od_hal_adc.h"

extern "C" void od_action_app_led(uint8_t colour, uint8_t brightness)
{
    ledActionFlash(colour, brightness);
}

extern "C" void od_action_app_led_stop(void)
{
    ledActionStop();
}

extern "C" void od_action_app_display_slot(uint8_t slot)
{
    (void)slot;
}

extern "C" void od_action_app_buzzer(uint8_t melody)
{
    (void)melody;
}

extern "C" bool od_action_app_has_buzzer(void)
{
    return false;
}

/* Die temperature, the same source readChipTemperature() reports. The classic ESP32 has no
 * usable sensor, so there temperature entries load as inactive. */
extern "C" int8_t od_action_app_temperature_c(void)
{
    const float c = od_hal_adc_die_temp_c();

    if (c < -128.0f || c > 127.0f) {
        return INT8_MIN;
    }
    return (int8_t)c;
}
