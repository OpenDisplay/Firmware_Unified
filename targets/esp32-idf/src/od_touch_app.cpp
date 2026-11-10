/* ESP32's half of od_touch_app.h, and the wiring that keeps the shared machine scheduled.
 *
 * WHAT IS HERE IS DRIVER, NOT POLICY. GPIO, the ISR table, the cached-bus re-selection this
 * target needs and the advertisement write. Cadence, the coordinate map, publish-on-change, the
 * address cascade and the failure backoff are all in shared/core/od_touch_gt911.c.
 */

#include "od_touch_app.h"
#include "touch_input.h"

#include "od_hal_gpio.h"
#include "od_hal_sleep.h"
#include "od_hal_time.h"
#include "od_touch_gt911.h"
#include "display_service.h"

#include "esp_attr.h"      // IRAM_ATTR
#include "driver/gpio.h"    // gpio_hold_*

/* Declared here rather than via main.h: that header pulls bb_epaper.h, which is deliberately off
 * the include path and granted per-source in main/CMakeLists.txt. */
extern struct od_config globalConfig;
extern uint8_t dynamicreturndata[11];

/* ONE ISR PER CONTROLLER, because od_hal_gpio_config_irq() takes a bare handler and the index has
 * to come from somewhere. The _arg variant exists and would collapse these four, but it is what
 * the button port wants; keeping touch on the plain form leaves the two independent. */
static volatile uint8_t s_irq_mask = 0;

static void IRAM_ATTR touch_isr_0(void) { s_irq_mask |= 1u << 0; }
static void IRAM_ATTR touch_isr_1(void) { s_irq_mask |= 1u << 1; }
static void IRAM_ATTR touch_isr_2(void) { s_irq_mask |= 1u << 2; }
static void IRAM_ATTR touch_isr_3(void) { s_irq_mask |= 1u << 3; }

static void (*const s_isrs[OD_TOUCH_MAX_CONTROLLERS])(void) = {
    touch_isr_0, touch_isr_1, touch_isr_2, touch_isr_3
};

void od_touch_app_delay_ms(uint16_t ms)
{
    od_hal_delay_ms(ms);
}

void od_touch_app_delay_us(uint32_t us)
{
    od_hal_delay_us(us);
}

void od_touch_app_gpio_set_mode_output(uint8_t pin)
{
    od_hal_gpio_set_mode_output(pin);
}

void od_touch_app_gpio_config_input(uint8_t pin, bool pull_up)
{
    od_hal_gpio_config_input(pin, pull_up, /*pull_down=*/false);
}

void od_touch_app_gpio_write(uint8_t pin, bool level_high)
{
    od_hal_gpio_write(pin, level_high);
}

int od_touch_app_gpio_read(uint8_t pin)
{
    // ASK BEFORE READING: od_hal_gpio_read() returns 0 for a pin it cannot read, so "unreadable"
    // and LOW are the same value there. The driver treats < 0 as "no level", and the charger seam
    // is the standing example of what folding the two together costs (DIVERGENCE_MATRIX 21).
    if (!od_hal_gpio_pin_valid(pin)) {
        return -1;
    }
    return od_hal_gpio_read(pin);
}

bool od_touch_app_gpio_attach_int(uint8_t idx, uint8_t pin)
{
    if (idx >= OD_TOUCH_MAX_CONTROLLERS) {
        return false;
    }
    return od_hal_gpio_config_irq(pin, OD_GPIO_EDGE_FALLING, s_isrs[idx]) == 0;
}

void od_touch_app_gpio_detach_int(uint8_t pin)
{
    od_hal_gpio_clear_irq(pin);
}

void od_touch_app_bus_invalidate(void)
{
    invalidateOpenDisplayWire();
}

bool od_touch_app_bus_prepare(uint8_t bus_id)
{
    // This target caches one live IDF bus, so a transaction for another device's bus has to
    // re-select before it can be trusted. Nordic re-initialises per operation and returns true.
    return initOrRestoreWireForBus(bus_id);
}

void od_touch_app_irq_consume(uint8_t bits)
{
    if (bits == 0u) {
        return;
    }
    od_hal_gpio_irq_lock();
    s_irq_mask = (uint8_t)(s_irq_mask & ~bits);
    od_hal_gpio_irq_unlock();
}

void od_touch_app_msd_write(uint8_t index, uint8_t value)
{
    if (index < sizeof(dynamicreturndata)) {
        dynamicreturndata[index] = value;
    }
}

void od_touch_app_msd_publish(void)
{
    updatemsdata();
}

/* ------------------------------------------------------------------ scheduling */

static uint32_t s_next_due_ms = 0;

/* A touch reset/enable line keeps whatever level it was last driven, and a bare write does not
 * survive deep sleep, so the off level is latched with a pad hold. A GT911 RST left floating
 * reads HIGH: out of reset, chip powered. rst_pin's only unused sentinel is 0xFF (GPIO0 is a
 * valid reset pin); enable_pin treats 0 and 0xFF as unused. */
static bool touch_enable_pin_used(uint8_t pin) { return pin != 0u && pin != 0xFFu; }

static void touch_pad_hold_disable(uint8_t pin)
{
    if (pin == 0xFFu) return;
    gpio_hold_dis((gpio_num_t)pin);
}

static void touch_pad_hold(uint8_t pin)
{
    if (pin == 0xFFu) return;
#if !defined(CONFIG_IDF_TARGET_ESP32C6)
    gpio_deep_sleep_hold_en();
#endif
    gpio_hold_en((gpio_num_t)pin);
}

static void touch_drive_off(const struct TouchController *tc, bool hold)
{
    if (tc->rst_pin != 0xFFu) {
        od_hal_gpio_config_output(tc->rst_pin, false);   /* GT911 RST is active-low */
        if (hold) touch_pad_hold(tc->rst_pin);
    }
    if (touch_enable_pin_used(tc->enable_pin)) {
        od_hal_gpio_config_output(tc->enable_pin, false);
        if (hold) touch_pad_hold(tc->enable_pin);
    }
}

void touchPrepareForDeepSleep(void)
{
    for (uint8_t i = 0; i < globalConfig.touch_controller_count; i++) {
        touch_drive_off(&globalConfig.touch_controllers[i], true);
    }
}

void initTouchInput(void)
{
    const uint32_t now = od_hal_uptime_ms();

    for (uint8_t i = 0; i < globalConfig.touch_controller_count; i++) {
        const struct TouchController *tc = &globalConfig.touch_controllers[i];
        /* A line held across the previous deep sleep ignores writes until released. */
        touch_pad_hold_disable(tc->rst_pin);
        if (tc->enable_pin != 0u) touch_pad_hold_disable(tc->enable_pin);
        /* Force-disabled controller: hold it in reset rather than leave RST floating. */
        if (tc->touch_ic_type == OD_TOUCH_IC_NONE) touch_drive_off(tc, false);
    }

    s_irq_mask = 0;
    s_next_due_ms = now + od_touch_gt911_init(&globalConfig, now);
}

void processTouchInput(void)
{
    uint32_t now;
    uint8_t  mask;

    // Skip while a transfer is streaming: GT911 reads contend with it for the bus AND the loop
    // task. The shared lifecycle query covers direct, PIPE and partial streams. The refresh case
    // is the driver's own suspend/resume, because it has to re-establish the part afterwards.
    if (transferActive()) {
        return;
    }
    // AN EDGE PULLS SERVICE FORWARD. Checking the deadline first would mean a controller
    // scheduled 255 ms out could not be serviced by an interrupt until that expired -- the
    // authority looks at pending interrupts every 100 ms, so gating on the deadline alone
    // REGRESSES interrupt latency for any configured interval above it.
    od_hal_gpio_irq_lock();
    mask = s_irq_mask;
    od_hal_gpio_irq_unlock();

    now = od_hal_uptime_ms();
    if (mask == 0u && (int32_t)(now - s_next_due_ms) < 0) {
        return;
    }

    // The machine clears bits itself through od_touch_app_irq_consume(), at the instant it acts on
    // one and immediately for one it never will. Nothing is cleared here: doing it after the walk
    // is what discarded edges arriving during the I2C.
    s_next_due_ms = now + od_touch_gt911_service(&globalConfig, now, mask);
}

void touchSuspendForEpdRefresh(void)
{
    od_touch_gt911_suspend();
}

// A RESUME THAT DID NOTHING MUST CHANGE NOTHING. OD_TOUCH_NO_CHANGE means the machine was not
// suspended, or is still nested -- and abortToKnownState() force-resumes on EVERY teardown,
// including ordinary disconnects with nothing suspended. Invalidating the bus and installing a
// delay there would postpone actively polling touch by up to a second on each one.
static void touch_apply_resume(uint32_t delay_ms, uint32_t now)
{
    if (delay_ms == OD_TOUCH_NO_CHANGE) {
        return;
    }
    s_next_due_ms = now + delay_ms;
}

void touchResumeAfterEpdRefresh(void)
{
    const uint32_t now = od_hal_uptime_ms();

    touch_apply_resume(od_touch_gt911_resume(&globalConfig, now), now);
}

void touchForceResume(void)
{
    const uint32_t now = od_hal_uptime_ms();

    touch_apply_resume(od_touch_gt911_force_resume(&globalConfig, now), now);
}

bool touch_input_gpio_is_touch_int(uint8_t pin)
{
    return od_touch_gt911_is_int_pin(&globalConfig, pin);
}
