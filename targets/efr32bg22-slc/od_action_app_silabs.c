/* od_action_app on EFR32BG22 -- the effects the action runner can drive here.
 *
 * Each one hands off to the subsystem that already owns the hardware rather than
 * touching pins itself. The LED goes through opendisplay_led, which reads its
 * channel pins out of config; the display goes through the same deferred queue a
 * CMD_FLASH_DISPLAY uses, so an action cannot start a 20-second refresh inside a
 * step.
 */
#include "od_action_app.h"

#include <limits.h>

#include "em_emu.h"

#include "od_cmd_flash.h"
#include "opendisplay_led.h"

void od_action_app_led(uint8_t colour, uint8_t brightness)
{
    /* The 12-byte LedFlashPattern opendisplay_led_activate expects, built for a
     * single flash: mode 1 in the low nibble, brightness-1 in the high nibble,
     * one stage, one loop, no repeat. The odd nibble packing is the wire format's,
     * not ours -- see LedFlashPattern in the contract. */
    uint8_t pat[12] = { 0 };
    const uint8_t b = (brightness == 0u) ? 1u : (brightness > 16u ? 16u : brightness);

    pat[0] = (uint8_t)(0x01u | ((uint8_t)(b - 1u) << 4));  /* mode | brightness-1 */
    pat[1] = colour;
    pat[2] = 0x11u;        /* stage 1: one loop, shortest on-time */
    pat[3] = 0x02u;        /* inter-loop delay */
    pat[10] = 1u;          /* group_repeats */

    (void)opendisplay_led_activate(0u, pat, sizeof pat);
}

void od_action_app_led_stop(void)
{
    (void)opendisplay_led_stop(0u, true);
}

void od_action_app_display_slot(uint8_t slot)
{
    /* Queued, not driven. od_display_from_slot_arm only records the request; the
     * superloop runs the refresh, which is the whole reason an action can name a
     * display effect without stalling every later step behind it. */
    (void)od_display_from_slot_arm(slot);
}

void od_action_app_buzzer(uint8_t melody)
{
    (void)melody;   /* unreachable: has_buzzer() is false, so no entry gets here */
}

bool od_action_app_has_buzzer(void)
{
    /* No buzzer on this board. Answering honestly is what makes a buzzer entry
     * report as INACTIVE at load instead of silently doing nothing at trigger
     * time, which is the difference between a diagnosable config and a mystery. */
    return false;
}

int8_t od_action_app_temperature_c(void)
{
    /* DIE temperature, the same source the advertisement publishes. It reads
     * high from self-heating and is not the panel's temperature -- fine for a
     * window trigger, wrong for anything claiming to measure the environment. */
    const float c = EMU_TemperatureGet();

    if (c < -128.0f || c > 127.0f) {
        return INT8_MIN;      /* implausible: report "no reading", not a number */
    }
    return (int8_t)c;
}
