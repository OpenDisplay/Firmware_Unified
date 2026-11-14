/* TLSR825x adapter for the shared LED runner (shared/core/od_led.c): LED_ACTIVATE 0x0073 and
 * LED_STOP 0x0074.
 *
 * The runner never sleeps; od_led_tlsr_poll() calls it from the main loop once its requested
 * delay has passed, and the MCU is kept out of suspend while a pattern runs so 100 ms steps are
 * not stretched to the advertising interval. LED pins are outputs only while in use: a finished,
 * stopped or displaced run parks them, which hands PA7 -- the blue LED on ATC boards, and SWS --
 * back to wired flashing. */

#include "od_led_tlsr.h"

#include "od_hal_time.h"
#include "od_led.h"
#include "od_led_app.h"
#include "od_tlsr.h"
#include "opendisplay_structs.h"
#include "tlsr_port.h"

#include <string.h>

/* Boot blink: green, full brightness, ~0.17 s (each flash is 16 x 0.7 ms of PWM). */
#define BOOT_BLINK_COLOR   0x1Cu
#define BOOT_BLINK_FLASHES 15u

static bool s_running;
static uint32_t s_due_ms;
static struct od_led_pins s_pins;   /* the running pattern's pins, parked when it ends */

static void claim(const struct od_led_pins *p)
{
    tlsr_port_gpio_output(p->r, (p->flags & 0x01u) != 0u);
    tlsr_port_gpio_output(p->g, (p->flags & 0x02u) != 0u);
    tlsr_port_gpio_output(p->b, (p->flags & 0x04u) != 0u);
}

static void park(const struct od_led_pins *p)
{
    tlsr_port_led_park(p->r);
    tlsr_port_led_park(p->g);
    tlsr_port_led_park(p->b);
}

static void end_run(void)
{
    if (s_running) {
        park(&s_pins);
        tlsr_port_stay_awake(TLSR_PORT_AWAKE_LED, false);
        s_running = false;
    }
}

static bool pins_of(uint8_t instance, struct od_led_pins *out)
{
    const struct LedConfig *led = od_tlsr_led(instance);

    if (led == NULL) {
        return false;
    }
    out->r = led->led_1_r;
    out->g = led->led_2_g;
    out->b = led->led_3_b;
    out->flags = led->led_flags;
    return true;
}

/* ------------------------------------------------------------------ the od_led seam --- */

void od_led_app_write(uint8_t pin_cfg, bool level_high)
{
    tlsr_port_gpio_write(pin_cfg, level_high);   /* ignores OD_PIN_UNUSED */
}

uint8_t od_led_app_mode(uint8_t instance)
{
    const struct LedConfig *led = od_tlsr_led(instance);

    return led == NULL ? 0u : (uint8_t)(led->reserved[0] & 0x0Fu);
}

void od_led_app_finished(uint8_t instance)
{
    struct LedConfig *led = od_tlsr_led(instance);

    if (led != NULL) {
        led->reserved[0] = 0x00u;
    }
}

/* --------------------------------------------------------------------- the wire API --- */

int od_led_tlsr_activate(uint8_t instance, const uint8_t *rest, uint16_t rest_len)
{
    struct LedConfig *led = od_tlsr_led(instance);
    uint32_t now = od_hal_uptime_ms();

    if (led == NULL) {
        return 2;
    }
    if (rest != NULL && rest_len >= OD_LED_PATTERN_LEN) {
        memcpy(led->reserved, rest, OD_LED_PATTERN_LEN);
    }
    end_run();                                   /* a displaced run's pins may differ */
    (void)pins_of(instance, &s_pins);
    claim(&s_pins);
    if (od_led_activate(instance, &s_pins, led->reserved, now) != 0) {
        /* Mode is not "run": the deployed contract answers success and leaves the LEDs off. */
        (void)od_led_stop(0u, false);
        park(&s_pins);
        return 0;
    }
    s_running = true;
    s_due_ms = now;                              /* the first step may run at once */
    tlsr_port_stay_awake(TLSR_PORT_AWAKE_LED, true);
    return 0;
}

int od_led_tlsr_stop(uint8_t instance, bool instance_given)
{
    int rc = od_led_stop(instance, instance_given);

    if (rc == 0) {
        end_run();
    }
    return rc;
}

void od_led_tlsr_poll(void)
{
    uint32_t now, delay;

    if (!s_running) {
        return;
    }
    now = od_hal_uptime_ms();
    if ((uint32_t)(now - s_due_ms) >= 0x80000000u) {
        return;                                  /* not due yet (wrap-safe) */
    }
    delay = od_led_service(now);
    if (delay == OD_LED_IDLE) {
        end_run();
        return;
    }
    s_due_ms = now + delay;
}

void od_led_tlsr_boot_blink(void)
{
    struct od_led_pins p;
    uint8_t i;

    if (s_running || !pins_of(0u, &p) || p.g == OD_PIN_UNUSED) {
        return;
    }
    p.r = OD_PIN_UNUSED;                       /* green only: leave PA7 (blue) on SWS */
    p.b = OD_PIN_UNUSED;
    claim(&p);
    for (i = 0u; i < BOOT_BLINK_FLASHES; i++) {
        od_led_flash_once(&p, BOOT_BLINK_COLOR, 16u);
    }
    park(&p);
}
