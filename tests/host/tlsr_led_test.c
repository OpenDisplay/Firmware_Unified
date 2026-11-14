/* targets/telink-tlsr's LED adapter (od_led_tlsr.c) over the real shared runner, against a fake
 * port that records pin states. What it pins down is the adapter's own job: pins are outputs only
 * while in use, PA7 (SWS) is handed back to its SWS function after every run and never touched by
 * the boot blink, suspend is held exactly while a pattern runs, and the wire answers match BG22. */

#include "od_led_tlsr.h"
#include "od_tlsr.h"
#include "opendisplay_structs.h"
#include "tlsr_port.h"

#include <stdio.h>
#include <string.h>

static int s_checks, s_failures;
#define CHECK(c) do { ++s_checks; if (!(c)) { ++s_failures; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CASE(name) printf("%s\n", name)

enum { PIN_R = 26, PIN_G = 27, PIN_B = 7 };      /* PD2, PD3, PA7 as on the ATC boards */
enum { PARKED = 0, OUTPUT, SWS };

static uint8_t s_state[40];
static bool s_level[40], s_ever_high[40];
static int s_writes[40];
static uint8_t s_awake;
static uint32_t s_now;
static struct LedConfig s_led;
static bool s_have_led = true;

/* ------------------------------------------------------------------------ fake port --- */

void tlsr_port_gpio_output(uint8_t pin, bool level)
{
    if (pin == OD_PIN_UNUSED) return;
    s_state[pin] = OUTPUT;
    s_level[pin] = level;
}

void tlsr_port_gpio_write(uint8_t pin, bool level)
{
    if (pin == OD_PIN_UNUSED) return;
    ++s_writes[pin];
    CHECK(s_state[pin] == OUTPUT || !level);      /* never driven high while not an output */
    s_level[pin] = level;
    if (level) s_ever_high[pin] = true;
}

void tlsr_port_led_park(uint8_t pin)
{
    if (pin == OD_PIN_UNUSED) return;
    s_state[pin] = pin == TLSR_PORT_PIN_SWS ? SWS : PARKED;
}

void tlsr_port_stay_awake(uint8_t holder, bool on)
{
    s_awake = on ? (uint8_t)(s_awake | holder) : (uint8_t)(s_awake & ~holder);
}

uint32_t od_hal_uptime_ms(void) { return s_now; }
void od_hal_delay_us(uint32_t us) { (void)us; }

struct LedConfig *od_tlsr_led(uint8_t instance)
{
    return s_have_led && instance == 0u ? &s_led : NULL;
}

/* -------------------------------------------------------------------------- helpers --- */

static void reset(void)
{
    memset(s_state, 0, sizeof(s_state));
    memset(s_level, 0, sizeof(s_level));
    memset(s_ever_high, 0, sizeof(s_ever_high));
    memset(s_writes, 0, sizeof(s_writes));
    memset(&s_led, 0, sizeof(s_led));
    s_led.led_1_r = PIN_R;
    s_led.led_2_g = PIN_G;
    s_led.led_3_b = PIN_B;
    s_have_led = true;
    s_now = 1000u;
}

/* Mode run, brightness 16; stage 1 = `color`, 2 loops 100 ms apart; stages 2-3 off; one group. */
static void pattern(uint8_t out[12], uint8_t color)
{
    memset(out, 0, 12);
    out[0] = 0xF1u;
    out[1] = color;
    out[2] = (1u << 4) | 2u;
}

/* Poll with the clock advancing 10 ms a step until the run ends; returns the steps taken. */
static int run_to_end(int limit)
{
    int n;
    for (n = 0; n < limit && s_awake != 0u; n++) {
        od_led_tlsr_poll();
        s_now += 10u;
    }
    return n;
}

static bool all_parked(void)
{
    return s_state[PIN_R] == PARKED && s_state[PIN_G] == PARKED && s_state[PIN_B] == SWS;
}

/* ---------------------------------------------------------------------------- cases --- */

static void test_boot_blink(void)
{
    CASE("boot blink: green only, PA7 left on SWS, pins parked after");
    reset();
    s_state[PIN_B] = SWS;
    od_led_tlsr_boot_blink();
    CHECK(s_ever_high[PIN_G]);
    CHECK(s_writes[PIN_R] == 0 && s_writes[PIN_B] == 0);
    CHECK(s_state[PIN_B] == SWS && s_state[PIN_G] == PARKED);
    CHECK(s_awake == 0u);

    CASE("boot blink without a green LED does nothing");
    reset();
    s_led.led_2_g = OD_PIN_UNUSED;
    od_led_tlsr_boot_blink();
    CHECK(s_writes[PIN_R] == 0 && s_writes[PIN_B] == 0);
}

static void test_pattern_runs_and_parks(void)
{
    uint8_t p[12];

    CASE("a blue pattern drives PA7, holds suspend off, then hands PA7 back to SWS");
    reset();
    pattern(p, 0x03u);                                /* blue, full */
    CHECK(od_led_tlsr_activate(0u, p, sizeof(p)) == 0);
    CHECK(s_state[PIN_B] == OUTPUT && (s_awake & TLSR_PORT_AWAKE_LED) != 0u);
    CHECK(run_to_end(1000) < 1000);
    CHECK(s_ever_high[PIN_B]);
    CHECK(all_parked());
    CHECK(s_awake == 0u);
    CHECK((s_led.reserved[0] & 0x0Fu) == 0u);         /* finished: mode nibble cleared */

    CASE("a config reload (mode nibble cleared) ends the run on the next step");
    reset();
    pattern(p, 0xE0u);                                /* red */
    p[10] = 255u;                                     /* repeat forever */
    CHECK(od_led_tlsr_activate(0u, p, sizeof(p)) == 0);
    od_led_tlsr_poll();
    s_led.reserved[0] = 0u;
    s_now += 1000u;
    CHECK(run_to_end(10) < 10);
    CHECK(all_parked() && s_awake == 0u);
}

static void test_stop_and_refusals(void)
{
    uint8_t p[12];

    CASE("LED_STOP mid-run parks at once");
    reset();
    pattern(p, 0x1Cu);
    p[10] = 255u;
    CHECK(od_led_tlsr_activate(0u, p, sizeof(p)) == 0);
    od_led_tlsr_poll();
    CHECK(od_led_tlsr_stop(0u, false) == 0);
    CHECK(all_parked() && s_awake == 0u);

    CASE("LED_STOP for another instance than the running one is refused");
    reset();
    CHECK(od_led_tlsr_activate(0u, p, sizeof(p)) == 0);
    CHECK(od_led_tlsr_stop(1u, true) == 2);
    CHECK(s_awake != 0u);
    CHECK(od_led_tlsr_stop(0u, true) == 0);

    CASE("mode 0 answers success and leaves the LEDs off and parked");
    reset();
    p[0] = 0x00u;
    CHECK(od_led_tlsr_activate(0u, p, sizeof(p)) == 0);
    CHECK(!s_ever_high[PIN_R] && !s_ever_high[PIN_G] && !s_ever_high[PIN_B]);
    CHECK(all_parked() && s_awake == 0u);

    CASE("no LED in the config: instance refused, nothing touched");
    reset();
    s_have_led = false;
    pattern(p, 0x03u);
    CHECK(od_led_tlsr_activate(0u, p, sizeof(p)) == 2);
    CHECK(s_state[PIN_B] == PARKED && s_awake == 0u);
}

int main(void)
{
    test_boot_blink();
    test_pattern_runs_and_parks();
    test_stop_and_refusals();
    printf("tlsr_led: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures != 0;
}
