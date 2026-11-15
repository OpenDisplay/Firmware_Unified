/* Time, reset cause, planned reboot, watchdog and the factory address. */
#include "od_tlsr_sdk.h"

#include "od_tlsr_target.h"

#include "od_tlsr_flash.h"
#include "od_tlsr_layout.h"
#include "od_tlsr_mac.h"
#include "od_tlsr_ota.h"
#include "od_tlsr_pins.h"

#include "od_config.h"

#include "od_hal_reset.h"
#include "od_hal_time.h"

#include <math.h>
#include <string.h>

/* ---- time: the 16 MHz system timer, extended past its 268 s wrap ---- */

static uint32_t s_last_tick, s_ms, s_rem_ticks;

uint32_t od_hal_uptime_ms(void)
{
    uint32_t now = clock_time();
    uint32_t d = now - s_last_tick;

    s_last_tick = now;
    d += s_rem_ticks;
    s_ms += d / (1000u * sys_tick_per_us);
    s_rem_ticks = d % (1000u * sys_tick_per_us);
    return s_ms;
}

void od_hal_delay_us(uint32_t us)
{
    sleep_us(us);
}

void od_tlsr_feed_watchdog(void)
{
    wd_clear();
}

/* ---- reset cause ----
 * DEEP_ANA_REG1 is the one analog register that only a power cycle clears and that the SDK
 * leaves to the application (REG0 is USED_DEEP_ANA_REG and holds a ROM flag; REG2 is the SDK's
 * SYS_DEEP_ANA_REG). Its layout:
 *   [7:4] 0xA, written on every boot, so anything else means power was applied (a brownout
 *         included: this part cannot tell the two apart)
 *   [3]   set by od_hal_reset_reboot() just before a planned reboot
 *   [2:0] unplanned resets (watchdog, reset pin) since power was applied, saturating at 7
 * An unplanned reset is reported as OTHER, which never counts toward a boot gesture. */

#define ANA_STATE_REG     DEEP_ANA_REG1
#define ANA_BOOT_MARK     0xA0u
#define ANA_BOOT_MASK     0xF0u
#define ANA_PLANNED       0x08u
#define ANA_COUNT_MASK    0x07u

static uint32_t s_cause;
static uint8_t s_unplanned;

void od_tlsr_hal_boot(void)
{
    uint8_t st = analog_read(ANA_STATE_REG);

    s_unplanned = 0u;
    if ((st & ANA_BOOT_MASK) != ANA_BOOT_MARK) {
        s_cause = OD_HAL_RESET_POWER_ON;
    } else if ((st & ANA_PLANNED) != 0u) {
        s_cause = OD_HAL_RESET_SOFTWARE;
        s_unplanned = st & ANA_COUNT_MASK;
    } else {
        s_cause = OD_HAL_RESET_OTHER;
        s_unplanned = st & ANA_COUNT_MASK;
        if (s_unplanned < ANA_COUNT_MASK) {
            s_unplanned++;
        }
    }
    analog_write(ANA_STATE_REG, (uint8_t)(ANA_BOOT_MARK | s_unplanned));
    s_last_tick = clock_time();
}

uint8_t od_tlsr_hal_unplanned(void)
{
    return s_unplanned;
}

void od_tlsr_ota_app_boot_info(uint8_t info[2])
{
    info[0] = s_cause == OD_HAL_RESET_POWER_ON ? OD_TLSR_BOOT_POWER_ON
            : s_cause == OD_HAL_RESET_SOFTWARE ? OD_TLSR_BOOT_PLANNED
                                               : OD_TLSR_BOOT_UNPLANNED;
    info[1] = s_unplanned;
}

void od_tlsr_ota_app_before_install(void)
{
    analog_write(ANA_STATE_REG, (uint8_t)(ANA_BOOT_MARK | ANA_PLANNED | s_unplanned));
    wd_clear();
}

uint32_t od_hal_reset_cause(void)
{
    return s_cause != 0u ? s_cause : OD_HAL_RESET_OTHER;
}

/* No RAM survives a reset here that the startup code does not clear; the claim-mode record
 * then simply does not continue across a planned reboot (od_hal_reset.h permits this). */
bool od_hal_retained_read(uint8_t buf[OD_HAL_RETAINED_LEN])
{
    (void)buf;
    return false;
}

bool od_hal_retained_write(const uint8_t buf[OD_HAL_RETAINED_LEN])
{
    (void)buf;
    return false;
}

void od_hal_retained_clear(void)
{
}

void od_hal_reset_reboot(void)
{
    analog_write(ANA_STATE_REG, (uint8_t)(ANA_BOOT_MARK | ANA_PLANNED | s_unplanned));
    start_reboot();
    for (;;) {
    }
}

/* ---- the tag's address (od_tlsr_mac.h), read once ---- */

static uint8_t s_mac[6];
static bool s_mac_ready;

void od_tlsr_mac(uint8_t mac[6])
{
    if (!s_mac_ready) {
        od_tlsr_mac_read(s_mac);
        s_mac_ready = true;
    }
    memcpy(mac, s_mac, 6u);
}

/* ---- MSD inputs ---- */

/* The glass temperature (od_tlsr_display.c); NaN, which the MSD encodes as its floor, until a
 * panel-up has read one. */
float od_tlsr_chip_temperature_c(void)
{
    int8_t c;

    return od_tlsr_display_temperature(&c) ? (float)c : NAN;
}

/* Supply voltage, the B85 way: PowerOption.battery_sense_pin, an otherwise unused ADC-capable
 * pin, is driven HIGH and sampled through the 1/8 prescaler, so it reads the rail it is driven
 * from. No divider and no enable pin; voltage_scaling_factor is not used. The legacy firmware does the
 * same on the pin its config names (PB3 on OD05B911). Sampled at most once a minute. */
#define BATTERY_TTL_MS 60000u

static uint16_t s_batt_mv;
static uint32_t s_batt_at;
static bool s_batt_valid;

uint16_t od_tlsr_battery_mv(void)
{
    const struct od_config *c = od_tlsr_config_loaded() ? od_tlsr_config() : NULL;
    const uint8_t p = c != NULL ? c->power_option.battery_sense_pin : 0xFFu;
    const uint32_t now = od_hal_uptime_ms();
    GPIO_PinTypeDef pin;

    if (!od_tlsr_pin_adc(p)) {
        return 0u;
    }
    if (s_batt_valid && (uint32_t)(now - s_batt_at) < BATTERY_TTL_MS) {
        return s_batt_mv;
    }
    pin = od_tlsr_pin_sdk(p);
    adc_init();
    adc_vbat_init(pin);
    adc_power_on_sar_adc(1);
    s_batt_mv = (uint16_t)adc_sample_and_get_result();
    adc_power_on_sar_adc(0);
    gpio_set_output_en(pin, 0u);
    gpio_set_input_en(pin, 0u);
    s_batt_at = now;
    s_batt_valid = true;
    return s_batt_mv;
}

bool od_tlsr_reboot_flag(void)
{
    return false;
}
